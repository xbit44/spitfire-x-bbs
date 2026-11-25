#pragma once

#include "alpha_operational_journal.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/new_caller_names.hpp"
#include <memory>

namespace sfx::apps::runtime {

// One bounded writer per Runtime. No caller, socket, assignment or listener is
// retained by this service. It stops before its authoritative storage dependency.
class AlphaDurableJournal final : public application::INewCallerNamePolicy {
public:
    AlphaDurableJournal(adapters::storage::SqliteStorage&, application::IIdentifierGenerator&,
        std::filesystem::path root, std::wstring owner, std::string bbs_time_zone,
        std::shared_ptr<AlphaOperationalJournal> operational);
    ~AlphaDurableJournal();
    // False means authoritative recovery failed: do not enable caller admission.
    bool start();
    void stop() noexcept;
    // Changes only presentation of future durable journal publications. Stored
    // UTC identities and already rendered rows are never rewritten.
    bool reconfigure_time_zone(std::string bbs_time_zone) noexcept;
    bool configure_logging(const application::LoggingConfiguration&) noexcept;
#if defined(__linux__)
    // Finish an active caller intent before the cut: its native file identity
    // cannot survive restore. Nest operational I/O and SQLite/file capture;
    // never flush or log inside the callback. Writers resume before archiving.
    // A maintenance owner may capture after normal stop has fully joined;
    // stopping/join-in-progress and never-started owners remain ineligible.
    bool capture_backup_files(const std::function<bool()>& capture,std::string& diagnostic) noexcept;
#endif
    const application::INewCallerNamePolicy& new_names() const noexcept { return *this; }
    application::NewCallerNameDecision evaluate(std::string_view name) const override;
    void attention(application::AttentionRecord) const noexcept;
    bool other_board(const application::OtherBoardLogRecord&) const noexcept;
    void activity(application::Identifier128 session, const application::CallerActivity&,
        std::string_view hostname_hint) noexcept;
    void wake() noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sfx::apps::runtime
