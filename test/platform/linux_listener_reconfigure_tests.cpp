#include "linux_caller_listener.hpp"
#include "linux_startup_message.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/platform/network.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <mutex>
#include <unistd.h>

namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
using sfx::platform::linux_host::FileDescriptor;
namespace {
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Clock final:app::IClock{
    std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    std::uint64_t monotonic_millis()const noexcept override{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
};
sockaddr_in address(std::uint16_t port){sockaddr_in value{};value.sin_family=AF_INET;value.sin_port=htons(port);value.sin_addr.s_addr=htonl(INADDR_LOOPBACK);return value;}
FileDescriptor reserve(std::uint16_t& port){FileDescriptor fd(socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0));auto value=address(0);check(fd&&bind(fd.get(),reinterpret_cast<sockaddr*>(&value),sizeof(value))==0&&listen(fd.get(),2)==0,"reserve isolated loopback port");socklen_t size=sizeof(value);check(getsockname(fd.get(),reinterpret_cast<sockaddr*>(&value),&size)==0,"read isolated port");port=ntohs(value.sin_port);return fd;}
FileDescriptor connect_to(std::uint16_t port){FileDescriptor fd(socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0));auto value=address(port);check(fd&&connect(fd.get(),reinterpret_cast<sockaddr*>(&value),sizeof(value))==0,"connect exact test listener");timeval timeout{2,0};check(setsockopt(fd.get(),SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0,"bound caller read");return fd;}
void echo(FileDescriptor& fd,char byte){char reply{};check(send(fd.get(),&byte,1,MSG_NOSIGNAL)==1&&recv(fd.get(),&reply,1,0)==1&&reply==byte,"original caller remains responsive across listener transaction");}
template<class F>void eventually(F condition,const char* why){for(unsigned i=0;i<300;++i){if(condition()){check(true,why);return;}std::this_thread::sleep_for(std::chrono::milliseconds(10));}check(false,why);}
}
int main(){try{
    check(geteuid()!=0,"normal user test");Clock clock;app::OperationalConfiguration config;config.logical_nodes=4;config.maximum_connections_per_source=4;config.telnet.enabled=true;config.telnet.bind_address="127.0.0.1";config.telnet.port=0;config.ssh.enabled=false;
    app::Identifier128 epoch{};epoch.bytes[0]=1;
    rt::LinuxCallerListenerPorts ports{clock,[](auto,bool,const auto& stopping){return rt::LinuxConnectionAdmission{!stopping.load(),false};},[](int fd,bool,const auto& assignment){char byte{};while(!assignment->stopping.load()){const auto n=recv(fd,&byte,1,0);if(n<=0)break;if(send(fd,&byte,1,MSG_NOSIGNAL)!=1)break;}return true;}};
    std::mutex events_mutex;std::vector<app::OperationalEvent> events;
    ports.observe=[&](app::OperationalEvent event){std::lock_guard<std::mutex> lock(events_mutex);events.push_back(std::move(event));};
    rt::LinuxCallerListener listener(epoch,config,std::move(ports));check(listener.start(),"native listener starts");const auto first=listener.status().telnet_port;check(first>1024,"kernel selected high test port");auto existing=connect_to(first);echo(existing,'a');
    for(bool ssh:{false,true}){
        auto conflict=config;conflict.telnet.port=ssh?0:first;conflict.ssh.enabled=ssh;conflict.ssh.bind_address="127.0.0.1";conflict.ssh.port=first;
        rt::LinuxCallerListenerPorts callbacks{clock,[](auto,bool,const auto&){return rt::LinuxConnectionAdmission{true,false};},[](int,bool,const auto&){return true;}};
        rt::LinuxCallerListener failed(epoch,conflict,std::move(callbacks));
        check(!failed.start()&&!failed.status().listening&&failed.status().telnet_port==0,"conflict refuses and rolls back partially opened endpoints");
        const auto code=std::string(ssh?"ssh":"telnet")+"_port_in_use:127.0.0.1:"+std::to_string(first);
        check(failed.status().diagnostic==code,"actual socket conflict retains endpoint and EADDRINUSE reason");
        check(rt::linux_startup_message(code).find("already in use")!=std::string::npos,"actual conflict maps to useful startup detail");
        echo(existing,'z');
    }
    std::uint16_t second{};auto held=reserve(second);auto candidate=config;candidate.telnet.port=second;bool published{};
    check(!listener.reconfigure_endpoints(candidate,[&]{published=true;return true;})&&!published&&listener.status().telnet_port==first,"occupied candidate cannot publish or replace old listener");echo(existing,'b');held=FileDescriptor{};
    check(!listener.reconfigure_endpoints(candidate,[&]{published=true;return false;})&&published&&listener.status().telnet_port==first,"refused publication preserves old listener");echo(existing,'c');
    check(listener.reconfigure_endpoints(candidate,[]{return true;})&&listener.status().telnet_port==second,"verified candidate publishes new endpoint");echo(existing,'d');auto next=connect_to(second);echo(next,'e');
    eventually([&]{return listener.status().pool.active==2;},"old and new callers coexist");
    auto boundary=candidate;boundary.hostname_lookup=!boundary.hostname_lookup;published=false;check(!listener.reconfigure_endpoints(boundary,[&]{published=true;return true;})&&!published,"zero-caller settings boundary rechecked atomically before publication");
    auto policy=candidate;policy.logical_nodes=3;published=false;check(!listener.reconfigure_endpoints(policy,[&]{published=true;return true;})&&!published,"node-policy mutation requires separate restart");
    auto disabled=candidate;disabled.telnet.enabled=false;check(listener.reconfigure_endpoints(disabled,[]{return true;})&&listener.status().telnet_port==0,"disabled listener closes admission without disconnecting callers");echo(existing,'f');echo(next,'g');
    auto restarted=disabled;restarted.telnet.enabled=true;restarted.telnet.port=0;check(listener.reconfigure_endpoints(restarted,[]{return true;}),"listener reenabled after disabled boundary");auto third=connect_to(listener.status().telnet_port);echo(third,'h');
    listener.stop();check(!listener.status().listening&&listener.status().pool.active==0&&listener.status().diagnostic=="stopped","final stop joins all actual callers");
    check(!listener.reconfigure_endpoints(config,[]{return true;}),"retired listener cannot reconfigure");
    {std::lock_guard<std::mutex> lock(events_mutex);check(!events.empty()&&events.front().kind==app::OperationalKind::listener_ready&&events.front().context.local_port==first,"actual bound listener port observed even when requested port is zero");
        check(events.back().kind==app::OperationalKind::listener_stopped&&events.back().context.local_port>1024,"final listener retirement records actual bound port");
        std::size_t releases{};for(const auto& event:events)if(event.kind==app::OperationalKind::assignment_released){++releases;check(event.context.node>0&&event.context.generation>0&&event.context.source_ip=="127.0.0.1","retirement pins original assignment and source");}
        check(releases==3,"one retirement observation per actual caller");}
    std::cout<<"PASS "<<checks<<" native listener transaction and surviving caller checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
