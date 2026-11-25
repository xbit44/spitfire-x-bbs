#pragma once
#ifdef _WIN32
#include "sfx/platform/win32/botgate_rule_files.hpp"
#else
#include "sfx/platform/linux/botgate_rule_files.hpp"
#endif
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <ctime>
#include <functional>

namespace sfx::apps::runtime {
#ifdef _WIN32
namespace botgate_rule_files=platform::win32;
#else
namespace botgate_rule_files=platform::linux_host;
#endif
// Independent of feed HTTPS latency. Immutable admission snapshots are never
// mutated under caller threads. Invalid reload retains the last good rules.
class AlphaBotGateRules final {
public:
    AlphaBotGateRules(std::filesystem::path root,std::wstring owner,application::BotGateConfiguration legacy,std::function<void()> activated={})
        : root_(std::move(root)),owner_(std::move(owner)),legacy_(std::move(legacy)),activated_(std::move(activated)) {
        active_=std::make_shared<const application::BotGateRuleSnapshot>();
        refresh(); worker_=std::thread([this] {
            std::unique_lock<std::mutex> lock(wait_mutex_);
            while(!changed_.wait_for(lock,std::chrono::seconds(2),[this]{return stop_;})) {
                lock.unlock();refresh();lock.lock();
            }
        });
    }
    ~AlphaBotGateRules() { {std::lock_guard<std::mutex> lock(wait_mutex_);stop_=true;} changed_.notify_all();if(worker_.joinable())worker_.join(); try {persist_bans();} catch(...) {} }
    std::shared_ptr<const application::BotGateRuleSnapshot> snapshot() const { return std::atomic_load(&active_); }
    std::string status() const {std::lock_guard<std::mutex> lock(status_mutex_);return status_;}
    bool arrive(std::string_view source,std::uint64_t monotonic,std::int64_t utc) {
        const auto ip=application::parse_botgate_ipv4(source);if(!ip) return false;
        std::lock_guard<std::mutex> lock(rate_mutex_);
        const auto prior=limits_.banned_until(*ip);
        if(limits_.admit(*ip,monotonic)) return true;
        const auto until=limits_.banned_until(*ip);
        if(until>monotonic&&prior<=monotonic){try{if(activated_)activated_();}catch(...){}}
        if(until>monotonic && pending_bans_.size()<4096) pending_bans_[std::string(source)]=utc+static_cast<std::int64_t>(until-monotonic);
        return false;
    }
private:
    void persist_bans() {
        std::map<std::string,std::int64_t> bans;
        {std::lock_guard<std::mutex> lock(rate_mutex_);bans=pending_bans_;}
        if(bans.empty()) return;
        const auto text=botgate_rule_files::read_botgate_rule(root_,owner_,4);
        auto entries=application::parse_botgate_patterns(text,false,true);
        if(!entries) throw std::runtime_error("botgate_temporary_parse_failed");
        const auto now=static_cast<std::int64_t>(std::time(nullptr));std::string output;
        const auto line=[&](std::string ip,std::int64_t expiry) {
            if(expiry<=now) return;
            const auto stamp=static_cast<time_t>(expiry);tm time{};
#ifdef _WIN32
            if(gmtime_s(&time,&stamp)) throw std::runtime_error("botgate_temporary_time_failed");
#else
            if(!gmtime_r(&stamp,&time)) throw std::runtime_error("botgate_temporary_time_failed");
#endif
            char formatted[32]{};if(!std::strftime(formatted,sizeof(formatted),"%Y%m%dT%H%M%S+0000",&time)) throw std::runtime_error("botgate_temporary_time_failed");
            output+=ip+"\te="+formatted+"\n";
        };
        for(const auto& entry:*entries) {
            if(entry.network) {
                const auto ip=entry.network->address;
                const auto address=std::to_string(ip>>24)+'.'+std::to_string((ip>>16)&255)+'.'+std::to_string((ip>>8)&255)+'.'+std::to_string(ip&255);
                unsigned bits=0;for(auto mask=entry.network->mask;mask;mask<<=1) ++bits;
                line((entry.negated ? "!" : "")+address+(bits==32 ? "" : "/"+std::to_string(bits)),entry.expires);
            } else line((entry.negated ? "!" : "")+entry.text,entry.expires);
        }
        for(const auto& ban:bans) if(!application::botgate_patterns_match(*entries,ban.first,now)) line(ban.first,ban.second);
        botgate_rule_files::save_botgate_temporary(root_,owner_,output);
        {std::lock_guard<std::mutex> lock(rate_mutex_);for(const auto& ban:bans) {auto i=pending_bans_.find(ban.first);if(i!=pending_bans_.end() && i->second==ban.second) pending_bans_.erase(i);}}
    }
    void refresh() noexcept {
        try {
            if(!initialized_) {botgate_rule_files::initialize_botgate_files(root_,owner_,legacy_);initialized_=true;}
            persist_bans();
            auto next=botgate_rule_files::load_botgate_files(root_,owner_);
            {std::lock_guard<std::mutex> lock(status_mutex_);status_=next.status;}
            if(next.ready) std::atomic_store(&active_,std::make_shared<const application::BotGateRuleSnapshot>(std::move(next)));
        } catch(const std::exception& e) {std::lock_guard<std::mutex> lock(status_mutex_);status_=e.what();}
        catch(...) {std::lock_guard<std::mutex> lock(status_mutex_);status_="rules_reload_failed";}
    }
    std::filesystem::path root_;std::wstring owner_;application::BotGateConfiguration legacy_;std::function<void()> activated_;
    std::shared_ptr<const application::BotGateRuleSnapshot> active_;
    mutable std::mutex status_mutex_;std::mutex wait_mutex_;std::condition_variable changed_;
    std::string status_{"rules_not_loaded"}; bool initialized_{},stop_{};std::thread worker_;
    std::mutex rate_mutex_;application::BotGateArrivalLimits limits_;std::map<std::string,std::int64_t> pending_bans_;
};
}
