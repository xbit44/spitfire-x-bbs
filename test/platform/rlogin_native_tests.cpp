#include <winsock2.h>
#include <windows.h>
#include "alpha_rlogin.hpp"
#include "alpha_rlogin_resolver.hpp"
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace app=sfx::application;
namespace runtime=sfx::apps::runtime;
namespace native=sfx::platform::win32;
namespace {
unsigned checks{};
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Server {
    SOCKET listener{INVALID_SOCKET};std::atomic<SOCKET> peer{INVALID_SOCKET};std::thread worker;
    std::uint16_t port{};std::string wire,keys,payload{"\x1b[2J\x80\xfe@SHELL@\r\n"};bool accepted{};
    explicit Server(bool acknowledge=true,bool hold=false){
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(listener!=INVALID_SOCKET,"native listener socket");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        check(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && listen(listener,1)==0,"loopback listener");
        int size=sizeof(address);check(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size)==0,"loopback address");port=ntohs(address.sin_port);
        worker=std::thread([&,acknowledge,hold]{
            fd_set r;FD_ZERO(&r);FD_SET(listener,&r);timeval timeout{5,0};if(select(0,&r,nullptr,nullptr,&timeout)!=1)return;
            const auto socket=accept(listener,nullptr,nullptr);if(socket==INVALID_SOCKET)return;peer=socket;
            const DWORD milliseconds=2000;setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&milliseconds),sizeof(milliseconds));setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&milliseconds),sizeof(milliseconds));
            unsigned zeroes{};while(wire.size()<512 && zeroes<4){char byte{};if(recv(socket,&byte,1,0)!=1)break;wire+=byte;if(!byte)++zeroes;}
            if(zeroes!=4)return;accepted=true;
            if(!acknowledge){Sleep(1500);return;}
            const char ack=0;if(send(socket,&ack,1,0)!=1)return;
            for(const auto byte:payload)if(send(socket,&byte,1,0)!=1)return;
            if(hold){char byte{};while(recv(socket,&byte,1,0)>0){}return;}
            std::array<char,32> input{};while(keys.size()<2){const auto n=recv(socket,input.data(),static_cast<int>(2-keys.size()),0);if(n<=0)break;keys.append(input.data(),static_cast<std::size_t>(n));}
            shutdown(socket,SD_SEND);
        });
    }
    ~Server(){if(worker.joinable())worker.join();if(peer!=INVALID_SOCKET)closesocket(peer.load());if(listener!=INVALID_SOCKET)closesocket(listener);}
    void join(){if(worker.joinable())worker.join();}
};
struct Caller {
    std::string displayed,input{"Q\r"};std::size_t offset{};std::uint64_t read_after{};unsigned keyboards{};
    app::RloginBridgePorts ports(){
        app::RloginBridgePorts p;p.now=[] {return GetTickCount64();};p.stop=[]()->std::optional<app::RloginEnd>{return {};};p.wait=[] {Sleep(10);};
        p.caller_read=[&](std::size_t maximum){using O=app::InteractiveStreamOutcome;if(offset==input.size()||GetTickCount64()<read_after)return app::InteractiveStreamReadResult{O::timed_out,{}};const auto n=std::min(maximum,input.size()-offset);std::vector<std::uint8_t> bytes(input.begin()+static_cast<std::ptrdiff_t>(offset),input.begin()+static_cast<std::ptrdiff_t>(offset+n));offset+=n;return app::InteractiveStreamReadResult{O::completed,std::move(bytes)};};
        p.caller_write=[&](std::string_view b){displayed+=b;return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,b.size()};};p.keyboard=[&]{++keyboards;};p.dimensions=[] {return std::make_pair(std::uint16_t(25),std::uint16_t(80));};return p;
    }
};
std::vector<std::string> fields(std::string_view s){std::vector<std::string> result;std::string value;for(const auto c:s){if(c)value+=c;else{result.push_back(value);value.clear();}}return result;}
}
int main(){WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return 1;struct Winsock{~Winsock(){WSACleanup();}} winsock;
try{
    const auto protected_secret=native::protect_rlogin_secret("NativeDummyCredential");check(protected_secret.has_value(),"Windows owner DPAPI protect");
    const auto recovered=native::unprotect_rlogin_secret(*protected_secret);check(recovered && recovered->view()=="NativeDummyCredential","DPAPI exact round trip");
    auto altered=*protected_secret;altered.back()^=1;check(!native::unprotect_rlogin_secret(altered),"tampered secret rejected without fallback");
    check(!native::protect_rlogin_secret("")&&!native::protect_rlogin_secret(std::string(129,'x')),"native secret bounds");
    app::RloginCallerIdentity identity{"Caller",92,1700000000,"BOARD"};
    for(const auto hashed:{false,true}){
        Server server;Caller caller;app::RloginTarget target;target.host=hashed?"localhost":"127.0.0.1";target.port=server.port;target.prompt_credential=true;
        target.server_identity=app::RloginIdentity::configured;target.server_name="Remote";target.terminal_type="xtrn=EXAMPLE";
        target.authentication=hashed?app::RloginAuthentication::synchronet_caller_hash:app::RloginAuthentication::traditional;
        std::vector<bool> sounds;
        const auto result=runtime::run_alpha_rlogin(target,identity,app::RloginSecret(hashed?"Password":"RemotePass"),app::RloginSecret(hashed?"Pepper":""),caller.ports(),{},false,[&](bool entry){sounds.push_back(entry);});server.join();
        check(sounds==std::vector<bool>({true,false}),"actual accepted outbound gateway entry and exit exactly once");
        check(result.end==app::RloginEnd::remote_closed && server.accepted,"native handshake/remote close");
        const auto parts=fields(server.wire);check(parts.size()==4&&parts[0].empty()&&parts[2]=="Remote"&&parts[3]=="xtrn=EXAMPLE","native RFC framing and selector");
        check(parts[1]==(hashed?"8e062f49250787126ade779fd6f970d924ad9954":"RemotePass"),"independent Python SHA1 vector / explicit traditional mode");
        check(caller.displayed==server.payload&&server.keys=="Q\r","untrusted ANSI/CP437/token-like bytes and keyboard are transparent");
        check(caller.keyboards==1,"queued connection input resets AFK once at its actual arrival");
        check(runtime::alpha_rlogin_live_sockets()==0,"native remote socket reaped");
    }
    {Server server(false);Caller caller;app::RloginTarget target;target.host="127.0.0.1";target.port=server.port;target.connection_timeout_seconds=1;
        unsigned sounds{};
        const auto before=GetTickCount64();const auto result=runtime::run_alpha_rlogin(target,identity,app::RloginSecret{},app::RloginSecret{},caller.ports(),{},false,[&](bool){++sounds;});const auto elapsed=GetTickCount64()-before;
        check(!sounds,"failed handshake creates no gateway sound");
        check(result.end==app::RloginEnd::handshake_failed&&elapsed<1300,"shared bounded connect/ack timeout");server.join();}
    for(const auto reason:{app::RloginEnd::operator_terminated,app::RloginEnd::caller_disconnected,app::RloginEnd::shutdown,app::RloginEnd::gateway_escape}){
        Server server(true,true);Caller caller;caller.input.clear();const auto started=GetTickCount64();
        app::RloginTarget target;target.host="localhost";target.port=server.port;auto ports=caller.ports();
        if(reason==app::RloginEnd::gateway_escape){caller.input=std::string({29,'D'});caller.read_after=started+200;}
        else ports.stop=[=]()->std::optional<app::RloginEnd>{return GetTickCount64()-started>=200?std::optional<app::RloginEnd>(reason):std::nullopt;};
        unsigned sounds{};
        const auto result=runtime::run_alpha_rlogin(target,identity,app::RloginSecret{},app::RloginSecret{},ports,{},false,[&](bool){++sounds;throw std::runtime_error("optional sound failure");});server.join();
        check(sounds==2,"gateway exit event survives audio exception on cancellation/loss/shutdown/escape");
        check(server.accepted&&result.end==reason&&GetTickCount64()-started<1200,"connected native cancellation/loss/shutdown/escape reaps promptly");
        check(runtime::alpha_rlogin_live_sockets()==0,"all native cancellation sockets reaped");
    }
    {Server first,second;Caller a,b;app::RloginTarget one,two;one.host=two.host="localhost";one.port=first.port;two.port=second.port;
        app::RloginBridgeResult left,right;
        std::thread x([&]{left=runtime::run_alpha_rlogin(one,identity,app::RloginSecret{},app::RloginSecret{},a.ports());});
        std::thread y([&]{right=runtime::run_alpha_rlogin(two,identity,app::RloginSecret{},app::RloginSecret{},b.ports());});x.join();y.join();first.join();second.join();
        check(left.end==app::RloginEnd::remote_closed&&right.end==app::RloginEnd::remote_closed&&first.keys=="Q\r"&&second.keys=="Q\r","two native callers share resolver safely with independent sockets and keyboard");}
    for(unsigned n=0;n<160;++n){unsigned polls{};(void)runtime::resolve_alpha_rlogin_ipv4("localhost",[&]{return ++polls<=2;});}
    const auto dns_deadline=GetTickCount64()+2000;
    check(!runtime::resolve_alpha_rlogin_ipv4("localhost",[=] {return GetTickCount64()<dns_deadline;}).empty(),"160 subscribed DNS cancellations leave resolver usable");
    check(runtime::alpha_rlogin_live_sockets()==0&&runtime::alpha_rlogin_resolver_unavailable_slots()==0,"successful DNS buffers reusable; no remote socket leak");
    runtime::close_alpha_rlogin_process();check(runtime::alpha_rlogin_resolver_worker_count()==0,"DNS coordinator joined at final process close");
    std::cout<<"PASS "<<checks<<" native RLogin checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
