// Administrative fault injection only in the four new named rc3 fixtures.
// Does not launch Doors, modify programs or touch any earlier installation.
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include <sddl.h>
#include <iostream>
namespace ins=sfx::platform::win32::installation;
int wmain(int argc,wchar_t** argv)try {
    ins::require(argc==3&&ins::elevated(),"closed elevated fixture action required");
    const auto root=ins::canonical_root(argv[2]);const auto leaf=root.filename().wstring();
    ins::require(root.parent_path()==ins::Path(L"C:/Program Files")&&(leaf==L"SFX-Config-Service-Fresh-Test-20261105-r1"||
        leaf==L"SFX-Config-Service-Desktop-Upgrade-Test-20261105-r1"||leaf==L"SFX-Config-Service-Service-Upgrade-Test-20261105-r1"||
        leaf==L"SFX-Config-Service-Uninstall-Test-20261105-r1"),"new rc3 fixture only");
    const auto owner=ins::operator_sid();const auto r=ins::read_receipt(root,owner);ins::verify_structure(r);
    const auto m=ins::read_service_registration(root);ins::require(m&&m->id==r.id,"stable registration required before injection");
    sfx::platform::win32::OfflineInstallationGuard guard;ins::require(guard.acquire(root,owner).succeeded(),"fixture is stopped and exclusive");
    const auto action=std::wstring_view(argv[1]);
    auto manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);ins::require(manager,"SCM unavailable");
    struct Close {SC_HANDLE h;~Close(){if(h)CloseServiceHandle(h);}} cm{manager};
    auto service=OpenServiceW(manager,m->name.c_str(),SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS|WRITE_DAC|
        (action==L"restore-access"?0:SERVICE_CHANGE_CONFIG)|(action==L"retire-registration"?DELETE:0));ins::require(service,"exact test service unavailable");Close cs{service};
    ins::require(ins::query_owned_service(*m).state==SERVICE_STOPPED,"test service must remain stopped");
    if(action==L"retire-registration"){
        ins::require(!m->enabled&&ins::query_owned_service(*m).start_type==SERVICE_DISABLED,"retire only finished Desktop Disabled test registration");
        ins::require(DeleteService(service),"retire exact stopped test SCM registration; preserve all fixture bytes");
    }else if(action==L"retire-pending"){
        ins::require(leaf==L"SFX-Config-Service-Uninstall-Test-20261105-r1","retirement recovery only in uninstall fixture");
        ins::remove_owned_service(root,owner);
    }else if(action==L"revise")ins::configure_owned_service(root,owner,m->enabled);
    else if(action==L"manual")ins::require(m->enabled&&ChangeServiceConfigW(service,SERVICE_NO_CHANGE,SERVICE_DEMAND_START,SERVICE_NO_CHANGE,
        nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr),"set owned service Manual");
    else if(action==L"pending-manual"||action==L"pending-desktop") {
        ins::require(m->enabled,"pending recovery fixture begins in Service mode");
        const bool manual=action==L"pending-manual";
        const auto narrow=[](const std::wstring& s){std::string text;for(const auto c:s)text+=static_cast<char>(c);return text;};
        const auto text="SFX-SERVICE-REGISTRATION-2\n"+narrow(r.id)+"\n"+narrow(owner)+"\n"+
            (manual?"SERVICE\n":"DESKTOP\n")+narrow(ins::unique_id())+(manual?"\n3\n":"\n4\n");
        auto sd=ins::structural_descriptor(owner);ins::create_file(root/L"Bin"/L"service-registration.pending",{text.begin(),text.end()},sd.get());
    }else if(action==L"deny-change"||action==L"restore-access") {
        const auto text=L"O:BAG:BAD:P"+std::wstring(action==L"deny-change"?L"(D;;DC;;;BA)":L"")+
            L"(A;;GA;;;SY)(A;;GA;;;BA)(A;;CCLCSWRPWPLOCRRC;;;"+owner+L")";
        PSECURITY_DESCRIPTOR raw{};ins::require(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(),SDDL_REVISION_1,&raw,nullptr),"test descriptor");
        ins::Descriptor sd(raw);ins::require(SetServiceObjectSecurity(service,DACL_SECURITY_INFORMATION,raw),"test authority fault applied");
    }else ins::require(false,"unknown test action");
    std::cout<<"PASS exact stopped rc3 fixture fault action\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
