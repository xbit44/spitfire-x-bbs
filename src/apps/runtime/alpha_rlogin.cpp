#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include "alpha_rlogin.hpp"
#include "alpha_rlogin_resolver.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

namespace sfx::apps::runtime {
namespace {
namespace app=application;
using Outcome=app::InteractiveStreamOutcome;
std::atomic<unsigned> sockets{};
struct Socket {
    SOCKET value{INVALID_SOCKET};
    ~Socket(){reset();}
    void reset(){if(value!=INVALID_SOCKET){shutdown(value,SD_BOTH);closesocket(value);value=INVALID_SOCKET;--sockets;}}
    void set(SOCKET s){reset();value=s;if(s!=INVALID_SOCKET)++sockets;}
};
struct Address {sockaddr_storage bytes{};int length{};};
std::vector<Address> resolve(std::string_view host,std::uint16_t port,const std::function<bool()>& running){
    if(!running())return {};
    std::vector<Address> result;Address numeric;const std::string name(host);
    auto* v4=reinterpret_cast<sockaddr_in*>(&numeric.bytes);
    if(InetPtonA(AF_INET,name.c_str(),&v4->sin_addr)==1){v4->sin_family=AF_INET;v4->sin_port=htons(port);numeric.length=sizeof(*v4);return {numeric};}
    auto* v6=reinterpret_cast<sockaddr_in6*>(&numeric.bytes);
    if(InetPtonA(AF_INET6,name.c_str(),&v6->sin6_addr)==1){v6->sin6_family=AF_INET6;v6->sin6_port=htons(port);numeric.length=sizeof(*v6);return {numeric};}
    for(const auto& ip:resolve_alpha_rlogin_ipv4(host,running)){
        Address a;auto* destination=reinterpret_cast<sockaddr_in*>(&a.bytes);destination->sin_family=AF_INET;destination->sin_port=htons(port);std::memcpy(&destination->sin_addr,ip.data(),ip.size());a.length=sizeof(*destination);result.push_back(a);
    }
    return result;
}
std::optional<std::array<std::uint8_t,20>> sha1(std::string_view input){
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    struct Cleanup {BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Cleanup(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}} cleanup{algorithm,hash};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA1_ALGORITHM,nullptr,0)<0)return {};
    DWORD object_bytes{},returned{};
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&object_bytes),sizeof(object_bytes),&returned,0)<0 || object_bytes>65536)return {};
    std::vector<std::uint8_t> object(object_bytes);
    struct Wipe{std::vector<std::uint8_t>& b;BCRYPT_HASH_HANDLE& h;~Wipe(){if(h){BCryptDestroyHash(h);h=nullptr;}if(!b.empty())app::erase_rlogin_secret(b.data(),b.size());}} wipe{object,hash};
    std::array<std::uint8_t,20> digest{};
    if(BCryptCreateHash(algorithm,&hash,object.data(),object_bytes,nullptr,0,0)<0 ||
        BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),static_cast<ULONG>(input.size()),0)<0 ||
        BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0)return {};
    // Destroy before erasing its backing memory.
    BCryptDestroyHash(hash);hash=nullptr;return digest;
}
}
unsigned alpha_rlogin_live_sockets() noexcept{return sockets.load();}
unsigned alpha_rlogin_resolver_unavailable_slots() noexcept{return alpha_rlogin_native_uncertain_requests();}
app::RloginBridgeResult run_alpha_rlogin(const app::RloginTarget& target,
    const app::RloginCallerIdentity& caller,app::RloginSecret credential,app::RloginSecret pepper,
    app::RloginBridgePorts ports,std::vector<std::uint8_t> prefix,bool discard_lf,std::function<void(bool)> gateway_sound){
    app::RloginBridgeResult result;result.end=app::RloginEnd::connection_failed;
    if(!app::valid_rlogin_target(target)||!ports.stop||!ports.now||!ports.caller_read||!ports.caller_write||prefix.size()>4096)return result;
    auto frame=app::rlogin_handshake(target,caller,credential.view(),pepper.view(),sha1);
    credential.clear();pepper.clear();
    if(!frame){result.end=app::RloginEnd::handshake_failed;return result;}
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return result;
    struct Winsock {~Winsock(){WSACleanup();}} winsock;
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
        if(stopped)return false;if(const auto reason=initial_stop()){stopped=reason;return false;}
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
    const auto addresses=resolve(target.host,target.port,running);Socket remote;
    for(const auto& address:addresses){
        if(!running())break;remote.set(socket(address.bytes.ss_family,SOCK_STREAM,IPPROTO_TCP));if(remote.value==INVALID_SOCKET)continue;
        u_long nonblocking=1;const int buffer=65536,immediate=1;
        if(ioctlsocket(remote.value,FIONBIO,&nonblocking) || setsockopt(remote.value,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer)) ||
            setsockopt(remote.value,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer)) ||
            setsockopt(remote.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&immediate),sizeof(immediate))){remote.reset();continue;}
        const auto connected=connect(remote.value,reinterpret_cast<const sockaddr*>(&address.bytes),address.length);
        if(connected==0)break;if(WSAGetLastError()!=WSAEWOULDBLOCK){remote.reset();continue;}
        bool ready{};
        while(running()){
            fd_set write,error;FD_ZERO(&write);FD_ZERO(&error);FD_SET(remote.value,&write);FD_SET(remote.value,&error);timeval wait{0,20000};
            const auto selected=select(0,nullptr,&write,&error,&wait);if(selected<0)break;
            if(selected){int code{},length=sizeof(code);ready=getsockopt(remote.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&code),&length)==0 && !code;break;}
        }
        if(ready)break;remote.reset();
    }
    if(remote.value==INVALID_SOCKET){if(stopped)result.end=*stopped;return result;}
    const auto write=[&](std::string_view bytes){
        const auto n=send(remote.value,bytes.data(),static_cast<int>(std::min<std::size_t>(bytes.size(),4096)),0);
        if(n>0)return app::InteractiveStreamWriteResult{Outcome::completed,static_cast<std::size_t>(n)};
        return app::InteractiveStreamWriteResult{n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK?Outcome::timed_out:Outcome::disconnected,0};
    };
    const auto read=[&](std::size_t maximum){
        std::array<std::uint8_t,4096> bytes{};const auto n=recv(remote.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(std::min(maximum,bytes.size())),0);
        if(n>0)return app::InteractiveStreamReadResult{Outcome::completed,{bytes.begin(),bytes.begin()+n}};
        return app::InteractiveStreamReadResult{n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK?Outcome::timed_out:Outcome::disconnected,{}};
    };
    std::size_t sent{};
    while(sent<frame->view().size() && running()){
        const auto w=write(frame->view().substr(sent));if(w.outcome==Outcome::disconnected)break;sent+=w.bytes_written;if(!w.bytes_written)Sleep(10);
    }
    const bool frame_sent=sent==frame->view().size();frame->clear();bool acknowledged{};
    while(frame_sent && running()){
        const auto r=read(1);if(r.outcome==Outcome::completed){acknowledged=r.bytes.front()==0;break;}
        if(r.outcome!=Outcome::timed_out)break;Sleep(10);
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
    ports.wait=[] {Sleep(10);};
    return app::run_rlogin_bridge(ports,{},discard_lf);
}

