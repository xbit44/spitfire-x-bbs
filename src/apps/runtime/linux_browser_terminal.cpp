#include "linux_browser_terminal.hpp"
#include "linux_web_sessions.hpp"
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <map>
#include <sys/socket.h>
#include <thread>

namespace sfx::apps::runtime::web {
namespace {
using FD=platform::linux_host::FileDescriptor;
bool send_now(int socket,std::string_view bytes){
    std::size_t offset{};
    while(offset<bytes.size()){
        const auto count=send(socket,bytes.data()+offset,bytes.size()-offset,MSG_NOSIGNAL|MSG_DONTWAIT);
        if(count>0)offset+=static_cast<std::size_t>(count);
        else if(count<0&&errno==EINTR)continue;
        else return false;
    }
    return true;
}
}
class BrowserTerminals::Impl {
public:
    struct Session {
        std::string handle=browser_token();application::NodeAssignmentIdentity identity;
        FD socket;application::ControlPanelTerminalMirror mirror;
        std::function<bool()> authorized;std::uint64_t touched{},next_input{1};
        std::uint16_t node{};bool connecting{true};unsigned state{},command{};std::string sub;
        std::array<bool,256> will{},doing{};
        bool decode(std::string_view data){
            std::string text,reply;
            for(const unsigned char byte:data){
                if(state==0){if(byte==255)state=1;else text+=static_cast<char>(byte);}
                else if(state==1){
                    if(byte==255){text+=static_cast<char>(255);state=0;}
                    else if(byte>=251&&byte<=254){command=byte;state=2;}
                    else if(byte==250){sub.clear();state=3;}else state=0;
                }else if(state==2){
                    if(command==253){const bool allowed=byte==0||byte==3||byte==24;
                        if(!allowed||!will[byte])reply.append({static_cast<char>(255),static_cast<char>(allowed?251:252),static_cast<char>(byte)});
                        will[byte]=allowed;
                    }else if(command==251){const bool allowed=byte==0||byte==1||byte==3;
                        if(!allowed||!doing[byte])reply.append({static_cast<char>(255),static_cast<char>(allowed?253:254),static_cast<char>(byte)});
                        doing[byte]=allowed;
                    }else if(command==254)will[byte]=false;else doing[byte]=false;
                    state=0;
                }else if(state==3){if(byte==255)state=4;else if(sub.size()<64)sub+=static_cast<char>(byte);else return false;}
                else if(byte==240){if(sub.size()==2&&static_cast<unsigned char>(sub[0])==24&&sub[1]==1)reply.append("\xff\xfa\x18\x00" "ANSI\xff\xf0",10);state=0;}
                else if(byte==255){if(sub.size()>=64)return false;sub+=static_cast<char>(255);state=3;}else return false;
            }
            mirror.consume(text);return reply.empty()||send_now(socket.get(),reply);
        }
    };
    Ports ports;std::mutex mutex;std::map<std::string,std::unique_ptr<Session>> sessions;std::atomic<bool> stopping{};std::thread worker;
    explicit Impl(Ports value):ports(std::move(value)),worker([this]{run();}){}
    ~Impl(){stopping=true;if(worker.joinable())worker.join();close_all();}
    bool valid(const Session& session){try{return session.authorized&&session.authorized()&&ports.clock.monotonic_millis()-session.touched<15000;}catch(...){return false;}}
    void retire(Session& session){if(session.socket){shutdown(session.socket.get(),SHUT_RDWR);session.socket=FD{};}
        if(session.identity.valid())ports.disconnect(session.identity);}
    void run()noexcept{
        while(!stopping){
            {std::lock_guard<std::mutex> lock(mutex);
                for(auto it=sessions.begin();it!=sessions.end();){auto& session=*it->second;bool keep=valid(session);
                    if(keep&&!session.connecting){
                        for(unsigned batch=0;batch<16;++batch){std::array<char,4096> bytes{};const auto read=recv(session.socket.get(),bytes.data(),bytes.size(),MSG_DONTWAIT);
                            if(read>0){if(!session.decode({bytes.data(),static_cast<std::size_t>(read)})){keep=false;break;}}
                            else if(read==0){keep=false;break;}
                            else if(errno==EAGAIN||errno==EWOULDBLOCK)break;
                            else if(errno!=EINTR){keep=false;break;}
                        }
                    }
                    if(!keep){retire(session);it=sessions.erase(it);}else ++it;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    bool open(std::string owner,std::uint16_t node,std::function<bool()> authorized){
        std::string handle;
        {std::lock_guard<std::mutex> lock(mutex);if(stopping||sessions.size()>=32||sessions.count(owner)||!authorized||!authorized())return false;
            auto session=std::make_unique<Session>();session->authorized=std::move(authorized);session->node=node;session->touched=ports.clock.monotonic_millis();handle=session->handle;sessions.emplace(owner,std::move(session));}
        LinuxBrowserNodeStart result{{application::LocalControlResponseCode::internal_failure,"browser_open_failed"},{},FD{}};
        try{result=ports.open(node);}catch(...){(void)close(owner,handle);return false;}
        std::lock_guard<std::mutex> lock(mutex);const auto found=sessions.find(owner);
        if(found==sessions.end()||found->second->handle!=handle||!valid(*found->second)||!result.client){
            if(result.client){shutdown(result.client.get(),SHUT_RDWR);result.client=FD{};ports.disconnect(result.identity);}
            if(found!=sessions.end()&&found->second->handle==handle)sessions.erase(found);
            return false;
        }
        found->second->socket=std::move(result.client);found->second->identity=result.identity;found->second->connecting=false;return true;
    }
    std::optional<BrowserTerminalView> view(std::string_view owner){
        std::lock_guard<std::mutex> lock(mutex);const auto found=sessions.find(std::string(owner));if(found==sessions.end())return {};
        auto& session=*found->second;if(!valid(session)){retire(session);sessions.erase(found);return {};}
        session.touched=ports.clock.monotonic_millis();return BrowserTerminalView{session.handle,session.identity,session.mirror.snapshot(),session.next_input,session.connecting,
            static_cast<std::uint16_t>(session.mirror.cursor_column()),static_cast<std::uint16_t>(session.mirror.cursor_row())};
    }
    bool input(std::string_view owner,std::string_view handle,std::uint64_t sequence,std::string_view bytes){
        std::lock_guard<std::mutex> lock(mutex);const auto found=sessions.find(std::string(owner));if(found==sessions.end())return false;auto& session=*found->second;
        if(session.connecting||!browser_secret_equal(session.handle,handle)||sequence!=session.next_input||sequence==UINT64_MAX||bytes.empty()||bytes.size()>256)return false;
        if(!valid(session)){retire(session);sessions.erase(found);return false;}
        std::string encoded;for(const unsigned char c:bytes){encoded+=static_cast<char>(c);if(c==255)encoded+=static_cast<char>(255);}
        ++session.next_input;const bool sent=send_now(session.socket.get(),encoded);sodium_memzero(encoded.data(),encoded.size());
        if(!sent){retire(session);sessions.erase(found);return false;}
        session.touched=ports.clock.monotonic_millis();return true;
    }
    bool close(std::string_view owner,std::string_view handle){
        std::lock_guard<std::mutex> lock(mutex);const auto found=sessions.find(std::string(owner));if(found==sessions.end()||!browser_secret_equal(found->second->handle,handle))return false;
        retire(*found->second);sessions.erase(found);return true;
    }
    void close_all(){std::lock_guard<std::mutex> lock(mutex);for(auto& entry:sessions)retire(*entry.second);sessions.clear();}
};
BrowserTerminals::BrowserTerminals(Ports ports):impl_(std::make_unique<Impl>(std::move(ports))){}
BrowserTerminals::~BrowserTerminals()=default;
bool BrowserTerminals::open(std::string owner,std::uint16_t node,std::function<bool()> authorized){return impl_->open(std::move(owner),node,std::move(authorized));}
std::optional<BrowserTerminalView> BrowserTerminals::view(std::string_view owner){return impl_->view(owner);}
bool BrowserTerminals::input(std::string_view owner,std::string_view handle,std::uint64_t sequence,std::string_view bytes){return impl_->input(owner,handle,sequence,bytes);}
bool BrowserTerminals::close(std::string_view owner,std::string_view handle){return impl_->close(owner,handle);}
void BrowserTerminals::close_all(){impl_->close_all();}
}
