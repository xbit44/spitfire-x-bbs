#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
namespace fixture {
std::atomic<unsigned> mode{},sequence{};
std::atomic<unsigned> reverse_calls{},forward_calls{};
HANDLE answer(HWND window,UINT message,char* buffer,bool forward) {
    const auto task=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(++sequence));
    if (mode==3 || mode==4) return task; // no completion; confirmed/uncertain cancel
    std::memset(buffer,0,MAXGETHOSTSTRUCT);
    auto* entry=reinterpret_cast<hostent*>(buffer);
    entry->h_addrtype=AF_INET; entry->h_length=4;
    entry->h_name=buffer+64; constexpr char name[]="blocked.example"; std::memcpy(entry->h_name,name,sizeof(name));
    if (mode==5) entry->h_name=buffer+256;
    entry->h_addr_list=reinterpret_cast<char**>(buffer+128);
    entry->h_addr_list[0]=buffer+192; entry->h_addr_list[1]=nullptr;
    const unsigned char address[]={192,0,2,static_cast<unsigned char>(mode==1 && forward ? 2 : 1)};
    std::memcpy(buffer+192,address,4);
    PostMessageW(window,message,reinterpret_cast<WPARAM>(task),WSAMAKEASYNCREPLY(200,mode==2 ? WSAHOST_NOT_FOUND : 0));
    return task;
}
HANDLE reverse(HWND window,UINT message,const char*,int,int,char* buffer,int) { ++reverse_calls; return answer(window,message,buffer,false); }
HANDLE forward(HWND window,UINT message,const char*,char* buffer,int) { ++forward_calls; return answer(window,message,buffer,true); }
int cancel(HANDLE) { return mode==4 ? SOCKET_ERROR : 0; }
}
#define WSAAsyncGetHostByAddr fixture::reverse
#define WSAAsyncGetHostByName fixture::forward
#define WSACancelAsyncRequest fixture::cancel
#include "../../src/apps/runtime/alpha_verified_domain.cpp"
#undef WSAAsyncGetHostByAddr
#undef WSAAsyncGetHostByName
#undef WSACancelAsyncRequest
int main() {
    unsigned checks{};
    const auto require=[&](bool value,const char* label) { ++checks; if (!value) throw std::runtime_error(label); };
    try {
        std::atomic<bool> stop{false};
        for (unsigned n=0;n<6;++n) {
            fixture::mode=n%3;
            const auto result=sfx::apps::runtime::verify_peer_domain(0xc0000201,stop);
            require(result.completed,"native state machine completes");
            require(result.name==(n%3==0 ? "blocked.example" : ""),"PTR alone never becomes verified security identity");
        }
        require(fixture::forward_calls==4,"forward confirmation required for every successful PTR");
        fixture::mode=5;
        require(!sfx::apps::runtime::verify_peer_domain(0xc0000201,stop).completed,"pointers outside returned result are rejected even within allocated capacity");
        fixture::mode=3;
        auto began=GetTickCount64();
        require(!sfx::apps::runtime::verify_peer_domain(0xc0000201,stop).completed,"DNS timeout refuses unavailable verification");
        require(GetTickCount64()-began<2300,"total reverse/forward deadline bounded");
        Sleep(50); fixture::mode=0;
        require(sfx::apps::runtime::verify_peer_domain(0xc0000201,stop).completed,"confirmed cancellation permits later reuse");
        fixture::mode=4;
        for (unsigned n=0;n<4;++n) {
            require(!sfx::apps::runtime::verify_peer_domain(0xc0000201,stop).completed,"uncertain cancellation fails closed");
            Sleep(50);
        }
        fixture::mode=0; began=GetTickCount64();
        require(!sfx::apps::runtime::verify_peer_domain(0xc0000201,stop).completed,"quarantined buffers are never reused");
        require(GetTickCount64()-began<250,"exhausted fixed arena is bounded, not unbounded thread growth");
        std::cout<<"PASS Section4 verified-domain checks="<<checks<<'\n'; return 0;
    } catch (const std::exception& e) { std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n'; return 1; }
}
