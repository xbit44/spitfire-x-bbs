#include "linux_rlogin.hpp"
#include "sfx/platform/network.hpp"
#include <openssl/evp.h>
#include <thread>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace sfx::apps::runtime {
namespace {
namespace app=application;
namespace net=platform::network;
void sleep_tick(){std::this_thread::sleep_for(std::chrono::milliseconds(10));}
bool retry_io(){return errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR;}
using Outcome=app::InteractiveStreamOutcome;
std::atomic<unsigned> sockets{};
struct Socket {
    int value{-1};
    ~Socket(){reset();}
    void reset(){if(value!=-1){shutdown(value,SHUT_RDWR);close(value);value=-1;--sockets;}}
    void set(int s){reset();value=s;if(s!=-1)++sockets;}
};
struct Address {sockaddr_storage bytes{};socklen_t length{};};
std::vector<Address> resolve(const std::filesystem::path& root,std::string_view host,std::uint16_t port,const std::function<bool()>& running){
    if(!running())return {};
    std::vector<Address> result;Address numeric;const std::string name(host);
    auto* v4=reinterpret_cast<sockaddr_in*>(&numeric.bytes);
    if(inet_pton(AF_INET,name.c_str(),&v4->sin_addr)==1){v4->sin_family=AF_INET;v4->sin_port=htons(port);numeric.length=sizeof(*v4);return {numeric};}
    auto* v6=reinterpret_cast<sockaddr_in6*>(&numeric.bytes);
    if(inet_pton(AF_INET6,name.c_str(),&v6->sin6_addr)==1){v6->sin6_family=AF_INET6;v6->sin6_port=htons(port);numeric.length=sizeof(*v6);return {numeric};}
    for(const auto& ip:resolve_linux_gateway_ipv4(root,host,running)){
        Address a;auto* destination=reinterpret_cast<sockaddr_in*>(&a.bytes);destination->sin_family=AF_INET;destination->sin_port=htons(port);std::memcpy(&destination->sin_addr,ip.data(),ip.size());a.length=sizeof(*destination);result.push_back(a);
    }
    return result;
}
std::optional<std::array<std::uint8_t,20>> sha1(std::string_view input){
    std::array<std::uint8_t,20> digest{};std::size_t bytes{};
    if(EVP_Q_digest(nullptr,"SHA1",nullptr,input.data(),input.size(),digest.data(),&bytes)!=1||bytes!=digest.size())return {};
    return digest;
}
}
unsigned linux_gateway_live_sockets() noexcept{return sockets.load();}
app::RloginBridgeResult run_linux_rlogin(const std::filesystem::path& root,const app::RloginTarget& target,
    const app::RloginCallerIdentity& caller,app::RloginSecret credential,app::RloginSecret pepper,
    app::RloginBridgePorts ports,std::vector<std::uint8_t> prefix,bool discard_lf,std::function<void(bool)> gateway_sound){
    app::RloginBridgeResult result;result.end=app::RloginEnd::connection_failed;
    if(!app::valid_rlogin_target(target)||!ports.stop||!ports.now||!ports.caller_read||!ports.caller_write||prefix.size()>4096)return result;
    auto frame=app::rlogin_handshake(target,caller,credential.view(),pepper.view(),sha1);
    credential.clear();pepper.clear();
    if(!frame){result.end=app::RloginEnd::handshake_failed;return result;}
    const auto deadline=ports.now()+static_cast<std::uint64_t>(target.connection_timeout_seconds)*1000;
    const auto initial_stop=ports.stop;const auto initial_read=ports.caller_read;const auto keyboard=ports.keyboard;
    std::vector<std::uint8_t> waiting;waiting.reserve(app::kRloginBridgeBufferBytes);std::size_t waiting_offset{};
    bool escape{};std::uint64_t escape_time{};std::optional<app::RloginEnd> stopped;
    const auto literal_escape=[&]{waiting.push_back(29);waiting.push_back(29);};
    const auto expire_escape=[&]{if(escape && ports.now()-escape_time>=500){literal_escape();escape=false;}};
    const auto input=[&](const std::vector<std::uint8_t>& bytes){
        expire_escape();
        for(const auto c:bytes){
            if(discard_lf){discard_lf=false;if(c==10)continue;}
            if(escape){escape=false;if(c=='d'||c=='D'){stopped=app::RloginEnd::gateway_escape;return;}
                literal_escape();if(c==29)continue;
            }else if(c==29){escape=true;escape_time=ports.now();continue;}
            waiting.push_back(c);
        }
    };
    input(prefix);prefix.clear();
    const auto running=[&]{
        if(stopped)return false;
        if(const auto reason=initial_stop()){stopped=reason;return false;}
        if(ports.now()>=deadline)return false;
        expire_escape();
        if(app::kRloginBridgeBufferBytes-waiting.size()>3){
            const auto maximum=std::min<std::size_t>(4096,(app::kRloginBridgeBufferBytes-waiting.size()-2)/2);
            const auto r=initial_read(maximum);
            if(!app::valid_interactive_stream_read_result(maximum,r)){stopped=app::RloginEnd::transport_failed;return false;}
            if(r.outcome==Outcome::completed){
                if(keyboard)keyboard();
                input(r.bytes);if(stopped)return false;
            }else if(r.outcome!=Outcome::timed_out){stopped=app::RloginEnd::caller_disconnected;return false;}
        }
        return true;
    };
    const auto addresses=resolve(root,target.host,target.port,running);Socket remote;
    for(const auto& address:addresses){
        if(!running())break;
        remote.set(socket(address.bytes.ss_family,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,IPPROTO_TCP));if(remote.value==-1)continue;
        const int buffer=65536,immediate=1;
        if(setsockopt(remote.value,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer)) ||
            setsockopt(remote.value,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer)) ||
            setsockopt(remote.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&immediate),sizeof(immediate))){remote.reset();continue;}
        const auto connected=connect(remote.value,reinterpret_cast<const sockaddr*>(&address.bytes),address.length);
        if(connected==0)break;
        if(errno!=EINPROGRESS&&errno!=EINTR){remote.reset();continue;}
        bool ready{};
        while(running()){
            pollfd request{remote.value,POLLOUT,0};const auto selected=poll(&request,1,20);
            if(selected<0){if(errno==EINTR)continue;break;}
            if(selected){int code{};socklen_t length=sizeof(code);ready=getsockopt(remote.value,SOL_SOCKET,SO_ERROR,&code,&length)==0&&!code;break;}
        }
        if(ready)break;
        remote.reset();
    }
    if(remote.value==-1){if(stopped)result.end=*stopped;return result;}
    const auto write=[&](std::string_view bytes){
        const auto n=net::send_bytes(remote.value,bytes.data(),static_cast<int>(std::min<std::size_t>(bytes.size(),4096)));
        if(n>0)return app::InteractiveStreamWriteResult{Outcome::completed,static_cast<std::size_t>(n)};
        return app::InteractiveStreamWriteResult{n<0 && retry_io()?Outcome::timed_out:Outcome::disconnected,0};
    };
    const auto read=[&](std::size_t maximum){
        std::array<std::uint8_t,4096> bytes{};const auto n=net::receive_bytes(remote.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(std::min(maximum,bytes.size())));
        if(n>0)return app::InteractiveStreamReadResult{Outcome::completed,{bytes.begin(),bytes.begin()+n}};
        return app::InteractiveStreamReadResult{n<0 && retry_io()?Outcome::timed_out:Outcome::disconnected,{}};
    };
    std::size_t sent{};
    while(sent<frame->view().size() && running()){
        const auto w=write(frame->view().substr(sent));if(w.outcome==Outcome::disconnected)break;sent+=w.bytes_written;if(!w.bytes_written)sleep_tick();
    }
    const bool frame_sent=sent==frame->view().size();frame->clear();bool acknowledged{};
    while(frame_sent && running()){
        const auto r=read(1);if(r.outcome==Outcome::completed){acknowledged=r.bytes.front()==0;break;}
        if(r.outcome!=Outcome::timed_out)break;
        sleep_tick();
    }
    if(!acknowledged){result.end=stopped.value_or(app::RloginEnd::handshake_failed);return result;}
    const auto notify=[&](bool entry)noexcept{try{if(gateway_sound)gateway_sound(entry);}catch(...){}};
    notify(true);
    struct GatewayExit { const decltype(notify)& callback; ~GatewayExit(){callback(false);} } exit_sound{notify};
    expire_escape();if(escape)waiting.push_back(29);
    bool cached_read{};ports.caller_read=[&](std::size_t maximum){
        cached_read=waiting_offset<waiting.size();if(!cached_read)return initial_read(maximum);
        const auto n=std::min(maximum,waiting.size()-waiting_offset);std::vector<std::uint8_t> b(waiting.begin()+static_cast<std::ptrdiff_t>(waiting_offset),waiting.begin()+static_cast<std::ptrdiff_t>(waiting_offset+n));waiting_offset+=n;return app::InteractiveStreamReadResult{Outcome::completed,std::move(b)};
    };
    ports.keyboard=[&]{if(!cached_read && keyboard)keyboard();};ports.remote_read=read;ports.remote_write=write;
    ports.urgent=[&]()->std::optional<std::uint8_t>{unsigned char byte{};const auto n=recv(remote.value,reinterpret_cast<char*>(&byte),1,MSG_OOB);return n==1?std::optional<std::uint8_t>(byte):std::nullopt;};
    ports.wait=[] {sleep_tick();};
    return app::run_rlogin_bridge(ports,{},discard_lf);
}

