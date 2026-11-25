#include "alpha_durable_journal.hpp"
#include "sfx/application/durable_journal.hpp"
#include "runtime_journal_platform.hpp"
#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>

namespace sfx::apps::runtime {
namespace app = application;
class AlphaDurableJournal::Impl final {
public:
    class Clock final : public app::IClock {
        std::int64_t unix_seconds() const noexcept override {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }
        std::uint64_t monotonic_millis() const noexcept override {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }
    } clock;
    Impl(adapters::storage::SqliteStorage& store, app::IIdentifierGenerator& ids,
        std::filesystem::path root, std::wstring owner, std::string zone,
        std::shared_ptr<AlphaOperationalJournal> diagnostics)
        : storage(store), identifiers(ids), operational(std::move(diagnostics)),
          names_root(root), names_owner(owner),
          files(std::move(root), std::move(owner)),
          rules(NativeBbsTimeZoneRuleProvider::create(zone)) {
        if (rules) local = std::make_unique<app::BbsLocalTimeAdapters>(rules->configuration(), *rules, clock);
        publisher = std::make_unique<app::DurableJournalPublisher>(storage, files,
            [this](std::int64_t utc) { return local ? local->format_utc_seconds(utc) : std::nullopt; });
    }
    void degraded() noexcept { operational->mark_durable_journal_failed(); }
    bool set_time_zone(std::string zone) noexcept {
        try {
            auto replacement_rules =
                NativeBbsTimeZoneRuleProvider::create(zone);
            if (!replacement_rules) return false;
            auto replacement_local = std::make_unique<app::BbsLocalTimeAdapters>(
                replacement_rules->configuration(), *replacement_rules, clock);
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return false;
            rules = std::move(replacement_rules);
            local = std::move(replacement_local);
            return true;
        } catch (...) {
            return false;
        }
    }
    bool publish_other_board(const app::OtherBoardLogRecord& value) {
        std::lock_guard<std::mutex> lock(mutex);
        if(stopping)return false;
        return publisher->publish_other_board(value);
    }
    bool publish_attention(const app::AttentionRecord& value) {
        std::lock_guard<std::mutex> lock(mutex);
        return publisher->publish_attention(value);
    }
    app::JournalPublishResult publish_one_caller() {
        std::lock_guard<std::mutex> lock(mutex);
        return publisher->publish_one_caller();
    }
    bool recover() {
        // Bounded pages, before any new admission. Old successful history never
        // gains invented journal rows. Only R1's retained unfinished rows recover.
        for (unsigned page_number = 0; page_number < 64; ++page_number) {
            const auto pending = storage.pending_caller_journals(true, 16);
            if (!pending || !pending.value) return false;
            if (pending.value->empty()) return true;
            for (const auto& record : *pending.value) {
                const auto& session = record.session;
                if (!storage.mark_caller_recovered(session.session_id) ||
                    !storage.finalize_session({session.session_id, session.persisted_seconds,
                        session.last_persisted_utc_seconds, app::SessionOutcomeCode::runtime_error,
                        identifiers.generate()})) return false;
            }
        }
        return false; // corrupt/excessive unfinished history never enables reuse
    }
    void run() noexcept {
        try {
            // Create the two fixed active files even before their first event.
#if defined(__linux__)
            bool initialized{};
            {
                // Header creation and initial pruning are fenced writes too.
                std::lock_guard<std::mutex> lock(mutex);
                initialized=files.inspect(app::DurableJournalKind::callers).has_value() &&
                    files.inspect(app::DurableJournalKind::attention).has_value();
            }
            if (!initialized) degraded();
#else
            if (!files.inspect(app::DurableJournalKind::callers) ||
                !files.inspect(app::DurableJournalKind::attention)) degraded();
#endif
            // One startup pass drains a retained publication intent or caller
            // completed during recovery. After a no-record result, the worker
            // sleeps until caller cleanup or an attention record wakes it.
            bool caller_work = true;
            bool retry_wait = false;
            auto retry_delay = std::chrono::milliseconds(250);
            for (;;) {
                std::optional<app::AttentionRecord> next;
                bool ending{};
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    const auto ready = [this] { return stopping || notified || !queue.empty(); };
                    if (!caller_work) changed.wait(lock, ready);
                    else if (retry_wait) (void)changed.wait_for(lock, retry_delay, ready);
                    if (notified) {
                        caller_work = true;
                        retry_wait = false;
                        retry_delay = std::chrono::milliseconds(250);
                    }
                    notified = false;
                    ending = stopping;
                    if (!queue.empty()) { next = std::move(queue.front()); queue.pop_front(); }
                }
                if (next && !publish_attention(*next)) degraded();
                if (ending) {
                    // Bounded shutdown publication, no DNS/caller waits or full
                    // historical backlog drain. Remaining caller intents stay in SQLite.
                    for (unsigned i = 0; i < 32; ++i) {
                        std::optional<app::AttentionRecord> item;
                        { std::lock_guard<std::mutex> lock(mutex);
                          if (!queue.empty()) { item = std::move(queue.front()); queue.pop_front(); } }
                        if (item && !publish_attention(*item)) degraded();
                    }
                    for (unsigned i = 0; i < 4; ++i) {
                        const auto last = publish_one_caller();
                        if (last == app::JournalPublishResult::unavailable) degraded();
                        if (last != app::JournalPublishResult::published) break;
                    }
                    return;
                }
                if (caller_work) {
                    const auto result = publish_one_caller();
                    if (result == app::JournalPublishResult::published) {
                        retry_wait = false;
                        retry_delay = std::chrono::milliseconds(250);
                    } else if (result == app::JournalPublishResult::no_record) {
                        caller_work = false;
                        retry_wait = false;
                        retry_delay = std::chrono::milliseconds(250);
                    } else {
                        degraded();
                        retry_wait = true;
                        retry_delay = std::chrono::milliseconds(
                            std::min<std::int64_t>(retry_delay.count() * 2, 5000));
                    }
                }
            }
        } catch (...) { degraded(); }
    }
    adapters::storage::SqliteStorage& storage;
    app::IIdentifierGenerator& identifiers;
    std::shared_ptr<AlphaOperationalJournal> operational;
    const std::filesystem::path names_root;
    const std::wstring names_owner;
    std::atomic<bool> names_invalid_reported{false};
    NativeDurableJournalFiles files;
    std::unique_ptr<NativeBbsTimeZoneRuleProvider> rules;
    std::unique_ptr<app::BbsLocalTimeAdapters> local;
    std::unique_ptr<app::DurableJournalPublisher> publisher;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<app::AttentionRecord> queue; // at most 32 records, each <=1024 rendered bytes
    std::thread worker;
    bool started{false}, stopping{false}, notified{false};
#if defined(__linux__)
    bool joined{false}; // Set only after stop has joined (or proved no worker).
#endif
};
AlphaDurableJournal::AlphaDurableJournal(adapters::storage::SqliteStorage& storage,
    app::IIdentifierGenerator& ids, std::filesystem::path root, std::wstring owner,
    std::string zone, std::shared_ptr<AlphaOperationalJournal> operational)
    : impl_(std::make_unique<Impl>(storage, ids, std::move(root), std::move(owner), std::move(zone), std::move(operational))) {}
