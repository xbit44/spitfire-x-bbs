#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sfx/application/storage.hpp"
#include "sfx/core/identity.hpp"

namespace sfx::application {

inline constexpr std::size_t kAccountNameMaximum = 30;
inline constexpr std::size_t kRealNameMaximum = 64;
inline constexpr std::size_t kLocationMaximum = 64;
inline constexpr std::size_t kPhoneMaximum = 32;
inline constexpr std::size_t kEmailMaximum = 254;
inline constexpr std::size_t kSecurityLabelMaximum = 32;
inline constexpr std::size_t kSecurityDescriptionMaximum = 128;
inline constexpr std::size_t kTypedCodeMaximum = 32;
inline constexpr std::size_t kCredentialEnvelopeMaximum = 16U * 1024U;
inline constexpr std::uint16_t kDefaultAuditOnlineRetentionMonths = 24;
inline constexpr std::uint16_t kDefaultAuditTotalRetentionMonths = 84;
inline constexpr std::uint16_t kDefaultSessionOnlineRetentionMonths = 12;
inline constexpr std::uint16_t kDefaultSessionTotalRetentionMonths = 24;
inline constexpr std::uint16_t kMaximumRetentionMonths = 1200;
inline constexpr std::int64_t kMinimumUtcSeconds = -62'135'596'800LL;
inline constexpr std::int64_t kMaximumUtcSeconds = 253'402'300'799LL;

using AccountId = core::AccountId;

struct Identifier128 {
    std::array<std::uint8_t, 16> bytes{};

    bool valid() const noexcept;
    friend bool operator==(const Identifier128& left, const Identifier128& right) noexcept;
    friend bool operator!=(const Identifier128& left, const Identifier128& right) noexcept;
};

class IIdentifierGenerator {
public:
    virtual ~IIdentifierGenerator() = default;
    virtual Identifier128 generate() = 0;
};

enum class AccountState : std::uint8_t {
    active,
    locked,
    deletion_scheduled,
    tombstoned
};

enum class SecurityLevelState : std::uint8_t { active, retired };

enum class AuditEventKind : std::uint8_t {
    account_created,
    account_renamed,
    account_state_changed,
    account_profile_changed,
    account_credential_replaced,
    account_tombstoned,
    security_policy_changed,
    retention_policy_changed,
    first_run_completed,
    session_completed
};

enum class AuditReasonCode : std::uint8_t {
    initial_setup,
    registration,
    sysop_action,
    inactivity_policy,
    security_policy,
    retention_policy,
    session_finalization,
    caller_action
};

enum class AuditResultCode : std::uint8_t { succeeded };

enum class RetentionDisposition : std::uint8_t {
    keep_online,
    archive_required,
    prune_eligible
};

enum class SessionOutcomeCode : std::uint8_t {
    normal,
    disconnected,
    timed_out,
    shutdown,
    runtime_error
};

std::string_view storage_code(AccountState value) noexcept;
std::string_view storage_code(SecurityLevelState value) noexcept;
std::string_view storage_code(AuditEventKind value) noexcept;
std::string_view storage_code(AuditReasonCode value) noexcept;
std::string_view storage_code(AuditResultCode value) noexcept;
std::string_view storage_code(SessionOutcomeCode value) noexcept;

std::optional<std::string> normalize_account_name(std::string_view candidate);
std::optional<std::string> account_name_comparison_key(std::string_view normalized_name);
std::optional<std::string> normalize_real_name(std::string_view candidate);
std::optional<std::string> normalize_optional_profile_text(
    std::string_view candidate,
    std::size_t maximum_length);
bool valid_iso_civil_date(std::string_view candidate) noexcept;
bool valid_utc_seconds(std::int64_t value) noexcept;

struct AccountProfile {
    std::optional<std::string> real_name;
    std::optional<std::string> location;
    std::optional<std::string> phone;
    std::optional<std::string> email;
    std::optional<std::string> birth_date;
    std::optional<char> sex;
};

struct CredentialEnvelope {
    std::uint16_t envelope_format_version{0};
    std::string scheme_identifier;
    std::uint16_t parameter_encoding_version{0};
    std::vector<std::uint8_t> canonical_parameters;
    std::vector<std::uint8_t> salt;
    std::vector<std::uint8_t> verifier;
    std::optional<std::string> pepper_key_identifier;
    std::int64_t created_utc_seconds{0};
    bool password_change_required{false};
};

struct AuditContext {
    Identifier128 event_id;
    Identifier128 correlation_id;
    std::optional<AccountId> actor_account_id;
    std::int64_t occurred_utc_seconds{0};
    AuditReasonCode reason{AuditReasonCode::sysop_action};
    // Runtime-bound only, never decoded from account-editor IPC. When present,
    // the writer revalidates this exact live Sysop before changing any rows.
    std::optional<Identifier128> sysop_session{};
};

struct AccountStatistics {
    std::int64_t successful_login_count{0};
    std::int64_t authenticated_session_seconds{0};
    std::optional<std::int64_t> last_successful_login_utc_seconds;
    std::optional<std::int64_t> last_completed_session_utc_seconds;
    std::optional<SessionOutcomeCode> last_session_outcome;
};

struct AccountRecord {
    AccountId account_id{0};
    std::optional<std::string> display_name;
    std::optional<std::string> name_key;
    std::optional<std::uint16_t> security_level;
    AccountState state{AccountState::active};
    std::int64_t revision{0};
    std::optional<bool> purge_allowed;
    std::int64_t created_utc_seconds{0};
    std::int64_t modified_utc_seconds{0};
    std::optional<std::int64_t> deletion_scheduled_utc_seconds;
    std::optional<std::int64_t> tombstoned_utc_seconds;
    std::optional<std::string> tombstone_reason_code;
    AccountProfile profile;
    AccountStatistics statistics;
};

struct SecurityLevelRecord {
    std::uint16_t level{0};
    std::string label;
    std::optional<std::string> description;
    SecurityLevelState state{SecurityLevelState::active};
    std::optional<std::uint32_t> minutes_per_call{}, minutes_per_day{}; // null = unlimited
    std::optional<std::uint32_t> private_chat_minutes{}, private_chat_minutes_per_day{};
};

struct SecurityPolicyRecord {
    std::int64_t revision{0};
    std::uint16_t new_user_level{0};
    std::uint16_t sysop_threshold{0};
    std::string policy_hash;
    std::optional<std::int64_t> published_revision;
    std::optional<std::string> published_policy_hash;
};

struct RetentionPolicyRecord {
    std::int64_t revision{0};
    std::uint16_t audit_online_months{0};
    std::uint16_t audit_total_months{0};
    std::uint16_t session_online_months{0};
    std::uint16_t session_total_months{0};
    std::int64_t modified_utc_seconds{0};
};

struct FirstRunState {
    bool complete{false};
    std::optional<AccountId> staged_sysop_account_id;
    std::optional<std::int64_t> completed_utc_seconds;
};

struct SessionRecord {
    Identifier128 session_id;
    AccountId account_id{0};
    std::uint16_t node_id{0};
    std::int64_t started_utc_seconds{0};
    std::int64_t persisted_seconds{0};
    std::int64_t last_persisted_utc_seconds{0};
    std::optional<std::int64_t> completed_utc_seconds;
    std::optional<SessionOutcomeCode> outcome;
    Identifier128 correlation_id;
};

struct AuditRecord {
    Identifier128 event_id;
    AuditEventKind event_kind{AuditEventKind::account_created};
    AuditResultCode result{AuditResultCode::succeeded};
    std::optional<AccountId> subject_account_id;
    std::optional<AccountId> actor_account_id;
    std::int64_t occurred_utc_seconds{0};
    Identifier128 correlation_id;
    AuditReasonCode reason{AuditReasonCode::sysop_action};
};

template <typename T>
struct StorageResult {
    StorageStatus status;
    std::optional<T> value;

