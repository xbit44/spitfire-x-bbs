#pragma once

#include "sfx/application/account_storage.hpp"
#include <array>
#include <functional>

namespace sfx::application {

inline constexpr std::size_t kCallerJournalActivityLimit = 32;
inline constexpr std::size_t kCallerJournalRecordBytes = 8192;
inline constexpr std::size_t kAttentionJournalRecordBytes = 1024;
inline constexpr std::uint64_t kDurableJournalFileBytes = 10U * 1024U * 1024U;
inline constexpr std::uint64_t kDurableJournalMaximumFileBytes = 100U * 1024U * 1024U;
inline constexpr std::size_t kDurableJournalArchiveCount = 10;

enum class CallerActivityKind : std::uint8_t {
    successful_logon, registration_committed, main_menu,
    // Extension points only; the owning features are not implemented by R1.
    message_area, file_area, door_entered, door_returned, chat_entered, chat_left
};
struct CallerActivity {
    CallerActivityKind kind{CallerActivityKind::successful_logon};
    std::int64_t utc_seconds{0};
    // Only a validated, configured feature label, never arbitrary caller text.
    std::string configured_label;
};
struct CallerJournalRecord {
    std::int64_t caller_number{0};
    SessionRecord session;
    std::string display_name;
    std::string transport{"unknown"};
    std::string source_ip;
    std::string hostname_hint; // PTR display hint only; no identity/security authority.
    int ansi{-1}; // -1 = historical/absent evidence, 0 = ASCII, 1 = ANSI.
    bool recovered{false};
    std::vector<CallerActivity> activities;
};

enum class AttentionKind : std::uint8_t {
    duplicate_login, password_attempts_exhausted, new_name_denied, name_list_invalid,
    // Reserved typed categories, not current producers.
    sysop_only_upload, questionnaire_security_change, archive_integrity_failure,
    configured_event, verified_sysop_utility, door_helper_failure,
    botgate_security, botgate_feed_failure, operational_logging_failure,
    clamav_failure, clamav_recovery, clamav_finding,
    saved_connection_unavailable, work_preserved, door_bulletin_unavailable
};
enum class AttentionIdentity : std::uint8_t { none, claimed, verified };
struct AttentionRecord {
    AttentionKind kind{AttentionKind::name_list_invalid};
    std::int64_t utc_seconds{0};
    std::uint16_t node{0};
    std::uint16_t existing_node{0};
    std::string transport;
    std::string source_ip;
    AttentionIdentity identity{AttentionIdentity::none};
    std::string account_name;
    std::string diagnostic;
    std::uint64_t occurrences{1};
    std::string filename;
    std::vector<std::string> signatures;
    bool definitions_usable{},scanner_problem{},deletion_confirmed{},deletion_journal_confirmed{};
    std::string resource; // bounded safe identifier; never credentials/command lines
};
// Per-Runtime-epoch bounded health transitions, shared by auto/manual updates
// and scan producers. The owner serializes access; no crash-durable claim.
class ClamAvAttentionTracker {
public:
    std::optional<AttentionRecord> observe(bool scanning,bool healthy,bool usable,
        std::string diagnostic,std::int64_t utc);
private:
    std::array<std::string,2> problems_;
};

// Formatter returns YYYY-MM-DD HH:MM:SS in the configured BBS timezone.
// No local-machine timezone fallback is allowed if that conversion fails.
using JournalLocalTime = std::function<std::optional<std::string>(std::int64_t)>;
bool valid_caller_activity(const CallerActivity& activity) noexcept;
std::optional<std::string> encode_caller_activities(const std::vector<CallerActivity>& activities);
std::optional<std::vector<CallerActivity>> decode_caller_activities(std::string_view encoded);
std::optional<std::string> render_caller_journal(const CallerJournalRecord&, const JournalLocalTime&);
std::optional<std::string> render_attention_journal(const AttentionRecord&, const JournalLocalTime&);

struct CallerJournalPublication {
    std::int64_t caller_number{0};
    std::string file_identity;
    std::uint64_t byte_offset{0};
    std::string block;
};
class ICallerJournalStorage {
public:
    virtual ~ICallerJournalStorage() = default;
    virtual StorageResult<std::vector<CallerJournalRecord>> pending_caller_journals(bool unfinished, std::size_t limit) = 0;
    virtual StorageStatus append_caller_activity(Identifier128 session, const CallerActivity&, std::string_view hostname_hint) = 0;
    virtual StorageStatus mark_caller_recovered(Identifier128 session) = 0;
    virtual StorageResult<std::optional<CallerJournalPublication>> caller_publication() = 0;
    virtual StorageStatus prepare_caller_publication(const CallerJournalPublication&) = 0;
    virtual StorageStatus acknowledge_caller_publication(std::int64_t caller_number) = 0;
};

} // namespace sfx::application
