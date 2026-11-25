#include "linux_browser_terminal.hpp"
#include "linux_web_sessions.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>

namespace web=sfx::apps::runtime::web;
namespace rt=sfx::apps::runtime;
namespace app=sfx::application;
using FD=sfx::platform::linux_host::FileDescriptor;
namespace {
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Clock final:app::IClock{std::atomic<std::uint64_t> now{100};std::uint64_t monotonic_millis()const noexcept override{return now;}std::int64_t unix_seconds()const noexcept override{return 0;}};
template<class F>void eventually(F predicate,const char* why){for(unsigned i=0;i<200;++i){if(predicate()){check(true,why);return;}std::this_thread::sleep_for(std::chrono::milliseconds(5));}check(false,why);}
std::string read(int fd){std::array<char,1024> bytes{};const auto count=recv(fd,bytes.data(),bytes.size(),MSG_DONTWAIT);return count>0?std::string(bytes.data(),static_cast<std::size_t>(count)):std::string{};}
}
int main(){try{
    check(geteuid()!=0&&sodium_init()>=0,"normal user and random source");Clock clock;FD peer;std::uint64_t generation{};std::atomic<unsigned> closed{};std::atomic<bool> permission{true};
    app::AlphaNodeStatus ready;ready.enabled=ready.available=true;ready.state=sfx::core::NodeState::ready;
    check(web::browser_node_ready(ready),"idle enabled node is offered for logon");ready.enabled=false;check(!web::browser_node_ready(ready),"disabled node not offered");ready.enabled=true;ready.available=false;check(!web::browser_node_ready(ready),"cleanup-unavailable node not offered");ready.available=true;ready.state=sfx::core::NodeState::connecting;check(!web::browser_node_ready(ready),"unused desktop reservation not offered");ready.state=sfx::core::NodeState::ready;check(web::browser_node_ready(ready),"released reenabled node offered again");ready.caller_connected=true;check(!web::browser_node_ready(ready),"connected transport never offered");
    web::BrowserTerminals terminals({clock,[&](std::uint16_t node){int pair[2];if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,pair)!=0)throw std::runtime_error("fixture socketpair");peer=FD{pair[0]};app::Identifier128 epoch;epoch.bytes[0]=1;app::NodeAssignmentIdentity identity{epoch,node,++generation};return rt::LinuxBrowserNodeStart{{app::LocalControlResponseCode::ok,{}},identity,FD{pair[1]}};},[&](auto){++closed;}});
    check(terminals.open("owner1",3,[&]{return permission.load();}),"browser owns one exact transport");
    auto view=terminals.view("owner1");check(view&&view->identity.node==3&&!view->connecting&&view->next_input==1,"view carries exact identity and input sequence");
    const auto old=view->handle;check(!terminals.view("owner2")&&!terminals.close("owner2",old)&&!terminals.input("owner2",old,1,"x"),"another browser cannot view type or close transport");
    check(!terminals.open("owner1",4,[]{return true;}),"duplicate open cannot replace active transport");
    const std::string negotiation="\xff\xfd\x18";check(send(peer.get(),negotiation.data(),negotiation.size(),MSG_NOSIGNAL)==3,"send terminal type negotiation");std::string reply;
    eventually([&]{reply+=read(peer.get());return reply.size()>=3;},"browser terminal responds to negotiation");check(reply==std::string("\xff\xfb\x18",3),"browser offers only selected terminal option");
    const std::string request="\xff\xfa\x18\x01\xff\xf0";check(send(peer.get(),request.data(),request.size(),MSG_NOSIGNAL)==6,"send ANSI terminal type request");reply.clear();eventually([&]{reply+=read(peer.get());return reply.size()>=10;},"terminal type response arrives");check(reply==std::string("\xff\xfa\x18\x00" "ANSI\xff\xf0",10),"terminal reports ANSI with correctly framed subnegotiation");
    const std::string display="\x1b[2J\x1b[H\x1b[31mRED";check(send(peer.get(),display.data(),display.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(display.size()),"send caller ANSI output");
    eventually([&]{return terminals.view("owner1")->screen.cells[0].character=='R';},"shared terminal mirror receives caller output");view=terminals.view("owner1");check((view->screen.cells[0].attribute&15)==4&&view->screen.cells[2].character=='D',"existing ANSI and CP437 cell rendering retained");
    check(view->cursor_row==0&&view->cursor_column==3,"shared terminal cursor follows actual rendered bytes");
    check(!terminals.input("owner1",old,2,"x")&&terminals.input("owner1",old,1,"secret\r")&&!terminals.input("owner1",old,1,"secret\r"),"ordered input rejects skipped and repeated requests");reply.clear();eventually([&]{reply+=read(peer.get());return reply.size()>=7;},"typed input reaches caller once");check(reply=="secret\r"&&terminals.view("owner1")->screen.cells[0].character=='R',"input never appears in screen unless caller echoes it");
    const std::string ff(1,static_cast<char>(255));check(terminals.input("owner1",old,2,ff),"CP437 high byte accepted");reply.clear();eventually([&]{reply+=read(peer.get());return reply.size()>=2;},"IAC byte encoded");check(reply==std::string(2,static_cast<char>(255)),"Telnet IAC is doubled exactly");
    permission=false;eventually([&]{return closed.load()==1;},"revoked browser automatically disconnects caller");check(!terminals.view("owner1"),"revoked screen is no longer available");permission=true;
    check(terminals.open("owner1",3,[&]{return permission.load();}),"fresh connection after revocation");view=terminals.view("owner1");check(view->handle!=old&&view->identity.generation>1&&!terminals.close("owner1",old),"stale handle cannot disconnect new assignment");
    clock.now+=15000;eventually([&]{return closed.load()==2;},"lost browser heartbeat disconnects after fifteen seconds");
    check(terminals.open("owner1",3,[]{return true;}),"new transport for remote hangup");peer=FD{};eventually([&]{return closed.load()==3;},"caller hangup retires browser handle");
    std::string launch;web::BrowserSessions sessions(clock,[&](const auto& token){launch=token;});const auto session=sessions.launch(launch);check(session&&sessions.valid(session->first),"passive local session validity");clock.now+=1799999;check(sessions.valid(session->first),"validity check before idle deadline");clock.now+=1;check(!sessions.valid(session->first),"background validity checks do not extend local session idle lifetime");
    web::RemoteBrowserSessions remote(clock);const auto lan=remote.establish("192.168.1.12",7);check(lan&&remote.valid(lan->first,"192.168.1.12",7)&&!remote.valid(lan->first,"192.168.1.13",7),"passive LAN validity remains source bound");clock.now+=1800000;check(!remote.valid(lan->first,"192.168.1.12",7),"background checks do not extend LAN idle lifetime");
    std::cout<<"PASS "<<checks<<" browser terminal ownership transport and lifetime checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
