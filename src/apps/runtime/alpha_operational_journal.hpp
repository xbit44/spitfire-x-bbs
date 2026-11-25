#pragma once

#include "sfx/application/operational_log.hpp"
#include "sfx/application/event_sounds.hpp"
#include "sfx/application/operational_backlog.hpp"
#include "sfx/application/botgate.hpp"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <vector>
#include <thread>
#include <deque>
#include <condition_variable>
#include <memory>
#include <functional>
#include <map>
#include "sfx/application/caller_journal.hpp"

namespace sfx::apps::runtime {

// One owner per Runtime epoch, shared across listener retry/replacement. It does
// not own caller authority. Runtime-lifetime history is bounded separately from
// the protected durable file. snapshot() is only the small status notification tail.
class AlphaOperationalJournal final {
public:
    AlphaOperationalJournal(std::filesystem::path path, std::wstring owner_sid,
        application::Identifier128 epoch,std::shared_ptr<application::EventSoundStream> sounds={});
    ~AlphaOperationalJournal();
    bool configure(const application::LoggingConfiguration&) noexcept;
    bool flush() noexcept;
#if defined(__linux__)
    // Capture a short native file snapshot between appends. The writer and
    // event producers resume before compression starts; capture must not log.
    bool capture_backup_file(const std::function<bool()>& capture) noexcept;
    // Coordinated capture owns the durable publication fence. Never flush
    // here: the operational writer may be delivering durable attention.
    bool capture_backup_files(const std::function<bool()>& capture);
#endif
    void attention_sink(std::function<void(application::AttentionRecord)> sink);
    void clamav_health(bool scanning,bool healthy,bool definitions_usable,std::string diagnostic) noexcept;
    void clamav_finding(application::AttentionRecord) noexcept;
    void problem(application::AttentionKind,std::string resource,std::string diagnostic) noexcept;
    void emit(application::OperationalEvent event) noexcept;
    void sound(application::SoundEvent event) noexcept;
    application::SoundPollSnapshot sound_poll(const application::SoundPollRequest& r,const application::EventSoundConfiguration& c);
    void gate_count(unsigned index) noexcept;
    void gate_active(bool entering) noexcept;
    void gate_status(application::BotGateFeedStatus&) const noexcept;
    std::uint64_t next_attempt() noexcept;
    std::vector<application::AlphaOperationalLogEntry> snapshot() const;
    application::OperationalReportingSnapshot reporting() const;
    std::optional<application::OperationalLogPage> page(application::OperationalLogCursor cursor) const;
    std::uint8_t health() const noexcept { return health_.load(); }
    void mark_event_failed() noexcept { health_.fetch_or(application::operational_log_event_rejected); }
    void mark_durable_journal_failed() noexcept {
        if (!(health_.fetch_or(application::operational_durable_journal_failed) & application::operational_durable_journal_failed))
            emit({application::OperationalKind::durable_journal_degraded, {}, {}});
    }
    static std::vector<application::AlphaOperationalLogEntry> read(
        const std::filesystem::path& path, std::uint8_t& health) noexcept;
private:
    void writer() noexcept;
    void dispatch_clamav_attention(application::AttentionRecord);
    const std::filesystem::path path_;
    const std::wstring owner_sid_;
    const application::Identifier128 epoch_;
    std::shared_ptr<application::EventSoundStream> sounds_;
    mutable std::mutex mutex_, file_mutex_;
#if defined(__linux__)
    std::mutex file_io_mutex_;
#endif
    application::OperationalBacklog records_;
    application::OperationalReportingSnapshot reporting_{true,{}, {}};
    std::uint64_t delivery_{0}, origin_{0}; // serialized with file_mutex_
    std::atomic<std::uint64_t> attempts_{0};
    std::array<std::atomic<std::uint64_t>,7> gate_counts_{};
    std::uint64_t gate_started_{};
    std::atomic<std::uint8_t> health_{0};
    bool blocked_append_{false};
    std::condition_variable changed_, flushed_;
    std::deque<std::string> pending_;
    application::LoggingConfiguration limits_;
    std::thread worker_;
    bool stopping_{}, writing_{};
    bool flush_attention_{};
    std::uint64_t flood_window_{};
    unsigned flood_emitted_{};
    static constexpr std::size_t kinds=static_cast<std::size_t>(application::OperationalKind::external_event_problem)+1;
    std::array<std::uint64_t,kinds> suppressed_{};
    std::array<std::string,kinds> samples_{};
    std::function<void(application::AttentionRecord)> attention_;
    std::mutex clamav_health_mutex_;
    application::ClamAvAttentionTracker clamav_health_;
    std::mutex clamav_delivery_mutex_;
    std::deque<application::AttentionRecord> clamav_pending_attention_; // startup only, max32
    struct ProblemWindow {std::uint64_t emitted{},suppressed{};};
    std::mutex problem_mutex_;
    std::map<std::string,ProblemWindow> problem_windows_; // bounded, current Runtime only
};
} // namespace sfx::apps::runtime
