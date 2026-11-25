#include "clamav_download.hpp"
#include "clamav_https.hpp"
#include "sfx/platform/win32/clamav_installation.hpp"
#include "../product_version.h"
#include "botgate_ca.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace sfx::apps::setup {
namespace {
namespace ins=platform::win32::installation;namespace win=platform::win32;
constexpr wchar_t zip_name[]=L"clamav-1.4.7.win.win32.zip";
constexpr char zip_utf8[]="clamav-1.4.7.win.win32.zip";
#include "clamav_https_reader.inc"
}
namespace clamav_https {
std::unique_ptr<ClamAvDownloadReader> open(const ClamAvDownloadPump& pump,const char* fixture_url,const std::string* fixture_ca,long deadline){
    return std::make_unique<OfficialReader>(pump,fixture_url?fixture_url:official_url,fixture_ca,deadline,fixture_url!=nullptr);
}
}
struct DownloadedClamAv::Impl {
    std::filesystem::path root,path;std::wstring owner;std::uint64_t id{};DWORD volume{};
    std::optional<win::FilePayloadDirectory> directory;
    std::optional<win::FilePayloadLease> pin;
    std::optional<application::FileObjectIdentity> object;
    ~Impl(){
        // Exact newly created file/root only. Unexpected/locked/replaced objects
        // are retained; cleanup never follows a link or recurses through Temp.
        pin.reset();
        try{
            if(directory&&object&&!directory->absent(zip_utf8)){
                auto file=directory->read(zip_utf8,true);const auto actual=file.object_identity();
                ins::require(actual.volume==object->volume&&actual.file_id==object->file_id,"clamav_download_cleanup_identity");file.remove_exact();
            }
            directory.reset();
            if(id){auto folder=ins::open_object(root,true,DELETE|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ);
                ins::require(ins::file_id(folder.get())==id&&ins::volume_serial(folder.get())==volume,"clamav_download_cleanup_root");
                FILE_DISPOSITION_INFO remove{TRUE};ins::require(SetFileInformationByHandle(folder.get(),FileDispositionInfo,&remove,sizeof(remove))!=FALSE,"clamav_download_cleanup_busy");}
        }catch(...){/* Preserve a changed/busy temporary object rather than delete it. */}
    }
};
DownloadedClamAv::DownloadedClamAv():impl_(std::make_unique<Impl>()){}
DownloadedClamAv::~DownloadedClamAv()=default;
const std::filesystem::path& DownloadedClamAv::path()const noexcept{return impl_->path;}
std::unique_ptr<DownloadedClamAv> DownloadedClamAv::fetch(const ClamAvDownloadProgress& progress,const ClamAvDownloadPump& pump){
    platform::win32::installation::require(platform::win32::clamav_available(),"optional_component_unavailable_on_this_os");
    if(pump&&!pump())throw ClamAvDownloadCancelled();
    if(progress)progress(L"Connecting to the official ClamAV download site over HTTPS...");
    auto reader=clamav_https::open(pump);
    return receive(*reader,progress,pump);
}
std::unique_ptr<DownloadedClamAv> DownloadedClamAv::receive(ClamAvDownloadReader& reader,const ClamAvDownloadProgress& progress,const ClamAvDownloadPump& pump){
    platform::win32::installation::require(platform::win32::clamav_available(),"optional_component_unavailable_on_this_os");
    if(pump&&!pump())throw ClamAvDownloadCancelled();
    const auto length=reader.content_length();
    if(length&&*length!=kClamAvDownloadBytes)throw std::runtime_error("Portable ClamAV download has an unexpected size. Your installation was not changed.");
    auto result=std::unique_ptr<DownloadedClamAv>(new DownloadedClamAv);auto& p=*result->impl_;
    p.owner=ins::operator_sid();ins::require(!ins::elevated(),"clamav_download_ordinary_user_only");
    std::array<wchar_t,32768> temporary{};const auto count=GetTempPathW(static_cast<DWORD>(temporary.size()),temporary.data());ins::require(count&&count<temporary.size(),"clamav_download_temp_path");
    std::wstring temporary_directory=temporary.data();
    while(temporary_directory.size()>3&&temporary_directory.back()==L'\\')temporary_directory.pop_back();
    const auto parent=ins::canonical_root(temporary_directory);
    p.root=parent/(L"SFX-ClamAV-download-"+ins::unique_id());p.path=p.root/zip_name;
    const auto security=ins::mutable_descriptor(p.owner);ins::create_directory(p.root,security.get());
    {const auto root=ins::open_object(p.root,true);p.id=ins::file_id(root.get());p.volume=ins::volume_serial(root.get());}
    p.directory.emplace(win::FilePayloadDirectory::private_store(p.root,p.owner));
    p.directory->require_space(kClamAvDownloadBytes);
    std::array<std::uint8_t,65536> buffer{};std::uint64_t total{},last=UINT64_MAX;const auto started=GetTickCount64();
    {
        auto file=p.directory->create(zip_utf8,kClamAvDownloadBytes);p.object=file.object_identity();
        for(;;){
            if(pump&&!pump())throw ClamAvDownloadCancelled();
            ins::require(GetTickCount64()-started<600000,"Portable ClamAV download timed out. Your installation was not changed.");
            const auto n=reader.read(buffer.data(),buffer.size());ins::require(n<=buffer.size(),"clamav_download_transport_bound");if(!n)break;
            if(n>kClamAvDownloadBytes-total)throw std::runtime_error("Portable ClamAV download exceeds the supported size. Your installation was not changed.");
            file.append(buffer.data(),n);total+=n;
            if(total/(1024*1024)!=last){last=total/(1024*1024);if(progress)progress(L"Downloading Portable ClamAV: "+std::to_wstring(total)+L" / "+std::to_wstring(kClamAvDownloadBytes)+L" bytes");}
        }
        if(total!=kClamAvDownloadBytes)throw std::runtime_error("Portable ClamAV download was incomplete. Your installation was not changed.");
        (void)file.identity();
    }
    if(pump&&!pump())throw ClamAvDownloadCancelled();
    if(progress)progress(L"Verifying the downloaded Portable ClamAV package...");
    try{win::verify_clamav_package(p.path);}catch(...){throw std::runtime_error("Portable ClamAV package verification failed. Your installation was not changed.");}
    p.pin.emplace(p.directory->read(zip_utf8));
    if(progress)progress(L"Portable ClamAV download verified. Continuing Setup...");
    return result;
}
} // namespace sfx::apps::setup
