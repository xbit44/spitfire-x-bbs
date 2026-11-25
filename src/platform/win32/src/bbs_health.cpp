#include "sfx/platform/win32/bbs_health.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_access.hpp"
#include "sfx/platform/win32/portable_credentials.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include "sqlite_sfx_baseline.hpp"
#include "sqlite_statement.hpp"
#include <sqlite3.h>
#include <windows.h>
#include <algorithm>
#include <iomanip>
#include <memory>
#include <sstream>
namespace sfx::platform::win32 {
namespace {
namespace inst=installation;namespace sql=adapters::storage::internal;
const wchar_t* label(BbsHealthState state){return state==BbsHealthState::ok?L"OK":state==BbsHealthState::needs_attention?L"Needs attention":L"Could not check";}
std::wstring wide(std::string_view value){if(value.empty())return {};const auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);if(n<=0)return L"(unreadable text)";std::wstring out(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),out.data(),n);for(auto& c:out)if(c<L' ')c=L' ';return out;}
std::string utf8(const std::wstring& value){const auto n=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),out.data(),n,nullptr,nullptr);return out;}
void add(BbsHealthReport& report,BbsHealthState state,std::wstring subject,std::wstring detail,std::wstring next={}){
    if(report.findings.size()<255)report.findings.push_back({state,std::move(subject),std::move(detail),std::move(next)});
    else if(report.findings.size()==255)report.findings.push_back({BbsHealthState::could_not_check,L"Additional findings",L"The report reached its displayed finding limit.",L"Address the listed findings, then run the check again."});
}
enum class PathState {present,missing,unsupported,unavailable};
bool fixed_path(const std::filesystem::path& path){const auto text=path.native();return text.size()>=3&&iswalpha(text[0])&&text[1]==L':'&&(text[2]==L'\\'||text[2]==L'/')&&GetDriveTypeW(path.root_path().c_str())==DRIVE_FIXED;}
PathState local_path_state(const std::filesystem::path& path,bool directory,const std::function<void()>& budget={}){
    // Never probe UNC/mapped-network/device paths or follow a junction to a
    // remote target. Inspect each local ancestor's own reparse metadata first.
    if(!fixed_path(path))return PathState::unsupported;
    auto current=path.root_path();std::vector<inst::Handle> pins;
    try{
        const auto normalized=path.lexically_normal();if(normalized!=path)return PathState::unsupported;
        const auto components=path.relative_path();if(components.empty())return directory?PathState::present:PathState::missing;std::size_t index{},count=static_cast<std::size_t>(std::distance(components.begin(),components.end()));
        for(const auto& component:components){if(budget)budget();current/=component;++index;
            inst::Handle h(CreateFileW(current.c_str(),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
            if(h.get()==INVALID_HANDLE_VALUE){const auto error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?PathState::missing:PathState::unavailable;}
            BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(h.get(),&info))return PathState::unavailable;
            if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)return PathState::unsupported;
            const bool expected_directory=index<count||directory;if(((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)!=expected_directory)return PathState::missing;
            pins.push_back(std::move(h));
        }return PathState::present;
    }catch(...){if(budget)budget();return PathState::unavailable;}
}
bool existing(const std::filesystem::path& path,bool directory,const std::function<void()>& budget={}){return local_path_state(path,directory,budget)==PathState::present;}
std::string database_uri(const std::filesystem::path& path){constexpr char digits[]="0123456789ABCDEF";std::string uri="file:";for(const unsigned char c:path.generic_u8string()){if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='/'||c==':'||c=='-'||c=='_'||c=='.')uri+=static_cast<char>(c);else{uri+='%';uri+=digits[c>>4];uri+=digits[c&15];}}return uri+"?mode=ro&immutable=1";}
void save_report(BbsHealthReport& report,const std::filesystem::path& root,const std::wstring& owner){
    try{
        auto logs=inst::open_object(root/L"Logs",true,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);SYSTEMTIME now{};GetLocalTime(&now);std::wostringstream name;name<<L"BBS-Health-"<<std::setfill(L'0')<<std::setw(4)<<now.wYear<<std::setw(2)<<now.wMonth<<std::setw(2)<<now.wDay<<L'-'<<std::setw(2)<<now.wHour<<std::setw(2)<<now.wMinute<<std::setw(2)<<now.wSecond<<L'-'<<inst::unique_id()<<L".txt";
        const auto path=root/L"Logs"/name.str();const auto descriptor=inst::mutable_descriptor(owner);SECURITY_ATTRIBUTES security{sizeof(security),descriptor.get(),FALSE};
        inst::Handle output(CreateFileW(path.c_str(),GENERIC_WRITE|READ_CONTROL,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(output.get()==INVALID_HANDLE_VALUE)throw std::runtime_error("health_report_create_failed");
        const auto bytes=std::string("\xEF\xBB\xBF")+utf8(report.text());DWORD written{};
        if(bytes.size()>4U*1024U*1024U||!WriteFile(output.get(),bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(output.get()))throw std::runtime_error("health_report_write_failed");
        output.reset();if(!WindowsStorageDaclPolicy(owner).apply(path,StorageDaclObjectKind::file))throw std::runtime_error("health_report_protection_failed");report.saved_report=path;
    }catch(...){add(report,BbsHealthState::could_not_check,L"Saved report",L"The dated report could not be completely saved and protected. No saved-report result is claimed.",L"Check free space and access to Logs; any incomplete new report was preserved.");}
}
}
std::wstring BbsHealthReport::text()const{
    std::wstring result=L"SFX BBS Health Check\r\nReport-only: no repairs, migrations, hold changes or cleanup.\r\n\r\n";
    for(const auto& item:findings){result+=std::wstring(label(item.state))+L" - "+item.subject+L"\r\n"+item.detail+L"\r\n";if(!item.next_step.empty())result+=L"Next: "+item.next_step+L"\r\n";result+=L"\r\n";}return result;
}
BbsHealthReport run_bbs_health_check(const std::filesystem::path& root,const std::wstring& owner,const std::function<bool()>& cancelled){
    BbsHealthReport result;OfflineInstallationGuard stopped;
    const auto status=stopped.acquire(root,owner);if(!status){add(result,BbsHealthState::could_not_check,L"Stopped BBS",L"The BBS or another offline operation owns this folder, or exclusive access could not be established.",L"Stop the BBS and finish other maintenance, then run this check again.");return result;}
    result.stopped=true;std::vector<inst::Handle> pins;bool root_valid=false;const auto deadline=GetTickCount64()+15000;
    const auto check_budget=[&]{if(GetTickCount64()>=deadline||(cancelled&&cancelled()))throw std::runtime_error("health_cancelled_or_time_budget");};
    try{
        // Hold every ancestor against redirect/rename for this read operation.
        const auto normalized=root.lexically_normal();inst::require(normalized==root&&root.is_absolute()&&fixed_path(root),"health_root_invalid");auto current=root.root_path();pins.push_back(inst::open_object(current,true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE));
        for(const auto& component:root.relative_path()){check_budget();current/=component;pins.push_back(inst::open_object(current,true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE));}root_valid=true;
        const auto bindings=configure_portable_file_bindings(root);check_budget();
        const bool bindings_ok=bindings.state==PortableFileBindingState::ready&&!bindings.deferred;
        add(result,bindings_ok?BbsHealthState::ok:BbsHealthState::needs_attention,L"Copy-recovery file records",
            bindings_ok?L"The authenticated file-identity snapshot belongs to this folder and has no retained unresolved entries. Payload contents were not rehashed by this check.":
            bindings.state==PortableFileBindingState::different_host?L"File records still belong to the source copy; this folder needs preparation.":
            bindings.state==PortableFileBindingState::missing?L"The file-identity snapshot is missing. No replacement snapshot was created.":
            bindings.deferred?L"Unresolved file records retained: "+std::to_wstring(bindings.deferred)+L". Their files and earlier proofs were preserved.":L"The file-identity snapshot or its board key could not be verified.",
            bindings_ok?L"":L"Preserve this folder and review the matching complete recovery copy. Use preparation for a copied board; restore missing matching files before retrying. This check does not initialize or repair file records.");
        const auto configured_path=[&](const std::string& value,bool directory,const wchar_t* subject){check_budget();auto path=resolve_portable_owned_path(root,std::filesystem::u8path(value));if(path.is_relative())path=(root/path).lexically_normal();const auto state=local_path_state(path,directory,check_budget);
            if(state==PathState::missing)add(result,BbsHealthState::needs_attention,subject,wide(value),L"Restore or reconnect this configured resource. External files need their own copy; paths were not rewritten.");
            else if(state!=PathState::present)add(result,BbsHealthState::could_not_check,subject,wide(value),L"This path is remote, redirected, inaccessible or outside the supported local check scope. Review it separately; no remote target was probed.");};
        for(const auto* leaf:{L"Config",L"Data",L"Logs",L"Work",L"Files",L"Doors",L"Display",L"Node",L"BotGate",L"FTN",L"Backup"}){
            check_budget();const auto path=root/leaf;if(!existing(path,true,check_budget)){add(result,BbsHealthState::needs_attention,leaf,L"Required SFX folder is missing, inaccessible or redirected.",L"Restore the complete SFX folder, then use Prepare SFX on this computer if needed.");continue;}
            const auto access=assess_current_storage_access(path,StorageDaclObjectKind::directory);if(!access.status)add(result,BbsHealthState::needs_attention,leaf,wide(access.status.operation),L"Review current folder access or use preparation; this check does not change permissions.");
        }
        std::size_t missing_programs{};for(const auto* program:inst::programs){check_budget();if(!existing(root/L"Bin"/program,false,check_budget)){++missing_programs;add(result,BbsHealthState::needs_attention,L"Required SFX file",program,L"Restore the matching SFX program from the same package or run matching Setup Repair.");}}
        if(!missing_programs)add(result,BbsHealthState::ok,L"Required program files",L"All required SFX program paths are present. Program-generation validation is part of normal launch/preparation.");
        pins.push_back(inst::open_object(root/L"Config",true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE));pins.push_back(inst::open_object(root/L"Data",true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE));
        check_budget();const auto configuration=read_operational_configuration_file(root);add(result,configuration.state==ConfigurationFileState::loaded?BbsHealthState::ok:BbsHealthState::needs_attention,L"Board settings",configuration.state==ConfigurationFileState::loaded?L"The saved board settings can be decoded.":L"The saved board settings are missing or invalid.",configuration.state==ConfigurationFileState::loaded?L"":L"Restore the matching Config files or review settings after preparation.");
        const PortableCredentialReader secrets(root);auto key=load_portable_board_key(root);if(!key)add(result,BbsHealthState::needs_attention,L"Portable saved credentials",L"The board key or its metadata is unavailable or invalid. Nothing was regenerated.",L"Restore the matching complete Config copy; legacy credentials require their original Windows keys for the first update.");
        const auto database=root/L"Data"/L"SFX.DB";
        auto pin=inst::open_object(database,false,GENERIC_READ|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ);
        // Immutable SQLite never creates -wal/-shm or lock files. It cannot see
        // retained WAL frames, so refuse database claims until a normal stopped
        // checkpoint is available instead of inspecting a stale main snapshot.
        bool pending_wal=false;const auto wal=std::filesystem::path(database.native()+L"-wal");const auto attrs=GetFileAttributesW(wal.c_str());
        if(attrs!=INVALID_FILE_ATTRIBUTES){auto wal_pin=inst::open_object(wal,false,GENERIC_READ|FILE_READ_ATTRIBUTES,FILE_SHARE_READ);LARGE_INTEGER size{};if(!GetFileSizeEx(wal_pin.get(),&size)||size.QuadPart)pending_wal=true;pins.push_back(std::move(wal_pin));}
        else if(GetLastError()!=ERROR_FILE_NOT_FOUND)pending_wal=true;
        if(pending_wal){add(result,BbsHealthState::could_not_check,L"Database",L"Retained or inaccessible SQLite WAL data prevents an immutable read-only main-file diagnosis.",L"Preserve all Data files. Use a normal clean BBS start/stop or recover the consistent database first, then check again.");save_report(result,root,owner);return result;}
        sqlite3* raw{};const auto uri=database_uri(database);const auto opened=sqlite3_open_v2(uri.c_str(),&raw,SQLITE_OPEN_READONLY|SQLITE_OPEN_URI,nullptr);std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)> db(raw,sqlite3_close_v2);
        if(opened!=SQLITE_OK||!raw||sqlite3_db_readonly(raw,"main")!=1)throw std::runtime_error("health_database_readonly_unavailable");
        sqlite3_exec(raw,"PRAGMA query_only=ON;PRAGMA trusted_schema=OFF;PRAGMA temp_store=MEMORY;BEGIN",nullptr,nullptr,nullptr);
        struct Budget {const std::function<bool()>& cancelled;ULONGLONG end;bool expired{};} budget{cancelled,deadline};
        sqlite3_progress_handler(raw,1000,[](void* context)->int{auto& b=*static_cast<Budget*>(context);b.expired=GetTickCount64()>=b.end;return b.expired||(b.cancelled&&b.cancelled());},&budget);
        struct ResetProgress {sqlite3* db;~ResetProgress(){sqlite3_progress_handler(db,0,nullptr,nullptr);}} reset_progress{raw};
        const bool valid=sql::validate_sfx_shipping_database(raw,true);
        if(!valid){add(result,budget.expired||(cancelled&&cancelled())?BbsHealthState::could_not_check:BbsHealthState::needs_attention,L"Database integrity",L"The complete supported-schema/integrity/reference checks did not pass or finish. No data or holds were changed.",L"Preserve Data and review the matching recovery copy; rerun after resolving access or cancellation.");}
        else{
            add(result,BbsHealthState::ok,L"Database integrity",L"Supported schema, SQLite integrity, retained schema constraints and foreign-key references passed using an immutable read-only connection.");
            sql::SqliteStatement holds(raw,"SELECT count(*) FROM sfx_message_integrity_holds","inspect existing message holds without changing them");if(holds.row()&&holds.int64(0))add(result,BbsHealthState::needs_attention,L"Message holds",L"Existing message-integrity holds are still set.",L"Review the original findings through message maintenance. This Health Check does not clear holds.");
            sql::SqliteStatement callers(raw,"SELECT count(*) FROM sfx_accounts","inspect caller records");if(callers.row())add(result,BbsHealthState::ok,L"Caller records",L"Caller record count: "+std::to_wstring(callers.int64(0))+L". Database references were checked; authentication was not attempted.");
            sql::SqliteStatement areas(raw,"SELECT storage_path FROM sfx_file_area_paths ORDER BY area_id LIMIT 1000","inspect configured file paths");std::size_t area_count{};while(areas.row()){++area_count;configured_path(areas.text(0),true,L"File Area path");}
            add(result,BbsHealthState::ok,L"File Area configuration",L"Configured storage paths inspected: "+std::to_wstring(area_count)+L". Payload bytes and unfinished job outcomes were not modified or replayed.");
            sql::SqliteStatement doors(raw,"SELECT definition FROM sfx_door_definitions ORDER BY door_id LIMIT 1000","inspect Door definitions without launching them");std::size_t door_count{},credential_failures{};
            while(doors.row()){check_budget();++door_count;const auto door=application::decode_door_definition(doors.blob(0));if(!door){add(result,BbsHealthState::needs_attention,L"Door definition",L"A saved Door definition cannot be decoded.",L"Review the Door catalog from the matching recovery copy.");continue;}
                if(door->kind==application::DoorExecutionKind::local&&!door->program.startup_directory.empty())configured_path(door->program.startup_directory,true,L"Door startup folder");
                for(const auto* value:{&door->rlogin.protected_credential,&door->rlogin.protected_pepper}){check_budget();if(!value->empty()&&!(secrets.legacy_allowed()?unprotect_rlogin_secret(*value):secrets.open({'S','F','X','R'},"RLOGIN",*value))){++credential_failures;add(result,BbsHealthState::needs_attention,L"Door "+std::to_wstring(door->id)+(value==&door->rlogin.protected_credential?L" remote password":L" hash pepper"),L"This saved value could not be verified. Its encrypted original was preserved.",L"Restore matching Config files or correct only this connection value in Doors settings.");}}
            }
            add(result,BbsHealthState::ok,L"Door configuration",L"Saved definitions inspected: "+std::to_wstring(door_count)+L". Doors and cleanup commands were not executed.");
            sql::SqliteStatement ftn(raw,"SELECT definition FROM sfx_ftn_configuration WHERE singleton_id=1","inspect FTN settings and saved connection protection");if(ftn.row()){
                const auto c=application::decode_ftn_configuration(ftn.blob(0));if(!c)add(result,BbsHealthState::needs_attention,L"FTN settings",L"The saved FTN configuration cannot be decoded.",L"Review the matching recovery copy; no repair was attempted.");
                else{for(const auto& link:c->links)for(const auto item:{std::pair{FtnCredentialKind::session,&link.credentials.session},std::pair{FtnCredentialKind::packet,&link.credentials.packet},std::pair{FtnCredentialKind::areafix,&link.credentials.areafix},std::pair{FtnCredentialKind::tic,&link.credentials.tic}}){check_budget();if(!item.second->empty()&&!(secrets.legacy_allowed()?unprotect_legacy_ftn_secret(root.u8string(),link.network,link.code,item.first,*item.second):secrets.open({'S','F','X','F'},portable_ftn_purpose(link.network,link.code,item.first),*item.second))){++credential_failures;const wchar_t* kinds[]{L"session password",L"packet password",L"AreaFix password",L"TIC password"};add(result,BbsHealthState::needs_attention,L"FTN "+wide(link.network)+L" / "+wide(link.code)+L" / "+kinds[static_cast<unsigned>(item.first)],L"This saved value could not be verified. Its encrypted original was preserved.",L"Restore matching Config files or correct only this connection value in FTN settings. Other unchanged values can be retained.");}}
                    for(const auto& path:{c->settings.external_executable,c->settings.tls_certificate,c->settings.tls_private_key})if(!path.empty())configured_path(path,false,L"FTN configured path");if(!c->settings.msg_netmail_directory.empty())configured_path(c->settings.msg_netmail_directory,true,L"FTN configured path");
                    add(result,BbsHealthState::ok,L"FTN configuration",L"Saved FTN settings, credential readability and configured external paths were inspected. No poll, delivery, ACK change or replay was attempted.");}
            }
            add(result,credential_failures?BbsHealthState::needs_attention:BbsHealthState::ok,L"Saved connection credentials",credential_failures?L"Saved values unavailable: "+std::to_wstring(credential_failures)+L". Their original encrypted records were preserved.":L"All configured saved connection values were readable. Passwords and keys are excluded from this report.",credential_failures?L"Restore the matching board key/Config copy; complete a readable legacy update before losing the old Windows keys.":L"");
        }
    }catch(...){add(result,BbsHealthState::could_not_check,L"Remaining board checks",L"Required data or read access was unavailable, redirected, cancelled or incompatible. No files were created or repaired in Config/Data.",L"Preserve the complete folder and review preparation, required files and access, then retry.");}
    if(root_valid)save_report(result,root,owner);return result;
}
}
