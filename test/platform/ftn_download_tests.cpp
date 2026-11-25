#include "sfx/platform/win32/ftn_echo_list_download.hpp"
#include "sfx/application/ftn.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <thread>
namespace win=sfx::platform::win32;
unsigned checks{};void check(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
int main(int argc,char** argv){try{
    check(argc==4,"isolated HTTP, HTTPS and fixture CA required");const std::string http=argv[1],https=argv[2];std::ifstream input(argv[3],std::ios::binary);const std::string trust{std::istreambuf_iterator<char>(input),{}};check(!trust.empty(),"synthetic CA read");std::atomic<bool> cancel{};
    const auto h=win::download_ftn_echo_list(http+"/list.na",cancel);check(h.code==win::FtnEchoListDownloadCode::ok&&sfx::application::parse_ftn_echo_list(h.bytes).entries.size()==2,"actual explicit HTTP list");
    const auto s=win::download_ftn_echo_list(https+"/list.na",cancel,&trust);check(s.code==win::FtnEchoListDownloadCode::ok&&s.bytes==h.bytes,"actual certificate-verified HTTPS list");
    check(win::download_ftn_echo_list(https+"/list.na",cancel).code==win::FtnEchoListDownloadCode::certificate,"untrusted HTTPS certificate refused without retry");
    check(win::download_ftn_echo_list(https+"/downgrade",cancel,&trust).code==win::FtnEchoListDownloadCode::redirect,"HTTPS to HTTP downgrade refused");
    check(win::download_ftn_echo_list(http+"/upgrade",cancel,&trust).code==win::FtnEchoListDownloadCode::ok,"explicit HTTP redirect to verified HTTPS accepted");
    check(win::download_ftn_echo_list(https+"/relative",cancel,&trust).code==win::FtnEchoListDownloadCode::ok,"verified relative HTTPS redirect");
    check(win::download_ftn_echo_list(http+"/cycle",cancel).code==win::FtnEchoListDownloadCode::redirect,"redirect cycle bounded");
    check(win::download_ftn_echo_list(http+"/missing",cancel).code==win::FtnEchoListDownloadCode::http_error,"HTTP failure is not an empty list");
    check(win::download_ftn_echo_list(http+"/large",cancel).code==win::FtnEchoListDownloadCode::too_large,"oversized list body refused");
    check(win::download_ftn_echo_list(http+"/broken",cancel).code!=win::FtnEchoListDownloadCode::ok,"partial body never published");
    cancel=true;check(win::download_ftn_echo_list(http+"/list.na",cancel).code==win::FtnEchoListDownloadCode::cancelled,"pre-cancel performs no download");cancel=false;
    const auto began=GetTickCount64();std::thread stop([&]{Sleep(150);cancel=true;});const auto interrupted=win::download_ftn_echo_list(http+"/slow",cancel);stop.join();check(interrupted.code==win::FtnEchoListDownloadCode::cancelled&&interrupted.bytes.empty()&&GetTickCount64()-began<4000,"in-flight cancellation returns without partial bytes");cancel=false;
    for(const auto* bad:{"file:///local.na","ftp://example.invalid/a.na","https://user:password@example.invalid/a.na"})check(win::download_ftn_echo_list(bad,cancel).code==win::FtnEchoListDownloadCode::invalid_url,"unsafe or credential-bearing source refused");
    std::cout<<"PASS "<<checks<<" actual loopback HTTP/HTTPS FTN downloader checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
