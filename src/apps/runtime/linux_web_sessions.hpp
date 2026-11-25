#pragma once
#include "sfx/application/time.hpp"
#include <sodium.h>
#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace sfx::apps::runtime::web {
inline std::string browser_token(){
    std::array<unsigned char,32> bytes{};randombytes_buf(bytes.data(),bytes.size());
    std::array<char,65> hex{};sodium_bin2hex(hex.data(),hex.size(),bytes.data(),bytes.size());return hex.data();
}
inline bool browser_secret_equal(std::string_view a,std::string_view b){
    return a.size()==64&&b.size()==64&&sodium_memcmp(a.data(),b.data(),64)==0;
}
// Only the native owner reads the renewal credential. It never enters a URL,
// browser response, cookie or LAN authentication path.
class BrowserSessions final {
    struct Session {std::string csrf;std::uint64_t created,last;};
    application::IClock& clock_;std::function<void(const std::string&)> publish_;
    std::mutex mutex_;std::string owner_=browser_token(),launch_;
    std::uint64_t launch_expires_{};std::map<std::string,Session> sessions_;
    void rotate(){const auto next=browser_token();publish_(next);launch_=next;launch_expires_=clock_.monotonic_millis()+300000;}
    void expire(){const auto now=clock_.monotonic_millis();for(auto it=sessions_.begin();it!=sessions_.end();){
        if(now-it->second.last>=1800000||now-it->second.created>=28800000)it=sessions_.erase(it);else ++it;}}
public:
    BrowserSessions(application::IClock& clock,std::function<void(const std::string&)> publish):clock_(clock),publish_(std::move(publish)){rotate();}
    const std::string& owner_secret()const noexcept{return owner_;}
    std::optional<std::string> renew(std::string_view owner){
        std::lock_guard<std::mutex> lock(mutex_);if(!browser_secret_equal(owner,owner_))return {};rotate();return launch_;
    }
    void revoke_all(){std::lock_guard<std::mutex> lock(mutex_);sessions_.clear();rotate();}
    std::optional<std::pair<std::string,std::string>> launch(std::string_view token,std::string_view existing={}){
        std::lock_guard<std::mutex> lock(mutex_);expire();
        if(clock_.monotonic_millis()>=launch_expires_||!browser_secret_equal(token,launch_))return {};
        const auto found=sessions_.find(std::string(existing));
        if(found!=sessions_.end()){rotate();found->second.last=clock_.monotonic_millis();return std::make_pair(found->first,found->second.csrf);}
        // A deliberate native launch may replace the least recently used local
        // browser. Keep the bound instead of forcing a running BBS to restart.
        if(sessions_.size()>=16){auto oldest=sessions_.begin();for(auto it=sessions_.begin();it!=sessions_.end();++it)if(it->second.last<oldest->second.last)oldest=it;sessions_.erase(oldest);}
        const auto id=browser_token(),csrf=browser_token();const auto now=clock_.monotonic_millis();
        rotate();sessions_.emplace(id,Session{csrf,now,now});return std::make_pair(id,csrf);
    }
    std::optional<std::string> authorized(std::string_view id,std::optional<std::string_view> csrf){
        std::lock_guard<std::mutex> lock(mutex_);expire();if(id.size()!=64)return {};
        const auto found=sessions_.find(std::string(id));if(found==sessions_.end()||(csrf&&!browser_secret_equal(*csrf,found->second.csrf)))return {};
        found->second.last=clock_.monotonic_millis();return found->second.csrf;
    }
    bool valid(std::string_view id){
        std::lock_guard<std::mutex> lock(mutex_);expire();return id.size()==64&&sessions_.count(std::string(id))!=0;
    }
};
// LAN sessions are a separate namespace. They require successful native System
// Password verification and remain bound to its revocation epoch and source IP.
class RemoteBrowserSessions final {
    struct Session {std::string csrf,source;std::uint64_t epoch,created,last;};
    application::IClock& clock_;std::mutex mutex_;std::map<std::string,Session> sessions_;std::uint64_t latest_epoch_{};
    void expire(){const auto now=clock_.monotonic_millis();for(auto it=sessions_.begin();it!=sessions_.end();){
        if(now-it->second.last>=1800000||now-it->second.created>=28800000)it=sessions_.erase(it);else ++it;}}
public:
    explicit RemoteBrowserSessions(application::IClock& clock):clock_(clock){}
    void revoke_all(){std::lock_guard<std::mutex> lock(mutex_);sessions_.clear();}
    std::optional<std::pair<std::string,std::string>> establish(std::string_view source,std::uint64_t epoch){
        std::lock_guard<std::mutex> lock(mutex_);expire();if(!epoch||epoch==UINT64_MAX||source.empty()||epoch<latest_epoch_)return {};
        if(epoch>latest_epoch_){sessions_.clear();latest_epoch_=epoch;}
        if(sessions_.size()>=16)return {};
        const auto id=browser_token(),csrf=browser_token();const auto now=clock_.monotonic_millis();
        sessions_.emplace(id,Session{csrf,std::string(source),epoch,now,now});return std::make_pair(id,csrf);
    }
    std::optional<std::string> authorized(std::string_view id,std::optional<std::string_view> csrf,std::string_view source,std::uint64_t epoch){
        std::lock_guard<std::mutex> lock(mutex_);expire();if(id.size()!=64||!epoch||epoch==UINT64_MAX)return {};
        const auto found=sessions_.find(std::string(id));if(found==sessions_.end())return {};
        if(found->second.epoch!=epoch){sessions_.erase(found);return {};}
        if(found->second.source!=source||(csrf&&!browser_secret_equal(*csrf,found->second.csrf)))return {};
        found->second.last=clock_.monotonic_millis();return found->second.csrf;
    }
    bool valid(std::string_view id,std::string_view source,std::uint64_t epoch){
        std::lock_guard<std::mutex> lock(mutex_);expire();if(id.size()!=64||!epoch||epoch==UINT64_MAX)return false;
        const auto found=sessions_.find(std::string(id));if(found==sessions_.end())return false;
        if(found->second.epoch!=epoch){sessions_.erase(found);return false;}
        return found->second.source==source;
    }
};
} // namespace sfx::apps::runtime::web