AlphaDurableJournal::~AlphaDurableJournal() { stop(); }
bool AlphaDurableJournal::start() {
    if (impl_->started) return false;
    if (!impl_->recover()) return false;
    impl_->started = true;
    (void)evaluate({}); // Inspect once for an early diagnostic; never retain this snapshot.
    try { impl_->worker = std::thread([this] { impl_->run(); }); }
    catch (...) { impl_->degraded(); } // file recording loss cannot close admission
    return true;
}
void AlphaDurableJournal::stop() noexcept {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->stopping = true; }
    impl_->changed.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
#if defined(__linux__)
    { std::lock_guard<std::mutex> lock(impl_->mutex);impl_->joined=true; }
#endif
}
bool AlphaDurableJournal::reconfigure_time_zone(std::string zone) noexcept {
    return impl_->set_time_zone(std::move(zone));
}
bool AlphaDurableJournal::configure_logging(const app::LoggingConfiguration& value) noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->files.configure(value) && impl_->publisher->configure(value);
    } catch (...) { return false; }
}
#if defined(__linux__)
bool AlphaDurableJournal::capture_backup_files(const std::function<bool()>& capture,std::string& diagnostic) noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if(!capture||!impl_->started||(impl_->stopping&&!impl_->joined)) {
            diagnostic="backup_durable_journal_not_running";return false;
        }
        if(!impl_->files.inspect(app::DurableJournalKind::callers)||
           !impl_->files.inspect(app::DurableJournalKind::attention)) {
            diagnostic="backup_durable_journal_initialization_unavailable";return false;
        }
        const auto pending=impl_->storage.caller_publication();
        if(!pending||!pending.value) {
            diagnostic="backup_caller_publication_state_unavailable";return false;
        }
        if(*pending.value&&impl_->publisher->publish_one_caller()!=app::JournalPublishResult::published) {
            diagnostic="backup_caller_publication_incomplete_work_preserved";return false;
        }
        const auto boundary=impl_->storage.caller_publication();
        if(!boundary||!boundary.value||*boundary.value) {
            diagnostic="backup_caller_publication_boundary_unavailable";return false;
        }
        if(!capture()) {diagnostic="backup_coordinated_journal_capture_refused";return false;}
        diagnostic.clear();return true;
    } catch(const std::exception& error) {diagnostic=error.what();return false;}
    catch(...) {diagnostic="backup_coordinated_journal_capture_failed";return false;}
}
#endif
app::NewCallerNameDecision AlphaDurableJournal::evaluate(std::string_view name) const {
    // Every check opens, verifies, reads and closes the fixed leaf. The bounded
    // read pins one complete version against concurrent writes/replacement; no
    // handle, contents or allowed decision survives this call.
    const auto names = native_new_caller_names(impl_->names_root, impl_->names_owner);
    if (!names.valid() && !impl_->names_invalid_reported.exchange(true)) {
        app::AttentionRecord notice;
        notice.utc_seconds = static_cast<app::IClock&>(impl_->clock).unix_seconds();
        attention(std::move(notice)); // At most one invalid-list notice per Runtime.
    }
    return names.evaluate(name);
}
bool AlphaDurableJournal::other_board(const app::OtherBoardLogRecord& record) const noexcept {
    try {if(impl_->publish_other_board(record))return true;}catch(...){}
    impl_->degraded();return false;
}
void AlphaDurableJournal::attention(app::AttentionRecord record) const noexcept {
    try {
        // Validate before retaining. No unbounded or arbitrary strings enter queue.
        if (!app::render_attention_journal(record, [](std::int64_t) {
                return std::optional<std::string>("2000-01-01 00:00:00"); })) { impl_->degraded(); return; }
        { std::lock_guard<std::mutex> lock(impl_->mutex);
          if (impl_->stopping || impl_->queue.size() >= 32) { impl_->degraded(); return; }
          impl_->queue.push_back(std::move(record)); }
        impl_->changed.notify_one();
    } catch (...) { impl_->degraded(); }
}
void AlphaDurableJournal::activity(app::Identifier128 session, const app::CallerActivity& event,
    std::string_view hostname_hint) noexcept {
    try { if (!impl_->storage.append_caller_activity(session, event, hostname_hint)) impl_->degraded(); }
    catch (...) { impl_->degraded(); }
}
void AlphaDurableJournal::wake() noexcept {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->notified = true; }
    impl_->changed.notify_one();
}
} // namespace sfx::apps::runtime