    bool succeeded() const noexcept { return status.succeeded(); }
    explicit operator bool() const noexcept { return succeeded(); }
};

struct MutationResult {
    StorageStatus status;
    std::optional<AccountId> account_id;
    std::optional<std::int64_t> revision;
    bool replayed{false};
    // Set only by the transaction that refused removal of the last usable Sysop.
    bool last_sysop_protected{false};

    bool succeeded() const noexcept { return status.succeeded(); }
    explicit operator bool() const noexcept { return succeeded(); }
};

struct CreateAccountRequest {
    std::string display_name;
    AccountProfile profile;
    CredentialEnvelope credential;
    AuditContext audit;
};

using StageInitialSysopRequest = CreateAccountRequest;

struct CompleteFirstRunRequest {
    AccountId staged_sysop_account_id{0};
    std::int64_t expected_policy_revision{0};
    std::string expected_policy_hash;
    std::string published_policy_hash;
    AuditContext audit;
    // Set only from normal Setup's protected, installation-bound fresh media.
    // Absence preserves existing and older incomplete installations.
    bool starter_doors_eligible{false};
};

struct RenameAccountRequest {
    AccountId account_id{0};
    std::int64_t expected_revision{0};
    std::string new_display_name;
    AuditContext audit;
};

struct UpdateProfileRequest {
    AccountId account_id{0};
    std::int64_t expected_revision{0};
    AccountProfile profile;
    AuditContext audit;
};

struct SetPurgeAllowedRequest {
    AccountId account_id{0};
    std::int64_t expected_revision{0};
    bool purge_allowed{true};
    AuditContext audit;
};

struct AssignAccountSecurityLevelRequest {
    AccountId account_id{0};
    std::int64_t expected_revision{0};
    std::int64_t expected_policy_revision{0};
    std::uint16_t security_level{0};
    AuditContext audit;
};

struct ReplaceCredentialRequest {
    AccountId account_id{0};
    std::int64_t expected_revision{0};
    CredentialEnvelope credential;
    AuditContext audit;
};

// One optimistic transaction for the ordinary user editor; never a sequence
// of partially saved name/profile/level changes.
struct SaveAccountRequest {
    AccountId account_id{};
    std::int64_t expected_revision{}, expected_policy_revision{};
    std::string display_name;
    AccountProfile profile;
    std::uint16_t security_level{};
    bool purge_allowed{};
    AuditContext audit;
};

struct ChangeAccountStateRequest {
    AccountId account_id{0};
    std::int64_t expected_revision{0};
    AccountState new_state{AccountState::active};
    std::optional<std::int64_t> deletion_scheduled_utc_seconds;
    AuditContext audit;
};

struct TombstoneAccountRequest {
    AccountId account_id{0};
    std::int64_t expected_revision{0};
    AuditReasonCode reason{AuditReasonCode::sysop_action};
    AuditContext audit;
};

struct SecurityLevelReassignment {
    std::uint16_t retired_level{0};
    std::uint16_t replacement_level{0};
};

struct PublishSecurityPolicyRequest {
    std::int64_t expected_revision{0};
    std::vector<SecurityLevelRecord> levels;
    std::vector<SecurityLevelReassignment> reassignments;
    std::uint16_t new_user_level{0};
    std::uint16_t sysop_threshold{0};
    std::string policy_hash;
    std::string published_policy_hash;
    AuditContext audit;
    // Optional for noninteractive migrations; local removal always supplies it.
    // Exact membership/revisions are rechecked inside the writer transaction.
    std::optional<std::vector<std::pair<AccountId,std::int64_t>>> expected_reassigned_accounts;
};

struct PublishRetentionPolicyRequest {
    std::int64_t expected_revision{0};
    std::uint16_t audit_online_months{0};
    std::uint16_t audit_total_months{0};
    std::uint16_t session_online_months{0};
    std::uint16_t session_total_months{0};
    bool shortening_effect_preview_confirmed{false};
    AuditContext audit;
};

struct StartSessionRequest {
    Identifier128 session_id;
    Identifier128 correlation_id;
    AccountId account_id{0};
    std::uint16_t node_id{0};
    std::int64_t started_utc_seconds{0};
    std::string journal_transport{"unknown"};
    std::string journal_source_ip{};
    int journal_ansi{-1};
    bool journal_new_registration{false};
    std::int64_t journal_registration_utc{0}; // actual retained account-commit audit time
};

struct PersistSessionProgressRequest {
    Identifier128 session_id;
    std::int64_t total_authenticated_seconds{0};
    std::int64_t persisted_utc_seconds{0};
    // Cumulative day segments for this session, not increments. Replays are
    // idempotent, and no earlier committed day may be removed or decreased.
    std::vector<std::pair<std::string,std::int64_t>> daily_seconds{};
};

struct FinalizeSessionRequest {
    Identifier128 session_id;
    std::int64_t total_authenticated_seconds{0};
    std::int64_t completed_utc_seconds{0};
    SessionOutcomeCode outcome{SessionOutcomeCode::normal};
    Identifier128 audit_event_id;
    std::vector<std::pair<std::string,std::int64_t>> daily_seconds{};
};
struct SessionAccountingSnapshot {
    std::int64_t total_seconds{};
    std::vector<std::pair<std::string,std::int64_t>> daily_seconds;
    // A lifetime deadline may end the post-authentication welcome stage before
    // the ready handoff reaches menus. Preserve its real terminal outcome.
    std::optional<SessionOutcomeCode> outcome{};
};

struct AccountListQuery {
    std::optional<std::string> name_prefix;
    std::optional<AccountState> state;
    std::optional<std::uint16_t> security_level;
    std::size_t limit{100};
    std::size_t offset{0};
    bool exclude_tombstoned{false}; // ordinary user lists; history/detail queries retain access
};

enum class InactivitySelectionKind : std::uint8_t { schedule_eligible, tombstone_due };

struct InactivityCandidateQuery {
    std::int64_t last_login_at_or_before_utc_seconds{0};
    std::size_t limit{100};
    InactivitySelectionKind kind{InactivitySelectionKind::schedule_eligible};
};

struct HistoryQuery {
    AccountId account_id{0};
    std::int64_t at_or_before_utc_seconds{kMaximumUtcSeconds};
    std::size_t limit{100};
    std::size_t offset{0};
    std::optional<std::pair<std::int64_t,Identifier128>> after_session{};
};

// Deliberately separate public projection: private profile/security/credentials
// never cross the directory presentation boundary.
struct CallerDirectoryEntry {
    std::string name;
    std::optional<std::string> location;
    std::optional<std::int64_t> last_logon_utc;
};
struct CallerDirectoryQuery {
    std::optional<std::string> name_fragment;
    std::optional<std::pair<std::int64_t,std::int64_t>> logon_utc_range; // [start,end)
    std::size_t limit{20}, offset{0};
};

struct AuditHistoryQuery {
    std::optional<AccountId> subject_account_id;
    std::optional<AuditEventKind> event_kind;
    std::int64_t at_or_before_utc_seconds{kMaximumUtcSeconds};
    std::size_t limit{100};
};

class IAccountStorage {
public:
    virtual ~IAccountStorage() = default;

