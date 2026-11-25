#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include "alpha_verified_domain.hpp"
#include <array>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace sfx::apps::runtime {
namespace {
constexpr UINT completed_message=WM_APP+12;
struct Native {
    alignas(16) std::array<char,MAXGETHOSTSTRUCT> bytes{};
    std::array<char,4> ip{};
    std::array<char,254> name{};
    std::size_t returned_bytes{};
};
// Uncertain native cancellation never frees or reuses its storage. A late
// completion can release the slot; process teardown is the final lifetime.
std::array<Native,4> arena{};
struct Query {
    std::uint32_t ip{};
    std::uint64_t deadline{};
    std::mutex mutex; std::condition_variable changed;
    bool done{}; VerifiedDomain result;
};
struct Slot { std::shared_ptr<Query> query; HANDLE task{}; bool forward{},quarantined{}; };
bool within(const Native& b,const void* pointer,std::size_t length) {
    const auto p=reinterpret_cast<std::uintptr_t>(pointer),first=reinterpret_cast<std::uintptr_t>(b.bytes.data());
    return p>=first && p<=first+b.returned_bytes && length<=first+b.returned_bytes-p;
}
void finish(const std::shared_ptr<Query>& q,VerifiedDomain result) {
    if (!q) return;
    std::lock_guard<std::mutex> lock(q->mutex);
    if (!q->done) { q->result=std::move(result); q->done=true; q->changed.notify_one(); }
}
class Resolver {
public:
    Resolver() { thread_=std::thread([this] { run(); }); }
    ~Resolver() { closing_=true; if (thread_.joinable()) thread_.join(); }
    VerifiedDomain resolve(std::uint32_t ip,const std::atomic<bool>& stop) {
        auto q=std::make_shared<Query>(); q->ip=ip; q->deadline=GetTickCount64()+2000;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (failed_ || pending_) return {};
            pending_=q;
        }
        std::unique_lock<std::mutex> lock(q->mutex);
        while (!q->done && !stop.load() && GetTickCount64()<q->deadline)
            q->changed.wait_for(lock,std::chrono::milliseconds(20));
        return q->done ? q->result : VerifiedDomain{};
    }
private:
    static LRESULT CALLBACK dispatch(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        auto* self=reinterpret_cast<Resolver*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if (message==WM_NCCREATE) {
            self=static_cast<Resolver*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message>=completed_message && message<completed_message+arena.size()) {
            self->complete(message-completed_message,reinterpret_cast<HANDLE>(wparam),lparam); return 0;
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
    void complete(std::size_t i,HANDLE task,LPARAM packed) {
        auto& slot=slots_[i]; auto& native=arena[i];
        if (slot.task!=task) return;
        slot.task=nullptr;
        if (slot.quarantined) { slot={}; return; }
        const auto q=slot.query;
        if (!q || GetTickCount64()>=q->deadline) { finish(q,{}); slot={}; return; }
        const int error=WSAGETASYNCERROR(packed);
        if (error) {
            finish(q,{error==WSAHOST_NOT_FOUND || error==WSANO_DATA,{}}); slot={}; return;
        }
        if (WSAGETASYNCBUFLEN(packed)<sizeof(hostent) || WSAGETASYNCBUFLEN(packed)>native.bytes.size()) {
            finish(q,{}); slot={}; return;
        }
        native.returned_bytes=WSAGETASYNCBUFLEN(packed);
        hostent entry{}; std::memcpy(&entry,native.bytes.data(),sizeof(entry));
        if (entry.h_addrtype!=AF_INET || entry.h_length!=4) { finish(q,{}); slot={}; return; }
        if (!slot.forward) {
            if (!within(native,entry.h_name,1)) { finish(q,{}); slot={}; return; }
            std::string name;
            for (std::size_t n=0;n<native.name.size();++n) {
                if (!within(native,entry.h_name+n,1)) { finish(q,{}); slot={}; return; }
                char c=entry.h_name[n]; if (!c) break;
                if (c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
                if (!((c>='a'&&c<='z') || (c>='0'&&c<='9') || c=='.' || c=='-')) {
                    finish(q,{true,{}}); slot={}; return;
                }
                name+=c;
            }
            if (!name.empty() && name.back()=='.') name.pop_back();
            if (name.empty() || name.size()>=native.name.size()-1) { finish(q,{true,{}}); slot={}; return; }
            std::copy(name.begin(),name.end(),native.name.begin()); native.name[name.size()]=0;
            slot.forward=true;
            slot.task=WSAAsyncGetHostByName(window_,completed_message+static_cast<UINT>(i),native.name.data(),
                native.bytes.data(),static_cast<int>(native.bytes.size()));
            if (!slot.task) { finish(q,{}); slot={}; }
        } else {
            if (!within(native,entry.h_addr_list,sizeof(char*))) { finish(q,{}); slot={}; return; }
            bool matched=false,terminated=false;
            for (unsigned n=0;n<64;++n) {
                if (!within(native,entry.h_addr_list+n,sizeof(char*))) break;
                char* address{}; std::memcpy(&address,entry.h_addr_list+n,sizeof(address));
                if (!address) { terminated=true; break; }
                if (!within(native,address,4)) break;
                matched=matched || std::memcmp(address,native.ip.data(),4)==0;
            }
            finish(q,{terminated,terminated && matched ? std::string(native.name.data()) : std::string{}}); slot={};
        }
    }
    void run() noexcept {
        WSADATA winsock{};
        const bool winsock_ready=WSAStartup(MAKEWORD(2,2),&winsock)==0;
        try {
            if (!winsock_ready) throw std::runtime_error("domain_winsock_failed");
            WNDCLASSW type{}; type.lpfnWndProc=dispatch; type.hInstance=GetModuleHandleW(nullptr);
            type.lpszClassName=L"SFXVerifiedDomainCoordinator";
            if (!RegisterClassW(&type)) throw std::runtime_error("domain_window_class_failed");
            window_=CreateWindowExW(0,type.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,type.hInstance,this);
            if (!window_) throw std::runtime_error("domain_window_failed");
            while (!closing_) {
                MSG message{};
                while (PeekMessageW(&message,window_,0,0,PM_REMOVE)) DispatchMessageW(&message);
                for (auto& slot:slots_) if (slot.query && GetTickCount64()>=slot.query->deadline) {
                    finish(slot.query,{}); slot.query.reset();
                    if (slot.task && WSACancelAsyncRequest(slot.task)!=0) slot.quarantined=true;
                    else slot={};
                }
                std::shared_ptr<Query> next;
                { std::lock_guard<std::mutex> lock(mutex_); next=std::move(pending_); }
                if (next) {
                    std::size_t index=0;
                    while (index<slots_.size() && (slots_[index].task || slots_[index].query)) ++index;
                    if (index==slots_.size() || GetTickCount64()>=next->deadline) finish(next,{});
                    else {
                        auto& native=arena[index]; auto& slot=slots_[index]; slot.query=next;
                        for (unsigned n=0;n<4;++n) native.ip[n]=static_cast<char>(next->ip>>((3-n)*8));
                        slot.task=WSAAsyncGetHostByAddr(window_,completed_message+static_cast<UINT>(index),native.ip.data(),4,AF_INET,
                            native.bytes.data(),static_cast<int>(native.bytes.size()));
                        if (!slot.task) { finish(next,{}); slot={}; }
                    }
                }
                MsgWaitForMultipleObjects(0,nullptr,FALSE,10,QS_ALLINPUT);
            }
        } catch (...) { std::lock_guard<std::mutex> lock(mutex_); failed_=true; finish(pending_,{}); pending_.reset(); }
        for (auto& slot:slots_) { finish(slot.query,{}); if (slot.task) WSACancelAsyncRequest(slot.task); }
        if (window_) DestroyWindow(window_);
        if (winsock_ready) WSACleanup();
    }
    std::array<Slot,4> slots_{}; std::mutex mutex_; std::shared_ptr<Query> pending_;
    bool failed_{}; std::atomic<bool> closing_{false}; std::thread thread_; HWND window_{};
};
}
VerifiedDomain verify_peer_domain(std::uint32_t ip,const std::atomic<bool>& stop) noexcept {
    try { static Resolver resolver; return resolver.resolve(ip,stop); } catch (...) { return {}; }
}
}
