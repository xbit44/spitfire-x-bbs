#include "botgate_https.hpp"
#include "../product_version.h"
#include "botgate_ca.hpp"
#include <curl/curl.h>
#include <openssl/x509_vfy.h>
#include <memory>
#include <string_view>
#ifndef _WIN32
#include <cerrno>
#include <pthread.h>
#include <signal.h>
#endif

namespace sfx::apps::runtime::https {
static_assert(std::string_view(bundle_hash)==sfx_botgate_ca_sha256,"CA diagnostic hash must match embedded bytes");
namespace {
constexpr std::size_t maximum=16u*1024u*1024u;
#ifndef _WIN32
// CURLOPT_NOSIGNAL leaves OpenSSL's possible SIGPIPE to its caller. Contain
// it in this worker only, including curl cleanup, without changing handlers
// or consuming a signal that was already pending when the caller entered.
class PipeSignals {
public:
    PipeSignals() noexcept {
        sigemptyset(&blocked_);sigaddset(&blocked_,SIGPIPE);
        if(pthread_sigmask(SIG_BLOCK,&blocked_,&original_)!=0) return;
        masked_=true;
        sigset_t pending{};
        if(sigpending(&pending)!=0) return;
        pending_before_=sigismember(&pending,SIGPIPE)==1;ready_=true;
    }
    ~PipeSignals() {
        if(!masked_) return;
        if(ready_ && !pending_before_) {
            timespec zero{};
            while(sigtimedwait(&blocked_,nullptr,&zero)<0 && errno==EINTR) {}
        }
        (void)pthread_sigmask(SIG_SETMASK,&original_,nullptr);
    }
    bool ready() const noexcept { return ready_; }
    PipeSignals(const PipeSignals&)=delete;
    PipeSignals& operator=(const PipeSignals&)=delete;
private:
    sigset_t blocked_{},original_{};
    bool masked_{},pending_before_{},ready_{};
};
#endif
struct Transfer {
    const std::atomic<bool>& stop;
    const std::function<void(std::uint64_t)>& progress;
    std::string bytes;
    bool too_large{};
    bool callback_failed{};
};
size_t write(char* data,size_t size,size_t count,void* raw) noexcept {
    auto& transfer=*static_cast<Transfer*>(raw);
    if(transfer.stop.load() || (size && count>maximum/size)) return 0;
    const auto length=size*count;
    if(length>maximum-transfer.bytes.size()) { transfer.too_large=true;return 0; }
    try {
        transfer.bytes.append(data,length);
        if(transfer.progress) transfer.progress(transfer.bytes.size());
        return length;
    } catch(...) { transfer.callback_failed=true;return 0; }
}
int progress(void* raw,curl_off_t,curl_off_t,curl_off_t,curl_off_t) noexcept {
    return static_cast<Transfer*>(raw)->stop.load()?1:0;
}
bool initialized() {
    // OpenSSL built no-autoload-config; curl built with auto-loading disabled.
    // One process-lifetime initialization, no cleanup racing other workers.
    static const bool ok=curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK;
    return ok;
}
}
std::string implementation() { return initialized()?curl_version():"curl_initialization_failed"; }
Result get(const char* url,const std::atomic<bool>& stop,
    const std::function<void(std::uint64_t)>& notify,const std::string* fixture_ca) {
    Result result;result.error="feed_https_initialization_failed";
    if(stop.load()) { result.error="feed_cancelled_or_timed_out";return result; }
#ifndef _WIN32
    PipeSignals signals;
    if(!signals.ready()) { result.error="feed_https_signal_mask_failed";return result; }
#endif
    if(!initialized()) return result;
    Transfer transfer{stop,notify,{}};
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> request(curl_easy_init(),curl_easy_cleanup);
    if(!request) return result;
    curl_blob ca{const_cast<void*>(static_cast<const void*>(sfx_botgate_ca)),sizeof(sfx_botgate_ca),CURL_BLOB_COPY};
    if(fixture_ca) { ca.data=const_cast<char*>(fixture_ca->data());ca.len=fixture_ca->size(); }
    auto set=[&](CURLoption key,auto value){ return curl_easy_setopt(request.get(),key,value)==CURLE_OK; };
    // No default/native CA or filesystem lookup, proxy environment, redirects,
    // alternate protocols, auth, client certs, cookies or session resumption.
    if(!set(CURLOPT_URL,url) || !set(CURLOPT_PROTOCOLS_STR,"https") ||
       !set(CURLOPT_FOLLOWLOCATION,0L) || !set(CURLOPT_MAXREDIRS,0L) ||
       !set(CURLOPT_SSL_VERIFYPEER,1L) || !set(CURLOPT_SSL_VERIFYHOST,2L) ||
       !set(CURLOPT_SSLVERSION,long(CURL_SSLVERSION_TLSv1_2)) ||
       !set(CURLOPT_SSL_OPTIONS,0L) || !set(CURLOPT_CAINFO,static_cast<const char*>(nullptr)) ||
       !set(CURLOPT_CAPATH,static_cast<const char*>(nullptr)) || !set(CURLOPT_CAINFO_BLOB,&ca) ||
       !set(CURLOPT_SSL_SESSIONID_CACHE,0L) || !set(CURLOPT_CONNECTTIMEOUT_MS,3000L) ||
       !set(CURLOPT_CERTINFO,1L) ||
       !set(CURLOPT_TIMEOUT_MS,30000L) || !set(CURLOPT_LOW_SPEED_LIMIT,1L) ||
       !set(CURLOPT_LOW_SPEED_TIME,3L) || !set(CURLOPT_MAXFILESIZE_LARGE,curl_off_t(maximum)) ||
       !set(CURLOPT_NOSIGNAL,1L) || !set(CURLOPT_NOPROGRESS,0L) ||
       !set(CURLOPT_XFERINFOFUNCTION,progress) || !set(CURLOPT_XFERINFODATA,&transfer) ||
       !set(CURLOPT_WRITEFUNCTION,write) || !set(CURLOPT_WRITEDATA,&transfer) ||
       !set(CURLOPT_USERAGENT,"Spitfire-X/" SFX_PRODUCT_VERSION_SHORT_STRING " BotGate") ||
       !set(CURLOPT_HTTP_VERSION,long(CURL_HTTP_VERSION_1_1))) return result;
    const auto code=curl_easy_perform(request.get());
    result.transport_result=static_cast<int>(code);
    (void)curl_easy_getinfo(request.get(),CURLINFO_SSL_VERIFYRESULT,&result.certificate_result);
    (void)curl_easy_getinfo(request.get(),CURLINFO_RESPONSE_CODE,&result.http_status);
    curl_certinfo* certificates{};
    if(curl_easy_getinfo(request.get(),CURLINFO_CERTINFO,&certificates)==CURLE_OK && certificates) {
        for(int i=0;i<certificates->num_of_certs && i<12;++i)
            for(auto* field=certificates->certinfo[i];field;field=field->next) {
                std::string text=field->data?field->data:"";
                if(text.size()>512) text.resize(512);
                for(auto& ch:text) if(static_cast<unsigned char>(ch)<32 || static_cast<unsigned char>(ch)>126) ch='?';
                if(text.rfind("Subject:",0)==0 || text.rfind("Issuer:",0)==0 ||
                   text.rfind("Start date:",0)==0 || text.rfind("Expire date:",0)==0)
                    result.certificate_summary+=std::to_string(i)+" "+text+"\n";
            }
    }
    if(code!=CURLE_OK) {
        if(stop.load() || code==CURLE_OPERATION_TIMEDOUT || code==CURLE_ABORTED_BY_CALLBACK)
            result.error="feed_cancelled_or_timed_out";
        else if(transfer.too_large || code==CURLE_FILESIZE_EXCEEDED) result.error="feed_download_too_large";
        else if(transfer.callback_failed) result.error="feed_download_callback_failed";
        else if(code==CURLE_PEER_FAILED_VERIFICATION) {
            switch(result.certificate_result) {
            case X509_V_ERR_CERT_HAS_EXPIRED:case X509_V_ERR_CERT_NOT_YET_VALID:
                result.error="feed_tls_certificate_date_invalid_check_clock";break;
            case X509_V_ERR_HOSTNAME_MISMATCH:case X509_V_ERR_IP_ADDRESS_MISMATCH:
                result.error="feed_tls_certificate_hostname_invalid";break;
            default: result.error="feed_tls_certificate_validation_failed_"+std::to_string(result.certificate_result);break;
            }
        } else result.error="feed_https_curl_error_"+std::to_string(result.transport_result);
        return result;
    }
    if(result.http_status!=200) { result.error="feed_http_status_"+std::to_string(result.http_status);return result; }
    result.bytes=std::move(transfer.bytes);result.error.clear();return result;
}
}