    virtual StorageResult<FirstRunState> first_run_state() = 0;
    virtual MutationResult stage_initial_sysop(const StageInitialSysopRequest& request) = 0;
    virtual MutationResult complete_first_run(const CompleteFirstRunRequest& request) = 0;
    virtual MutationResult create_account(const CreateAccountRequest& request) = 0;
    virtual MutationResult rename_account(const RenameAccountRequest& request) = 0;
    virtual MutationResult update_profile(const UpdateProfileRequest& request) = 0;
    virtual MutationResult set_purge_allowed(const SetPurgeAllowedRequest& request) = 0;
    virtual MutationResult assign_security_level(const AssignAccountSecurityLevelRequest& request) = 0;
    virtual MutationResult save_account(const SaveAccountRequest&) {
        return {{StorageErrorCode::internal, "account editor unavailable"}, {}, {}, false};
    }
    virtual MutationResult replace_credential(const ReplaceCredentialRequest& request) = 0;
    virtual MutationResult change_account_state(const ChangeAccountStateRequest& request) = 0;
    virtual MutationResult tombstone_account(const TombstoneAccountRequest& request) = 0;
    virtual StorageStatus publish_security_policy(const PublishSecurityPolicyRequest& request) = 0;
    virtual StorageStatus publish_retention_policy(const PublishRetentionPolicyRequest& request) = 0;

