// Closed, fresh Section14A installation only. Actual shipping Runtime/loopback.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "ftn_current_installed_client.inc"
#include "sfx/platform/win32/offline_account_administration.hpp"
#include <fstream>
#include <regex>
namespace {
const ins::Path root=ins::canonical_root(L"C:/Program Files/SFX-Sysop-Test-20261104-r2");
std::string visible_text(const std::string& wire){
    static const std::regex csi("\x1b\\[[0-?]*[ -/]*[@-~]");
    return std::regex_replace(wire,csi,"");
}

void wait_for_retirement(RuntimeSession& runtime){
    const auto deadline=GetTickCount64()+10000;
    do {
        const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& request)->std::optional<std::vector<std::uint8_t>>{
            return runtime.exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(request));
        });
        check(status&&runtime.tracker.accept(*status)&&!status->nodes.empty(),"verified installed Runtime status");
        if(std::all_of(status->nodes.begin(),status->nodes.end(),[](const auto& node){
            return !node.caller_connected&&!node.account_id&&
                (node.state==sfx::core::NodeState::ready||node.state==sfx::core::NodeState::offline);
        }))return;
        Sleep(50);
    }while(GetTickCount64()<deadline);
    throw std::runtime_error("caller assignment did not retire before the next test");
}

struct Client {
    SOCKET socket{INVALID_SOCKET};std::string transcript;bool authenticated_output{};
    Client(){socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(socket!=INVALID_SOCKET,"loopback socket");DWORD ms=15000;setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&ms),sizeof ms);sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(26551);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);check(connect(socket,reinterpret_cast<const sockaddr*>(&address),sizeof address)==0,"closed loopback listener");}
    ~Client(){if(socket!=INVALID_SOCKET){shutdown(socket,SD_BOTH);closesocket(socket);}}
    void send(std::string_view s){check(::send(socket,s.data(),static_cast<int>(s.size()),0)==static_cast<int>(s.size()),"bounded caller command");}
    std::string until(std::string_view expected){
        std::string wire, visible;
        std::size_t acknowledged{};
        const auto deadline=GetTickCount64()+20000;
        while(visible.find(expected)==std::string::npos&&GetTickCount64()<deadline){
            char bytes[4096];
            const auto n=recv(socket,bytes,sizeof bytes,0);
            if(n<=0){
                if(authenticated_output)std::cerr<<"POST-AUTH PROMPT: "<<visible.substr(visible.size()>1600?visible.size()-1600:0)<<std::endl;
                throw std::runtime_error("caller did not reach "+std::string(expected));
            }
            wire.append(bytes,n);
            check(wire.size()<1048576,"bounded caller output");
            // Color changes can split a single prompt, including the ENTER key.
            // Match visible text while preserving the original bytes for review.
            visible=visible_text(wire);
            if(authenticated_output&&expected=="MAIN MENU - ["){
                for(const std::string_view prompt:{"< ENTER > To Continue? ","<Enter> to continue ","Press <ENTER> to continue."}){
                    const auto found=visible.find(prompt,acknowledged);
                    if(found!=std::string::npos){
                        acknowledged=found+prompt.size();
                        send("\r");
                    }
                }
            }
        }
        check(visible.find(expected)!=std::string::npos,"caller prompt reached");
        transcript+=wire;
        return wire;
    }
    void login(bool ansi){until("[Y/n] ");send(ansi?"Y\r":"N\r");until("Enter your first name: ");send("Sysop\r");until("Enter your last name: ");send("Fixture\r");until("Enter your password: ");send("Synthetic-owner-only-123\r");until("su password: ");send("Synthetic-system-only-456\r");until("Press <ENTER> to continue.");authenticated_output=true;transcript.clear();send("\r");until("MAIN MENU - [");transcript.clear();}
};
}
int main(int argc,char** argv)try{
    check(argc==3,"action and fresh evidence root required");const std::string action=argv[1];check(action=="--bootstrap"||action=="--menus","closed action");const auto out=std::filesystem::absolute(argv[2]);check(!std::filesystem::exists(out),"retain prior runtime evidence");std::filesystem::create_directories(out);check(!ins::elevated(),"ordinary installed owner");ins::verify_runtime_installation(root,ins::operator_sid());
    if(action=="--bootstrap"){
        RuntimeSession runtime(root,true);app::AlphaBootstrapRequest q;q.configuration.bbs_name="Sysop Utilities Qualification";q.configuration.bbs_time_zone="Pacific Standard Time";q.configuration.logical_nodes=4;q.configuration.telnet.bind_address="127.0.0.1";q.configuration.telnet.port=26551;q.configuration.telnet.bot_gate_enabled=false;q.configuration.botgate.enabled=false;q.configuration.ssh.enabled=false;q.configuration.hostname_lookup=false;q.sysop_name="Sysop Fixture";q.sysop_password="Synthetic-owner-only-123";q.system_password="Synthetic-system-only-456";
        const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& request)->std::optional<std::vector<std::uint8_t>>{return runtime.exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(request));});check(status.has_value(),"first-run security policy");q.configuration.security_policy_revision=status->security_policy_revision;q.configuration.security_policy_hash=status->security_policy_hash;const auto response=app::decode_control_response(runtime.exchange(app::LocalControlMessageType::bootstrap_request,app::LocalControlMessageType::bootstrap_response,app::encode_bootstrap_request(q)));app::erase_bootstrap_secrets(q);check(response&&response->code==app::LocalControlResponseCode::ok,"shipping first-run bootstrap");runtime.stop();
        auto config=win::offline_ftn_configuration(root,ins::operator_sid());check(config&&config.value,"fresh FTN snapshot");config.value->settings.listener_enabled=config.value->settings.tls_listener_enabled=false;config.value->settings.bind_address="127.0.0.1";check(bool(win::offline_change_ftn_configuration(root,ins::operator_sid(),token(),*config.value)),"no live hub or listener configuration");
    }else{
        WSADATA data{};check(WSAStartup(MAKEWORD(2,2),&data)==0,"Winsock");struct Cleanup{~Cleanup(){WSACleanup();}} cleanup;RuntimeSession runtime(root);
        for(const bool ansi:{false,true}){
            wait_for_retirement(runtime);
            Client caller;caller.login(ansi);caller.send("@");const auto top=caller.until("Command: ");check(visible_text(top).find("Sysop Utilities")!=std::string::npos,"authenticated Main @ reaches implemented utilities");
            check((top.find('\x1b')!=std::string::npos)==ansi,"installed menu uses requested ANSI or plain mode");
            {std::ofstream frame(out/(ansi?"sysop-top.ans":"sysop-top.txt"),std::ios::binary);frame<<top;check(bool(frame),"installed top menu frame retained");}
            caller.send("C");caller.until("Command: ");caller.send("A");caller.until("Select row or command: ");caller.send("1\r");caller.until("Command: ");caller.send("T");caller.until("Command: ");caller.send("Q");caller.until("Select row or command: ");caller.send("Q\r");caller.until("Command: ");caller.send("Q");caller.until("Command: ");caller.send("L");caller.until("Command: ");caller.send("C");caller.until("Command: ");caller.send("Q");caller.until("Command: ");caller.send("Q");caller.until("Command: ");caller.send("Q");caller.until("MAIN MENU - [");
            check(caller.transcript.find("Synthetic-owner-only-123")==std::string::npos&&caller.transcript.find("Synthetic-system-only-456")==std::string::npos,"no secret in menu evidence");std::ofstream file(out/(ansi?"installed-menus.ans":"installed-menus.txt"),std::ios::binary);file<<caller.transcript;check(bool(file),"actual installed transcript retained");caller.send("/G");
            std::cout<<"PASS "<<(ansi?"ANSI":"plain")<<" installed Sysop walkthrough\n";
        }
        wait_for_retirement(runtime);
        runtime.stop();
    }
    std::cout<<"PASS "<<checks<<" actual installed Section14A checks; isolated loopback only\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
