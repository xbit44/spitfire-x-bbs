#include "setup_engine.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include <objbase.h>
#include <fstream>
#include <iostream>
namespace ins=sfx::platform::win32::installation;
namespace setup=sfx::apps::setup;
int wmain(int argc,wchar_t** argv)try{
    if(argc==4&&std::wstring_view(argv[1])==L"--worker")return setup::worker(argv[2],static_cast<DWORD>(std::stoul(argv[3])));
    ins::require(argc==4&&!ins::elevated(),"ordinary closed rc3 Setup action required");
    const auto root=ins::canonical_root(argv[2]);const auto name=root.filename().wstring();
    ins::require(root.parent_path()==ins::Path(L"C:/Program Files")&&(name==L"SFX-Config-Service-Fresh-Test-20261105-r1"||
        name==L"SFX-Config-Service-Desktop-Upgrade-Test-20261105-r1"||name==L"SFX-Config-Service-Service-Upgrade-Test-20261105-r1"||
        name==L"SFX-Config-Service-Uninstall-Test-20261105-r1"),"only new rc3 fixture roots");
    const std::wstring action=argv[1];ins::require(action==L"maintenance"||action==L"fresh"||action==L"uninstall","closed rc3 Setup operation");
    const auto out=ins::Path(argv[3]);ins::require(!std::filesystem::exists(out),"new evidence directory required");std::filesystem::create_directories(out);
    if(action==L"fresh")ins::require(!std::filesystem::exists(root),"fresh root cannot adopt old data");
    auto plan=setup::inspect_plan(root,action==L"uninstall");if(plan.operation==setup::Operation::maintenance)plan.backup.clear();
    setup::Options options;options.desktop=false;options.start_menu=false;
    ins::require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)),"COM initialized");
    struct Com{~Com(){CoUninitialize();}} com;
    ins::require(setup::execute(plan,options,nullptr,[](const auto& s){std::wcout<<s<<std::endl;},[]{return true;}),"normal Setup completed");
    if(action==L"uninstall")ins::require(!std::filesystem::exists(root/L"Bin"/L"installation.id"),"uninstall removed owned identity");
    else {
        const auto receipt=ins::read_receipt(root,ins::operator_sid());ins::verify_structure(receipt);
        const auto m=ins::read_service_registration(root);ins::require(m&&m->revision!=L"legacy-v1"&&!m->pending,"stable v2 registration");
        const auto actual=ins::query_owned_service(*m);ins::require(actual.state==SERVICE_STOPPED,"Setup leaves BBS stopped");
        ins::require(m->enabled||actual.start_type==SERVICE_DISABLED,"Desktop registration Disabled");
        std::ofstream f(out/L"receipt.txt");const auto b=ins::encode_receipt(receipt);f.write(reinterpret_cast<const char*>(b.data()),b.size());
        std::ofstream report(out/L"service.txt");report<<m->enabled<<'\n'<<actual.start_type<<'\n';
        for(const auto c:m->revision)report<<static_cast<char>(c);report<<'\n';
    }
    std::cout<<"PASS normal rc3 Setup registration/update/uninstall\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