app::RloginBridgeResult run_alpha_telnet(const app::TelnetTarget& target,
    app::RloginBridgePorts ports,const app::TelgateLocalPorts& local,
    std::vector<std::uint8_t> prefix,bool discard_lf){
    app::RloginBridgeResult result;result.end=app::RloginEnd::connection_failed;
    if(!app::valid_telnet_target(target)||!ports.stop||!ports.now||!ports.caller_read||!ports.caller_write||prefix.size()>4096)return result;
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))return result;
    struct Winsock {~Winsock(){WSACleanup();}} winsock;
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
        if(stopped)return false;if(const auto why=ports.stop()){stopped=why;return false;}
        if(ports.now()>=deadline)return false;
        const auto maximum=std::min<std::size_t>(1024,app::kRloginBridgeBufferBytes-waiting.size());
        if(maximum){const auto r=ports.caller_read(maximum);
            if(!app::valid_interactive_stream_read_result(maximum,r)){stopped=app::RloginEnd::transport_failed;return false;}
            if(r.outcome==Outcome::completed){if(!r.bytes.empty()&&ports.keyboard)ports.keyboard();input(r.bytes);}
            else if(r.outcome!=Outcome::timed_out){stopped=app::RloginEnd::caller_disconnected;return false;}
        }
        return !stopped;
    };
    const auto addresses=resolve(target.host,target.port,running);Socket remote;
    for(const auto& address:addresses){
        if(!running())break;remote.set(socket(address.bytes.ss_family,SOCK_STREAM,IPPROTO_TCP));if(remote.value==INVALID_SOCKET)continue;
        u_long nonblocking=1;const int buffer=65536,immediate=1;
        if(ioctlsocket(remote.value,FIONBIO,&nonblocking)||setsockopt(remote.value,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer))||
            setsockopt(remote.value,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer))||
            setsockopt(remote.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&immediate),sizeof(immediate))){remote.reset();continue;}
        if(connect(remote.value,reinterpret_cast<const sockaddr*>(&address.bytes),address.length)==0)break;
        if(WSAGetLastError()!=WSAEWOULDBLOCK){remote.reset();continue;}
        bool ready{};
        while(running()){
            fd_set writes,errors;FD_ZERO(&writes);FD_ZERO(&errors);FD_SET(remote.value,&writes);FD_SET(remote.value,&errors);timeval timeout{0,20000};
            const auto selected=select(0,nullptr,&writes,&errors,&timeout);if(selected<0)break;
            if(selected){int code{},length=sizeof(code);ready=getsockopt(remote.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&code),&length)==0&&!code;break;}
        }
        if(ready)break;remote.reset();
    }
    if(stopped){result.end=*stopped;return result;}if(remote.value==INVALID_SOCKET)return result;
    ports.remote_write=[&](std::string_view bytes){const auto n=send(remote.value,bytes.data(),static_cast<int>(std::min<std::size_t>(4096,bytes.size())),0);
        return app::InteractiveStreamWriteResult{n>0?Outcome::completed:n==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK?Outcome::timed_out:Outcome::disconnected,n>0?static_cast<std::size_t>(n):0};};
    ports.remote_read=[&](std::size_t maximum){std::array<std::uint8_t,4096> bytes{};const auto n=recv(remote.value,reinterpret_cast<char*>(bytes.data()),static_cast<int>(std::min(bytes.size(),maximum)),0);
        if(n>0)return app::InteractiveStreamReadResult{Outcome::completed,{bytes.begin(),bytes.begin()+n}};
        return app::InteractiveStreamReadResult{n==SOCKET_ERROR&&WSAGetLastError()==WSAEWOULDBLOCK?Outcome::timed_out:Outcome::disconnected,{}};};
    ports.wait=[] {Sleep(10);};
    return app::run_telgate_bridge(ports,target,local,std::move(waiting),false);
}
} // namespace sfx::apps::runtime
