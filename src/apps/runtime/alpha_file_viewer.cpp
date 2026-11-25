#include "alpha_file_viewer.hpp"
#include "sfx/archive/file_zip.hpp"
#include "sfx/application/message_source.hpp"
#include <algorithm>

namespace sfx::apps::runtime {
namespace app=application;
namespace win=platform::win32;
namespace ins=win::installation;
AlphaFileViewer::AlphaFileViewer(CallerFileViewerPorts p):p_(std::move(p)){}
bool AlphaFileViewer::notice(std::string_view text){const auto size=p_.list.dimensions?p_.list.dimensions():app::MessageEditorDimensions{};if(size.ansi&&p_.list.write&&!p_.list.write("\x1b[0;37;40m"))return false;return app::present_file_text(p_.list.terminal,"files.viewer.notice",text,size.columns,size.rows);}
bool AlphaFileViewer::view(app::FileId id,bool archive){
    try{
        const auto actor=p_.actor();if(!actor)return false;
        const auto record=p_.storage.file_information(*actor,id);if(!record||!record.value)return notice("File unavailable.");
        const auto config=p_.storage.file_configuration({true,{}});if(!config||!config.value)return notice("File unavailable.");
        const auto& file=*record.value;
        if((!archive&&file.bytes>app::kMessageSourceBytes)||(archive&&file.bytes>=0xffffffffULL))return notice("File exceeds the safe online-view limit. Download it to view locally.");
        const auto configured=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& a){return a.area.id==file.area;});if(configured==config.value->areas.end())return notice("File unavailable.");
        const auto group=std::find_if(config.value->groups.begin(),config.value->groups.end(),[&](const auto& g){return g.id==configured->area.group;});
        // Listing metadata is not a payload-read capability. In particular the
        // review Uploads area must not leak its content through online viewing.
        if(!p_.authority||group==config.value->groups.end()||!app::file_area_downloadable(*group,configured->area,p_.authority()))return notice("File unavailable for viewing.");
        std::vector<std::filesystem::path> others;for(const auto& a:config.value->areas)if(!(a.area.id==file.area))others.push_back(std::filesystem::u8path(a.storage_path));
        auto directory=win::FilePayloadDirectory::area(p_.installation,std::filesystem::u8path(configured->storage_path),others,{L"Bin",L"Config",L"Data",L"Display",L"Backup",L"Logs",L"Work",L"Doors",L"BotGate"},p_.owner);
        const auto managed=p_.storage.managed_file({true,{}},id);
        if(!managed||!managed.value||managed.value->file.revision!=file.revision||!(managed.value->file.area==file.area)||managed.value->payload.bytes!=file.bytes||managed.value->payload.sha256!=file.sha256)return notice("File changed or unavailable.");
        auto source=directory.read(app::file_managed_payload_name(*managed.value));
        const auto live=[&]{
            if(!p_.list.live()||!app::file_area_downloadable(*group,configured->area,p_.authority()))return false;const auto current=p_.actor();if(!current||current->session!=actor->session||current->account!=actor->account||current->account_revision!=actor->account_revision||current->policy_revision!=actor->policy_revision)return false;
            const auto catalog=p_.storage.file_catalog(*current);if(!catalog||!catalog.value||catalog.value->revision!=config.value->policy.catalog_revision)return false;
            const auto visible=p_.storage.file_information(*current,id);return visible&&visible.value&&visible.value->revision==file.revision&&visible.value->sha256==file.sha256&&visible.value->bytes==file.bytes;
        };
        const auto identity=source.identity([&](auto,auto){return live();});if(identity.bytes!=file.bytes||identity.sha256!=file.sha256||identity.volume!=managed.value->payload.volume||identity.file_id!=managed.value->payload.file_id)return notice("File changed or unavailable. Ask the Sysop to reconcile it.");
        const auto show=[&](std::string_view bytes){
            if(!live())return notice("File authorization changed. Return and select it again.");
            const auto size=p_.list.dimensions?p_.list.dimensions():app::MessageEditorDimensions{};
            const auto program=app::prepare_file_view(bytes,size.ansi);if(!program)return notice("This content is binary, unsupported, or exceeds safe online-view limits.");
            if(!p_.display)return notice("File viewer unavailable.");
            const auto outcome=p_.display(*program,live);
            return outcome==app::TerminalOutcome::completed||outcome==app::TerminalOutcome::cancelled;
        };
        if(!archive){std::string bytes;bytes.reserve(static_cast<std::size_t>(file.bytes));for(std::uint64_t at=0;at<file.bytes;){ins::require(live(),"file_view_revoked");const auto part=source.read(at,static_cast<std::size_t>(std::min<std::uint64_t>(65536,file.bytes-at)));ins::require(!part.empty(),"file_view_short_read");bytes.append(reinterpret_cast<const char*>(part.data()),part.size());at+=part.size();}return show(bytes);}
        sfx::archive::FileZipSource zip{file.bytes,[&](auto at,auto maximum){ins::require(live(),"file_view_revoked");return source.read(at,maximum);},[&]{return !live();}};
        const auto inspection=sfx::archive::inspect_file_zip(zip);if(!inspection.result.ok())return notice("Archive is unsupported, encrypted, damaged, or exceeds safe inspection limits.");
        std::size_t page{},selected{};
        for(;;){
            if(!live())return notice("File authorization changed. Return and select it again.");
            app::FileListPage rows;rows.title="Archive: "+file.name;rows.context="Enter reads a supported text/ANSI member; binary entries are list-only.";rows.columns=" # Member                                                      Size";
            rows.previous=page>0;rows.next=(page+1)*100<inspection.entries.size();
            for(std::size_t i=page*100;i<std::min((page+1)*100,inspection.entries.size());++i){const auto& entry=inspection.entries[i];rows.rows.push_back({static_cast<unsigned>(i+1),static_cast<std::int64_t>(i+1),1,entry.name+"  "+std::to_string(entry.bytes)+" bytes"+(entry.directory?" [directory]":entry.encrypted?" [encrypted]":"")});}
            auto list=p_.list;const auto preferences=p_.storage.file_caller_state(*actor);if(!preferences||!preferences.value)return notice("File preferences unavailable.");list.lightbars=list.lightbars&&preferences.value->lightbar;list.live=live;list.emitted={};const auto choice=app::choose_file_row(list,rows,selected);
            if(choice.action==app::FileListAction::cancel)return true;if(choice.action==app::FileListAction::failed)return false;
            if(choice.action==app::FileListAction::next){++page;selected=0;continue;}if(choice.action==app::FileListAction::previous){--page;selected=0;continue;}
            const auto index=static_cast<std::size_t>(choice.identity-1);if(index>=inspection.entries.size())return notice("Archive entry unavailable.");const auto& entry=inspection.entries[index];
            if(entry.directory||entry.encrypted||!entry.readable){if(!notice("This entry can only be listed."))return false;continue;}
            const auto text=sfx::archive::read_file_zip_entry(zip,index);if(!text.result.ok()){if(!notice("Entry unavailable or exceeds safe online-view limits."))return false;continue;}if(!show(text.bytes))return false;
        }
    }catch(...){return notice("File view unavailable. No file was changed.");}
}
} // namespace sfx::apps::runtime
