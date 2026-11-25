// Linux fixture uses SFX's existing test certificate generator; no external source.
#include "botgate_https.hpp"
#include "sfx/application/botgate.hpp"
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <memory>
#include <thread>
#include <iostream>
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
struct Socket{int value{-1};~Socket(){if(value>=0)close(value);}};
enum class Case{good,expired,wrong,untrusted,redirect,oversized,cancel,abrupt};
net::Result fixture(Case mode){
    auto ca_key=key();auto ca=certificate(ca_key.get(),nullptr,ca_key.get(),false,false);
    auto server_key=key();auto cert=certificate(server_key.get(),ca.get(),ca_key.get(),mode==Case::expired,mode==Case::wrong);
    Owned<SSL_CTX,SSL_CTX_free> context(SSL_CTX_new(TLS_server_method()),SSL_CTX_free);
    require(context&&SSL_CTX_use_certificate(context.get(),cert.get())==1&&SSL_CTX_use_PrivateKey(context.get(),server_key.get())==1,"server context");
    Socket listener{socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0)};require(listener.value>=0,"loopback socket");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    require(bind(listener.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(listener.value,1)==0,"isolated listener");
    socklen_t length=sizeof(address);require(getsockname(listener.value,reinterpret_cast<sockaddr*>(&address),&length)==0,"ephemeral port");
    const auto url="https://127.0.0.1:"+std::to_string(ntohs(address.sin_port))+"/feed";std::atomic<bool> stop{},accepted{};
    std::thread server([&]{
        // Only the fixture server thread blocks SIGPIPE. The real client retains
        // ordinary process signal behavior, including abrupt-peer test coverage.
        sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGPIPE);pthread_sigmask(SIG_BLOCK,&blocked,nullptr);
        pollfd waiting{listener.value,POLLIN,0};if(poll(&waiting,1,4000)<=0)return;
        Socket peer{accept4(listener.value,nullptr,nullptr,SOCK_CLOEXEC)};if(peer.value<0)return;
        timeval timeout{2,0};setsockopt(peer.value,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(peer.value,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
        if(mode==Case::abrupt){linger reset{1,0};setsockopt(peer.value,SOL_SOCKET,SO_LINGER,&reset,sizeof(reset));return;}
        Owned<SSL,SSL_free> stream(SSL_new(context.get()),SSL_free);
        if(!stream||SSL_set_fd(stream.get(),peer.value)!=1||SSL_accept(stream.get())!=1)return;
        char request[4096];if(SSL_read(stream.get(),request,sizeof(request))<=0)return;accepted=true;
        if(mode==Case::cancel){while(!stop.load())std::this_thread::sleep_for(std::chrono::milliseconds(5));return;}
        const std::string response=mode==Case::redirect?"HTTP/1.1 302 Found\r\nLocation: https://127.0.0.1:1/never\r\nContent-Length: 0\r\n\r\n":
            mode==Case::oversized?"HTTP/1.1 200 OK\r\nContent-Length: 16777217\r\nConnection: close\r\n\r\n":
            "HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\n1.2.3.4\n";
        (void)SSL_write(stream.get(),response.data(),static_cast<int>(response.size()));(void)SSL_shutdown(stream.get());
    });
    std::thread cancellation;
    if(mode==Case::cancel)cancellation=std::thread([&]{const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(!accepted.load()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(std::chrono::milliseconds(5));stop=true;});
    struct Join{std::thread& server;std::thread& cancel;std::atomic<bool>& stop;~Join(){stop=true;if(cancel.joinable())cancel.join();server.join();}}join{server,cancellation,stop};
    const auto trust=pem(ca.get());return net::get(url.c_str(),stop,{},mode==Case::untrusted?nullptr:&trust);
}
int main(){try{
    require(geteuid()!=0,"unprivileged test required");unsigned checks{};
    const auto check=[&](bool value,const char* message){++checks;require(value,message);};
    const auto good=fixture(Case::good);check(good.bytes&&*good.bytes=="1.2.3.4\n"&&good.certificate_result==0,"actual pinned TLS client accepts verified fixture");
    check(fixture(Case::expired).error=="feed_tls_certificate_date_invalid_check_clock","expired certificate refused");
    const auto wrong=fixture(Case::wrong);
    if(wrong.error!="feed_tls_certificate_hostname_invalid")std::cerr<<"Hostname diagnostic: "<<wrong.error<<" verify="<<wrong.certificate_result<<" transport="<<wrong.transport_result<<'\n';
    check(!wrong.bytes&&(wrong.error=="feed_tls_certificate_hostname_invalid"||wrong.error.rfind("feed_tls_certificate_validation_failed_",0)==0),"wrong hostname refused");
    check(!fixture(Case::untrusted).bytes,"untrusted fixture CA refused by embedded production trust");
    check(fixture(Case::redirect).error=="feed_http_status_302","redirect not followed");
    check(fixture(Case::oversized).error=="feed_download_too_large","oversized HTTP body refused");
    const auto began=std::chrono::steady_clock::now();check(fixture(Case::cancel).error=="feed_cancelled_or_timed_out","in-progress TLS transfer cancelled");
    check(std::chrono::steady_clock::now()-began<std::chrono::seconds(5),"cancel bounded and server joined");
    check(!fixture(Case::abrupt).bytes,"abrupt peer cannot terminate client process");
    std::atomic<bool> stopped{true};check(!net::get("https://127.0.0.1:1/unused",stopped,{}).bytes,"pre-cancelled request makes no connection");
    std::cout<<"PASS "<<checks<<" native pinned HTTPS certificate, trust, redirect, size, cancellation and peer checks\n"<<net::implementation()<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
