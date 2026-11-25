#include "alpha_ssh_stream.hpp"
#include "sfx/platform/network.hpp"
#include <iostream>
#include <stdexcept>

namespace net=sfx::platform::network;
namespace runtime=sfx::apps::runtime;
namespace {
unsigned checks{};
void check(bool value,const char* why) { ++checks;if(!value)throw std::runtime_error(why); }
struct Socket {
    int value;
    explicit Socket(int fd):value(fd){}
    ~Socket(){if(value>=0)close(value);}
    Socket(const Socket&)=delete;
    Socket& operator=(const Socket&)=delete;
};
}
int main(){try{
    Socket listener(socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP));
    check(listener.value>=0,"create TCP listener");
    check(runtime::AlphaSshStream::prepare_listener_socket(static_cast<std::uintptr_t>(listener.value)),"actual SSH listener accepts Linux receive-buffer accounting");
    int actual{};socklen_t length=sizeof(actual);
    check(getsockopt(listener.value,SOL_SOCKET,SO_RCVBUF,&actual,&length)==0&&actual>0&&actual<=65536,"kernel receive allocation bounded before handshake");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(bind(listener.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(listener.value,1)==0,"listen isolated loopback ephemeral port");
    socklen_t size=sizeof(address);check(getsockname(listener.value,reinterpret_cast<sockaddr*>(&address),&size)==0,"read actual bound port");
    Socket client(socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP));
    check(client.value>=0&&connect(client.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"connect actual peer");
    Socket peer(accept4(listener.value,nullptr,nullptr,SOCK_CLOEXEC));check(peer.value>=0,"accept actual peer");
    check(net::set_timeout(peer.value,SO_RCVTIMEO,80),"Linux timeval receive timeout");
    const auto started=net::monotonic_millis();char byte{};
    check(net::receive_bytes(peer.value,&byte,1)==-1&&net::retry_receive_error(),"idle receive reports retry");
    const auto elapsed=net::monotonic_millis()-started;
    check(elapsed>=40&&elapsed<2000,"actual receive deadline is bounded");
    check(net::send_bytes(client.value,"x",1)==1&&net::receive_bytes(peer.value,&byte,1)==1&&byte=='x',"bidirectional socket transfer");
    check(shutdown(client.value,SHUT_RDWR)==0,"close caller direction");
    check(net::receive_bytes(peer.value,&byte,1)==0,"caller EOF is not a timeout");
    check(shutdown(peer.value,SHUT_RDWR)==0,"shutdown server socket");
    check(net::send_bytes(peer.value,"x",1)==-1,"closed socket write returns without SIGPIPE");
    check(!runtime::AlphaSshStream::prepare_listener_socket(static_cast<std::uintptr_t>(-1)),"invalid listener refused");
    std::cout<<"PASS "<<checks<<" actual Linux socket/SSH listener-boundary checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
