#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sfx/application/account_storage.hpp"
#include "sfx/application/cancellation.hpp"

namespace sfx::application {

inline constexpr std::size_t kPasswordMinimumStandard = 4;
inline constexpr std::size_t kPasswordMinimumStrong = 15;
inline constexpr std::size_t kPasswordMaximum = 128;
inline constexpr std::uint32_t kArgon2idMemoryKib = 32U * 1024U;
inline constexpr std::uint32_t kArgon2idIterations = 3;
inline constexpr std::uint32_t kArgon2idParallelism = 1;
inline constexpr std::size_t kArgon2idSaltBytes = 16;
inline constexpr std::size_t kArgon2idVerifierBytes = 32;
inline constexpr std::size_t kMaximumConcurrentKdfJobs = 2;
inline constexpr std::int64_t kTemporaryPasswordLifetimeSeconds = 72LL * 60LL * 60LL;
inline constexpr std::uint8_t kFailedAttemptsPerConnection = 3;

struct Argon2idProfile {
    std::uint32_t memory_kib{kArgon2idMemoryKib};
    std::uint32_t iterations{kArgon2idIterations};
    std::uint32_t parallelism{kArgon2idParallelism};
    std::uint16_t salt_bytes{static_cast<std::uint16_t>(kArgon2idSaltBytes)};
    std::uint16_t verifier_bytes{static_cast<std::uint16_t>(kArgon2idVerifierBytes)};

    friend bool operator==(const Argon2idProfile& left, const Argon2idProfile& right) noexcept;
    friend bool operator!=(const Argon2idProfile& left, const Argon2idProfile& right) noexcept;
};

enum class PasswordMode : std::uint8_t { standard, strong };
enum class PasswordPolicyCode : std::uint8_t {
    accepted,
    too_short,
    too_long,
    non_printable_ascii,
    blocklisted,
    context_derived,
    blocklist_unavailable
};

struct PasswordPolicyContext {
    PasswordMode mode{PasswordMode::standard};
    std::string account_name;
    std::string bbs_name;
};

class IPasswordBlocklist {
public:
    virtual ~IPasswordBlocklist() = default;
    virtual bool ready() const noexcept = 0;
    virtual bool contains_complete_case_insensitive(std::string_view candidate) const = 0;
};

PasswordPolicyCode evaluate_password_policy(
    std::string_view candidate,
    const PasswordPolicyContext& context,
    const IPasswordBlocklist* blocklist) noexcept;

enum class CredentialEnvelopeCode : std::uint8_t {
    valid,
    malformed,
    oversized,
    unsupported_version,
    unsupported_scheme,
    unsupported_parameters
};

struct DecodedCredentialEnvelope {
    CredentialEnvelopeCode code{CredentialEnvelopeCode::malformed};
    Argon2idProfile profile;

    bool valid() const noexcept { return code == CredentialEnvelopeCode::valid; }
};

CredentialEnvelope make_argon2id_credential_envelope(
    const Argon2idProfile& profile,
    std::vector<std::uint8_t> salt,
    std::vector<std::uint8_t> verifier,
    std::int64_t created_utc_seconds,
    bool password_change_required);
DecodedCredentialEnvelope decode_argon2id_credential_envelope(
    const CredentialEnvelope& envelope) noexcept;
bool credential_needs_rehash(
    const CredentialEnvelope& envelope,
    const Argon2idProfile& current_profile) noexcept;

enum class PasswordKdfCode : std::uint8_t {
    completed,
    malformed_credential,
    unsupported_credential,
    backpressure,
    cancelled,
    unavailable
};

struct PasswordHashResult {
    PasswordKdfCode code{PasswordKdfCode::unavailable};
    std::optional<CredentialEnvelope> credential;

    bool succeeded() const noexcept {
        return code == PasswordKdfCode::completed && credential.has_value();
    }
};

struct PasswordVerifyResult {
    PasswordKdfCode code{PasswordKdfCode::unavailable};
    bool verified{false};
    bool rehash_required{false};

