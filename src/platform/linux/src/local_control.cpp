#include "sfx/platform/linux/local_control.hpp"
#include "sfx/platform/network.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

namespace sfx::platform::linux_host {
namespace {
namespace app=application;
namespace net=platform::network;
struct WipeBytes {
    std::vector<std::uint8_t>& bytes;
    ~WipeBytes() { volatile std::uint8_t* p=bytes.data(); for (std::size_t i=0;i<bytes.size();++i) p[i]=0; }
};
bool owner_peer(int socket) {
    ucred peer{};socklen_t size=sizeof(peer);
    return geteuid()!=0 && getsockopt(socket,SOL_SOCKET,SO_PEERCRED,&peer,&size)==0 &&
        size==sizeof(peer) && peer.uid==geteuid() && peer.pid>0;
}
bool ready(int socket, short events, std::uint64_t deadline, const std::atomic<bool>& stop) {
    while (!stop.load() && net::monotonic_millis()<deadline) {
        pollfd item{socket,events,0};
        const auto result=poll(&item,1,25);
        if (result<0 && errno==EINTR) continue;
        if (result<0) return false;
        if (result>0) return (item.revents&events)!=0;
    }
    return false;
}
bool transfer(int socket,std::uint8_t* bytes,std::size_t size,bool sending,
    std::uint64_t deadline,const std::atomic<bool>& stop) {
    std::size_t offset{};
    while (offset<size) {
        if (!ready(socket,sending?POLLOUT:POLLIN,deadline,stop)) return false;
        const auto count=sending ? send(socket,bytes+offset,size-offset,MSG_NOSIGNAL) : recv(socket,bytes+offset,size-offset,0);
        if (count<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (count<=0) return false;
        offset+=static_cast<std::size_t>(count);
    }
    return true;
}
std::optional<app::LocalControlFrame> receive_frame(int socket,std::uint64_t deadline,const std::atomic<bool>& stop) {
    std::vector<std::uint8_t> bytes(app::kLocalControlFrameHeaderBytes);
    WipeBytes wipe{bytes};
    if (!transfer(socket,bytes.data(),bytes.size(),false,deadline,stop)) return std::nullopt;
    // Validate the complete header through the shared decoder before allocating
    // its bounded body. A synthetic zero-length copy needs no second parser.
    std::uint32_t size{};
    for (unsigned i=0;i<4;++i) size|=static_cast<std::uint32_t>(bytes[16+i])<<(i*8U);
    if (size>app::kLocalControlMaximumPayloadBytes) return std::nullopt;
    auto header=bytes;
    std::fill(header.begin()+16,header.end(),0);
    if (!app::decode_local_control_frame(header).frame) return std::nullopt;
    bytes.resize(app::kLocalControlFrameHeaderBytes+size);
    if (!transfer(socket,bytes.data()+app::kLocalControlFrameHeaderBytes,size,false,deadline,stop)) return std::nullopt;
    return app::decode_local_control_frame(bytes).frame;
}
bool send_frame(int socket,const app::LocalControlFrame& frame,std::uint64_t deadline,const std::atomic<bool>& stop) {
    auto bytes=app::encode_local_control_frame(frame);
    WipeBytes wipe{bytes};
    return !bytes.empty() && transfer(socket,bytes.data(),bytes.size(),true,deadline,stop);
}
std::optional<sockaddr_un> address(std::string_view name) {
    if (name.size()!=36 || name.substr(0,4)!="sfx-" ||
        name.find_first_not_of("0123456789abcdef",4)!=std::string_view::npos) return std::nullopt;
    sockaddr_un value{};value.sun_family=AF_UNIX;
    std::memcpy(value.sun_path+1,name.data(),name.size());
    return value;
}
constexpr socklen_t address_size=static_cast<socklen_t>(offsetof(sockaddr_un,sun_path)+1+36);
std::string fresh_name() {
    LinuxSecureIdentifierGenerator generator;
    const auto identifier=generator.generate();
    constexpr char digits[]="0123456789abcdef";
    std::string name="sfx-";
    for (auto byte:identifier.bytes) { name+=digits[byte>>4U];name+=digits[byte&15U]; }
    return name;
}
std::string directory_binding(int descriptor) {
    struct stat info{};
    if (fstat(descriptor,&info)!=0) return {};
    return std::to_string(static_cast<std::uint64_t>(info.st_dev))+"\n"+
        std::to_string(static_cast<std::uint64_t>(info.st_ino))+"\n";
}
bool selected_directory(int pinned,const std::filesystem::path& path) {
    try {
        auto current=open_owned_directory(path);
        const auto binding=directory_binding(pinned);
        return !binding.empty() && binding==directory_binding(current.get());
    } catch (...) { return false; }
}
}

LinuxLocalControlServer::LinuxLocalControlServer(std::filesystem::path directory):directory_(std::move(directory)) {}
LinuxLocalControlServer::~LinuxLocalControlServer() { close(); }
bool LinuxLocalControlServer::open() {
    if (listener_ || ownership_ || geteuid()==0) return false;
    try {
        directory_pin_=open_owned_directory(directory_);
        if (!ownership_.acquire(directory_,"control.lock")) return false;
        auto current=open_owned_directory(directory_);
        const auto binding=directory_binding(directory_pin_.get());
        if (binding.empty() || binding!=directory_binding(current.get())) {close();return false;}
        const auto name=fresh_name();const auto endpoint=address(name);
        FileDescriptor candidate(socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
        if (!candidate || !endpoint || bind(candidate.get(),reinterpret_cast<const sockaddr*>(&*endpoint),address_size)!=0 ||
            listen(candidate.get(),16)!=0) { close();return false; }
        // Ephemeral connection binding only, regenerated on each start. A
        // copied backup must never connect to the original board's live server.
        const std::string record="SFX_LOCAL_CONTROL=1\n"+name+"\n"+binding;
        publish_private_file(directory_pin_.get(),"control.endpoint",{record.begin(),record.end()},true);
        listener_=std::move(candidate);
        return true;
    } catch (...) { close();return false; }
}
bool LinuxLocalControlServer::serve_one(const std::atomic<bool>& stop,const Handler& handler) {
    if (!listener_ || !handler || stop.load() || !selected_directory(directory_pin_.get(),directory_)) return false;
    if (!ready(listener_.get(),POLLIN,net::monotonic_millis()+100,stop)) return false;
    FileDescriptor client(accept4(listener_.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK));
    if (!client || !owner_peer(client.get())) return false;
    auto request=receive_frame(client.get(),net::monotonic_millis()+5000,stop);
    if (!request || stop.load()) return false;
    WipeBytes wipe_request{request->payload};
    if (!selected_directory(directory_pin_.get(),directory_)) return false;
    try {
        auto response=handler(*request);
        if (!response) return false;
        WipeBytes wipe_response{response->payload};
        if (response->request_id!=request->request_id ||
            response->version.major!=app::kAlphaLocalControlVersion.major ||
            response->version.minor!=app::kAlphaLocalControlVersion.minor) return false;
        return send_frame(client.get(),*response,net::monotonic_millis()+5000,stop);
    } catch (...) { return false; }
}
void LinuxLocalControlServer::close() noexcept {
    listener_=FileDescriptor{};
    directory_pin_=FileDescriptor{};
    ownership_.release();
    // Retain the private endpoint record. The abstract listener is gone; a new
    // lock owner atomically publishes its new endpoint, with no stale unlink.
}

std::optional<app::LocalControlFrame> exchange_local_control(const std::filesystem::path& directory,
    const app::LocalControlFrame& request,app::LocalControlMessageType response_type,
    const std::atomic<bool>& stop,std::uint32_t timeout_millis) {
    if (geteuid()==0 || timeout_millis==0 || timeout_millis>30000 || stop.load()) return std::nullopt;
    try {
        auto pinned=open_owned_directory(directory);
        const auto bytes=read_private_file(pinned.get(),"control.endpoint",128);
        const std::string record(bytes.begin(),bytes.end());
        constexpr std::string_view prefix="SFX_LOCAL_CONTROL=1\n";
        const auto binding=directory_binding(pinned.get());
        if (binding.empty() || record.size()!=prefix.size()+37+binding.size() ||
            record.compare(0,prefix.size(),prefix)!=0 || record[prefix.size()+36]!='\n' ||
            record.compare(prefix.size()+37,binding.size(),binding)!=0) return std::nullopt;
        const auto endpoint=address(std::string_view(record).substr(prefix.size(),36));
        if (!endpoint) return std::nullopt;
        FileDescriptor client(socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
        if (!client) return std::nullopt;
        const auto deadline=net::monotonic_millis()+timeout_millis;
        if (connect(client.get(),reinterpret_cast<const sockaddr*>(&*endpoint),address_size)!=0) {
            if (errno!=EINPROGRESS || !ready(client.get(),POLLOUT,deadline,stop)) return std::nullopt;
            int error{};socklen_t size=sizeof(error);
            if (getsockopt(client.get(),SOL_SOCKET,SO_ERROR,&error,&size)!=0 || error) return std::nullopt;
        }
        if (!owner_peer(client.get()) || !selected_directory(pinned.get(),directory) ||
            !send_frame(client.get(),request,deadline,stop)) return std::nullopt;
        auto response=receive_frame(client.get(),deadline,stop);
        if (!response) return std::nullopt;
        if (response->request_id!=request.request_id || response->type!=response_type ||
            !selected_directory(pinned.get(),directory)) {
            WipeBytes wipe{response->payload};return std::nullopt;
        }
        return response;
    } catch (...) { return std::nullopt; }
}
} // namespace sfx::platform::linux_host
