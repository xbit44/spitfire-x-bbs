#include "linux_caller_listener.hpp"
#include "sfx/platform/network.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <fcntl.h>

namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
using FD=sfx::platform::linux_host::FileDescriptor;
namespace {
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Clock final:app::IClock {
    std::atomic<std::uint64_t> advance{0};
    std::int64_t unix_seconds()const noexcept override{return 0;}
    std::uint64_t monotonic_millis()const noexcept override{return advance+static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
};
template<class F>void eventually(F predicate,const char* why){for(unsigned i=0;i<300;++i){if(predicate()){check(true,why);return;}std::this_thread::sleep_for(std::chrono::milliseconds(10));}check(false,why);}
void timeout(int fd){timeval value{2,0};check(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&value,sizeof(value))==0,"bounded fixture read");}
FD connect_to(std::uint16_t port){FD fd(socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0));sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);check(fd&&connect(fd.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"connect isolated loopback endpoint");timeout(fd.get());return fd;}
void echo(int fd,char byte){timeout(fd);char reply{};check(send(fd,&byte,1,MSG_NOSIGNAL)==1&&recv(fd,&reply,1,0)==1&&reply==byte,"exact node worker echoes transport bytes");}
rt::LinuxCallerListenerPorts ports(Clock& clock,bool clean=true){return {clock,[](auto,bool,const auto& stopping){return rt::LinuxConnectionAdmission{!stopping.load(),false};},[clean](int fd,bool ssh,const auto& assignment){if(ssh)return false;char byte{};while(!assignment->stopping.load()){if(recv(fd,&byte,1,0)!=1)break;if(send(fd,&byte,1,MSG_NOSIGNAL)!=1)break;}return clean;}};}
app::OperationalConfiguration config(){app::OperationalConfiguration result;result.logical_nodes=4;result.maximum_connections_per_source=4;result.telnet.enabled=true;result.telnet.bind_address="127.0.0.1";result.telnet.port=0;result.ssh.enabled=false;return result;}
}
int main(){try{
    check(geteuid()!=0,"normal user fixture");Clock clock;app::Identifier128 epoch;epoch.bytes[0]=4;
    rt::LinuxCallerListener listener(epoch,config(),ports(clock));
    check(!listener.open_browser_node(1).client&&listener.reserve_local_node(1).response.code!=app::LocalControlResponseCode::ok,"local logon refuses stopped listener");
    check(listener.start(),"listener starts");
    for(const auto node:{0,5,129})check(!listener.open_browser_node(static_cast<std::uint16_t>(node)).client&&listener.reserve_local_node(static_cast<std::uint16_t>(node)).response.code!=app::LocalControlResponseCode::ok,"out-of-range exact node refused");
    auto reserved=listener.reserve_local_node(1);check(reserved.response.code==app::LocalControlResponseCode::ok&&reserved.reservation.node==1&&reserved.host=="127.0.0.1"&&reserved.port>1024&&reserved.timeout_milliseconds==30000,"exact node reserved on one-use loopback high port");
    check(listener.status().pool.active==1&&listener.status().callers.size()==1&&listener.status().callers[0].activity=="Waiting for local client","reservation is visible and retains pool ownership");
    check(!listener.open_browser_node(1).client&&listener.reserve_local_node(1).response.code!=app::LocalControlResponseCode::ok,"same reserved node cannot be reassigned");
    auto public_caller=connect_to(listener.status().telnet_port);echo(public_caller.get(),'a');
    eventually([&]{return listener.status().pool.slots[1].occupied;},"public ingress skips exact reserved node");
    auto desktop=connect_to(reserved.port);echo(desktop.get(),'b');
    check(!listener.cancel_local_node(reserved.reservation),"consumed reservation cannot cancel an active caller");
    check(listener.status().pool.slots[0].identity==reserved.reservation,"desktop connection uses the exact reserved generation");
    auto browser=listener.open_browser_node(4);check(browser.client&&browser.identity.node==4,"browser gets exact requested node without TCP endpoint");
    sockaddr_storage local{};socklen_t length=sizeof(local);check(getsockname(browser.client.get(),reinterpret_cast<sockaddr*>(&local),&length)==0&&local.ss_family==AF_UNIX,"browser transport stays private socketpair");
    check((fcntl(browser.client.get(),F_GETFD)&FD_CLOEXEC)!=0,"browser endpoint not inherited by exec");echo(browser.client.get(),'c');
    auto foreign=browser.identity;++foreign.generation;check(!listener.disconnect(foreign)&&!listener.cancel_local_node(foreign),"future generation cannot cancel another caller");echo(browser.client.get(),'d');
    check(listener.disconnect(reserved.reservation)&&listener.wait_for_retirement(reserved.reservation,3000),"exact desktop caller retires before lease release");
    auto replacement=listener.reserve_local_node(1);check(replacement.response.code==app::LocalControlResponseCode::ok&&replacement.reservation.generation>reserved.reservation.generation,"replacement reservation advances generation");
    check(!listener.cancel_local_node(reserved.reservation)&&listener.status().pool.slots[0].occupied,"stale cancel cannot release replacement");
    check(!listener.wait_for_retirement(replacement.reservation,1),"reservation is not reported retired while held");
    check(listener.cancel_local_node(replacement.reservation)&&listener.wait_for_retirement(replacement.reservation,10),"exact reservation cancel releases owned endpoint and lease");
    const auto expiring=listener.reserve_local_node(1);clock.advance+=30001;eventually([&]{return !listener.status().pool.slots[0].occupied;},"unclaimed reservation times out at thirty seconds");check(!listener.cancel_local_node(expiring.reservation),"expired reservation cannot cancel later work");
    auto disabled=listener.reserve_local_node(1);check(listener.set_enabled(1,false)&&!listener.status().pool.slots[0].occupied,"disabling node retires reservation");check(!listener.open_browser_node(1).client,"disabled exact node refuses browser logon");check(listener.set_enabled(1,true),"node reenabled");
    auto draining=listener.reserve_local_node(1);check(draining.response.code==app::LocalControlResponseCode::ok&&listener.set_draining(true)&&!listener.status().pool.slots[0].occupied,"draining retires unused reservation");check(!listener.open_browser_node(3).client,"draining refuses browser logon");echo(public_caller.get(),'e');echo(browser.client.get(),'f');check(listener.set_draining(false),"admission resumes without replacing callers");
    check(listener.set_enabled(4,false)&&listener.wait_for_retirement(browser.identity,3000),"disable retires exact browser worker");
    auto pending=listener.reserve_local_node(3);check(pending.response.code==app::LocalControlResponseCode::ok,"reserve pending stop case");listener.stop();check(listener.status().pool.active==0&&listener.status().diagnostic=="stopped","stop joins callers and releases every pending reservation");
    check(!listener.open_browser_node(1).client,"retired listener cannot reopen browser node");
    auto limited_config=config();limited_config.maximum_connections_per_source=1;epoch.bytes[0]=5;rt::LinuxCallerListener limited(epoch,limited_config,ports(clock));check(limited.start(),"source-limit listener starts");
    const auto limited_reservation=limited.reserve_local_node(2);check(limited_reservation.response.code==app::LocalControlResponseCode::ok&&!limited.open_browser_node(4).client,"local reservation and browser share ordinary source limits");limited.stop();
    epoch.bytes[0]=6;rt::LinuxCallerListener failed(epoch,config(),ports(clock,false));check(failed.start(),"cleanup-failure fixture starts");auto dirty=failed.open_browser_node(3);check(bool(dirty.client),"cleanup-failure exact caller connects");echo(dirty.client.get(),'g');dirty.client=FD{};
    eventually([&]{const auto state=failed.status();return state.diagnostic=="caller_cleanup_failed"&&state.pool.slots[2].occupied&&!state.callers.empty()&&!state.callers[0].available;},"failed session cleanup retains unavailable exact-node ownership");check(!failed.open_browser_node(1).client&&failed.reserve_local_node(2).response.code!=app::LocalControlResponseCode::ok,"cleanup failure refuses all further local admission");failed.stop();
    for(const bool browser_mode:{false,true}){
        epoch.bytes[0]=browser_mode?8:7;std::atomic<bool> entered{},escaped{},returned{},stopped{};auto callbacks=ports(clock);
        callbacks.admit=[&](auto,bool,const auto& cancel){entered=true;while(!cancel.load()&&!escaped.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));return rt::LinuxConnectionAdmission{true,false};};
        rt::LinuxCallerListener blocked(epoch,config(),std::move(callbacks));check(blocked.start(),"blocked-admission fixture starts");
        std::thread request([&]{if(browser_mode)(void)blocked.open_browser_node(1);else (void)blocked.reserve_local_node(1);returned=true;});
        for(unsigned i=0;i<300&&!entered;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::thread stopping([&]{blocked.stop();stopped=true;});
        for(unsigned i=0;i<1000&&!stopped;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool cancelled=entered&&stopped&&returned;escaped=true;request.join();stopping.join();
        check(cancelled&&blocked.status().pool.active==0,"stop cancels blocked local admission and prevents post-stop allocation");
    }
    std::cout<<"PASS "<<checks<<" Linux exact-node listener checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<" "<<error.what()<<'\n';return 1;}}
