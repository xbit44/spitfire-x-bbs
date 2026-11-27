#include "sfx/platform/linux/bbs_health.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "sfx/platform/linux/ftn_secrets.hpp"
#include "sfx/platform/linux/installation_layout.hpp"
#include "sfx/application/operational_configuration.hpp"
#include "sfx/archive/backup_service.hpp"
#include "sqlite_sfx_baseline.hpp"
#include "sqlite_statement.hpp"
#include <sqlite3.h>
#include <chrono>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace sfx::platform::linux_host {
namespace {
namespace fs=std::filesystem;namespace app=application;namespace sql=adapters::storage::internal;
using State=BbsHealthState;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void add(BbsHealthReport& out,State state,std::string subject,std::string detail,std::string next={}){
    if(out.findings.size()<255)out.findings.push_back({state,std::move(subject),std::move(detail),std::move(next)});
    else if(out.findings.size()==255)out.findings.push_back({State::could_not_check,"Additional findings","The displayed finding limit was reached.","Address listed findings, then check again."});
}
bool same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
void save(BbsHealthReport& report,const fs::path& root){
    try{
        auto logs=open_owned_directory(root/"logs");const auto id=LinuxSecureIdentifierGenerator{}.generate();
        std::string suffix;for(auto byte:id.bytes){suffix+="0123456789abcdef"[byte>>4];suffix+="0123456789abcdef"[byte&15];}
        const auto name="bbs-health-"+archive::current_timestamp().filename_component+"-"+suffix+".txt";
        const auto text=report.text();publish_private_file(logs.get(),name,{text.begin(),text.end()},false);
        report.saved_report=root/"logs"/name;
    }catch(...){add(report,State::could_not_check,"Saved report","A complete private report could not be saved.","Check Logs access and free space. Any incomplete new report remains preserved.");}
}
}
std::string BbsHealthReport::text()const{
    std::string out="SFX BBS Health Check\nReport-only: no repairs, migrations, preparation, hold changes or cleanup.\n\n";
    for(const auto& f:findings){out+=(f.state==State::ok?"OK":f.state==State::needs_attention?"Needs attention":"Could not check");out+=" - "+f.subject+"\n"+f.detail+"\n";if(!f.next_step.empty())out+="Next: "+f.next_step+"\n";out+='\n';}return out;
}
BbsHealthReport run_bbs_health_check(const fs::path& root,const std::function<bool()>& cancelled){
    BbsHealthReport result;InstallationLock stopped;FileDescriptor root_pin;struct stat root_stamp{};
    try{require(geteuid()!=0,"health_normal_account_required");layout::require_current(root);root_pin=open_owned_directory(root);require(fstat(root_pin.get(),&root_stamp)==0&&stopped.acquire(root,".sfx-runtime.lock"),"health_stopped_owner_required");}
    catch(...){add(result,State::could_not_check,"Stopped BBS","Exclusive access to this owned installation could not be established.","Stop the BBS and close management or other maintenance, then retry under its normal account.");return result;}
    result.stopped=true;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    const auto budget=[&]{require(std::chrono::steady_clock::now()<deadline&&(!cancelled||!cancelled()),"health_cancelled_or_time_budget");};
    try{
        for(const auto* role:{"config","data","logs","work","files","doors","display","botgate","backup"}){
            budget();try{(void)open_owned_directory(root/role);}catch(...){add(result,State::needs_attention,role,"Required private local folder is missing, redirected or inaccessible.","Restore the matching folder or review its access. No permissions were changed.");}
        }
        for(const auto* role:{"node","ftn"}){struct stat info{};budget();if(fstatat(root_pin.get(),role,&info,AT_SYMLINK_NOFOLLOW)==0){try{(void)open_owned_directory(root/role);}catch(...){add(result,State::needs_attention,role,"An existing optional work folder is redirected or inaccessible.","Review this folder and its access; no permissions were changed.");}}else require(errno==ENOENT,"health_optional_role_unavailable");}
        auto config=open_owned_directory(root/"config");auto data=open_owned_directory(root/"data");struct stat pending{};
        const auto marked=fstatat(config.get(),"compaction-pending.v1",&pending,AT_SYMLINK_NOFOLLOW);
        if(marked==0)add(result,State::needs_attention,"Interrupted compaction","The pending compaction marker is present. A readable database alone does not prove that compaction completed.","Keep the marker, work and logs. Use a verified pre-compaction backup in Restore, then Prepare copied file records if requested. Do not delete the marker to resume.");
        else require(errno==ENOENT,"health_compaction_marker_unavailable");
        try{const auto raw=read_private_file(config.get(),"sfx.conf",app::kOperationalConfigurationMaximumBytes);require(app::parse_operational_configuration(std::string(raw.begin(),raw.end())).valid(),"invalid_configuration");add(result,State::ok,"Board settings","Saved board settings decode successfully.");}
        catch(...){add(result,State::needs_attention,"Board settings","Saved board settings are missing or invalid.","Restore matching Config files before starting the BBS.");}
        auto key=load_portable_board_key(root);add(result,key?State::ok:State::needs_attention,"Portable board key",key?"The existing portable board key and identity are valid. No key material is included in this report.":"The existing board key or identity is unavailable. Nothing was regenerated.",key?"":"Restore the matching complete Config copy.");
        std::shared_ptr<LinuxFilePayloadBindings> bindings;
        try{bindings=LinuxFilePayloadBindings::open(root);const auto deferred=bindings->deferred_records();add(result,deferred?State::needs_attention:State::ok,"Copy-recovery file records",deferred?std::to_string(deferred)+" unresolved file records remain preserved.":"Authenticated file records belong to this folder. Payload contents were not rehashed.",deferred?"Review preserved file-operation evidence and run explicit Prepare when appropriate.":"");}
        catch(...){add(result,State::needs_attention,"Copy-recovery file records","File records are missing, invalid or belong to a copied/restored folder. No map was created or repaired.","Use Prepare for a complete copied/restored board. Restore missing matching files first; a pending compaction requires verified Restore first.");}
        const auto configured_path=[&](const std::string& value,bool directory,const char* subject){
            if(value.empty())return;
            budget();try{auto path=fs::u8path(value);if(bindings)path=bindings->resolve_path(path);else if(path.is_relative())path=root/path;
                const auto relative=path.lexically_relative(root);
                if(relative.empty()||*relative.begin()==".."){add(result,State::could_not_check,subject,"An external configured path needs separate review; no external target was probed.","Check this path in its configuration page. External resources need their own copy.");return;}
                if(directory)(void)open_owned_directory(path);else{auto parent=open_owned_directory(path.parent_path());FileDescriptor file(openat(parent.get(),path.filename().c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));require(private_regular_file(file.get(),UINT64_MAX),"health_configured_file_unavailable");}
            }catch(...){add(result,State::needs_attention,subject,"An owned configured resource is unavailable or redirected.","Review the configured path and matching recovery copy. Paths were not rewritten.");}
        };
        budget();FileDescriptor database(openat(data.get(),"sfx.db",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));struct stat stamp{};
        require(private_regular_file(database.get(),UINT64_MAX)&&fstat(database.get(),&stamp)==0&&stamp.st_size>0,"health_database_unavailable");
        bool sidecar=false;
        for(const auto* name:{"sfx.db-wal","sfx.db-journal"}){struct stat info{};if(fstatat(data.get(),name,&info,AT_SYMLINK_NOFOLLOW)==0){if(!S_ISREG(info.st_mode)||info.st_size!=0)sidecar=true;}else if(errno!=ENOENT)sidecar=true;}
        if(sidecar){add(result,State::could_not_check,"Database integrity","Retained SQLite WAL or journal data prevents a complete immutable main-file diagnosis.","Preserve all Data files. Recover a consistent database before retrying; do not delete sidecars.");save(result,root);return result;}
        sqlite3* raw{};const auto uri="file:/proc/self/fd/"+std::to_string(database.get())+"?mode=ro&immutable=1";
        const auto opened=sqlite3_open_v2(uri.c_str(),&raw,SQLITE_OPEN_READONLY|SQLITE_OPEN_URI,nullptr);std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)> db(raw,sqlite3_close_v2);
        require(opened==SQLITE_OK&&raw&&sqlite3_db_readonly(raw,"main")==1,"health_readonly_database_unavailable");
        require(sqlite3_exec(raw,"PRAGMA query_only=ON;PRAGMA trusted_schema=OFF;PRAGMA temp_store=MEMORY;BEGIN",nullptr,nullptr,nullptr)==SQLITE_OK,"health_readonly_transaction_unavailable");
        struct Budget {const std::function<bool()>& cancelled;std::chrono::steady_clock::time_point end;} progress{cancelled,deadline};
        sqlite3_progress_handler(raw,1000,[](void* context){const auto& b=*static_cast<Budget*>(context);return int(std::chrono::steady_clock::now()>=b.end||(b.cancelled&&b.cancelled()));},&progress);
        struct Reset {sqlite3* db;~Reset(){sqlite3_progress_handler(db,0,nullptr,nullptr);}} reset{raw};
        require(sql::validate_sfx_shipping_database(raw,true),"health_database_validation_failed");budget();
        struct stat after{},named{};require(fstat(database.get(),&after)==0&&fstatat(data.get(),"sfx.db",&named,AT_SYMLINK_NOFOLLOW)==0&&same(stamp,after)&&same(stamp,named)&&stamp.st_size==after.st_size&&stamp.st_mtim.tv_sec==after.st_mtim.tv_sec&&stamp.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&stamp.st_ctim.tv_sec==after.st_ctim.tv_sec&&stamp.st_ctim.tv_nsec==after.st_ctim.tv_nsec,"health_database_changed");
        add(result,State::ok,"Database integrity","Supported schema, SQLite integrity and foreign-key references passed on a pinned immutable read-only database. This does not clear recovery guards.");
        sql::SqliteStatement holds(raw,"SELECT count(*) FROM sfx_message_integrity_holds","read message holds");if(holds.row()&&holds.int64(0))add(result,State::needs_attention,"Message holds","Existing integrity holds remain set.","Review their original message-maintenance findings. Health Check never clears holds.");
        sql::SqliteStatement accounts(raw,"SELECT count(*) FROM sfx_accounts","read caller count");if(accounts.row())add(result,State::ok,"Caller records","Caller record count: "+std::to_string(accounts.int64(0))+". No authentication was attempted.");
        sql::SqliteStatement areas(raw,"SELECT storage_path FROM sfx_file_area_paths ORDER BY area_id LIMIT 1001","read file-area paths");std::size_t count{};while(areas.row()){if(++count>1000){add(result,State::could_not_check,"File Area configuration","The 1,000-path check limit was reached.");break;}configured_path(areas.text(0),true,"File Area path");}
        sql::SqliteStatement doors(raw,"SELECT definition FROM sfx_door_definitions ORDER BY door_id LIMIT 1001","read Door definitions");count=0;std::size_t bad_secrets{};bool secrets_complete=true;
        while(doors.row()){budget();if(++count>1000){secrets_complete=false;add(result,State::could_not_check,"Door configuration","The 1,000-Door check limit was reached.");break;}const auto door=app::decode_door_definition(doors.blob(0));if(!door){secrets_complete=false;add(result,State::needs_attention,"Door definition","A saved Door definition cannot be decoded.");continue;}
            if(door->kind==app::DoorExecutionKind::local)configured_path(door->program.startup_directory,true,"Door startup folder");
            for(const auto* secret:{&door->rlogin.protected_credential,&door->rlogin.protected_pepper})if(!secret->empty()&&(!key||!portable::open_portable_credential(key.value->key,key.value->board,{'S','F','X','R'},"RLOGIN",*secret)))++bad_secrets;
        }
        sql::SqliteStatement ftn(raw,"SELECT definition FROM sfx_ftn_configuration WHERE singleton_id=1","read FTN configuration");if(ftn.row()){
            const auto bytes=ftn.blob(0);const auto c=bytes.empty()?std::optional<app::FtnConfiguration>(app::FtnConfiguration{}):app::decode_ftn_configuration(bytes);
            if(!c){secrets_complete=false;add(result,State::needs_attention,"FTN settings","Saved FTN settings cannot be decoded.");}
            else{for(const auto& link:c->links)for(const auto& item:{std::pair{FtnCredentialKind::session,&link.credentials.session},std::pair{FtnCredentialKind::packet,&link.credentials.packet},std::pair{FtnCredentialKind::areafix,&link.credentials.areafix},std::pair{FtnCredentialKind::tic,&link.credentials.tic}}){budget();if(!item.second->empty()&&!unprotect_ftn_secret(root.u8string(),link.network,link.code,item.first,*item.second))++bad_secrets;}
                if(!c->networks.empty())configured_path((root/"ftn").u8string(),true,"FTN work folder");
                for(const auto& path:{c->settings.external_executable,c->settings.tls_certificate,c->settings.tls_private_key})configured_path(path,false,"FTN configured file");
                configured_path(c->settings.msg_netmail_directory,true,"FTN configured folder");}
        }else secrets_complete=false;
        add(result,!secrets_complete?State::could_not_check:bad_secrets?State::needs_attention:State::ok,"Saved connection credentials",!secrets_complete?"Not all saved connection settings could be inspected. No complete credential-readability claim is made.":bad_secrets?std::to_string(bad_secrets)+" saved values could not be read. Their encrypted originals remain unchanged.":"All configured saved connection values were readable. No passwords or keys are included.",!secrets_complete||bad_secrets?"Restore the matching board key/Config copy or review the affected connection settings.":"");
        add(result,State::ok,"Configured operations","No Doors, commands, network calls, FTN deliveries, message purges or file jobs were run or replayed.");
    }catch(...){add(result,State::could_not_check,"Remaining board checks","A required read or schema check failed, was cancelled, or exceeded its time budget. No repair was attempted.","Preserve this board and review its files and matching recovery copy before retrying.");}
    try{auto current=open_owned_directory(root);struct stat now{};require(fstat(current.get(),&now)==0&&same(now,root_stamp),"health_root_changed");save(result,root);}
    catch(...){add(result,State::could_not_check,"Saved report","The original installation path changed or became unavailable. No report path is claimed.");}
    return result;
}
}
