#pragma once
#include "alpha_operational_journal.hpp"
#include "sfx/application/botgate.hpp"
#include <condition_variable>
#include <functional>
#include <memory>
#include <thread>

namespace sfx::apps::runtime {
using BotGateFeedStatus=application::BotGateFeedStatus;
// One owner per Runtime, shared by all listener replacements. Numeric immutable
// snapshots make admission independent of filesystem/HTTPS latency.
class AlphaBotGateFeed final {
public:
    using Progress=std::function<void(std::uint64_t)>;
    using Fetch=std::function<std::optional<std::string>(const std::atomic<bool>&,Progress,std::string&)>;
    AlphaBotGateFeed(std::filesystem::path root,std::wstring owner,
        std::shared_ptr<AlphaOperationalJournal> journal,Fetch fixture_fetch={});
    ~AlphaBotGateFeed();
    bool start();
    void stop() noexcept;
    bool update_now() noexcept;
    BotGateFeedStatus status() const;
    std::shared_ptr<const std::vector<std::uint32_t>> snapshot() const noexcept;
private:
    void run() noexcept;
    const std::filesystem::path root_;
    const std::wstring owner_;
    const std::shared_ptr<AlphaOperationalJournal> journal_;
    const Fetch fetch_;
    std::shared_ptr<const std::vector<std::uint32_t>> active_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    BotGateFeedStatus status_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    bool requested_{}, started_{};
    std::uint64_t next_due_{};
};
} // namespace sfx::apps::runtime
