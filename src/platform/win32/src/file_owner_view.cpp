#include "sfx/platform/win32/file_owner_view.hpp"
#include "sfx/platform/win32/file_payload_store.hpp"
#include "sfx/archive/file_zip.hpp"
#include "sfx/application/file_presentation.hpp"
#include "sfx/application/message_source.hpp"
#include <algorithm>
namespace sfx::platform::win32 {
namespace app=application;namespace ins=installation;
namespace {
bool same_view_payload(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b){return a.bytes==b.bytes&&a.sha256==b.sha256&&a.volume==b.volume&&a.file_id==b.file_id;}
}
struct WindowsFileOwnerView::Impl {
    app::IFileStorage& storage;
    app::FileConfigurationActor actor;
    app::ManagedFile record;
    std::int64_t catalog_revision{};
    std::optional<FilePayloadLease> source;
    std::optional<archive::FileZipInspection> zip_directory;
    bool archive_mode{};
    std::string member_name;
    std::vector<app::FileViewCell> cells;
    std::uint32_t rows{};
    explicit Impl(app::IFileStorage& db,app::FileConfigurationActor a):storage(db),actor(std::move(a)){}
    void current(){
        const auto config=storage.file_configuration(actor);const auto file=storage.managed_file(actor,record.file.id);
        ins::require(config&&config.value&&config.value->policy.catalog_revision==catalog_revision&&file&&file.value&&file.value->file.revision==record.file.revision&&file.value->file.area==record.file.area&&file.value->file.state==app::FileCatalogState::available&&!file.value->operation_pending&&!file.value->purged&&same_view_payload(file.value->payload,record.payload),"file_view_context_changed");
    }
    std::string bytes(const Progress& progress){
        ins::require(record.payload.bytes<=app::kMessageSourceBytes,"file_view_text_size_limit");std::string result;result.reserve(static_cast<std::size_t>(record.payload.bytes));
        for(std::uint64_t at=0;at<record.payload.bytes;){ins::require(!progress||progress(at,record.payload.bytes),"file_view_cancelled");const auto part=source->read(at,static_cast<std::size_t>(std::min<std::uint64_t>(65536,record.payload.bytes-at)));ins::require(!part.empty(),"file_view_short_read");result.append(reinterpret_cast<const char*>(part.data()),part.size());at+=part.size();}return result;
    }
    archive::FileZipSource zip_source(const Progress& progress){return {record.payload.bytes,[this,progress](std::uint64_t at,std::size_t maximum){ins::require(!progress||progress(at,record.payload.bytes),"file_view_cancelled");return source->read(at,maximum);},{}};}
    void text(std::string_view bytes){
        ins::require(bytes.size()<=app::kMessageSourceBytes,"file_view_text_size_limit");cells.clear();rows=0;
        if(bytes.find('\x1b')!=std::string_view::npos){
            auto validated=app::validate_message_source({app::MessageSourceFormat::ansi_canvas,1,std::string(bytes)},app::kMessageCanvasRows);ins::require(static_cast<bool>(validated),"file_view_unsupported_content");rows=validated.off.extent;cells.reserve(validated.off.cells.size());for(const auto& c:validated.off.cells)cells.push_back({c.glyph,c.attribute});return;
        }
        // Reuse the caller's safe normalization/UTF-8->CP437/tab/line rules.
        // The returned plain program has no ANSI instructions to interpret.
        const auto program=app::prepare_file_view(bytes,false);ins::require(program.has_value()||bytes.empty(),"file_view_unsupported_content");cells.resize(app::kMessageCanvasRows*app::kFileViewColumns);
        std::size_t row{},column{};rows=1;if(!program)return;
        for(const auto& op:program->operations){
            if(op.kind==app::TerminalDisplayOperationKind::cp437_text){for(unsigned char c:op.bytes){ins::require(row<app::kMessageCanvasRows&&column<app::kFileViewColumns&&c>=32&&c!=127,"file_view_cell_limit");cells[row*app::kFileViewColumns+column++]={c,7};rows=static_cast<std::uint32_t>(row+1);}}
            else if(op.kind==app::TerminalDisplayOperationKind::paging_newline||op.kind==app::TerminalDisplayOperationKind::logical_newline){ins::require(row<app::kMessageCanvasRows,"file_view_row_limit");rows=static_cast<std::uint32_t>(row+1);++row;column=0;}
            else throw std::runtime_error("file_view_unexpected_plain_instruction");
        }
    }
    app::FileManagementViewPage page(std::uint16_t index){
        current();app::FileManagementViewPage result;result.file=record.file.id;result.revision=record.file.revision;result.filename=record.file.name;result.member=member_name;result.archive=archive_mode;result.page=index;
        if(archive_mode){ins::require(zip_directory.has_value(),"file_view_archive_unavailable");const auto& entries=zip_directory->entries;result.total=static_cast<std::uint32_t>(entries.size());const auto first=static_cast<std::size_t>(index)*app::kFileViewArchivePage;ins::require(first<entries.size()||(first==0&&entries.empty()),"file_view_page_unavailable");
            for(std::size_t n=first;n<std::min(entries.size(),first+app::kFileViewArchivePage);++n){const auto& e=entries[n];auto name=app::sanitize_file_text(e.name,512,false);if(name.empty())name="[unprintable member]";result.entries.push_back({static_cast<std::uint16_t>(n),std::move(name),e.bytes,e.directory,e.encrypted,e.readable});}
        }else{result.total=rows;const auto first=static_cast<std::size_t>(index)*app::kFileViewRows;ins::require(first<rows,"file_view_page_unavailable");const auto last=std::min<std::size_t>(rows,first+app::kFileViewRows);result.cells.assign(cells.begin()+first*app::kFileViewColumns,cells.begin()+last*app::kFileViewColumns);}
        return result;
    }
};
WindowsFileOwnerView::WindowsFileOwnerView(app::IFileStorage& storage,std::filesystem::path root,std::wstring owner,app::FileId id,bool zip,const Progress& progress,app::FileConfigurationActor actor):impl_(std::make_unique<Impl>(storage,actor)){
    const auto config=storage.file_configuration(actor);const auto record=storage.managed_file(actor,id);ins::require(config&&config.value&&record&&record.value,"file_view_owner_context_unavailable");auto& p=*impl_;p.record=*record.value;p.catalog_revision=config.value->policy.catalog_revision;p.current();
    ins::require(zip?p.record.payload.bytes<UINT32_MAX:p.record.payload.bytes<=app::kMessageSourceBytes,"file_view_size_limit");
    const auto selected=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&p](const auto& a){return a.area.id==p.record.file.area;});ins::require(selected!=config.value->areas.end(),"file_view_area_unavailable");
    std::vector<std::filesystem::path> others;for(const auto& a:config.value->areas)if(!(a.area.id==p.record.file.area))others.push_back(std::filesystem::u8path(a.storage_path));
    auto directory=FilePayloadDirectory::area(root,std::filesystem::u8path(selected->storage_path),others,{L"Bin",L"Config",L"Data",L"Display",L"Backup",L"Logs",L"Work",L"Doors",L"BotGate"},owner);p.source.emplace(directory.read(app::file_managed_payload_name(p.record)));
    ins::require(same_view_payload(p.source->identity(progress),p.record.payload),"file_view_payload_changed");
    if(zip){p.zip_directory=archive::inspect_file_zip(p.zip_source(progress));ins::require(p.zip_directory->result.ok(),"file_view_unsupported_archive");p.archive_mode=true;}
    else p.text(p.bytes(progress));p.current();
}
WindowsFileOwnerView::~WindowsFileOwnerView()=default;
app::FileManagementViewPage WindowsFileOwnerView::page(std::uint16_t index){return impl_->page(index);}
app::FileManagementViewPage WindowsFileOwnerView::member(std::uint16_t index,const Progress& progress){
    auto& p=*impl_;p.current();ins::require(p.zip_directory&&index<p.zip_directory->entries.size()&&p.zip_directory->entries[index].readable,"file_view_member_list_only");const auto result=archive::read_file_zip_entry(p.zip_source(progress),index);ins::require(result.result.ok(),"file_view_member_unavailable");p.text(result.bytes);p.archive_mode=false;p.member_name=app::sanitize_file_text(p.zip_directory->entries[index].name,512,false);return p.page(0);
}
app::FileManagementViewPage WindowsFileOwnerView::archive_index(std::uint16_t page){auto& p=*impl_;ins::require(p.zip_directory.has_value(),"file_view_archive_unavailable");p.archive_mode=true;p.member_name.clear();return p.page(page);}
} // namespace sfx::platform::win32
