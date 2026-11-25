// Normal Setup coordinator, elevated worker and exact shipping resources.
// These closed fixtures are the only installations this driver may change.
#include "setup_engine.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include <shellapi.h>
#include <objbase.h>
#include <fstream>
#include <iostream>
namespace ins=sfx::platform::win32::installation;
namespace setup=sfx::apps::setup;
namespace {
unsigned checks{};
void check(bool ok,const char* reason){++checks;ins::require(ok,reason);std::cout<<"PASS "<<reason<<std::endl;}
bool owned(const ins::Path& p) {
    return p==ins::Path(L"C:/Program Files/SFX-Service-Test-20261105-r1")||
        p==ins::Path(L"C:/Program Files/SFX-Desktop-Test-20261105-r1")||
        p==ins::Path(L"C:/Program Files/SFX-Service-Recovery-Test-20261105-r1")||
        p==ins::Path(L"C:/Program Files/SFX-Service-Fresh-Test-20261105-r2")||
        p==ins::Path(L"C:/Program Files/SFX-Existing-Desktop-Test-20261105-r2")||
        p==ins::Path(L"C:/Program Files/SFX-Service-Uninstall-Test-20261105-r1");
}
}
int wmain(int argc,wchar_t** argv)try{
    if(argc==4&&std::wstring_view(argv[1])==L"--worker")
        return setup::worker(argv[2],static_cast<DWORD>(std::stoul(argv[3])));
    check(argc==4&&!ins::elevated(),"ordinary bounded setup fixture invocation");
    const std::wstring action=argv[1];const auto root=ins::canonical_root(argv[2]);
    check(owned(root),"only named new Section15 installation");
    const auto evidence=std::filesystem::absolute(argv[3]);
    check(!std::filesystem::exists(evidence),"new evidence path preserves all earlier runs");
    std::filesystem::create_directories(evidence);
    check(action==L"fresh-service"||action==L"fresh-desktop"||action==L"service"||action==L"desktop"||
        action==L"maintenance"||action==L"uninstall"||action==L"cancel","closed Setup action");
    const bool fresh=action==L"fresh-service"||action==L"fresh-desktop";
    if(fresh)check(!std::filesystem::exists(root),"fresh target cannot adopt an existing fixture");
    auto plan=setup::inspect_plan(root,action==L"uninstall");
    check(!fresh||plan.operation==setup::Operation::fresh,"fresh plan is actually fresh");
    // No owner recovery ZIP. Ordinary fixture maintenance deliberately declines
    // Setup's optional data archive; mandatory rebuild recovery remains intact.
    if(plan.operation==setup::Operation::maintenance)plan.backup.clear();
    setup::Options options;options.start_menu=false;options.desktop=false;
    if(action==L"fresh-service"||action==L"service")options.service_mode=true;
    if(action==L"desktop")options.service_mode=false;
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    check(SUCCEEDED(initialized),"ordinary COM initialization");
    struct Com{~Com(){CoUninitialize();}} com;
    const auto complete=setup::execute(plan,options,nullptr,[](const auto& status){std::wcout<<status<<std::endl;},
        [&]{return action!=L"cancel";});
    check(complete,"normal Setup worker completed");
    if(action!=L"uninstall") {
        const auto receipt=ins::read_receipt(root,ins::operator_sid());ins::verify_structure(receipt);
        check(receipt.committed&&receipt.program_count==12,"complete current protected program generation");
        const auto mode=ins::read_service_mode(root);
        if(action==L"fresh-service"||action==L"service") {
            check(mode.has_value(),"explicit Service Mode recorded");
            const auto status=ins::query_owned_service(*mode);
            check(status.state==SERVICE_STOPPED,"newly configured service remains truthfully stopped until Start BBS");
            std::ofstream out(evidence/L"service-identity.txt");
            for(const auto c:mode->name)out<<static_cast<char>(c);
            out<<'\n'<<root.u8string()<<'\n';
            check(bool(out),"owned service name and root retained for exact cleanup");
        } else if(action==L"fresh-desktop"||action==L"desktop")check(!mode,"Desktop Mode retained without service");
        std::ofstream out(evidence/L"receipt.txt");const auto bytes=ins::encode_receipt(receipt);
        out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        check(bool(out),"exact committed receipt recorded");
    } else check(!std::filesystem::exists(root/L"Bin"/L"installation.id"),"only owned program identity removed");
    std::cout<<"PASS "<<checks<<" Section15 normal Setup checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
