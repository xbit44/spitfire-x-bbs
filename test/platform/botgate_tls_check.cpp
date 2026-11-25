// Standalone owner diagnostic. No installation, registry, trust-store or OS changes.
#include "botgate_https.hpp"
#include "sfx/application/botgate.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <memory>
#include <stdexcept>

namespace net=sfx::apps::runtime::https;
template<class T,void(*F)(T*)> using Owned=std::unique_ptr<T,decltype(F)>;
void require(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
Owned<EVP_PKEY,EVP_PKEY_free> key() {
    Owned<EVP_PKEY_CTX,EVP_PKEY_CTX_free> context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,nullptr),EVP_PKEY_CTX_free);
    EVP_PKEY* out{};
    require(context && EVP_PKEY_keygen_init(context.get())>0 &&
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(),2048)>0 && EVP_PKEY_keygen(context.get(),&out)>0,"key_generation");
    return {out,EVP_PKEY_free};
}
void extension(X509* certificate,X509* issuer,int type,const char* value) {
    X509V3_CTX context{};X509V3_set_ctx(&context,issuer,certificate,nullptr,nullptr,0);
    Owned<X509_EXTENSION,X509_EXTENSION_free> ext(X509V3_EXT_conf_nid(nullptr,&context,type,value),X509_EXTENSION_free);
    require(ext && X509_add_ext(certificate,ext.get(),-1)==1,"certificate_extension");
}
Owned<X509,X509_free> certificate(EVP_PKEY* public_key,X509* issuer,EVP_PKEY* signer,bool expired,bool wrong) {
    Owned<X509,X509_free> out(X509_new(),X509_free);require(bool(out),"certificate_allocation");
    require(X509_set_version(out.get(),2)==1 && ASN1_INTEGER_set(X509_get_serialNumber(out.get()),issuer?2:1)==1 &&
        X509_gmtime_adj(X509_getm_notBefore(out.get()),-86400*2)!=nullptr &&
        X509_gmtime_adj(X509_getm_notAfter(out.get()),expired?-86400:86400)!=nullptr &&
        X509_set_pubkey(out.get(),public_key)==1,"certificate_fields");
    auto* subject=X509_get_subject_name(out.get());
    require(X509_NAME_add_entry_by_txt(subject,"CN",MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(issuer?"SFX ephemeral TLS test":"SFX ephemeral test CA"),-1,-1,0)==1 &&
        X509_set_issuer_name(out.get(),issuer?X509_get_subject_name(issuer):subject)==1,"certificate_names");
    extension(out.get(),issuer?issuer:out.get(),NID_basic_constraints,issuer?"critical,CA:FALSE":"critical,CA:TRUE");
    extension(out.get(),issuer?issuer:out.get(),NID_key_usage,issuer?"critical,digitalSignature,keyEncipherment":"critical,keyCertSign,cRLSign");
    if(issuer) {
        extension(out.get(),issuer,NID_ext_key_usage,"serverAuth");
        extension(out.get(),issuer,NID_subject_alt_name,wrong?"DNS:wrong.example":"IP:127.0.0.1");
    }
    require(X509_sign(out.get(),signer,EVP_sha256())>0,"certificate_sign");return out;
}
std::string pem(X509* certificate) {
    Owned<BIO,BIO_free_all> output(BIO_new(BIO_s_mem()),BIO_free_all);
    require(output && PEM_write_bio_X509(output.get(),certificate)==1,"certificate_pem");
    char* data{};const auto length=BIO_get_mem_data(output.get(),&data);return {data,static_cast<size_t>(length)};
}
struct Socket {
    SOCKET value=INVALID_SOCKET;
    ~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}
};
net::Result fixture(bool expired,bool wrong,bool untrusted,bool cancelled=false) {
    auto ca_key=key();auto ca=certificate(ca_key.get(),nullptr,ca_key.get(),false,false);
    auto server_key=key();auto server_cert=certificate(server_key.get(),ca.get(),ca_key.get(),expired,wrong);
    Owned<SSL_CTX,SSL_CTX_free> context(SSL_CTX_new(TLS_server_method()),SSL_CTX_free);
    require(context && SSL_CTX_use_certificate(context.get(),server_cert.get())==1 &&
        SSL_CTX_use_PrivateKey(context.get(),server_key.get())==1,"server_context");
    Socket listener{socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)};
    require(listener.value!=INVALID_SOCKET,"server_socket");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    require(bind(listener.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && listen(listener.value,1)==0,"server_bind");
    int size=sizeof(address);require(getsockname(listener.value,reinterpret_cast<sockaddr*>(&address),&size)==0,"server_address");
    const std::string url="https://127.0.0.1:"+std::to_string(ntohs(address.sin_port))+"/feed";
    std::thread server([&]{
        fd_set readable;FD_ZERO(&readable);FD_SET(listener.value,&readable);timeval timeout{5,0};
        if(select(0,&readable,nullptr,nullptr,&timeout)<=0)return;
        Socket connection{accept(listener.value,nullptr,nullptr)};if(connection.value==INVALID_SOCKET)return;
        DWORD wait=4000;setsockopt(connection.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&wait),sizeof(wait));
        setsockopt(connection.value,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&wait),sizeof(wait));
        Owned<SSL,SSL_free> stream(SSL_new(context.get()),SSL_free);
        if(!stream || SSL_set_fd(stream.get(),static_cast<int>(connection.value))!=1 || SSL_accept(stream.get())!=1)return;
        char input[4096];if(SSL_read(stream.get(),input,sizeof(input))<=0)return;
        constexpr char response[]="HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\n1.2.3.4\n";
        (void)SSL_write(stream.get(),response,sizeof(response)-1);(void)SSL_shutdown(stream.get());
    });
    struct Join {std::thread& thread;~Join(){thread.join();}} join{server};
    std::atomic<bool> stop{cancelled};const auto trust=pem(ca.get());
    return net::get(url.c_str(),stop,{},untrusted?nullptr:&trust);
}
struct Internet {HINTERNET value{};~Internet(){if(value)WinHttpCloseHandle(value);}};
void CALLBACK status(HINTERNET,DWORD_PTR context,DWORD kind,LPVOID info,DWORD size) {
    if(context && kind==WINHTTP_CALLBACK_STATUS_SECURE_FAILURE && info && size==sizeof(DWORD))
        reinterpret_cast<std::atomic<DWORD>*>(context)->fetch_or(*static_cast<DWORD*>(info));
}
void legacy(std::ostream& report) {
    std::atomic<DWORD> flags{};
    Internet session{WinHttpOpen(L"Spitfire-X/0.0.3 BotGate",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    require(session.value!=nullptr,"legacy_session");
    DWORD tls=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    require(WinHttpSetOption(session.value,WINHTTP_OPTION_SECURE_PROTOCOLS,&tls,sizeof(tls)) &&
        WinHttpSetTimeouts(session.value,3000,3000,3000,3000),"legacy_options");
    Internet connection{WinHttpConnect(session.value,L"raw.githubusercontent.com",443,0)};
    require(connection.value!=nullptr,"legacy_connect");
    Internet request{WinHttpOpenRequest(connection.value,L"GET",L"/borestad/blocklist-abuseipdb/refs/heads/main/abuseipdb-s100-30d.ipv4",
        nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
    require(request.value!=nullptr,"legacy_request");
    require(WinHttpSetStatusCallback(request.value,status,WINHTTP_CALLBACK_STATUS_SECURE_FAILURE,0)!=WINHTTP_INVALID_STATUS_CALLBACK,"legacy_callback");
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER,disable=WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION;
    require(WinHttpSetOption(request.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)) &&
        WinHttpSetOption(request.value,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable)),"legacy_request_options");
    const bool ok=WinHttpSendRequest(request.value,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,
        reinterpret_cast<DWORD_PTR>(&flags)) && WinHttpReceiveResponse(request.value,nullptr);
    const DWORD error=ok?0:GetLastError();
    report<<"legacy_winhttp_response="<<ok<<" error="<<error<<" secure_flags="<<flags.load()<<"\n";
    report<<"legacy_invalid_CA="<<bool(flags.load()&WINHTTP_CALLBACK_STATUS_FLAG_INVALID_CA)<<"\n";
}
void record(std::ostream& report,const char* name,const net::Result& result,bool passed) {
    report<<name<<"="<<(passed?"PASS":"FAIL")<<" curl="<<result.transport_result<<" verify="<<result.certificate_result
        <<" http="<<result.http_status<<" bytes="<<(result.bytes?result.bytes->size():0)<<" error="<<result.error<<"\n";
    report<<result.certificate_summary;
}
int main(int argc,char**) {
    std::ostringstream report;bool passed=true;
    try {
        report<<"SFX BotGate TLS compatibility report v1\n";
        SYSTEMTIME time{};GetSystemTime(&time);report<<"UTC="<<time.wYear<<"-"<<time.wMonth<<"-"<<time.wDay<<" "<<time.wHour<<":"<<time.wMinute<<"\n";
        using Version=LONG(WINAPI*)(OSVERSIONINFOW*);
        auto version=reinterpret_cast<Version>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
        OSVERSIONINFOW os{sizeof(os)};if(version && version(&os)==0)report<<"Windows="<<os.dwMajorVersion<<"."<<os.dwMinorVersion<<" build="<<os.dwBuildNumber<<"\n";
        report<<"implementation="<<net::implementation()<<"\nCA_date="<<net::bundle_date<<"\nCA_SHA256="<<net::bundle_hash<<"\n";
        WSADATA winsock{};require(WSAStartup(MAKEWORD(2,2),&winsock)==0,"winsock");
        auto valid=fixture(false,false,false);bool ok=valid.bytes && *valid.bytes=="1.2.3.4\n";
        record(report,"local_valid",valid,ok);passed&=ok;
        auto unknown=fixture(false,false,true);ok=!unknown.bytes && unknown.transport_result==60 && unknown.certificate_result!=0;
        record(report,"reject_untrusted",unknown,ok);passed&=ok;
        auto expired=fixture(true,false,false);ok=!expired.bytes && expired.transport_result==60 && expired.certificate_result==X509_V_ERR_CERT_HAS_EXPIRED;
        record(report,"reject_expired",expired,ok);passed&=ok;
        auto wrong=fixture(false,true,false);ok=!wrong.bytes && wrong.transport_result==60;
        record(report,"reject_wrong_host",wrong,ok);passed&=ok;
        std::atomic<bool> active{false};auto plain=net::get("http://127.0.0.1/",active,{});
        ok=!plain.bytes && plain.transport_result==1;record(report,"reject_plain_http",plain,ok);passed&=ok;
        std::atomic<bool> stop{true};auto cancel=net::get(net::feed_url,stop,{});
        ok=!cancel.bytes && cancel.error=="feed_cancelled_or_timed_out";record(report,"cancel_before_connect",cancel,ok);passed&=ok;
        if(argc==1) {
            try {legacy(report);} catch(const std::exception& error) {report<<"legacy_probe_error="<<error.what()<<"\n";}
            stop=false;auto live=net::get(net::feed_url,stop,{});
            const auto entries=live.bytes?sfx::application::parse_botgate_feed(*live.bytes):std::nullopt;
            ok=entries && !entries->empty();record(report,"official_feed",live,ok);passed&=ok;
            if(entries)report<<"validated_feed_entries="<<entries->size()<<"\n";
        } else report<<"official_feed=NOT_RUN local-only engineering mode\n";
    } catch(const std::exception& error) {passed=false;report<<"exception="<<error.what()<<"\n";}
    report<<"RESULT="<<(passed?"PASS":"FAIL")<<"\nNo OS trust/settings or SFX installation was changed.\n";
    std::cout<<report.str();
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);
    const auto file=std::filesystem::path(module).parent_path()/L"SFX-BotGate-TLS-Report.txt";
    // Each invocation is a new report; refuse to silently overwrite prior evidence.
    const auto handle=CreateFileW(file.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle!=INVALID_HANDLE_VALUE) {
        const auto text=report.str();DWORD written{};
        const bool saved=WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) && written==text.size();
        CloseHandle(handle);if(!saved)return 2;
    } else {std::cout<<"Report file already exists or is not writable; console output retained.\n";return 2;}
    return passed?0:1;
}
