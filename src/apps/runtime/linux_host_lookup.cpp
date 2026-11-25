#include "sfx/application/botgate.hpp"
#include "sfx/application/hostname_lookup.hpp"
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <iostream>
#include <memory>
int main(int argc,char** argv){
    namespace app=sfx::application;
    if(geteuid()==0)return 64;
    if(argc==3&&std::string_view(argv[1])=="--forward"){
        const auto normalized=app::normalize_hostname_hint(argv[2]);if(!normalized)return 64;
        const std::string hostname(normalized->bytes.data(),normalized->size);
        addrinfo hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;addrinfo* resolved{};
        if(getaddrinfo(hostname.c_str(),nullptr,&hints,&resolved)!=0)return 2;
        std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> owned(resolved,freeaddrinfo);unsigned count{};
        for(const auto* candidate=resolved;candidate&&count<16;candidate=candidate->ai_next){
            if(candidate->ai_family!=AF_INET||candidate->ai_addrlen<sizeof(sockaddr_in))continue;
            std::array<char,INET_ADDRSTRLEN> address{};
            if(!inet_ntop(AF_INET,&reinterpret_cast<const sockaddr_in*>(candidate->ai_addr)->sin_addr,address.data(),address.size()))return 2;
            std::cout<<address.data()<<'\n';++count;
        }
        return std::cout?0:2;
    }
    const bool ptr_only=argc==3&&std::string_view(argv[1])=="--ptr";
    if(argc!=2&&!ptr_only)return 64;
    const auto ip=app::parse_botgate_ipv4(argv[ptr_only?2:1]);if(!ip)return 64;
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(*ip);
    std::array<char,NI_MAXHOST> name{};
    const auto reverse=getnameinfo(reinterpret_cast<const sockaddr*>(&address),sizeof(address),name.data(),name.size(),nullptr,0,NI_NAMEREQD);
    if(reverse)return reverse==EAI_NONAME?0:2;
    const auto normalized=app::normalize_hostname_hint(name.data());if(!normalized)return 0;
    const std::string hostname(normalized->bytes.data(),normalized->size);
    if(ptr_only){std::cout<<hostname;return std::cout?0:2;}
    addrinfo hints{};hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;addrinfo* resolved{};
    if(getaddrinfo(hostname.c_str(),nullptr,&hints,&resolved)!=0)return 2;
    std::unique_ptr<addrinfo,decltype(&freeaddrinfo)> owned(resolved,freeaddrinfo);
    for(const auto* candidate=resolved;candidate;candidate=candidate->ai_next){
        if(candidate->ai_family==AF_INET&&candidate->ai_addrlen>=sizeof(sockaddr_in)&&
            reinterpret_cast<const sockaddr_in*>(candidate->ai_addr)->sin_addr.s_addr==address.sin_addr.s_addr){
            std::cout<<hostname;return std::cout?0:2;
        }
    }
    return 0;
}
