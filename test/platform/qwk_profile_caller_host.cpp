// Focused timing qualification: current linked host, frozen installed programs.
#include "../../src/apps/runtime/alpha_runtime_host.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <iostream>
namespace inst=sfx::platform::win32::installation;
namespace app=sfx::application;
int main()try{
    inst::require(!inst::elevated(),"ordinary owner token required");
    const auto root=inst::canonical_root(L"C:/Program Files/SFX-Beta2-Test-20261112-r1");
    inst::verify_runtime_installation(root,inst::operator_sid());
    sfx::apps::runtime::AlphaRuntimeHost host(root);
    const auto ready=host.start();
    inst::require(ready.code==app::LocalControlResponseCode::ok,"normal current-host start");
    std::cout<<"READY current linked AlphaRuntimeHost; not installed Runtime exe"<<std::endl;
    for(std::string line;std::getline(std::cin,line)&&line!="stop";){}
    inst::require(host.shutdown().code==app::LocalControlResponseCode::ok,"normal current-host shutdown");
    std::cout<<"STOPPED clean"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"FAIL current QWK caller host: "<<e.what()<<'\n';return 1;}
