// Actual production reader. Only ephemeral loopback endpoints/CA overrides;
// neither application settings nor machine trust is changed.
#include "clamav_https.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <array>
namespace setup=sfx::apps::setup;namespace ins=sfx::platform::win32::installation;
int main(int argc,char** argv)try{
    ins::require(argc==7&&!ins::elevated(),"bounded ordinary native HTTPS qualification");
    const std::string url=argv[1],ca_path=argv[2],expected=argv[3];
    ins::require(url.rfind("https://127.0.0.1:26060/",0)==0||url=="http://127.0.0.1:26060/stream","closed local HTTPS test endpoint");
    const auto wanted=std::stoull(argv[4]);const auto cancel=std::stoull(argv[5]);const auto timeout=std::stol(argv[6]);
    ins::require(wanted<=setup::kClamAvDownloadBytes&&cancel<=10000&&timeout>0&&timeout<=600000,"bounded native qualification budgets");
    std::string ca;const std::string* selected{};
    if(ca_path!="embedded"){std::ifstream input(ca_path,std::ios::binary);ins::require(bool(input),"fixture CA readable");ca.assign(std::istreambuf_iterator<char>(input),{});ins::require(ca.size()>100&&ca.size()<16384,"bounded ephemeral CA");selected=&ca;}
    const auto start=GetTickCount64();std::uint64_t total{};unsigned pumps{},reads{};bool refused{};
    try{
        auto reader=setup::clamav_https::open([&]{++pumps;return !cancel||GetTickCount64()-start<cancel;},url.c_str(),selected,timeout);
        std::array<std::uint8_t,65536> bytes{};
        for(;;){const auto n=reader->read(bytes.data(),(reads++%3)?bytes.size():17);if(!n)break;
            ins::require(n<=bytes.size()&&total+n<=setup::kClamAvDownloadBytes,"bounded native reader output");
            for(std::size_t i=0;i<n;++i)ins::require(bytes[i]==static_cast<std::uint8_t>((total+i)%251),"exact unmodified TLS stream");total+=n;
        }
        ins::require(expected=="success"&&total==wanted,"expected exact HTTPS success");
    }catch(const std::exception& error){
        const std::string text=error.what();std::cout<<"OBSERVED "<<text<<std::endl;
        ins::require(expected!="success"&&text.find(expected)!=std::string::npos,"expected fail-closed HTTPS error");refused=true;
    }
    ins::require(!refused||total==wanted,"refused HTTPS exact exposed-byte count");
    ins::require(pumps>0,"UI pump exercised during connection/stream");
    std::cout<<"PASS HTTPS bytes="<<total<<" reads="<<reads<<" pumps="<<pumps<<" elapsed_ms="<<GetTickCount64()-start<<" buffer=65536"<<std::endl;return 0;
}catch(const std::exception& error){std::cerr<<"FAIL HTTPS "<<error.what()<<std::endl;return 1;}
