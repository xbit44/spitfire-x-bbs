// Actual Linux socket loopback and OpenSSL handshakes, fixture-only certificate/key
// material. No owner/hub endpoints, Runtime installation or peer-auth claims.
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <vector>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include "sfx/platform/linux/ftn_native_transport.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace native=sfx::platform::linux_host;
namespace app=sfx::application;

namespace {
using Clock=std::chrono::steady_clock;
unsigned checks{};
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
template<class F>void refused(F action,const char* reason){bool failed{};try{action();}catch(const std::exception&){failed=true;}check(failed,reason);}
using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
using Certificate=std::unique_ptr<X509,decltype(&X509_free)>;
using Bio=std::unique_ptr<BIO,decltype(&BIO_free)>;
Key key(){Key out(EVP_PKEY_Q_keygen(nullptr,nullptr,"EC","prime256v1"),EVP_PKEY_free);check(bool(out),"fixture EC key generated");return out;}
void extension(X509* certificate,X509* issuer,int nid,const char* text){X509V3_CTX context{};X509V3_set_ctx(&context,issuer,certificate,nullptr,nullptr,0);std::unique_ptr<X509_EXTENSION,decltype(&X509_EXTENSION_free)> value(X509V3_EXT_conf_nid(nullptr,&context,nid,const_cast<char*>(text)),X509_EXTENSION_free);check(value&&X509_add_ext(certificate,value.get(),-1)==1,"fixture extension");}
Certificate certificate(EVP_PKEY* public_key,X509* issuer,EVP_PKEY* signer,bool ca){
    Certificate out(X509_new(),X509_free);check(bool(out)&&X509_set_version(out.get(),2)==1&&ASN1_INTEGER_set(X509_get_serialNumber(out.get()),ca?1:2)==1,"fixture certificate version");
    check(X509_gmtime_adj(X509_getm_notBefore(out.get()),-300)&&X509_gmtime_adj(X509_getm_notAfter(out.get()),86400),"fixture validity");
    auto* name=X509_get_subject_name(out.get());check(X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>(ca?"SFX ISOLATED TEST CA":"sfx-test.invalid"),-1,-1,0)==1&&X509_set_issuer_name(out.get(),issuer?X509_get_subject_name(issuer):name)==1&&X509_set_pubkey(out.get(),public_key)==1,"fixture subject/issuer");
    extension(out.get(),issuer?issuer:out.get(),NID_basic_constraints,ca?"critical,CA:TRUE":"critical,CA:FALSE");extension(out.get(),issuer?issuer:out.get(),NID_key_usage,ca?"critical,keyCertSign,cRLSign":"critical,digitalSignature");
    if(!ca){extension(out.get(),issuer,NID_ext_key_usage,"serverAuth");extension(out.get(),issuer,NID_subject_alt_name,"DNS:sfx-test.invalid");}check(X509_sign(out.get(),signer?signer:public_key,EVP_sha256())>0,"fixture signature");return out;
}
void write_bio(const std::filesystem::path& path,BIO* source){char* bytes{};const auto count=BIO_get_mem_data(source,&bytes);check(count>0,"fixture PEM bytes");std::ofstream output(path,std::ios::binary);output.write(bytes,count);check(output.good(),"fixture PEM saved in new private directory");output.close();check(::chmod(path.c_str(),0600)==0,"fixture PEM mode");}
void save_key(const std::filesystem::path& path,EVP_PKEY* key){Bio output(BIO_new(BIO_s_mem()),BIO_free);check(output&&PEM_write_bio_PrivateKey(output.get(),key,nullptr,nullptr,0,nullptr,nullptr)==1,"fixture key PEM");write_bio(path,output.get());char* bytes{};const auto count=BIO_get_mem_data(output.get(),&bytes);if(count>0)OPENSSL_cleanse(bytes,static_cast<std::size_t>(count));}
void save_certificate(const std::filesystem::path& path,X509* certificate){Bio output(BIO_new(BIO_s_mem()),BIO_free);check(output&&PEM_write_bio_X509(output.get(),certificate)==1,"fixture certificate PEM");write_bio(path,output.get());}
struct PairResult {bool server_encrypted{},server_verified{},client_encrypted{},client_verified{};std::string server_error,client_error,server_bytes,client_bytes;};
PairResult exchange(native::FtnNativeListener& listener,const app::FtnLink& link,bool raw_client=false,int version=0){
    PairResult result;const auto deadline=Clock::now()+std::chrono::seconds(8);const auto live=[&]{return Clock::now()<deadline;};
    std::thread server([&]{try{std::unique_ptr<native::FtnNativeChannel> channel;while(!channel&&live())channel=listener.accept(std::chrono::milliseconds(100),live);if(!channel)throw std::runtime_error("test_accept_timeout");channel->handshake(live);result.server_encrypted=channel->encrypted();result.server_verified=channel->verified_server_tls();while(result.server_bytes.size()<4&&live()){const auto bytes=channel->read(std::chrono::milliseconds(100),live);if(bytes.outcome==native::FtnReadOutcome::closed)break;result.server_bytes+=bytes.bytes;}if(result.server_bytes=="ping")channel->write("pong",live);}catch(const std::exception& error){result.server_error=error.what();}});
    try{
        if(!raw_client){auto dial=link;dial.port=listener.port();auto channel=native::FtnNativeChannel::connect_ipv4({127,0,0,1},listener.port(),dial,live);result.client_encrypted=channel->encrypted();result.client_verified=channel->verified_server_tls();channel->write("ping",live);while(result.client_bytes.size()<4&&live()){const auto bytes=channel->read(std::chrono::milliseconds(100),live);if(bytes.outcome==native::FtnReadOutcome::closed)break;result.client_bytes+=bytes.bytes;}}
        else{
            // Independently configured native TLS client for exact version
            // negotiation/refusal. Its setup does not use production policy.
            const auto socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(socket==-1)throw std::runtime_error("raw_socket");struct Close{int value;~Close(){::close(value);}} close{socket};timeval timeout{3,0};setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(listener.port());address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);if(::connect(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0)throw std::runtime_error("raw_connect");
            std::unique_ptr<SSL_CTX,decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_client_method()),SSL_CTX_free);SSL_CTX_set_security_level(context.get(),0);SSL_CTX_set_min_proto_version(context.get(),version);SSL_CTX_set_max_proto_version(context.get(),version);SSL_CTX_set_verify(context.get(),SSL_VERIFY_PEER,nullptr);if(SSL_CTX_load_verify_locations(context.get(),link.tls_ca_file.c_str(),nullptr)!=1)throw std::runtime_error("raw_ca");std::unique_ptr<SSL,decltype(&SSL_free)> ssl(SSL_new(context.get()),SSL_free);SSL_set_fd(ssl.get(),static_cast<int>(socket));SSL_set1_host(ssl.get(),"sfx-test.invalid");if(SSL_connect(ssl.get())!=1)throw std::runtime_error("raw_handshake_refused");result.client_encrypted=true;result.client_verified=SSL_get_verify_result(ssl.get())==X509_V_OK;if(SSL_write(ssl.get(),"ping",4)!=4)throw std::runtime_error("raw_write");char bytes[4]{};const auto count=SSL_read(ssl.get(),bytes,4);if(count>0)result.client_bytes.assign(bytes,count);
        }
    }catch(const std::exception& error){result.client_error=error.what();}
    server.join();return result;
}
}
int main(int argc,char** argv){try{
    check(argc==2,"new isolated fixture root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve previous native transport fixtures");std::filesystem::create_directories(root.parent_path());check(::mkdir(root.c_str(),0700)==0,"private new fixture directory");
    std::vector<native::FileDescriptor> held;for(int i=0;i<1100;++i){held.emplace_back(::open("/dev/null",O_RDONLY|O_CLOEXEC));check(bool(held.back()),"high FD fixture");}
    auto ca_key=key(),server_key=key(),wrong_key=key();auto ca=certificate(ca_key.get(),nullptr,nullptr,true);auto server=certificate(server_key.get(),ca.get(),ca_key.get(),false);save_certificate(root/L"ca.pem",ca.get());save_certificate(root/L"server.pem",server.get());save_key(root/L"server-key.pem",server_key.get());save_key(root/L"wrong-key.pem",wrong_key.get());
    app::FtnSettings settings;settings.tls_certificate=(root/L"server.pem").u8string();settings.tls_private_key=(root/L"server-key.pem").u8string();app::FtnLink link;link.host="sfx-test.invalid";link.tls_server_name="sfx-test.invalid";link.tls_ca_file=(root/L"ca.pem").u8string();
    auto classic=native::FtnNativeListener::open("127.0.0.1",0,false,settings);check(classic->port()!=0,"classic actual ephemeral loopback bind");refused([&]{native::FtnNativeListener::open("127.0.0.1",classic->port(),false,settings);},"actual occupied classic port refuses duplicate activation");
    auto plain=exchange(*classic,link);check(plain.client_error.empty()&&plain.server_error.empty()&&plain.server_bytes=="ping"&&plain.client_bytes=="pong"&&!plain.client_verified&&!plain.server_verified&&!plain.client_encrypted&&!plain.server_encrypted,"actual clear socket full duplex; no fabricated peer verification");
    auto tls=native::FtnNativeListener::open("127.0.0.1",0,true,settings);check(tls->port()!=classic->port(),"independent classic and explicit TLS listeners coexist");link.tls=true;auto secure=exchange(*tls,link);check(secure.client_error.empty()&&secure.server_error.empty()&&secure.server_bytes=="ping"&&secure.client_bytes=="pong"&&secure.client_verified&&!secure.server_verified&&secure.client_encrypted&&secure.server_encrypted,"actual verified CA/name server TLS; answering TLS alone does not verify client");
    for(const auto version:{TLS1_2_VERSION,TLS1_3_VERSION}){const auto exact=exchange(*tls,link,true,version);check(exact.client_error.empty()&&exact.server_error.empty()&&exact.server_bytes=="ping"&&exact.client_bytes=="pong"&&exact.client_verified,"independent native client actually negotiates approved TLS version");}
    const auto obsolete=exchange(*tls,link,true,TLS1_1_VERSION);check(!obsolete.client_error.empty()&&!obsolete.server_error.empty()&&obsolete.server_bytes.empty(),"TLS1.1 refused before application data");
    auto mismatch=link;mismatch.tls_server_name="wrong.invalid";const auto wrong_name=exchange(*tls,mismatch);check(!wrong_name.client_error.empty()&&!wrong_name.server_error.empty()&&wrong_name.server_bytes.empty(),"wrong expected TLS server name refuses with no plaintext fallback");
    auto untrusted=link;untrusted.tls_ca_file.clear();const auto unknown_ca=exchange(*tls,untrusted);check(!unknown_ca.client_error.empty()&&!unknown_ca.server_error.empty()&&unknown_ca.server_bytes.empty(),"test CA absent from real Linux system trust is refused without fallback");
    auto clear=link;clear.tls=false;const auto clear_to_tls=exchange(*tls,clear);check(!clear_to_tls.server_error.empty()&&clear_to_tls.server_bytes.empty(),"clear traffic on explicitly TLS listener never enters application session");
    auto bad=settings;bad.tls_private_key=(root/L"wrong-key.pem").u8string();refused([&]{native::FtnNativeListener::open("127.0.0.1",0,true,bad);},"mismatched actual certificate/private key prevents activation");bad.tls_private_key=(root/L"absent.pem").u8string();refused([&]{native::FtnNativeListener::open("127.0.0.1",0,true,bad);},"missing native key prevents TLS activation");
    const auto original_key=std::filesystem::path(settings.tls_private_key);
    check(::chmod(original_key.c_str(),0644)==0,"make copied key public");
    refused([&]{native::FtnNativeListener::open("127.0.0.1",0,true,settings);},"public private key refused");
    check(::chmod(original_key.c_str(),0600)==0,"restore fixture key privacy");
    const auto linked=root/"linked-key.pem";check(::symlink(original_key.c_str(),linked.c_str())==0,"key symlink fixture");
    bad=settings;bad.tls_private_key=linked.string();refused([&]{native::FtnNativeListener::open("127.0.0.1",0,true,bad);},"symlink key refused");
    const auto hard=root/"hard-key.pem";check(::link(original_key.c_str(),hard.c_str())==0,"hard-linked key fixture");
    refused([&]{native::FtnNativeListener::open("127.0.0.1",0,true,settings);},"multiply linked key refused");
    check(::unlink(hard.c_str())==0,"remove only test-created hard link");
    const auto fifo=root/"fifo-key.pem";check(::mkfifo(fifo.c_str(),0600)==0,"FIFO key fixture");
    bad=settings;bad.tls_private_key=fifo.string();const auto fifo_start=Clock::now();
    refused([&]{native::FtnNativeListener::open("127.0.0.1",0,true,bad);},"FIFO key fails without blocking");
    check(Clock::now()-fifo_start<std::chrono::seconds(1),"FIFO bounded refusal");
    check(::chmod(std::filesystem::path(settings.tls_certificate).c_str(),0644)==0,"public read-only certificate fixture");
    auto copied_cert=native::FtnNativeListener::open("127.0.0.1",0,true,settings);
    check(copied_cert->port()!=0,"read-only public certificate allowed with private key");
    const auto after_failure=exchange(*classic,clear);check(after_failure.server_bytes=="ping"&&after_failure.client_bytes=="pong","failed TLS activations leave unrelated classic listener available");
    const auto saved_port=classic->port();
    refused([&]{native::FtnNativeListener::open("127.0.0.1",saved_port,false,settings);},"reuse policy still refuses a duplicate live listener");
    classic.reset();classic=native::FtnNativeListener::open("127.0.0.1",saved_port,false,settings);
    check(classic->port()==saved_port,"closed completed-session listener rebinds its saved port immediately");
    const auto rebound=exchange(*classic,clear);check(rebound.server_bytes=="ping"&&rebound.client_bytes=="pong","rebound listener exchanges real bytes after prior sessions close");
    const auto before=Clock::now();refused([&]{classic->accept(std::chrono::milliseconds(100),[]{return false;});},"cancel interrupts native accept");check(Clock::now()-before<std::chrono::milliseconds(500),"cancel is bounded without abandoned socket worker");
    std::cout<<"PASS "<<checks<<" actual Linux socket/TLS loopback checks; no BinkP session/Runtime/hub claim\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<" errno="<<errno<<'\n';return 1;}}
