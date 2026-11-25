#include "clamav_download.hpp"
#include "sfx/platform/win32/clamav_installation.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <cstring>
namespace setup=sfx::apps::setup;namespace ins=sfx::platform::win32::installation;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;ins::require(ok,why);std::cout<<"PASS "<<why<<std::endl;}
struct Reader:setup::ClamAvDownloadReader {
    std::optional<std::uint64_t> length;std::uint64_t left{};bool failure{};
    std::optional<std::uint64_t> content_length()const override{return length;}
    std::size_t read(std::uint8_t* out,std::size_t maximum)override{if(failure)throw std::runtime_error("synthetic network failure");const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(left,maximum));std::memset(out,0,n);left-=n;return n;}
};
struct FileReader:setup::ClamAvDownloadReader {
    std::ifstream input;explicit FileReader(const std::filesystem::path& path):input(path,std::ios::binary){ins::require(bool(input),"pinned qualification ZIP input");}
    std::optional<std::uint64_t> content_length()const override{return setup::kClamAvDownloadBytes;}
    std::size_t read(std::uint8_t* out,std::size_t maximum)override{input.read(reinterpret_cast<char*>(out),static_cast<std::streamsize>(maximum));ins::require(!input.bad(),"qualification input read");return static_cast<std::size_t>(input.gcount());}
};
std::set<std::filesystem::path> jobs(){
    std::array<wchar_t,32768> path{};const auto n=GetTempPathW(static_cast<DWORD>(path.size()),path.data());ins::require(n&&n<path.size(),"temporary path");std::set<std::filesystem::path> result;
    for(const auto& entry:std::filesystem::directory_iterator(path.data()))if(entry.path().filename().native().rfind(L"SFX-ClamAV-download-",0)==0)result.insert(entry.path());return result;
}
void refused(Reader& reader,const char* expected,const setup::ClamAvDownloadPump& pump={}){
    const auto before=jobs();bool caught{};
    try{auto result=setup::DownloadedClamAv::receive(reader,{},pump);}catch(const std::exception& error){std::cout<<"Observed refusal: "<<error.what()<<std::endl;caught=std::string(error.what()).find(expected)!=std::string::npos;}
    check(caught,expected);check(jobs()==before,"exact failed-download staging cleaned, no previous jobs changed");
}
}
int wmain(int argc,wchar_t** argv)try{
    check(argc==2&&!ins::elevated(),"ordinary native download qualification with exact official ZIP");
    sfx::platform::win32::verify_clamav_package(argv[1]);check(true,"original official ZIP size/hash verified");
    Reader wrong;wrong.length=setup::kClamAvDownloadBytes+1;refused(wrong,"unexpected size");
    Reader truncated;truncated.left=12;refused(truncated,"incomplete");
    Reader network;network.failure=true;refused(network,"synthetic network failure");
    Reader cancel;cancel.left=setup::kClamAvDownloadBytes;unsigned pumps{};refused(cancel,"cancelled",[&]{return ++pumps<4;});
    Reader large;large.left=setup::kClamAvDownloadBytes+1;refused(large,"exceeds the supported size");
    Reader mismatch;mismatch.length=setup::kClamAvDownloadBytes;mismatch.left=setup::kClamAvDownloadBytes;refused(mismatch,"verification failed");
    const auto before=jobs();std::filesystem::path output;
    {FileReader reader(argv[1]);unsigned progress{};auto downloaded=setup::DownloadedClamAv::receive(reader,[&](const std::wstring&){++progress;},[]{return true;});output=downloaded->path();check(std::filesystem::file_size(output)==setup::kClamAvDownloadBytes,"exact supported bytes staged");sfx::platform::win32::verify_clamav_package(output);check(progress>1,"truthful bounded progress and verified package");}
    check(!std::filesystem::exists(output)&&jobs()==before,"successful staging removed after last reader, originals retained");
    bool cancelled{};try{auto result=setup::DownloadedClamAv::fetch({},[]{return false;});}catch(const setup::ClamAvDownloadCancelled&){cancelled=true;}
    check(cancelled&&jobs()==before,"cancel before connection creates no staging or installation");
    std::cout<<"PASS native download checks="<<checks<<"; fake failure transports and exact real ZIP stream, actual HTTPS/install separate\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}
