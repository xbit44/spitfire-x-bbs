#include <windows.h>
#include "sfx/platform/win32/ftn_echo_list_download.hpp"
#include "sfx/application/ftn.hpp"
#include "botgate_ca.hpp"
#include <curl/curl.h>
#include <memory>

namespace sfx::platform::win32 {
namespace {
constexpr std::size_t maximum=4*1024*1024;
struct Transfer {
    const std::atomic<bool>& cancel;std::uint64_t deadline{};std::string bytes;
    std::size_t headers{};bool too_large{},callback_failed{};
};
size_t body(char* data,size_t size,size_t count,void* raw) noexcept {
    auto& t=*static_cast<Transfer*>(raw);
    if(t.cancel.load())return 0;
    if(size&&count>maximum/size){t.too_large=true;return 0;}
    const auto n=size*count;if(n>maximum-t.bytes.size()){t.too_large=true;return 0;}
    try{t.bytes.append(data,n);return n;}catch(...){t.callback_failed=true;return 0;}
}
size_t header(char*,size_t size,size_t count,void* raw) noexcept {
    auto& t=*static_cast<Transfer*>(raw);
    if(size&&count>65536/size){t.too_large=true;return 0;}
    const auto n=size*count;if(n>65536-t.headers){t.too_large=true;return 0;}t.headers+=n;return n;
}
int progress(void* raw,curl_off_t,curl_off_t,curl_off_t,curl_off_t) noexcept {
    const auto& t=*static_cast<Transfer*>(raw);return t.cancel.load()||GetTickCount64()>=t.deadline?1:0;
}
bool initialized(){static const bool ok=curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK;return ok;}
}
std::string_view ftn_echo_list_download_message(FtnEchoListDownloadCode c) noexcept {
    switch(c){
    case FtnEchoListDownloadCode::ok:return "Echo list downloaded. Review the selected entries before importing.";
    case FtnEchoListDownloadCode::invalid_url:return "Enter an HTTP or HTTPS EchoList URL without embedded credentials.";
    case FtnEchoListDownloadCode::cancelled:return "Echo list download cancelled. Your settings remain.";
    case FtnEchoListDownloadCode::too_large:return "Echo list exceeds the bounded download size.";
    case FtnEchoListDownloadCode::timeout:return "Echo list download timed out. Your settings remain.";
    case FtnEchoListDownloadCode::certificate:return "The HTTPS certificate could not be verified. Check the URL and system time.";
    case FtnEchoListDownloadCode::redirect:return "Echo list redirect was refused. Review the URL; HTTPS is never downgraded automatically.";
    case FtnEchoListDownloadCode::http_error:return "The EchoList server did not return a successful file response.";
    default:return "The EchoList connection failed. Your settings remain.";
    }
}
FtnEchoListDownload download_ftn_echo_list(std::string url,const std::atomic<bool>& cancel,const std::string* fixture_trust){
    if(!application::valid_ftn_echo_list_url(url))return {FtnEchoListDownloadCode::invalid_url,{}};
    if(cancel.load())return {FtnEchoListDownloadCode::cancelled,{}};
    if(!initialized())return {};
    const auto deadline=GetTickCount64()+30000;bool secure=false;
    for(unsigned redirect=0;redirect<=5;++redirect){
        if(cancel.load())return {FtnEchoListDownloadCode::cancelled,{}};
        if(GetTickCount64()>=deadline)return {FtnEchoListDownloadCode::timeout,{}};
        const bool https=url.compare(0,8,"https://")==0;
        if(secure&&!https)return {FtnEchoListDownloadCode::redirect,{}};
        secure=secure||https;
        std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> easy(curl_easy_init(),curl_easy_cleanup);if(!easy)return {};
        Transfer transfer{cancel,deadline,{}};
        curl_blob trust{const_cast<unsigned char*>(sfx_botgate_ca),sizeof(sfx_botgate_ca),CURL_BLOB_COPY};
        if(fixture_trust){trust.data=const_cast<char*>(fixture_trust->data());trust.len=fixture_trust->size();}
        const auto set=[&](CURLoption option,auto value){return curl_easy_setopt(easy.get(),option,value)==CURLE_OK;};
        const auto current=GetTickCount64();if(current>=deadline)return {FtnEchoListDownloadCode::timeout,{}};
        const auto remaining=static_cast<long>(deadline-current);
        // The pinned SfxBotGateTLS build compiles proxy, HTTP auth and netrc out.
        // Their disabled options return UNKNOWN_OPTION, even when asked to turn
        // them off; keep that dependency policy rather than enabling features.
        if(!set(CURLOPT_URL,url.c_str())||!set(CURLOPT_PROTOCOLS_STR,https?"https":"http,https")||
           !set(CURLOPT_FOLLOWLOCATION,0L)||!set(CURLOPT_MAXREDIRS,0L)||
           !set(CURLOPT_SSL_VERIFYPEER,1L)||!set(CURLOPT_SSL_VERIFYHOST,2L)||
           !set(CURLOPT_SSLVERSION,long(CURL_SSLVERSION_TLSv1_2))||!set(CURLOPT_CAINFO,static_cast<const char*>(nullptr))||
           !set(CURLOPT_CAPATH,static_cast<const char*>(nullptr))||!set(CURLOPT_CAINFO_BLOB,&trust)||
           !set(CURLOPT_SSL_SESSIONID_CACHE,0L)||!set(CURLOPT_CONNECTTIMEOUT_MS,3000L)||
           !set(CURLOPT_TIMEOUT_MS,remaining)||!set(CURLOPT_LOW_SPEED_LIMIT,1L)||!set(CURLOPT_LOW_SPEED_TIME,3L)||
           !set(CURLOPT_MAXFILESIZE_LARGE,curl_off_t(maximum))||!set(CURLOPT_NOSIGNAL,1L)||
           !set(CURLOPT_WRITEFUNCTION,&body)||!set(CURLOPT_WRITEDATA,&transfer)||
           !set(CURLOPT_HEADERFUNCTION,&header)||!set(CURLOPT_HEADERDATA,&transfer)||
           !set(CURLOPT_NOPROGRESS,0L)||!set(CURLOPT_XFERINFOFUNCTION,&progress)||!set(CURLOPT_XFERINFODATA,&transfer))return {};
        const auto result=curl_easy_perform(easy.get());long status{};curl_easy_getinfo(easy.get(),CURLINFO_RESPONSE_CODE,&status);
        if(result!=CURLE_OK){
            if(cancel.load())return {FtnEchoListDownloadCode::cancelled,{}};
            if(transfer.too_large||result==CURLE_FILESIZE_EXCEEDED)return {FtnEchoListDownloadCode::too_large,{}};
            if(result==CURLE_OPERATION_TIMEDOUT||result==CURLE_ABORTED_BY_CALLBACK)return {FtnEchoListDownloadCode::timeout,{}};
            if(result==CURLE_PEER_FAILED_VERIFICATION)return {FtnEchoListDownloadCode::certificate,{}};
            return {};
        }
        if(status==200)return {FtnEchoListDownloadCode::ok,std::move(transfer.bytes)};
        if(status<300||status>399)return {FtnEchoListDownloadCode::http_error,{}};
        char* next=nullptr;curl_easy_getinfo(easy.get(),CURLINFO_REDIRECT_URL,&next);
        if(redirect==5||!next||!application::valid_ftn_echo_list_url(next))return {FtnEchoListDownloadCode::redirect,{}};
        url=next;
    }
    return {FtnEchoListDownloadCode::redirect,{}};
}
}
