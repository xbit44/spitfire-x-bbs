#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/portable_installation.hpp"
#include <sddl.h>
#include <sstream>
#include <array>

namespace sfx::platform::win32::installation {
namespace {
constexpr auto mode_leaf=L"service-registration.v2";
constexpr auto pending_leaf=L"service-registration.pending";
constexpr auto legacy_leaf=L"service-mode.v1";
constexpr auto legacy_pending=L"service-mode.pending";
struct ScHandle {SC_HANDLE h{};~ScHandle(){if(h)CloseServiceHandle(h);}operator SC_HANDLE() const{return h;}};
bool absent_mode(const Path& p) {
    if(GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES)return false;
    const auto e=GetLastError();require(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND,"service_metadata_unavailable");return true;
}
std::wstring command(const Path& root) {return L"\""+(root/L"Bin"/L"SFXRuntime.exe").native()+L"\" --windows-service";}
ServiceMode decode_mode(const Path& root,const Path& path,bool verify_programs=true) {
    const auto bytes=read_file(path,4096);std::istringstream in(std::string(bytes.begin(),bytes.end()));
    std::string magic,id,owner,extra;require(std::getline(in,magic)&&
        (magic=="SFX-SERVICE-MODE-1"||magic=="SFX-SERVICE-REGISTRATION-2")&&
        std::getline(in,id)&&std::getline(in,owner),"service_metadata_invalid");
    ServiceMode m{root,{owner.begin(),owner.end()},{id.begin(),id.end()},L"SFXBBS_"+std::wstring(id.begin(),id.end())};
    if(magic=="SFX-SERVICE-REGISTRATION-2") {
        std::string selected,revision,policy;require(std::getline(in,selected)&&(selected=="SERVICE"||selected=="DESKTOP")&&
            std::getline(in,revision)&&revision.size()==32&&revision.find_first_not_of("0123456789ABCDEF")==std::string::npos,
            "service_metadata_invalid");
        m.enabled=selected=="SERVICE";m.revision={revision.begin(),revision.end()};
        require(std::getline(in,policy)&&(policy=="2"||policy=="3"||policy=="4"),"service_metadata_policy_invalid");
        m.start_policy=static_cast<DWORD>(policy[0]-'0');
        require(m.enabled||m.start_policy==SERVICE_DISABLED,"service_metadata_policy_mismatch");
    }
    require(!std::getline(in,extra),"service_metadata_invalid");
    const auto r=read_receipt(root,m.owner);require(r.id==m.id&&r.committed,"service_metadata_binding_mismatch");
    verify_structure(r,verify_programs);auto file=open_object(path,false);auto sd=structural_descriptor(m.owner);
    require(exact_security(file.get(),sd.get()),"service_metadata_security_invalid");return m;
}
DWORD validate_config(SC_HANDLE service,const ServiceMode& m) {
    DWORD bytes{};QueryServiceConfigW(service,nullptr,0,&bytes);
    require(bytes>=sizeof(QUERY_SERVICE_CONFIGW)&&bytes<65536,"service_config_unavailable");
    std::vector<std::uint8_t> storage(bytes);auto* c=reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
    require(QueryServiceConfigW(service,c,bytes,&bytes)&&c->dwServiceType==SERVICE_WIN32_OWN_PROCESS&&
        c->lpBinaryPathName&&std::wstring(c->lpBinaryPathName)==command(m.root)&&c->lpServiceStartName&&
        !_wcsicmp(c->lpServiceStartName,L"LocalSystem"),"service_identity_or_image_mismatch");
    return c->dwStartType;
}
std::string narrow(std::wstring_view s) {
    std::string out;for(const auto c:s){require(c>=32&&c<127,"service_metadata_text_invalid");out+=static_cast<char>(c);}return out;
}
void require_stopped(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};DWORD bytes{};
    require(QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&bytes)&&
        status.dwCurrentState==SERVICE_STOPPED,"service_must_be_stopped_before_setup");
}
bool system_token() {
    auto token=process_token();DWORD bytes{};GetTokenInformation(token.get(),TokenUser,nullptr,0,&bytes);
    std::vector<std::uint8_t> buffer(bytes);
    return bytes&&GetTokenInformation(token.get(),TokenUser,buffer.data(),bytes,&bytes)&&
        IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,WinLocalSystemSid);
}
}
std::optional<ServiceMode> read_service_registration(const Path& root,bool recovery) {
    const auto bin=root/L"Bin";
    if(!absent_mode(bin/pending_leaf)) {
        require(recovery,"service_transition_incomplete_retry_config_or_setup");
        auto m=decode_mode(root,bin/pending_leaf);m.pending=true;return m;
    }
    if(!absent_mode(bin/mode_leaf))return decode_mode(root,bin/mode_leaf);
    if(!absent_mode(bin/legacy_pending)) {
        require(recovery,"service_transition_incomplete_run_setup");
        auto m=decode_mode(root,bin/legacy_pending);m.pending=true;
        m.enabled=!absent_mode(bin/legacy_leaf);return m;
    }
    if(!absent_mode(bin/legacy_leaf))return decode_mode(root,bin/legacy_leaf);
    return {};
}
std::optional<ServiceMode> read_service_mode(const Path& root) {
    auto m=read_service_registration(root);return m&&m->enabled?m:std::nullopt;
}
std::optional<ServiceMode> read_service_selection(const Path& root,bool recovery) {
    const auto bin=root/L"Bin";
    if(!absent_mode(bin/pending_leaf)) {require(recovery,"service_transition_incomplete_retry_config_or_setup");auto m=decode_mode(root,bin/pending_leaf,false);m.pending=true;return m;}
    if(!absent_mode(bin/mode_leaf))return decode_mode(root,bin/mode_leaf,false);
    if(!absent_mode(bin/legacy_pending)){require(recovery,"service_transition_incomplete_run_setup");auto m=decode_mode(root,bin/legacy_pending,false);m.pending=true;m.enabled=!absent_mode(bin/legacy_leaf);return m;}
    if(!absent_mode(bin/legacy_leaf))return decode_mode(root,bin/legacy_leaf,false);
    return {};
}
ServiceState query_owned_service(const ServiceMode& m) {
    ScHandle manager{OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)};
    require(manager.h,"service_manager_unavailable");
    ScHandle service{OpenServiceW(manager,m.name.c_str(),SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG)};
    require(service.h,"owned_service_unavailable");const auto start=validate_config(service,m);
    SERVICE_STATUS_PROCESS status{};DWORD bytes{};
    require(QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&bytes),
        "service_status_unavailable");return {status.dwCurrentState,status.dwProcessId,status.dwWin32ExitCode,start};
}
ServiceState query_selected_service(const ServiceMode& expected) {
    const auto bin=expected.root/L"Bin";
    require(absent_mode(bin/pending_leaf)&&absent_mode(bin/legacy_pending),"service_transition_incomplete_reopen_after_recovery");
    const auto current=decode_mode(expected.root,bin/(absent_mode(bin/mode_leaf)?legacy_leaf:mode_leaf),false);
    require(current.enabled&&current.id==expected.id&&current.owner==expected.owner&&current.revision==expected.revision,
        "service_selection_changed_reopen_control_panel");
    return query_owned_service(current);
}
bool start_owned_service(const ServiceMode& m,std::string& diagnostic) {
    try {
        const auto current=read_service_mode(m.root);
        require(current&&current->revision==m.revision&&current->id==m.id,"service_mode_changed_reopen_control_panel");
        ScHandle manager{OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)};require(manager.h,"service_manager_unavailable");
        ScHandle service{OpenServiceW(manager,m.name.c_str(),SERVICE_START|SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG)};
        require(service.h,"service_start_not_authorized");validate_config(service,m);
        const auto state=query_owned_service(m);
        if(state.state==SERVICE_RUNNING)return true;
        require(state.state==SERVICE_STOPPED,"service_transition_in_progress");
        const auto first=read_prepared_first_start(m.root,m.owner);
        const auto argument=first?L"--sfx-first-start="+first->operation:std::wstring{};
        const wchar_t* arguments[]{argument.c_str()};
        require(StartServiceW(service,first?1:0,first?arguments:nullptr),"service_start_failed");
        const auto deadline=GetTickCount64()+15000;
        do {const auto now=query_owned_service(m);
            if(now.state==SERVICE_RUNNING){diagnostic="service_running";return true;}
            if(now.state==SERVICE_STOPPED){diagnostic="service_start_failed_"+std::to_string(now.win32_exit_code);return false;}
            Sleep(50);
        }while(GetTickCount64()<deadline);
        diagnostic="service_start_pending";return false;
    }catch(const std::exception& e){diagnostic=e.what();return false;}
}
void verify_owned_service_peer(const ServiceMode& m,DWORD pid) {
    const auto current=read_service_mode(m.root);
    require(current&&current->id==m.id&&current->owner==m.owner&&current->name==m.name,"service_peer_mode_changed");
    const auto receipt=read_receipt(m.root,m.owner);
    require(receipt.id==m.id,"service_peer_receipt_changed");
    InstallationVerificationLease pinned(receipt);
    const auto before=query_owned_service(*current);
    require(pid&&before.state==SERVICE_RUNNING&&before.process_id==pid,"service_peer_pid_mismatch");
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));
    require(process.get()!=nullptr&&WaitForSingleObject(process.get(),0)==WAIT_TIMEOUT,"service_peer_process_unavailable");
    std::vector<wchar_t> path(32768,L'\0');DWORD size=static_cast<DWORD>(path.size());
    require(QueryFullProcessImageNameW(process.get(),0,path.data(),&size)&&size&&size<path.size()&&
        !_wcsicmp(std::wstring(path.data(),size).c_str(),(m.root/L"Bin"/L"SFXRuntime.exe").c_str()),"service_peer_image_mismatch");
    const auto after=query_owned_service(*current);
    require(after.state==SERVICE_RUNNING&&after.process_id==pid,"service_peer_epoch_changed");
}
bool stop_owned_service(const ServiceMode& expected,DWORD expected_pid,std::string& diagnostic) {
    try {
        const auto m=read_service_mode(expected.root);
        require(m&&m->id==expected.id&&m->owner==expected.owner&&m->revision==expected.revision,"service_selection_changed_refresh_before_retry");
        ScHandle manager{OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)};require(manager.h,"service_manager_unavailable");
        ScHandle service{OpenServiceW(manager,m->name.c_str(),SERVICE_STOP|SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG)};
        require(service.h,"service_stop_not_authorized");validate_config(service,*m);
        const auto before=query_owned_service(*m);
        require(before.state==SERVICE_RUNNING&&before.process_id==expected_pid,"service_epoch_changed_refresh_before_retry");
        verify_owned_service_peer(*m,expected_pid);
        SERVICE_STATUS status{};require(ControlService(service,SERVICE_CONTROL_STOP,&status),"service_stop_failed");
        const auto deadline=GetTickCount64()+15000;
        do {const auto now=query_owned_service(*m);if(now.state==SERVICE_STOPPED){diagnostic="service_stopped";return true;}
            require(!now.process_id||now.process_id==expected_pid,"service_epoch_changed_during_stop");Sleep(50);
        }while(GetTickCount64()<deadline);
        diagnostic="service_stop_pending";return false;
    }catch(const std::exception& e){diagnostic=e.what();return false;}
}
namespace {
void configure_registration(const Path& root,const std::wstring& owner,const std::optional<bool>& selected,const std::wstring& operation={},std::optional<DWORD> requested_policy={}) {
    require(elevated(),"service_setup_elevation_required");const auto receipt=read_receipt(root,owner);
    require(receipt.committed,"service_requires_committed_installation");verify_structure(receipt);
    const ServiceMode expected{root,owner,receipt.id,L"SFXBBS_"+receipt.id};
    const auto mode=root/L"Bin"/mode_leaf,pending=root/L"Bin"/pending_leaf;
    const auto old=read_service_registration(root,true);
    if(old)require(old->id==expected.id&&old->owner==owner,"service_owner_mismatch");
    const bool enabled=selected?*selected:old&&old->enabled;
    if(old&&old->pending)require(old->enabled==enabled,"service_pending_choice_must_be_recovered_first");
    ScHandle manager{OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT|SC_MANAGER_CREATE_SERVICE)};
    require(manager.h,"service_manager_admin_required");
    ScHandle service{OpenServiceW(manager,expected.name.c_str(),SERVICE_ALL_ACCESS)};
    if(service.h) {
        require(old.has_value(),"service_name_collision_preserved");
        validate_config(service,expected);require_stopped(service);
    }else require(GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST,"service_open_failed");
    const bool have_pending=!absent_mode(pending);
    const DWORD policy=have_pending?old->start_policy:!enabled?SERVICE_DISABLED:requested_policy?*requested_policy:
        selected||!service.h?SERVICE_AUTO_START:validate_config(service,expected);
    require((policy==SERVICE_AUTO_START||policy==SERVICE_DEMAND_START||policy==SERVICE_DISABLED)&&
        (enabled||policy==SERVICE_DISABLED),"service_start_policy_invalid");
    if(!operation.empty())require(operation.size()==32&&operation.find_first_not_of(L"0123456789ABCDEF")==operation.npos&&
        (!have_pending||old->revision==operation),"service_operation_binding_invalid");
    // Journal the desired mode before changing SCM. A crash refuses both
    // Runtime entry paths until this same protected choice is recovered.
    if(!have_pending) {
        const auto text="SFX-SERVICE-REGISTRATION-2\n"+narrow(receipt.id)+"\n"+narrow(owner)+"\n"+
            (enabled?"SERVICE\n":"DESKTOP\n")+narrow(operation.empty()?unique_id():operation)+"\n"+std::to_string(policy)+"\n";
        auto sd=structural_descriptor(owner);create_file(pending,{text.begin(),text.end()},sd.get());
    }
    {
        bool created=false;
        if(!service.h) {
            const auto display=L"Spitfire-X BBS ("+receipt.id.substr(0,8)+L")";
            service.h=CreateServiceW(manager,expected.name.c_str(),display.c_str(),SERVICE_ALL_ACCESS,
                SERVICE_WIN32_OWN_PROCESS,policy,SERVICE_ERROR_NORMAL,command(root).c_str(),
                nullptr,nullptr,nullptr,nullptr,nullptr); // Explicit owner-selected LocalSystem.
            require(service.h,"service_create_failed_pending_preserved");
            created=true;
        }
        validate_config(service,expected);
        const auto sddl=L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;CCLCSWRPWPLOCRRC;;;"+owner+L")";
        PSECURITY_DESCRIPTOR raw{};
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw,nullptr),"service_security_descriptor_invalid");
        Descriptor security(raw);
        require(SetServiceObjectSecurity(service,DACL_SECURITY_INFORMATION,raw),"service_security_failed");
        if(created||!old||old->revision==L"legacy-v1") {
            SERVICE_SID_INFO sid{SERVICE_SID_TYPE_UNRESTRICTED};
            require(ChangeServiceConfig2W(service,SERVICE_CONFIG_SERVICE_SID_INFO,&sid),"service_sid_failed");
        }
        SERVICE_DESCRIPTIONW description{const_cast<LPWSTR>(L"Optional SFX Runtime. Closing Control Panel leaves the BBS running. Uses LocalSystem by explicit Sysop choice.")};
        require(ChangeServiceConfig2W(service,SERVICE_CONFIG_DESCRIPTION,&description),"service_description_failed");
        // Replay the journaled policy exactly, including Manual during an
        // interrupted maintenance operation. The choice is durable before SCM.
        require(ChangeServiceConfigW(service,SERVICE_NO_CHANGE,
            policy,SERVICE_NO_CHANGE,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr),
            "service_start_policy_failed_pending_preserved");
        require(MoveFileExW(pending.c_str(),mode.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"service_mode_commit_failed");
    }
    for(const auto* leaf:{legacy_leaf,legacy_pending})if(!absent_mode(root/L"Bin"/leaf))
        require(DeleteFileW((root/L"Bin"/leaf).c_str()),"service_legacy_metadata_retained_run_setup");
}
}
void configure_owned_service(const Path& root,const std::wstring& owner,bool enabled,const std::wstring& operation,std::optional<DWORD> policy) {
    require(!policy||(*policy==SERVICE_AUTO_START||*policy==SERVICE_DEMAND_START||(!enabled&&*policy==SERVICE_DISABLED)),"service_requested_policy_invalid");
    configure_registration(root,owner,enabled,operation,policy);
}
void ensure_owned_service(const Path& root,const std::wstring& owner) {
    configure_registration(root,owner,std::nullopt);
}
void remove_owned_service(const Path& root,const std::wstring& owner) {
    require(elevated(),"service_setup_elevation_required");
    const auto receipt=read_receipt(root,owner);verify_structure(receipt);
    const auto m=read_service_registration(root,true);if(!m)return;
    require(m->owner==owner&&m->id==receipt.id,"service_owner_mismatch");
    ScHandle manager{OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)};require(manager.h,"service_manager_unavailable");
    ScHandle service{OpenServiceW(manager,m->name.c_str(),SERVICE_ALL_ACCESS)};
    if(service.h){validate_config(service,*m);require_stopped(service);}
    else require(GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST,"service_open_failed");
    // Keep startup blocked if uninstall loses its ordinary coordinator before
    // invalidating the receipt. Matching Setup can recover this stopped state.
    const auto pending=root/L"Bin"/pending_leaf;
    if(absent_mode(pending)){
        const auto policy=m->enabled?(service.h?validate_config(service,*m):m->start_policy):SERVICE_DISABLED;
        const auto text="SFX-SERVICE-REGISTRATION-2\n"+narrow(receipt.id)+"\n"+narrow(owner)+"\n"+
            (m->enabled?"SERVICE\n":"DESKTOP\n")+narrow(unique_id())+"\n"+std::to_string(policy)+"\n";
        auto sd=structural_descriptor(owner);create_file(pending,{text.begin(),text.end()},sd.get());
    }
    if(service.h)require(DeleteService(service),"service_remove_failed_pending_preserved");
}
void remove_obsolete_owned_service(const Receipt& receipt) {
    require(elevated()&&receipt.committed&&receipt.owner==operator_sid(),"service_obsolete_receipt_unverified");
    const auto registered=registered_installations({receipt.id},true);
    require(registered.size()==1&&!registered.front().available&&
        encode_receipt(registered.front().receipt)==encode_receipt(receipt),"service_obsolete_receipt_changed");
    const ServiceMode expected{receipt.root,receipt.owner,receipt.id,L"SFXBBS_"+receipt.id};
    ScHandle manager{OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT)};
    require(manager.h,"service_manager_unavailable");
    ScHandle service{OpenServiceW(manager,expected.name.c_str(),DELETE|SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS)};
    if(!service.h){
        require(GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST,"service_obsolete_open_failed");
        return;
    }
    validate_config(service,expected);
    require_stopped(service);
    require(DeleteService(service),"service_obsolete_remove_failed");
}
bool is_owned_service_runtime(const Path& root,const std::wstring& owner) noexcept {
    try {
        if(!system_token())return false;
        const auto mode=read_service_mode(root);if(!mode||mode->owner!=owner)return false;
        const auto state=query_owned_service(*mode);
        return state.process_id==GetCurrentProcessId()&&state.state!=SERVICE_STOPPED&&
            !_wcsicmp(module_path().c_str(),(root/L"Bin"/L"SFXRuntime.exe").c_str());
    }catch(...){return false;}
}
ServiceMode verify_service_runtime_entry(const Path& root) {
    const auto mode=read_service_mode(root);require(mode&&is_owned_service_runtime(root,mode->owner),
        "service_runtime_identity_unverified");return *mode;
}
}