    bool completed() const noexcept { return code == PasswordKdfCode::completed; }
};

struct PasswordKdfHealth {
    std::size_t maximum_active_jobs{0};
    std::size_t queue_capacity{0};
    std::size_t active_jobs{0};
    std::size_t queued_jobs{0};
    std::size_t active_high_water{0};
    std::size_t queue_high_water{0};
    std::size_t estimated_memory_high_water_bytes{0};
    std::uint64_t completed_jobs{0};
    std::uint64_t rejected_jobs{0};
    std::uint64_t cancelled_jobs{0};
    bool accepting{false};
};

class IPasswordKdf {
public:
    virtual ~IPasswordKdf() = default;
    virtual PasswordHashResult create(
        std::string_view password,
        std::int64_t created_utc_seconds,
        bool password_change_required,
        const CancellationToken& cancellation) = 0;
    virtual PasswordVerifyResult verify(
        const CredentialEnvelope& credential,
        std::string_view candidate,
        const CancellationToken& cancellation) = 0;
    virtual PasswordKdfHealth health() const = 0;
    virtual void shutdown() noexcept = 0;
};

enum class SysopSequenceAction : std::uint8_t {
    reject,
    request_system_password
};

SysopSequenceAction next_sysop_sequence_action(
    bool account_password_verified,
    std::uint16_t account_security_level,
    std::uint16_t configured_sysop_threshold) noexcept;
bool sysop_authority_granted(
    SysopSequenceAction prior_action,
    bool system_password_verified) noexcept;

enum class SessionRevocationKind : std::uint8_t {
    account_all,
    account_except_current,
    sysop_level_all
};

struct SessionRevocationCommand {
    SessionRevocationKind kind{SessionRevocationKind::account_all};
    std::optional<AccountId> account_id;
    std::optional<Identifier128> current_session_id;
};

class ISessionRevocationSink {
public:
    virtual ~ISessionRevocationSink() = default;
    virtual bool revoke(const SessionRevocationCommand& command) = 0;
    // Hosts with a live authorization view refresh it in place. Other hosts
    // fail safely by revoking; no stale elevated session may remain usable.
    virtual bool refresh_account_access(AccountId account_id) {
        return revoke({SessionRevocationKind::account_all, account_id, {}});
    }
};

struct ConnectionLimitPolicy {
    std::uint16_t maximum_connections_per_source{4};
    std::uint16_t maximum_sessions_per_account{1};
    std::uint16_t maximum_logical_nodes{128};
};

// Exact canonical IP overrides affect only connection counts, never password,
// session-account, rate, SY or BotGate authority. No hostnames/CIDRs/wildcards.
struct SourceConnectionLimit {
    std::string address;
    std::uint16_t maximum_connections{4};
    friend bool operator==(const SourceConnectionLimit& a,const SourceConnectionLimit& b) noexcept {
        return a.address == b.address && a.maximum_connections == b.maximum_connections;
    }
};
inline constexpr std::size_t kMaximumSourceConnectionExceptions = 32;
bool valid_source_connection_limits(const std::vector<SourceConnectionLimit>& exceptions);
std::optional<std::vector<SourceConnectionLimit>> parse_source_connection_limits(std::string_view serialized);
std::string serialize_source_connection_limits(const std::vector<SourceConnectionLimit>& exceptions);

bool valid_connection_limit_policy(const ConnectionLimitPolicy& policy) noexcept;

enum class ConnectionAttemptAction : std::uint8_t { continue_login, disconnect };

struct ConnectionAttemptState {
    std::uint8_t failed_account_attempts{0};
};

ConnectionAttemptAction record_failed_account_attempt(ConnectionAttemptState& state) noexcept;

enum class ConnectionAdmissionCode : std::uint8_t {
    admitted,
    source_connection_limit,
    logical_node_limit,
    account_session_limit
};

ConnectionAdmissionCode evaluate_prelogin_admission(
    std::uint16_t active_source_connections,
    std::uint16_t active_logical_nodes,
    const ConnectionLimitPolicy& policy) noexcept;
ConnectionAdmissionCode evaluate_account_session_admission(
    std::uint16_t active_account_sessions,
    const ConnectionLimitPolicy& policy) noexcept;

enum class RateLimitScope : std::uint8_t { account, source, system_password, global };

// Local administration chooses exactly one existing rate scope. Account uses a
// canonical positive decimal ID; source uses one canonical IP; shared scopes
// require an empty subject. This never creates an exception or changes policy.
struct LocalThrottleClearRequest {
    RateLimitScope scope{RateLimitScope::source};
    std::string subject;
};
bool valid_local_throttle_clear(const LocalThrottleClearRequest& request);

struct RateLimitKey {
    RateLimitScope scope{RateLimitScope::account};
    std::array<std::uint8_t, 32> subject_hash{};

    friend bool operator==(const RateLimitKey& left, const RateLimitKey& right) noexcept;
};

struct RateScopePolicy {
    std::uint32_t burst_allowance{0};
    std::uint32_t base_delay_seconds{0};
    std::uint32_t maximum_delay_seconds{0};
    std::uint32_t decay_interval_seconds{0};
    std::uint32_t state_ttl_seconds{0};
};

struct RateLimitProfile {
    RateScopePolicy account;
    RateScopePolicy source;
    RateScopePolicy system_password;
    RateScopePolicy global;
};

bool valid_rate_limit_profile(const RateLimitProfile& profile) noexcept;
RateLimitProfile provisional_rate_profile_a() noexcept;
RateLimitProfile provisional_rate_profile_b() noexcept;

struct RateLimitSnapshot {
    RateLimitKey key;
    std::uint32_t failure_count{0};
    std::int64_t window_started_utc_seconds{0};
    std::int64_t last_failure_utc_seconds{0};
    std::int64_t blocked_until_utc_seconds{0};
    std::int64_t expires_utc_seconds{0};
};

struct RateLimitDecision {
    bool admitted{true};
    std::uint32_t delay_seconds{0};
    std::uint32_t failure_count{0};
};

struct RateLimitHealth {
    std::size_t tracked_subjects{0};
    std::size_t tracked_high_water{0};
    std::uint64_t admitted_attempts{0};
    std::uint64_t delayed_attempts{0};
    std::uint64_t expired_states{0};
};

class RateControlCoordinator final {
public:
    explicit RateControlCoordinator(RateLimitProfile profile, std::size_t maximum_subjects = 4096);
    ~RateControlCoordinator();

    RateControlCoordinator(const RateControlCoordinator&) = delete;
    RateControlCoordinator& operator=(const RateControlCoordinator&) = delete;

    RateLimitDecision assess(const RateLimitKey& key, std::int64_t now_utc_seconds);
    RateLimitDecision record_failure(const RateLimitKey& key, std::int64_t now_utc_seconds);
    // Read-only, allocation-free observation; never assesses/adopts/decays state.
    bool temporarily_blocked(const RateLimitKey& key,std::int64_t now_utc_seconds) const noexcept;
    void record_success(const RateLimitKey& key) noexcept;
    void clear(const RateLimitKey& key) noexcept;
    bool restore(const RateLimitSnapshot& snapshot, std::int64_t now_utc_seconds);
    std::vector<RateLimitSnapshot> snapshots(std::int64_t now_utc_seconds) const;
    RateLimitHealth health() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sfx::application
