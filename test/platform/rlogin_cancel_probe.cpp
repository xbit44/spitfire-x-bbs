// Standalone native cancellation pressure probe. No owner account, credential,
// remote connection or terminal UI. It must leave the resolver usable.
#include <winsock2.h>
#include <windows.h>
#include "alpha_rlogin.hpp"
#include <iostream>
int main(){WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return 2;
    namespace app=sfx::application;namespace rt=sfx::apps::runtime;
    app::RloginTarget target;target.host="localhost";target.port=1;
    app::RloginCallerIdentity caller{"Probe",1,1700000000,"PROBE"};
    app::RloginBridgePorts ports;ports.now=[] {return GetTickCount64();};
    ports.stop=[]()->std::optional<app::RloginEnd>{return app::RloginEnd::operator_terminated;};
    ports.caller_read=[](std::size_t){return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};};
    ports.caller_write=[](std::string_view b){return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,b.size()};};
    unsigned wrong{};for(unsigned n=0;n<160;++n){
#ifdef SFX_RLOGIN_OWNED_SECRETS
        const auto result=rt::run_alpha_rlogin(target,caller,app::RloginSecret{},app::RloginSecret{},ports);
#else
        const auto result=rt::run_alpha_rlogin(target,caller,"","",ports);
#endif
        if(result.end!=app::RloginEnd::operator_terminated)++wrong;
    }
    const auto unavailable=rt::alpha_rlogin_resolver_unavailable_slots(),sockets=rt::alpha_rlogin_live_sockets();
    const bool passed=!wrong&&!unavailable&&!sockets;
    std::cout<<"{\"result\":\""<<(passed?"PASS":"FAIL")<<"\",\"early_cancellations\":160,\"incorrect_results\":"<<wrong<<",\"unavailable_resolver_slots\":"<<unavailable<<",\"live_remote_sockets\":"<<sockets<<"}\n";
#ifdef SFX_RLOGIN_OWNED_SECRETS
    rt::close_alpha_rlogin_process();
#endif
    WSACleanup();return passed?0:1;
}
