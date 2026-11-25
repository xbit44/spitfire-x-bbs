#include <winsock2.h>
#include <windows.h>
#include "alpha_rlogin.hpp"
#include <atomic>
#include <thread>
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;namespace runtime=sfx::apps::runtime;
namespace {
unsigned checks{};void check(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
std::string bytes(std::initializer_list<unsigned> list){std::string s;for(auto b:list)s+=char(b);return s;}
struct Peer {
    SOCKET listener{INVALID_SOCKET};std::atomic<SOCKET> peer{INVALID_SOCKET};std::thread worker;
    std::uint16_t port{};std::string received;bool binary{},hold{},ok{};
    Peer(bool bin,bool wait):binary(bin),hold(wait){
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(listener!=INVALID_SOCKET,"peer socket");sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        check(!bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof(a))&&!listen(listener,1),"loopback ephemeral listener");int n=sizeof(a);check(!getsockname(listener,reinterpret_cast<sockaddr*>(&a),&n),"bound address");port=ntohs(a.sin_port);
        worker=std::thread([&]{
            fd_set r;FD_ZERO(&r);FD_SET(listener,&r);timeval t{3,0};if(select(0,&r,nullptr,nullptr,&t)!=1)return;
            const auto p=accept(listener,nullptr,nullptr);if(p==INVALID_SOCKET)return;peer=p;DWORD timeout=3000;setsockopt(p,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
            const auto negotiation=binary?bytes({255,251,0,255,253,0}):bytes({255,251,1,255,251,3,255,253,24,255,250,24,1,255,240,255,253,31});
            for(char c:negotiation){if(send(p,&c,1,0)!=1)return;Sleep(1);}const std::string ready="PEER READY\r\n";if(send(p,ready.data(),static_cast<int>(ready.size()),0)<0)return;
            unsigned state{},verb{};std::string sub;const auto until=GetTickCount64()+6000;
            while(GetTickCount64()<until){char c{};if(recv(p,&c,1,0)!=1)break;const auto b=static_cast<unsigned char>(c);
                if(state==0){if(b==255)state=1;else received+=c;}
                else if(state==1){if(b==255){received+=c;state=0;}else if(b>=251&&b<=254){verb=b;state=2;}else if(b==250){sub.clear();state=3;}else state=0;}
                else if(state==2){if(b!=0&&b!=1&&b!=3&&b!=24&&b!=31){const auto response=bytes({255,verb==251?254U:252U,b});send(p,response.data(),3,0);}state=0;}
                else if(state==3){if(b==255)state=4;else sub+=c;}
                else if(state==4){if(b==240)state=0;else{if(b==255)sub+=c;state=3;}}
                if(!hold&&received.size()>=(binary?256U:3U)){ok=true;break;}
            }
            shutdown(p,SD_SEND);
        });
    }
    void join(){if(worker.joinable())worker.join();}
    ~Peer(){if(peer!=INVALID_SOCKET)shutdown(peer,SD_BOTH);join();if(peer!=INVALID_SOCKET)closesocket(peer);closesocket(listener);}
};
struct Caller {
    std::string input,shown;std::size_t offset{};unsigned keyboard{};ULONGLONG start{GetTickCount64()};std::optional<app::RloginEnd> stop_reason;
    app::RloginBridgePorts ports(){
        app::RloginBridgePorts p;p.now=[] {return GetTickCount64();};
        p.stop=[&]()->std::optional<app::RloginEnd>{if(stop_reason&&GetTickCount64()-start>750)return stop_reason;if(GetTickCount64()-start>6000)return app::RloginEnd::shutdown;return {};};
        p.caller_read=[&](std::size_t n){if(shown.find("PEER READY")==std::string::npos||offset==input.size())return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};n=std::min(n,input.size()-offset);std::vector<std::uint8_t> b(input.begin()+static_cast<std::ptrdiff_t>(offset),input.begin()+static_cast<std::ptrdiff_t>(offset+n));offset+=n;return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,std::move(b)};};
        p.caller_write=[&](std::string_view s){const auto n=std::min<std::size_t>(11,s.size());shown+=s.substr(0,n);return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,n};};p.keyboard=[&]{++keyboard;};p.dimensions=[] {return std::pair<std::uint16_t,std::uint16_t>{std::uint16_t{25},std::uint16_t{80}};};return p;
    }
};
}
int main(){WSADATA w{};if(WSAStartup(MAKEWORD(2,2),&w))return 1;int code{};try{
    for(bool binary:{false,true}){Peer peer(binary,false);Caller caller;if(binary)for(unsigned b=0;b<256;++b)caller.input+=char(b);else caller.input="Q\r\n";
        const auto result=runtime::run_alpha_telnet({"127.0.0.1",peer.port,"ansi",2},caller.ports(),{});peer.join();
        check(result.end==app::RloginEnd::remote_closed&&peer.ok&&peer.received==caller.input,"native independent peer receives exact NVT or all256 binary bytes");check(caller.keyboard==1&&runtime::alpha_rlogin_live_sockets()==0,"actual keyboard activity and closed outbound socket");}
    for(auto reason:{app::RloginEnd::shutdown,app::RloginEnd::operator_terminated,app::RloginEnd::calling_time,app::RloginEnd::inactivity}){
        Peer peer(false,true);Caller caller;caller.input=bytes({24})+"P";caller.stop_reason=reason;
        const auto result=runtime::run_alpha_telnet({"localhost",peer.port,"ansi",2},caller.ports(),{});peer.join();
        check(result.end==reason&&caller.shown.find("Local node")!=std::string::npos&&GetTickCount64()-caller.start<2000,"native supervision remains active during local menu");check(runtime::alpha_rlogin_live_sockets()==0,"cancel closes only owned outbound socket");}
    {Peer first(true,false),second(true,false);Caller a,b;for(unsigned n=0;n<256;++n){a.input+=char(n);b.input+=char(255-n);}app::RloginBridgeResult ra,rb;
        std::thread x([&]{ra=runtime::run_alpha_telnet({"127.0.0.1",first.port},a.ports(),{});});std::thread y([&]{rb=runtime::run_alpha_telnet({"127.0.0.1",second.port},b.ports(),{});});x.join();y.join();first.join();second.join();
        check(ra.end==app::RloginEnd::remote_closed&&rb.end==app::RloginEnd::remote_closed&&first.received==a.input&&second.received==b.input,"two native sessions remain independent");}
    {Peer peer(false,false);Caller caller;caller.input="Q";const auto result=runtime::run_alpha_telnet({"127.0.0.1",peer.port},caller.ports(),{},std::vector<std::uint8_t>{24,24,'D'});peer.join();check(result.end==app::RloginEnd::remote_closed&&peer.received==bytes({24,'D','Q'})&&caller.keyboard==1,"doubled connect-prefix escape is literal; cached input does not reset AFK twice");}
    {Caller caller;const auto result=runtime::run_alpha_telnet({"127.0.0.1",1,"ansi",1},caller.ports(),{},std::vector<std::uint8_t>{24,'D'});check(result.end==app::RloginEnd::gateway_escape,"connect-prefix cancellation before remote connection");}
    runtime::close_alpha_rlogin_process();check(runtime::alpha_rlogin_live_sockets()==0,"all sockets released");std::cout<<"PASS "<<checks<<" native Telgate checks\n";
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';code=1;runtime::close_alpha_rlogin_process();}WSACleanup();return code;}