    virtual MutationResult record_session_started(const StartSessionRequest& request) = 0;
    virtual StorageStatus persist_session_progress(const PersistSessionProgressRequest& request) = 0;
    virtual MutationResult finalize_session(const FinalizeSessionRequest& request) = 0;

    virtual StorageResult<AccountRecord> find_account_by_name(std::string_view candidate) = 0;
    virtual StorageResult<AccountRecord> find_account_by_id(AccountId account_id) = 0;
    virtual StorageResult<CredentialEnvelope> load_credential(AccountId account_id) = 0;
    virtual StorageResult<std::vector<AccountRecord>> list_accounts(const AccountListQuery& query) = 0;
    virtual StorageResult<std::vector<CallerDirectoryEntry>> caller_directory(const CallerDirectoryQuery&) {
        return {{StorageErrorCode::internal,"caller directory unavailable"}, {}};
    }
    virtual StorageResult<std::int64_t> caller_day_seconds(AccountId,std::string_view) {
        return {{StorageErrorCode::internal,"caller time accounting unavailable"},{}};
    }
    virtual StorageResult<std::vector<AccountRecord>> inactivity_candidates(
        const InactivityCandidateQuery& query) = 0;
    virtual StorageResult<std::vector<SessionRecord>> session_history(const HistoryQuery& query) = 0;
    virtual StorageResult<std::vector<AuditRecord>> audit_history(const AuditHistoryQuery& query) = 0;
    virtual StorageResult<std::vector<SecurityLevelRecord>> security_levels() = 0;
    virtual StorageResult<SecurityLevelRecord> security_level(std::uint16_t level) = 0;
    virtual StorageResult<SecurityPolicyRecord> security_policy() = 0;
    virtual StorageResult<RetentionPolicyRecord> retention_policy() = 0;
};

bool valid_profile(const AccountProfile& profile);
bool valid_credential_envelope(const CredentialEnvelope& credential) noexcept;
bool valid_audit_context(const AuditContext& audit) noexcept;
bool valid_sha256_text(std::string_view value) noexcept;
bool valid_typed_code(std::string_view value) noexcept;
bool valid_retention_policy(const RetentionPolicyRecord& policy) noexcept;
RetentionDisposition retention_disposition(
    std::int64_t record_utc_seconds,
    std::int64_t online_cutoff_utc_seconds,
    std::int64_t total_cutoff_utc_seconds,
    bool verified_archive_membership) noexcept;

}  // namespace sfx::application