app::RloginBridgeResult run_linux_telnet(const std::filesystem::path& root,const app::TelnetTarget& target,
    app::RloginBridgePorts ports,const app::TelgateLocalPorts& local,
    std::vector<std::uint8_t> prefix,bool discard_lf){
    app::RloginBridgeResult result;result.end=app::RloginEnd::connection_failed;
    if(!app::valid_telnet_target(target)||!ports.stop||!ports.now||!ports.caller_read||!ports.caller_write||prefix.size()>4096)return result;
    const auto deadline=ports.now()+std::uint64_t(target.connection_timeout_seconds)*1000;
    std::optional<app::RloginEnd> stopped;std::vector<std::uint8_t> waiting;bool escape{};
    const auto input=[&](const std::vector<std::uint8_t>& bytes){
        for(const auto c:bytes){
            if(discard_lf){discard_lf=false;if(c==10)continue;}
            if(escape&&(c=='D'||c=='d')){stopped=app::RloginEnd::gateway_escape;return;}
            escape=!escape&&c==24;waiting.push_back(c);
        }
    };
    input(prefix);
    const auto running=[&]{
        if(stopped)return false;
        if(const auto why=ports.stop()){stopped=why;return false;}
        if(ports.now()>=deadline)return false;
        const auto maximum=std::min<std::size_t>(1024,app::kRloginBridgeBufferBytes-waiting.size());
        if(maximum){const auto r=ports.caller_read(maximum);
            if(!app::valid_interactive_stream_read_result(maximum,r)){stopped=app::RloginEnd::transport_failed;return false;}
            if(r.outcome==Outcome::completed){if(!r.bytes.empty()&&ports.keyboard)ports.keyboard();input(r.bytes);}
            else if(r.outcome!=Outcome::timed_out){stopped=app::RloginEnd::caller_disconnected;return false;}
        }
        return !stopped;
    };
    const auto addresses=resolve(root,target.host,target.port,running);Socket remote;
    for(const auto& address:addresses){
        if(!running())break;
        remote.set(socket(address.bytes.ss_family,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,IPPROTO_TCP));if(remote.value==-1)continue;
        const int buffer=65536,immediate=1;
        if(setsockopt(remote.value,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer))||
            setsockopt(remote.value,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer))||
            setsockopt(remote.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&immediate),sizeof(immediate))){remote.reset();continue;}
        if(connect(remote.value,reinterpret_cast<const sockaddr*>(&address.bytes),address.length)==0)break;
        if(errno!=EINPROGRESS&&errno!=EINTR){remote.reset();continue;}
        bool ready{};
        while(running()){
            pollfd request{remote.value,POLLOUT,0};const auto selected=poll(&request,1,20);
            if(selected<0){if(errno==EINTR)continue;break;}
            if(selected){int code{};socklen_t length=sizeof(code);ready=getsockopt(remote.value,SOL_SOCKET,SO_ERROR,&code,&length)==0&&!code;break;}
        }
        if(ready)break;
        remote.reset();
    }
    if(stopped){result.end=*stopped;return result;}if(remote.value==-1)return result;
    ports.remote_write=[&](std::string_view bytes){const auto n=net::send_bytes(remote.value,bytes.data(),static_cast<int>(std::min<std::size_t>(4096,bytes.size())));
        return app::InteractiveStreamWriteResult{n>0?Outcome::completed:n<0&&retry_io()?Outcome::timed_out:Outcome::disconnected,n>0?static_cast<std::size_t>(n):0};};
    ports.remote_read=[&](std::size_t maximum){std::array<std::uint8_t,4096> bytes{};const auto n=net::receive_bytes(remote.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(std::min(bytes.size(),maximum)));
        if(n>0)return app::InteractiveStreamReadResult{Outcome::completed,{bytes.begin(),bytes.begin()+n}};
        return app::InteractiveStreamReadResult{n<0&&retry_io()?Outcome::timed_out:Outcome::disconnected,{}};};
    ports.wait=[] {sleep_tick();};
    return app::run_telgate_bridge(ports,target,local,std::move(waiting),false);
}
} // namespace sfx::apps::runtime
