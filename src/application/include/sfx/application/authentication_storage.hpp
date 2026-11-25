#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sfx/application/authentication_security.hpp"

namespace sfx::application {

enum class TemporaryPasswordState : std::uint8_t { active, consumed, cancelled, expired };
enum class AuthenticationProtocol : std::uint8_t { telnet, ssh };
enum class AuthenticationAuditEventKind : std::uint8_t {
    account_password_changed,
    account_credential_rehashed,
    temporary_password_issued,
    temporary_password_consumed,
    temporary_password_cancelled,
    temporary_password_expired,
    system_password_replaced,
    system_credential_rehashed,
    throttling_cleared
};

std::string_view storage_code(TemporaryPasswordState value) noexcept;
std::string_view storage_code(AuthenticationProtocol value) noexcept;
std::string_view storage_code(AuthenticationAuditEventKind value) noexcept;
std::string_view storage_code(RateLimitScope value) noexcept;

struct AuthenticationAuditContext {
    Identifier128 event_id;
    Identifier128 correlation_id;
    std::optional<AccountId> actor_account_id;
    std::int64_t occurred_utc_seconds{0};
    std::optional<Identifier128> sysop_session{};
};

struct SystemCredentialRecord {
    std::int64_t revision{0};
    CredentialEnvelope credential;
    std::int64_t modified_utc_seconds{0};
};

struct TemporaryPasswordRecord {
    AccountId account_id{0};
    std::int64_t revision{0};
    TemporaryPasswordState state{TemporaryPasswordState::active};
    std::int64_t issued_utc_seconds{0};
    std::int64_t expires_utc_seconds{0};
    std::optional<std::int64_t> ended_utc_seconds;
};

struct SuccessfulLoginIpRecord {
    Identifier128 event_id;
    AccountId account_id{0};
    std::string source_ip;
    AuthenticationProtocol protocol{AuthenticationProtocol::telnet};
    std::int64_t succeeded_utc_seconds{0};
};

struct AuthenticationAuditRecord {
    Identifier128 event_id;
    AuthenticationAuditEventKind event_kind{AuthenticationAuditEventKind::account_password_changed};
    std::optional<AccountId> subject_account_id;
    std::optional<AccountId> actor_account_id;
    std::int64_t occurred_utc_seconds{0};
    Identifier128 correlation_id;
};

struct ReplaceAccountPasswordRequest {
    AccountId account_id{0};
    std::int64_t expected_account_revision{0};
    CredentialEnvelope credential;
    AuthenticationAuditContext audit;
};

struct RehashAccountCredentialRequest {
    AccountId account_id{0};
    std::int64_t expected_account_revision{0};
    CredentialEnvelope credential;
    AuthenticationAuditContext audit;
};

struct IssueTemporaryPasswordRequest {
    AccountId account_id{0};
    std::int64_t expected_account_revision{0};
    CredentialEnvelope credential;
    std::int64_t expires_utc_seconds{0};
    AuthenticationAuditContext audit;
};

struct TransitionTemporaryPasswordRequest {
    AccountId account_id{0};
    std::int64_t expected_reset_revision{0};
    TemporaryPasswordState new_state{TemporaryPasswordState::consumed};
    AuthenticationAuditContext audit;
};

struct CompleteTemporaryPasswordRequest {
    AccountId account_id{0};
    std::int64_t expected_account_revision{0};
    std::int64_t expected_reset_revision{0};
    CredentialEnvelope credential;
    AuthenticationAuditContext consumed_audit;
    AuthenticationAuditContext changed_audit;
};

struct ReplaceSystemPasswordRequest {
    std::int64_t expected_revision{0};
    CredentialEnvelope credential;
    AuthenticationAuditContext audit;
};

struct RehashSystemCredentialRequest {
    std::int64_t expected_revision{0};
    CredentialEnvelope credential;
    AuthenticationAuditContext audit;
};

struct RecordSuccessfulLoginIpRequest {
    Identifier128 event_id;
    AccountId account_id{0};
    std::string source_ip;
    AuthenticationProtocol protocol{AuthenticationProtocol::telnet};
    std::int64_t succeeded_utc_seconds{0};
};

struct ClearRateLimitRequest {
    RateLimitKey key;
    AuthenticationAuditContext audit;
};

struct AuthenticationAuditQuery {
    std::optional<AccountId> subject_account_id;
    std::optional<AuthenticationAuditEventKind> event_kind;
    std::int64_t at_or_before_utc_seconds{kMaximumUtcSeconds};
    std::size_t limit{100};
};

class IAuthenticationStorage {
public:
    virtual ~IAuthenticationStorage() = default;

    virtual MutationResult replace_account_password(const ReplaceAccountPasswordRequest& request) = 0;
    virtual MutationResult rehash_account_credential(const RehashAccountCredentialRequest& request) = 0;
    virtual MutationResult issue_temporary_password(const IssueTemporaryPasswordRequest& request) = 0;
    virtual StorageStatus transition_temporary_password(
        const TransitionTemporaryPasswordRequest& request) = 0;
    virtual MutationResult complete_temporary_password(
        const CompleteTemporaryPasswordRequest& request) = 0;
    virtual StorageResult<TemporaryPasswordRecord> temporary_password(AccountId account_id) = 0;

    virtual StorageStatus replace_system_password(const ReplaceSystemPasswordRequest& request) = 0;
    virtual StorageStatus rehash_system_credential(const RehashSystemCredentialRequest& request) = 0;
    virtual StorageResult<SystemCredentialRecord> system_password() = 0;

    virtual StorageStatus record_successful_login_ip(const RecordSuccessfulLoginIpRequest& request) = 0;
    virtual StorageResult<std::vector<SuccessfulLoginIpRecord>> successful_login_ips(AccountId account_id) = 0;

    virtual StorageStatus store_rate_limit_snapshot(const RateLimitSnapshot& snapshot) = 0;
    virtual StorageStatus remove_rate_limit_snapshot(const RateLimitKey& key) = 0;
    virtual StorageStatus clear_rate_limit(const ClearRateLimitRequest& request) = 0;
    virtual StorageResult<std::vector<RateLimitSnapshot>> active_rate_limit_snapshots(
        std::int64_t now_utc_seconds,
        std::size_t limit) = 0;

    virtual StorageResult<std::vector<AuthenticationAuditRecord>> authentication_audit_history(
        const AuthenticationAuditQuery& query) = 0;
};

bool valid_authentication_audit_context(const AuthenticationAuditContext& audit) noexcept;
bool valid_source_ip_text(std::string_view value) noexcept;
bool valid_rate_limit_snapshot(const RateLimitSnapshot& snapshot) noexcept;

}  // namespace sfx::application
