#include "sfx/platform/win32/file_backup.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <sqlite3.h>

namespace sfx::platform::win32 {
namespace {
namespace app=application;namespace ins=installation;using Path=std::filesystem::path;
std::string digest(const app::FilePayloadIdentity& identity){std::string out;for(const auto b:identity.sha256){out+="0123456789ABCDEF"[b>>4];out+="0123456789ABCDEF"[b&15];}return out;}
bool cancelled(const archive::CancelCheck& check){return check&&check();}
FilePayloadDirectory directory(const Path& root,const std::wstring& owner,const app::FileConfiguration& config,std::int64_t id,const Path& collection){
    const auto found=std::find_if(config.areas.begin(),config.areas.end(),[id](const auto& a){return a.area.id.value==id;});ins::require(found!=config.areas.end(),"file_backup_area_not_configured");
    std::vector<Path> others;for(const auto& a:config.areas)if(a.area.id.value!=id)others.push_back(std::filesystem::u8path(a.storage_path));
    return FilePayloadDirectory::area(root,std::filesystem::u8path(found->storage_path),others,
        {L"Bin",L"Config",L"Data",L"Display",L"Backup",L"Logs",L"Work",L"Doors",L"BotGate",collection},owner);
}
std::vector<std::string> complete_names(const FilePayloadDirectory& directory,const archive::CancelCheck& cancel){
    WIN32_FIND_DATAW item{};const auto find=FindFirstFileW((L"\\\\?\\"+directory.path().native()+L"\\*").c_str(),&item);
    if(find==INVALID_HANDLE_VALUE){ins::require(GetLastError()==ERROR_FILE_NOT_FOUND,"file_backup_directory_unavailable");return {};}
    struct Close{HANDLE value;~Close(){FindClose(value);}} closer{find};std::vector<std::string> names;std::set<std::string> keys;
    do{
        const std::wstring wide=item.cFileName;if(wide==L"."||wide==L"..")continue;
        ins::require(!cancelled(cancel)&&names.size()<100000,"file_backup_cancelled_or_membership_limit");
        // SFX file areas are flat libraries. Never silently omit/follow a nested
        // directory, reparse point, unsupported name or case-colliding object.
        ins::require(!(item.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"file_backup_area_contains_directory_or_link");
        const auto name=Path(wide).u8string();const auto key=app::file_case_key(name);
        ins::require(app::valid_file_name(name)&&key&&keys.insert(*key).second,"file_backup_area_name_ambiguous");names.push_back(name);
    }while(FindNextFileW(find,&item));
    ins::require(GetLastError()==ERROR_NO_MORE_FILES,"file_backup_directory_interrupted");std::sort(names.begin(),names.end());return names;
}
bool same_configuration(const app::FileConfiguration& a,const app::FileConfiguration& b){
    if(a.policy.catalog_revision!=b.policy.catalog_revision||a.areas.size()!=b.areas.size())return false;
    for(const auto& area:a.areas){const auto found=std::find_if(b.areas.begin(),b.areas.end(),[&](const auto& x){return x.area.id==area.area.id;});if(found==b.areas.end()||found->area.revision!=area.area.revision||found->storage_path!=area.storage_path)return false;}return true;
}
}
FileBackupSnapshot::FileBackupSnapshot(const FilePayloadDirectory& directory,std::string name)
    :directory_(directory),name_(std::move(name)),pin_(directory_.read(name_)),identity_(pin_->object_identity()){}
FileBackupSnapshot::~FileBackupSnapshot()=default;
bool FileBackupSnapshot::cleanup() noexcept{
    if(attempted_)return removed_;
    attempted_=true;pin_.reset();
    try{
        if(directory_.absent(name_))return removed_=true;
        auto exact=directory_.read(name_,true);const auto current=exact.object_identity();
        if(current.volume!=identity_.volume||current.file_id!=identity_.file_id)return false;
        exact.remove_exact();removed_=true;
    }catch(...){return false;}
    return removed_;
}
app::FileConfiguration read_backup_file_configuration(const Path& root,const std::wstring& owner){
    auto data=FilePayloadDirectory::private_store(root/L"Data",owner);auto pin=data.read("SFX.DB");
    sqlite3* db{};struct Close{sqlite3*& db;~Close(){if(db)sqlite3_close(db);}} close{db};
    ins::require(sqlite3_open_v2(pin.path().u8string().c_str(),&db,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX,nullptr)==SQLITE_OK,"file_backup_database_open");
    ins::require(sqlite3_exec(db,"PRAGMA query_only=ON; BEGIN",nullptr,nullptr,nullptr)==SQLITE_OK,"file_backup_database_snapshot");
    struct Statement{sqlite3_stmt* p{};~Statement(){if(p)sqlite3_finalize(p);}} exists,policy,areas;
    ins::require(sqlite3_prepare_v2(db,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN('sfx_file_policy','sfx_file_areas','sfx_file_area_paths')",-1,&exists.p,nullptr)==SQLITE_OK&&sqlite3_step(exists.p)==SQLITE_ROW,"file_backup_schema_inspection");
    const auto tables=sqlite3_column_int(exists.p,0);app::FileConfiguration result;if(!tables)return result;ins::require(tables==3,"file_backup_schema_incomplete");
    ins::require(sqlite3_prepare_v2(db,"SELECT catalog_revision FROM sfx_file_policy WHERE singleton_id=1",-1,&policy.p,nullptr)==SQLITE_OK&&sqlite3_step(policy.p)==SQLITE_ROW,"file_backup_policy_read");result.policy.catalog_revision=sqlite3_column_int64(policy.p,0);ins::require(result.policy.catalog_revision>0&&sqlite3_step(policy.p)==SQLITE_DONE,"file_backup_policy_invalid");
    ins::require(sqlite3_prepare_v2(db,"SELECT a.area_id,a.revision,a.name,a.backup_contents,p.storage_path FROM sfx_file_areas a JOIN sfx_file_area_paths p ON p.area_id=a.area_id ORDER BY a.area_id",-1,&areas.p,nullptr)==SQLITE_OK,"file_backup_area_read");
    int status{};while((status=sqlite3_step(areas.p))==SQLITE_ROW){
        app::FileAreaConfiguration area;area.area.id.value=sqlite3_column_int64(areas.p,0);area.area.revision=sqlite3_column_int64(areas.p,1);
        const auto text=[&](int column){const auto n=sqlite3_column_bytes(areas.p,column);const auto* p=sqlite3_column_text(areas.p,column);ins::require(p&&n>0&&n<=2048,"file_backup_database_text");return std::string(reinterpret_cast<const char*>(p),static_cast<std::size_t>(n));};
        area.area.name=text(2);const auto enabled=sqlite3_column_int(areas.p,3);area.area.backup_contents=enabled!=0;area.storage_path=text(4);
        ins::require(result.areas.size()<10000&&area.area.id.value>0&&area.area.revision>0&&(enabled==0||enabled==1),"file_backup_database_area_invalid");result.areas.push_back(std::move(area));
    }
    ins::require(status==SQLITE_DONE,"file_backup_database_interrupted");return result;
}
struct FileBackupCapture::Impl {
    Path root;std::wstring owner;app::FileConfiguration configuration;archive::CancelCheck cancel;
    std::vector<VerifiedBackupMember> predecessors;
    std::vector<FilePayloadDirectory> directories;
    std::vector<std::vector<std::string>> names;
    std::vector<FilePayloadLease> payloads;
    std::vector<archive::CreateBackupRequest::SourceOverride> sources;
    archive::ContentIndex index;
    std::optional<FilePayloadLease> index_pin;std::optional<app::FileObjectIdentity> index_identity;std::string index_name;
    bool bound{},cleaned{};
};
FileBackupCapture::FileBackupCapture(std::unique_ptr<Impl> p):impl_(std::move(p)){}
FileBackupCapture::~FileBackupCapture()=default;
std::unique_ptr<FileBackupCapture> FileBackupCapture::begin(const Path& root,const std::wstring& owner,
    const app::FileConfiguration& configuration,const BackupCollection& collection,archive::BackupKind kind,const archive::CancelCheck& cancel){
    auto p=std::make_unique<Impl>();p->root=root;p->owner=owner;p->configuration=configuration;p->cancel=cancel;
    const bool enabled=std::any_of(configuration.areas.begin(),configuration.areas.end(),[](const auto& a){return a.area.backup_contents;});
    if(enabled){p->predecessors=verified_backup_members(root,owner,collection,kind,cancel);if(!p->predecessors.empty())verify_content_dependencies(p->predecessors,{p->predecessors.size()-1});}
    const VerifiedBackupMember* previous=p->predecessors.empty()?nullptr:&p->predecessors.back();std::set<std::string> included;
    for(const auto& a:configuration.areas){if(!a.area.backup_contents)continue;
        p->directories.push_back(directory(root,owner,configuration,a.area.id.value,collection.directory));auto& dir=p->directories.back();
        p->names.push_back(complete_names(dir,cancel));p->index.areas.push_back({a.area.id.value,a.area.revision,a.area.name,a.storage_path});
        bool same_area=false;if(previous)for(const auto& old:previous->contents.areas)if(old.id==a.area.id.value&&old.configured_path==a.storage_path)same_area=true;
        for(const auto& name:p->names.back()){
            ins::require(p->index.files.size()<100000&&!cancelled(cancel),"file_backup_cancelled_or_total_limit");
            p->payloads.push_back(dir.read(name));auto& lease=p->payloads.back();const auto identity=lease.identity([&](auto,auto){return !cancelled(cancel);});
            archive::ContentFile file{a.area.id.value,name,identity.bytes,digest(identity),{}, {},{}};file.member="FileContents/Objects/"+file.sha256;
            if(same_area){const auto old=std::find_if(previous->contents.files.begin(),previous->contents.files.end(),[&](const auto& f){return f.area==file.area&&f.name==file.name&&f.bytes==file.bytes&&f.sha256==file.sha256;});
                if(old!=previous->contents.files.end()){file.archive=old->archive.empty()?previous->path.filename().u8string():old->archive;file.archive_sha256=old->archive.empty()?previous->sha256:old->archive_sha256;}}
            if(file.archive.empty()&&included.insert(file.member).second)p->sources.push_back({file.member,lease.path()});p->index.files.push_back(std::move(file));
        }
    }
    return std::unique_ptr<FileBackupCapture>(new FileBackupCapture(std::move(p)));
}
void FileBackupCapture::bind(archive::ManagedBackupRequest& request,const app::FileConfiguration& current){
    auto& p=*impl_;ins::require(!p.bound&&!p.cleaned&&same_configuration(p.configuration,current)&&request.installation_root==p.root&&request.file_contents.empty(),"file_backup_configuration_changed_or_reused");
    for(std::size_t n=0;n<p.directories.size();++n)ins::require(complete_names(p.directories[n],p.cancel)==p.names[n],"file_backup_membership_changed_since_snapshot");
    // Include an empty index when all areas are Off. A later On therefore starts
    // a new complete baseline instead of inheriting an older disabled capture.
    const auto bytes=archive::encode_content_index(p.index);const auto work=FilePayloadDirectory::private_store(p.root/L"Work",p.owner);
    p.index_name="file-backup-"+Path(ins::unique_id()).u8string()+".index";
    {auto file=work.create(p.index_name,std::max<std::size_t>(1,bytes.size()));p.index_identity=file.object_identity();for(std::size_t at=0;at<bytes.size();at+=65536){const auto n=std::min<std::size_t>(65536,bytes.size()-at);file.append(bytes.data()+at,n);}(void)file.identity();}
    p.index_pin.emplace(work.read(p.index_name));request.file_contents=p.sources;request.file_contents.push_back({archive::file_contents_index,p.index_pin->path()});p.bound=true;
}
bool FileBackupCapture::cleanup()noexcept{
    if(!impl_||impl_->cleaned)return true;auto& p=*impl_;p.payloads.clear();p.directories.clear();p.predecessors.clear();p.index_pin.reset();
    try{if(p.index_identity){auto work=FilePayloadDirectory::private_store(p.root/L"Work",p.owner);auto file=work.read(p.index_name,true);const auto identity=file.object_identity();ins::require(identity.volume==p.index_identity->volume&&identity.file_id==p.index_identity->file_id,"file_backup_index_cleanup_identity_changed");file.remove_exact();}p.cleaned=true;return true;}catch(...){return false;}
}

struct FileContentRestore::Impl {
    Path root;std::wstring owner;app::FileConfiguration configuration;
    std::vector<VerifiedBackupMember> members;std::size_t point{};
    std::map<std::int64_t,FilePayloadDirectory> directories;
    std::vector<std::pair<std::int64_t,Path>> destinations;
    std::vector<FileContentRestoreItem> items;std::vector<archive::ContentFile> files;
    std::vector<FilePayloadLease> existing;
    bool consumed{};
};
FileContentRestore::FileContentRestore(std::unique_ptr<Impl> p):impl_(std::move(p)){}
FileContentRestore::~FileContentRestore()=default;
const std::vector<FileContentRestoreItem>& FileContentRestore::items()const noexcept{return impl_->items;}
const std::vector<std::pair<std::int64_t,Path>>& FileContentRestore::destinations()const noexcept{return impl_->destinations;}
std::unique_ptr<FileContentRestore> FileContentRestore::preview(const Path& root,const std::wstring& owner,
    const app::FileConfiguration& config,const BackupCollection& collection,const Path& point,
    const std::vector<std::int64_t>& selected,const archive::CancelCheck& cancel){
    ins::require(!selected.empty()&&selected.size()<=10000,"file_restore_select_areas");std::set<std::int64_t> chosen(selected.begin(),selected.end());ins::require(chosen.size()==selected.size(),"file_restore_duplicate_area");
    auto p=std::make_unique<Impl>();p->root=root;p->owner=owner;p->configuration=config;
    p->members=verified_backup_members(root,owner,collection,point.parent_path()==root/L"Backup"/L"Upgrades"?archive::BackupKind::setup_upgrade:archive::BackupKind::manual,cancel);
    const auto found=std::find_if(p->members.begin(),p->members.end(),[&](const auto& m){return m.path==point;});ins::require(found!=p->members.end(),"file_restore_point_not_owned_or_verified");p->point=static_cast<std::size_t>(found-p->members.begin());
    verify_content_dependencies(p->members,{p->point});
    for(const auto id:selected){const auto a=std::find_if(found->contents.areas.begin(),found->contents.areas.end(),[&](const auto& v){return v.id==id;});ins::require(a!=found->contents.areas.end(),"file_restore_area_not_in_point");
        auto destination=directory(root,owner,config,id,collection.directory);p->destinations.emplace_back(id,destination.path());p->directories.emplace(id,std::move(destination));}
    for(const auto& f:found->contents.files)if(chosen.count(f.area)){
        ins::require(!cancelled(cancel),"file_restore_preview_cancelled");auto& destination=p->directories.at(f.area);FileContentRestoreItem item{f.area,f.name,FileContentRestoreItem::State::missing};
        if(!destination.absent(f.name)){p->existing.push_back(destination.read(f.name));const auto identity=p->existing.back().identity([&](auto,auto){return !cancelled(cancel);});item.state=identity.bytes==f.bytes&&digest(identity)==f.sha256?FileContentRestoreItem::State::identical:FileContentRestoreItem::State::conflict;}
        p->items.push_back(item);p->files.push_back(f);
    }
    return std::unique_ptr<FileContentRestore>(new FileContentRestore(std::move(p)));
}
FileContentRestoreResult FileContentRestore::execute(const app::FileConfiguration& config,const archive::CancelCheck& cancel){
    FileContentRestoreResult result;auto& p=*impl_;
    try{
        ins::require(!p.consumed&&same_configuration(p.configuration,config),"file_restore_configuration_changed_or_reused");p.consumed=true;
        for(std::size_t i=0;i<p.items.size();++i){const auto& item=p.items[i];const auto& f=p.files[i];ins::require(!cancelled(cancel),"file_restore_cancelled");
            if(item.state==FileContentRestoreItem::State::identical){++result.identical;continue;}if(item.state==FileContentRestoreItem::State::conflict){++result.conflicts;continue;}
            auto& destination=p.directories.at(f.area);ins::require(destination.absent(f.name),"file_restore_new_conflict_no_overwrite");
            auto source=p.members[p.point].path;if(!f.archive.empty())source=source.parent_path()/std::filesystem::u8path(f.archive);
            const auto temporary="sfx-restore-"+Path(ins::unique_id()).u8string()+".partial";auto output=destination.create(temporary,std::max<std::uint64_t>(1,f.bytes));
            bool confirmed_cancel_before_publication{};
            try{
                const auto streamed=archive::stream_zip_member(source,f.member,f.bytes,[&](const auto* data,std::size_t count){output.append(data,count);return true;},cancel);
                confirmed_cancel_before_publication=streamed.error==archive::Error::cancelled;
                ins::require(streamed.ok(),"file_restore_member_read_failed");const auto identity=output.identity([&](auto,auto){return !cancelled(cancel);});
                ins::require(identity.bytes==f.bytes&&digest(identity)==f.sha256,"file_restore_member_hash_failed");output.rename_to(destination,f.name);++result.restored;
            }catch(...){
                // No async child remains here. Only a positively requested,
                // completed cancellation permits exact owned scratch cleanup.
                // Stream/hash/I/O uncertainty retains the partial for review.
                if(confirmed_cancel_before_publication){
                    try{output.remove_exact();result.diagnostic="file_restore_cancelled_owned_partial_removed";}
                    catch(...){result.diagnostic="file_restore_cancelled_partial_retained_review_destination";}
                }else result.diagnostic="file_restore_failed_partial_retained_review_destination";
                throw;
            }
        }
        result.completed=true;result.diagnostic="Contents restored without overwrite/delete. Catalog identities are unchanged; use File Manager Inspect/Reconcile before serving restored files.";
    }catch(const std::exception& e){if(result.diagnostic.empty())result.diagnostic=e.what();}catch(...){result.diagnostic="file_restore_incomplete_review_destination";}
    return result;
}
} // namespace sfx::platform::win32
