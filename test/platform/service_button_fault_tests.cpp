// Closed rc4 fixtures only; real offline contention and bounded admin faults.
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include <sddl.h>
#include <iostream>
namespace ins=sfx::platform::win32::installation;
int wmain(int argc,wchar_t** argv)try{
    ins::require(argc>=3,"closed service-button test action required");
    const auto root=ins::canonical_root(argv[2]);const auto name=root.filename().wstring();
    ins::require(root.parent_path()==ins::Path(L"C:/Program Files")&&(name==L"SFX-Service-Button-Fresh-Test-20261105-r1"||
        name==L"SFX-Service-Button-Upgrade-Test-20261105-r1"),"only new rc4 fixtures");
    const auto owner=ins::operator_sid();const auto r=ins::read_receipt(root,owner);ins::verify_structure(r);
    const auto action=std::wstring_view(argv[1]);
    if(action==L"hold-offline"){
        ins::require(argc==5&&!ins::elevated(),"ordinary bounded contention child");
        sfx::platform::win32::OfflineInstallationGuard guard;ins::require(guard.acquire(root,owner).succeeded(),"offline contention guard acquired");
        ins::Handle ready(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(std::stoull(argv[3]))));
        ins::Handle stop(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(std::stoull(argv[4]))));
        ins::require(SetEvent(ready.get())&&WaitForSingleObject(stop.get(),60000)==WAIT_OBJECT_0,"bounded inherited test events");
        std::cout<<"PASS real offline contender released normally\n";return 0;
    }
    ins::require(argc==3&&ins::elevated(),"administrative closed test action");
    const auto m=ins::read_service_registration(root,true);ins::require(m&&m->owner==owner&&m->id==r.id,"protected fixture service binding");
    if(action==L"pending"||action==L"clear-pending"){
        const auto narrow=[](const std::wstring& value){std::string v;for(const auto c:value)v+=static_cast<char>(c);return v;};
        const std::string content="SFX-SERVICE-REGISTRATION-2\n"+narrow(r.id)+"\n"+narrow(owner)+"\nSERVICE\nBBBBBBBB777777770000000044444444\n2\n";
        const auto path=root/L"Bin"/L"service-registration.pending";auto sd=ins::structural_descriptor(owner);
        ins::require(m->enabled&&ins::query_owned_service(*m).state==SERVICE_STOPPED,"pending test requires stopped Service mode");
        if(action==L"pending"){
            ins::require(!m->pending,"preserve existing pending transaction");ins::create_file(path,{content.begin(),content.end()},sd.get());
        }else {
            const auto b=ins::read_file(path,4096);auto pin=ins::open_object(path,false);
            ins::require(m->pending&&std::string(b.begin(),b.end())==content&&ins::exact_security(pin.get(),sd.get()),"only exact constructed pending fixture may be cleared");pin.reset();
            ins::require(DeleteFileW(path.c_str()),"clear exact constructed fault");
        }
        std::cout<<"PASS constructed pending fault action\n";return 0;
    }
    auto manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);ins::require(manager,"SCM connection");
    struct Close {SC_HANDLE h;~Close(){if(h)CloseServiceHandle(h);}} cm{manager};
    auto service=OpenServiceW(manager,m->name.c_str(),WRITE_DAC|(action==L"restore-query"?0:SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS)|
        (action==L"retire"?DELETE:0));ins::require(service,"only exact owned test service opened");Close cs{service};
    if(action!=L"restore-query")ins::require(ins::query_owned_service(*m).state==SERVICE_STOPPED,"test service must be stopped");
    if(action==L"deny-query"||action==L"restore-query"){
        const auto sddl=L"O:BAG:BAD:P"+std::wstring(action==L"deny-query"?L"(D;;CCLC;;;BA)":L"")+
            L"(A;;GA;;;SY)(A;;GA;;;BA)(A;;CCLCSWRPWPLOCRRC;;;"+owner+L")";
        PSECURITY_DESCRIPTOR raw{};ins::require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw,nullptr),"bounded test service descriptor");
        ins::Descriptor sd(raw);ins::require(SetServiceObjectSecurity(service,DACL_SECURITY_INFORMATION,raw),"test query authority action");
    }else if(action==L"retire"){
        ins::require(!m->enabled&&ins::query_owned_service(*m).start_type==SERVICE_DISABLED,"retire only finished Desktop Disabled fixture service");
        ins::require(DeleteService(service),"retire only exact new stopped service; no fixture file changes");
    }else ins::require(false,"unknown closed fault action");
    std::cout<<"PASS exact stopped rc4 fixture action\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
