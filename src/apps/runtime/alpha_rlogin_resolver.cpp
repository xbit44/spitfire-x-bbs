// Windows7 asynchronous resolver boundary. No synchronous getaddrinfo and no
// abandoned per-caller worker. User cancellation releases a subscription; it
// cannot cancel a lookup shared with another caller or poison a native buffer.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include "alpha_rlogin_resolver.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <climits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace sfx::apps::runtime {
namespace {
constexpr std::size_t capacity=128;
constexpr UINT first_message=WM_APP+91;
struct NativeBuffer {
    alignas(void*) std::array<char,MAXGETHOSTSTRUCT> bytes{};
    std::array<char,256> name{};
};
// Process-image storage. At final close, uncertain native cancellations never
// free, reset, protect or reuse these buffers, even after the coordinator joins.
std::array<NativeBuffer,capacity> native{};
std::atomic<unsigned> workers{},uncertain_requests{};
using Address=std::array<std::uint8_t,4>;
std::vector<Address> addresses(std::size_t slot,LPARAM packed){
    std::vector<Address> result;
    const auto bytes=static_cast<std::size_t>(WSAGETASYNCBUFLEN(packed));
    if(WSAGETASYNCERROR(packed)||bytes<sizeof(hostent)||bytes>MAXGETHOSTSTRUCT)return result;
    hostent header{};std::memcpy(&header,native[slot].bytes.data(),sizeof(header));
    const auto base=reinterpret_cast<std::uintptr_t>(native[slot].bytes.data());
    auto pointer=reinterpret_cast<std::uintptr_t>(header.h_addr_list);
    if(header.h_addrtype!=AF_INET||header.h_length!=4||pointer<base+sizeof(header))return result;
    for(unsigned n=0;n<16&&pointer<=base+bytes&&sizeof(char*)<=base+bytes-pointer;++n,pointer+=sizeof(char*)){
        char* value{};std::memcpy(&value,reinterpret_cast<void*>(pointer),sizeof(value));if(!value)break;
        const auto position=reinterpret_cast<std::uintptr_t>(value);
        if(position<base+sizeof(header)||position>base+bytes||base+bytes-position<4)return {};
        Address address{};std::memcpy(address.data(),value,address.size());result.push_back(address);
    }
    return result;
}
enum class State {free,queued,pending,complete};
struct Request {
    State state{State::free};std::string host;std::uint64_t generation{};
    unsigned subscribers{};HANDLE native_handle{};bool queue_drained{};
    std::vector<Address> result; // at most sixteen addresses
};
struct Token {std::size_t slot{};std::uint64_t generation{};explicit operator bool()const{return generation!=0;}};
class Resolver final {
public:
    ~Resolver(){close();}
    Token subscribe(std::string host){
        std::lock_guard<std::mutex> lock(mutex_);
        if(closed_||failed_||generation_==UINT64_MAX)return {};
        if(!thread_.joinable()){
            stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);work_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(!stop_||!work_){failed_=true;return {};}
            try{thread_=std::thread([this]{run();});}catch(...){failed_=true;return {};}
        }
        for(std::size_t n=0;n<requests_.size();++n){auto& r=requests_[n];if(r.state!=State::free&&r.host==host){if(r.subscribers==UINT_MAX)return {};++r.subscribers;return {n,r.generation};}}
        for(std::size_t n=0;n<requests_.size();++n){auto& r=requests_[n];if(r.state==State::free){r={};r.host=std::move(host);r.generation=++generation_;r.subscribers=1;r.state=State::queued;SetEvent(work_);return {n,r.generation};}}
        return {};
    }
    std::optional<std::vector<Address>> poll(Token token){
        std::lock_guard<std::mutex> lock(mutex_);
        if(closed_||failed_||!token||token.slot>=capacity)return std::vector<Address>{};
        const auto& r=requests_[token.slot];if(r.generation!=token.generation||r.state==State::free)return std::vector<Address>{};
        if(r.state!=State::complete)return {};return r.result;
    }
    void release(Token token) noexcept {
        try{std::lock_guard<std::mutex> lock(mutex_);if(token&&token.slot<capacity){auto& r=requests_[token.slot];if(r.generation==token.generation&&r.subscribers)--r.subscribers;}if(work_)SetEvent(work_);}catch(...){}
    }
    void close() noexcept {
        try{
            std::lock_guard<std::mutex> closing(close_mutex_);
            {std::lock_guard<std::mutex> lock(mutex_);closed_=true;if(stop_)SetEvent(stop_);}
            if(thread_.joinable())thread_.join();
            std::lock_guard<std::mutex> lock(mutex_);if(stop_)CloseHandle(stop_);if(work_)CloseHandle(work_);stop_=work_=nullptr;
        }catch(...){}
    }
private:
    static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM w,LPARAM l){
        auto* self=reinterpret_cast<Resolver*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(self&&message>=first_message&&message<first_message+capacity){self->completed(message-first_message,reinterpret_cast<HANDLE>(w),l);return 0;}
        return DefWindowProcW(window,message,w,l);
    }
    void completed(std::size_t index,HANDLE handle,LPARAM packed) noexcept {
        try{std::lock_guard<std::mutex> lock(mutex_);auto& r=requests_[index];if(r.state!=State::pending||r.native_handle!=handle)return;
            r.result=addresses(index,packed);r.native_handle=nullptr;r.state=State::complete;r.queue_drained=false;
        }catch(...){std::lock_guard<std::mutex> lock(mutex_);failed_=true;}
    }
    void drain(HWND window){
        MSG message{};unsigned count{};
        while(count<capacity&&PeekMessageW(&message,window,first_message,first_message+static_cast<UINT>(capacity)-1,PM_REMOVE)){DispatchMessageW(&message);++count;}
        if(!PeekMessageW(&message,window,first_message,first_message+static_cast<UINT>(capacity)-1,PM_NOREMOVE)){
            std::lock_guard<std::mutex> lock(mutex_);for(auto& r:requests_)if(r.state==State::complete)r.queue_drained=true;
        }
    }
    void process(HWND window){
        std::lock_guard<std::mutex> lock(mutex_);
        for(std::size_t n=0;n<capacity;++n){auto& r=requests_[n];
            if((r.state==State::complete&&r.queue_drained&&!r.subscribers)||(r.state==State::queued&&!r.subscribers)){r={};continue;}
            if(r.state!=State::queued)continue;
            std::copy(r.host.begin(),r.host.end(),native[n].name.begin());native[n].name[r.host.size()]=0;
            r.native_handle=WSAAsyncGetHostByName(window,first_message+static_cast<UINT>(n),native[n].name.data(),native[n].bytes.data(),static_cast<int>(native[n].bytes.size()));
            r.state=r.native_handle?State::pending:State::complete;
            // Failed initiation never gives a provider ownership of the arena.
            if(!r.native_handle)r.queue_drained=true;
        }
    }
    void run() noexcept {
        ++workers;struct Count{~Count(){--workers;}} count;
        WSADATA data{};const bool winsock=WSAStartup(MAKEWORD(2,2),&data)==0;
        WNDCLASSW definition{};definition.lpfnWndProc=window_proc;definition.hInstance=GetModuleHandleW(nullptr);definition.lpszClassName=L"SFXOutboundRloginResolverBroker";
        const bool registered=winsock&&RegisterClassW(&definition)!=0;
        const auto window=registered?CreateWindowExW(0,definition.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,definition.hInstance,nullptr):nullptr;
        if(window){
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
            try{
                const HANDLE events[]{stop_,work_};
                while(WaitForSingleObject(stop_,0)!=WAIT_OBJECT_0){drain(window);process(window);if(MsgWaitForMultipleObjects(2,events,FALSE,1000,QS_POSTMESSAGE)==WAIT_FAILED){std::lock_guard<std::mutex> lock(mutex_);failed_=true;break;}}
            }catch(...){std::lock_guard<std::mutex> lock(mutex_);failed_=true;}
            {std::lock_guard<std::mutex> lock(mutex_);for(auto& r:requests_)if(r.state==State::pending&&r.native_handle){(void)WSACancelAsyncRequest(r.native_handle);++uncertain_requests;}}
            SetWindowLongPtrW(window,GWLP_USERDATA,0);DestroyWindow(window);
        }else{std::lock_guard<std::mutex> lock(mutex_);failed_=true;}
        if(winsock)WSACleanup();
    }
    std::mutex mutex_,close_mutex_;std::array<Request,capacity> requests_{};
    std::uint64_t generation_{};bool closed_{},failed_{};HANDLE stop_{},work_{};std::thread thread_;
};
Resolver& resolver(){static Resolver value;return value;}
}
std::vector<std::array<std::uint8_t,4>> resolve_alpha_rlogin_ipv4(std::string_view host,const std::function<bool()>& running){
    if(host.empty()||host.size()>253||!running||!running())return {};
    std::string key(host);for(auto& c:key){const auto byte=static_cast<unsigned char>(c);if(byte<33||byte>126)return {};if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');}
    auto& broker=resolver();const auto token=broker.subscribe(std::move(key));if(!token)return {};
    struct Release {Resolver& owner;Token token;~Release(){owner.release(token);}} release{broker,token};
    while(running()){if(auto result=broker.poll(token))return *result;Sleep(10);}return {};
}
void close_alpha_rlogin_process() noexcept{resolver().close();}
unsigned alpha_rlogin_resolver_worker_count() noexcept{return workers.load();}
unsigned alpha_rlogin_native_uncertain_requests() noexcept{return uncertain_requests.load();}
} // namespace sfx::apps::runtime
