#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include "sfx/platform/linux/ftn_native_transport.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <filesystem>
#include <stdexcept>

namespace sfx::platform::linux_host {
namespace {
using Clock=std::chrono::steady_clock;
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
struct Socket {
    int value{-1};
    Socket()=default;Socket(const Socket&)=delete;Socket& operator=(const Socket&)=delete;
    ~Socket(){if(value>=0)(void)::close(value);}
    int release(){const auto out=value;value=-1;return out;}
};
// SSL's default socket BIO can raise SIGPIPE on Linux. This transport owns a
// non-closing BIO using MSG_NOSIGNAL, without changing process signal policy.
const BIO_METHOD* socket_method(){
    struct Method {
        BIO_METHOD* value{BIO_meth_new(BIO_TYPE_SOURCE_SINK|BIO_get_new_index(),"SFX nonblocking socket")};
        Method(){
            require(value!=nullptr,"ftn_tls_bio_failed");
            const bool ok=BIO_meth_set_create(value,[](BIO* b){BIO_set_init(b,1);return 1;})==1&&
                BIO_meth_set_destroy(value,[](BIO*){return 1;})==1&&
                BIO_meth_set_read(value,[](BIO* b,char* out,int count){
                    BIO_clear_retry_flags(b);const auto fd=*static_cast<int*>(BIO_get_data(b));
                    ssize_t n;do{n=::recv(fd,out,static_cast<std::size_t>(count),0);}while(n<0&&errno==EINTR);
                    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))BIO_set_retry_read(b);
                    return static_cast<int>(n);
                })==1&&BIO_meth_set_write(value,[](BIO* b,const char* in,int count){
                    BIO_clear_retry_flags(b);const auto fd=*static_cast<int*>(BIO_get_data(b));
                    ssize_t n;do{n=::send(fd,in,static_cast<std::size_t>(count),MSG_NOSIGNAL);}while(n<0&&errno==EINTR);
                    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))BIO_set_retry_write(b);
                    return static_cast<int>(n);
                })==1&&BIO_meth_set_ctrl(value,[](BIO*,int command,long,void*)->long{return command==BIO_CTRL_FLUSH?1:0;})==1;
            if(!ok){BIO_meth_free(value);value=nullptr;throw std::runtime_error("ftn_tls_bio_failed");}
        }
        ~Method(){BIO_meth_free(value);}
    };
    static const Method method;return method.value;
}
using Context=std::shared_ptr<SSL_CTX>;
Context context(bool server){
    Context out(SSL_CTX_new(server?TLS_server_method():TLS_client_method()),SSL_CTX_free);require(bool(out),"ftn_tls_context_failed");
    require(SSL_CTX_set_min_proto_version(out.get(),TLS1_2_VERSION)==1&&SSL_CTX_set_max_proto_version(out.get(),TLS1_3_VERSION)==1,"ftn_tls_version_policy_failed");
    SSL_CTX_set_options(out.get(),SSL_OP_NO_COMPRESSION|SSL_OP_NO_RENEGOTIATION);return out;
}
struct PemBytes {std::vector<std::uint8_t> value;PemBytes()=default;PemBytes(PemBytes&& other)noexcept:value(std::move(other.value)){}PemBytes(const PemBytes&)=delete;~PemBytes(){if(!value.empty())OPENSSL_cleanse(value.data(),value.size());}};
PemBytes read_pem(std::string_view configured,std::size_t maximum,bool secret=false){
    require(!configured.empty()&&configured.size()<=1024&&configured.find('\0')==std::string_view::npos,"ftn_tls_file_path_invalid");
    const auto path=std::filesystem::u8path(configured);
    require(path.is_absolute()&&path.lexically_normal()==path&&!path.filename().empty(),"ftn_tls_file_path_invalid");
    auto parent=open_owned_directory(path.parent_path(),false);
    FileDescriptor fd(openat(parent.get(),path.filename().c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
    struct stat before{},after{},named{};
    require(fd&&fstat(fd.get(),&before)==0&&S_ISREG(before.st_mode)&&before.st_uid==geteuid()&&
        before.st_nlink==1&&(before.st_mode&0022)==0&&(!secret||(before.st_mode&0077)==0)&&
        before.st_size>0&&static_cast<std::uint64_t>(before.st_size)<=maximum,"ftn_tls_file_unavailable");
    PemBytes out;out.value.resize(static_cast<std::size_t>(before.st_size));std::size_t at{};
    while(at<out.value.size()){
        const auto n=pread(fd.get(),out.value.data()+at,out.value.size()-at,static_cast<off_t>(at));
        if(n<0&&errno==EINTR)continue;
        require(n>0,"ftn_tls_file_read_failed");at+=static_cast<std::size_t>(n);
    }
    require(fstat(fd.get(),&after)==0&&fstatat(parent.get(),path.filename().c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&
        before.st_dev==named.st_dev&&before.st_ino==named.st_ino&&before.st_mode==after.st_mode&&
        before.st_uid==after.st_uid&&before.st_gid==after.st_gid&&after.st_nlink==1&&before.st_size==after.st_size&&
        before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&
        before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec,"ftn_tls_file_changed");
    // A replaced parent cannot make an unrelated namespace authoritative.
    auto current=open_owned_directory(path.parent_path(),false);struct stat a{},b{};
    require(fstat(parent.get(),&a)==0&&fstat(current.get(),&b)==0&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino,"ftn_tls_parent_changed");
    return out;
}
using Bio=std::unique_ptr<BIO,decltype(&BIO_free)>;
Bio bio(const PemBytes& bytes){return Bio(BIO_new_mem_buf(bytes.value.data(),static_cast<int>(bytes.value.size())),BIO_free);}
int no_password(char*,int,int,void*){return 0;}
Context server_context(const application::FtnSettings& settings){
    auto out=context(true);const auto certificates=read_pem(settings.tls_certificate,1024*1024);auto input=bio(certificates);require(bool(input),"ftn_tls_certificate_failed");
    std::unique_ptr<X509,decltype(&X509_free)> leaf(PEM_read_bio_X509(input.get(),nullptr,nullptr,nullptr),X509_free);require(leaf&&SSL_CTX_use_certificate(out.get(),leaf.get())==1,"ftn_tls_certificate_failed");
    unsigned count{};for(;;){X509* next=PEM_read_bio_X509(input.get(),nullptr,nullptr,nullptr);if(!next){ERR_clear_error();break;}if(++count>16||SSL_CTX_add_extra_chain_cert(out.get(),next)!=1){X509_free(next);throw std::runtime_error("ftn_tls_certificate_chain_failed");}}
    const auto key_bytes=read_pem(settings.tls_private_key,1024*1024,true);auto key_input=bio(key_bytes);require(bool(key_input),"ftn_tls_key_failed");
    std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(PEM_read_bio_PrivateKey(key_input.get(),nullptr,no_password,nullptr),EVP_PKEY_free);
    require(key&&SSL_CTX_use_PrivateKey(out.get(),key.get())==1&&SSL_CTX_check_private_key(out.get())==1,"ftn_tls_key_mismatch_or_unreadable");return out;
}
Context client_context(const application::FtnLink& link){
    auto out=context(false);SSL_CTX_set_verify(out.get(),SSL_VERIFY_PEER,nullptr);auto* store=SSL_CTX_get_cert_store(out.get());unsigned added{};
    const auto add=[&](X509* certificate){if(X509_STORE_add_cert(store,certificate)!=1){require(ERR_GET_REASON(ERR_peek_last_error())==X509_R_CERT_ALREADY_IN_HASH_TABLE,"ftn_tls_trust_add_failed");ERR_clear_error();}++added;};
    if(!link.tls_ca_file.empty()){
        const auto ca=read_pem(link.tls_ca_file,4*1024*1024);auto input=bio(ca);require(bool(input),"ftn_tls_ca_failed");
        for(;;){std::unique_ptr<X509,decltype(&X509_free)> certificate(PEM_read_bio_X509(input.get(),nullptr,nullptr,nullptr),X509_free);if(!certificate){ERR_clear_error();break;}require(added<256,"ftn_tls_ca_limit");add(certificate.get());}
    }else{
        // Read system trust only; never use caller-controlled SSL_CERT_FILE or
        // the build machine's private OpenSSL installation directory.
        for(const auto* path:{"/etc/ssl/certs/ca-certificates.crt","/etc/pki/tls/certs/ca-bundle.crt"}){
            struct stat info{};
            if(::stat(path,&info)==0&&S_ISREG(info.st_mode)&&info.st_uid==0&&(info.st_mode&0022)==0&&
                SSL_CTX_load_verify_locations(out.get(),path,nullptr)==1){++added;break;}
            ERR_clear_error();
        }
    }
    require(added>0,"ftn_tls_trust_empty");return out;
}
void nonblocking(int socket){
    const auto flags=fcntl(socket,F_GETFL);require(flags>=0&&fcntl(socket,F_SETFL,flags|O_NONBLOCK)==0,"ftn_socket_nonblocking_failed");
    const int one=1;require(setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))==0,"ftn_socket_nodelay_failed");
}
bool wait_socket(int socket,bool writing,Clock::time_point deadline,const std::function<bool()>& running){
    for(;;){
        require(running&&running(),"ftn_socket_cancelled");const auto now=Clock::now();if(now>=deadline)return false;
        const auto remaining=std::min(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now),std::chrono::milliseconds(100));
        pollfd selected{socket,static_cast<short>(writing?POLLOUT:POLLIN),0};
        const auto result=::poll(&selected,1,static_cast<int>(std::max<std::int64_t>(1,remaining.count())));
        if(result<0&&errno==EINTR)continue;
        require(result>=0&&(selected.revents&POLLNVAL)==0,"ftn_socket_wait_failed");
        if(result>0)return true; // read/write reports EOF or precise socket error.
    }
}
}
struct FtnNativeChannel::Impl {
    Socket socket;Context context;std::unique_ptr<SSL,decltype(&SSL_free)> ssl{nullptr,SSL_free};bool outbound{},ready{},verified{};
    std::string network,link,host,server_name,ca_file;std::uint16_t port{};bool configured_tls{};
    explicit Impl(Socket& held,Context ctx={},bool client=false):context(std::move(ctx)),outbound(client){socket.value=held.release();nonblocking(socket.value);ready=!context;
        if(context){ssl.reset(SSL_new(context.get()));require(bool(ssl),"ftn_tls_session_failed");auto* transport=BIO_new(socket_method());require(transport!=nullptr,"ftn_tls_bio_failed");BIO_set_data(transport,&socket.value);SSL_set_bio(ssl.get(),transport,transport);if(client)SSL_set_connect_state(ssl.get());else SSL_set_accept_state(ssl.get());}}
    bool ssl_wait(int result,Clock::time_point deadline,const std::function<bool()>& running){const auto error=SSL_get_error(ssl.get(),result);require(error==SSL_ERROR_WANT_READ||error==SSL_ERROR_WANT_WRITE,"ftn_tls_exchange_failed");return wait_socket(socket.value,error==SSL_ERROR_WANT_WRITE,deadline,running);}
};
FtnNativeChannel::FtnNativeChannel(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
FtnNativeChannel::~FtnNativeChannel()=default;
std::unique_ptr<FtnNativeChannel> FtnNativeChannel::connect_ipv4(const std::array<std::uint8_t,4>& address,std::uint16_t port,const application::FtnLink& link,const std::function<bool()>& running){
    require(port!=0&&port==link.port,"ftn_socket_port_policy_mismatch");Socket socket;socket.value=::socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP);require(socket.value!=-1,"ftn_socket_create_failed");nonblocking(socket.value);
    sockaddr_in target{};target.sin_family=AF_INET;target.sin_port=htons(port);std::copy(address.begin(),address.end(),reinterpret_cast<std::uint8_t*>(&target.sin_addr));
    const auto result=::connect(socket.value,reinterpret_cast<const sockaddr*>(&target),sizeof(target));if(result==-1){require(errno==EINPROGRESS||errno==EINTR,"ftn_socket_connect_failed");require(wait_socket(socket.value,true,Clock::now()+std::chrono::seconds(15),running),"ftn_socket_connect_timeout");int error{};socklen_t size=sizeof(error);require(getsockopt(socket.value,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size)==0&&error==0,"ftn_socket_connect_failed");}
    auto impl=std::make_unique<Impl>(socket,link.tls?client_context(link):Context{},true);
    impl->network=link.network;impl->link=link.code;impl->host=link.host;impl->port=port;impl->configured_tls=link.tls;impl->server_name=link.tls_server_name;impl->ca_file=link.tls_ca_file;
    if(link.tls){const auto name=link.tls_server_name.empty()?link.host:link.tls_server_name;require(!name.empty()&&name.size()<=253,"ftn_tls_server_name_invalid");auto* parameters=SSL_get0_param(impl->ssl.get());X509_VERIFY_PARAM_set_hostflags(parameters,X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);std::array<unsigned char,16> numeric{};
        if(inet_pton(AF_INET,name.c_str(),numeric.data())==1||inet_pton(AF_INET6,name.c_str(),numeric.data())==1)require(X509_VERIFY_PARAM_set1_ip_asc(parameters,name.c_str())==1,"ftn_tls_server_name_invalid");
        else require(SSL_set1_host(impl->ssl.get(),name.c_str())==1&&SSL_set_tlsext_host_name(impl->ssl.get(),name.c_str())==1,"ftn_tls_server_name_invalid");
    }
    auto out=std::unique_ptr<FtnNativeChannel>(new FtnNativeChannel(std::move(impl)));out->handshake(running);return out;
}
void FtnNativeChannel::handshake(const std::function<bool()>& running){
    require(running&&running(),"ftn_socket_cancelled");if(impl_->ready)return;const auto deadline=Clock::now()+std::chrono::seconds(15);
    for(;;){ERR_clear_error();const auto result=SSL_do_handshake(impl_->ssl.get());if(result==1)break;require(impl_->ssl_wait(result,deadline,running),"ftn_tls_handshake_timeout");}
    if(impl_->outbound){require(SSL_get_verify_result(impl_->ssl.get())==X509_V_OK&&SSL_get0_peer_certificate(impl_->ssl.get())!=nullptr,"ftn_tls_server_unverified");impl_->verified=true;}impl_->ready=true;
}
FtnReadResult FtnNativeChannel::read(std::chrono::milliseconds wait,const std::function<bool()>& running){
    require(impl_->ready&&wait.count()>0&&wait<=std::chrono::seconds(1),"ftn_socket_read_policy");const auto deadline=Clock::now()+wait;std::array<char,32768> bytes{};
    struct EraseRead {std::array<char,32768>& value;~EraseRead(){OPENSSL_cleanse(value.data(),value.size());}} erase{bytes};
    for(;;){require(running&&running(),"ftn_socket_cancelled");if(impl_->ssl){std::size_t count{};ERR_clear_error();const auto result=SSL_read_ex(impl_->ssl.get(),bytes.data(),bytes.size(),&count);if(result==1)return {FtnReadOutcome::data,std::string(bytes.data(),count)};const auto error=SSL_get_error(impl_->ssl.get(),result);if(error==SSL_ERROR_ZERO_RETURN)return {FtnReadOutcome::closed,{}};require(error==SSL_ERROR_WANT_READ||error==SSL_ERROR_WANT_WRITE,"ftn_tls_read_failed");if(!wait_socket(impl_->socket.value,error==SSL_ERROR_WANT_WRITE,deadline,running))return {};}
        else {const auto count=recv(impl_->socket.value,bytes.data(),static_cast<int>(bytes.size()),0);if(count>0)return {FtnReadOutcome::data,std::string(bytes.data(),static_cast<std::size_t>(count))};if(count==0)return {FtnReadOutcome::closed,{}};require((errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR),"ftn_socket_read_failed");if(!wait_socket(impl_->socket.value,false,deadline,running))return {};}
    }
}
void FtnNativeChannel::write(std::string_view bytes,const std::function<bool()>& running){
    require(impl_->ready&&!bytes.empty()&&bytes.size()<=65536,"ftn_socket_write_policy");const auto deadline=Clock::now()+std::chrono::seconds(15);
    while(!bytes.empty()){require(running&&running(),"ftn_socket_cancelled");if(impl_->ssl){std::size_t count{};ERR_clear_error();const auto result=SSL_write_ex(impl_->ssl.get(),bytes.data(),bytes.size(),&count);if(result==1){require(count>0,"ftn_tls_write_failed");bytes.remove_prefix(count);}else require(impl_->ssl_wait(result,deadline,running),"ftn_tls_write_timeout");}
        else {const auto count=send(impl_->socket.value,bytes.data(),static_cast<int>(bytes.size()),MSG_NOSIGNAL);if(count>0)bytes.remove_prefix(static_cast<std::size_t>(count));else{require(count==-1&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR),"ftn_socket_write_failed");require(wait_socket(impl_->socket.value,true,deadline,running),"ftn_socket_write_timeout");}}
    }
}
bool FtnNativeChannel::verified_server_tls()const noexcept{return impl_->ready&&impl_->verified;}
bool FtnNativeChannel::encrypted()const noexcept{return impl_->ready&&bool(impl_->ssl);}
bool FtnNativeChannel::matches_outbound_link(const application::FtnLink& link)const noexcept{return impl_->outbound&&impl_->ready&&impl_->network==link.network&&impl_->link==link.code&&impl_->host==link.host&&impl_->port==link.port&&impl_->configured_tls==link.tls&&impl_->server_name==link.tls_server_name&&impl_->ca_file==link.tls_ca_file;}
struct FtnNativeListener::Impl {Socket socket;Context context;std::uint16_t port{};};
FtnNativeListener::FtnNativeListener(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
FtnNativeListener::~FtnNativeListener()=default;
std::unique_ptr<FtnNativeListener> FtnNativeListener::open(std::string_view bind,std::uint16_t port,bool tls,const application::FtnSettings& settings){
    require(!bind.empty()&&bind.size()<=64,"ftn_listener_bind_invalid");auto impl=std::make_unique<Impl>();if(tls)impl->context=server_context(settings);
    const std::string text(bind);sockaddr_storage address{};socklen_t size{};auto* ipv4=reinterpret_cast<sockaddr_in*>(&address);auto* ipv6=reinterpret_cast<sockaddr_in6*>(&address);
    if(inet_pton(AF_INET,text.c_str(),&ipv4->sin_addr)==1){ipv4->sin_family=AF_INET;ipv4->sin_port=htons(port);size=sizeof(*ipv4);}else {require(inet_pton(AF_INET6,text.c_str(),&ipv6->sin6_addr)==1,"ftn_listener_bind_invalid");ipv6->sin6_family=AF_INET6;ipv6->sin6_port=htons(port);size=sizeof(*ipv6);}
    impl->socket.value=::socket(address.ss_family,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP);require(impl->socket.value!=-1,"ftn_listener_socket_failed");// No SO_REUSEPORT or shared live listener ownership.
    // Joined configuration changes may leave completed sessions in TIME_WAIT.
    // Permit rebinding the retired endpoint, while bind/listen still rejects
    // another live listener. Never enable SO_REUSEPORT.
    int reuse=1;require(setsockopt(impl->socket.value,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse))==0,"ftn_listener_reuse_policy_failed");
    if(address.ss_family==AF_INET6){int only=1;require(setsockopt(impl->socket.value,IPPROTO_IPV6,IPV6_V6ONLY,reinterpret_cast<const char*>(&only),sizeof(only))==0,"ftn_listener_ipv6_policy_failed");}
    require(::bind(impl->socket.value,reinterpret_cast<const sockaddr*>(&address),size)==0&&listen(impl->socket.value,4)==0,"ftn_listener_port_in_use_or_unavailable");nonblocking(impl->socket.value);socklen_t actual=sizeof(address);require(getsockname(impl->socket.value,reinterpret_cast<sockaddr*>(&address),&actual)==0,"ftn_listener_port_failed");impl->port=ntohs(address.ss_family==AF_INET?reinterpret_cast<sockaddr_in*>(&address)->sin_port:reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);return std::unique_ptr<FtnNativeListener>(new FtnNativeListener(std::move(impl)));
}
std::uint16_t FtnNativeListener::port()const noexcept{return impl_->port;}
std::unique_ptr<FtnNativeChannel> FtnNativeListener::accept(std::chrono::milliseconds wait,const std::function<bool()>& running){
    require(wait.count()>0&&wait<=std::chrono::seconds(1),"ftn_listener_wait_policy");if(!wait_socket(impl_->socket.value,false,Clock::now()+wait,running))return {};Socket accepted;accepted.value=::accept4(impl_->socket.value,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);if(accepted.value==-1){require((errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR),"ftn_listener_accept_failed");return {};}
    return std::unique_ptr<FtnNativeChannel>(new FtnNativeChannel(std::make_unique<FtnNativeChannel::Impl>(accepted,impl_->context,false)));
}
}
