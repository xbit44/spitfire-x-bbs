#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "sfx/application/account_storage.hpp"
#include "sfx/application/authentication_security.hpp"
#include "sfx/application/authentication_storage.hpp"

namespace sfx::application {

enum class AuthenticationOperationCode : std::uint8_t {
    completed,
    rejected,
    password_change_required,
    policy_rejected,
    malformed_record,
    unsupported_record,
    throttled,
    backpressure,
    cancelled,
    unavailable,
    storage_failure,
    conflict,
    revocation_failed
};

struct PasswordAuthenticationResult {
    AuthenticationOperationCode code{AuthenticationOperationCode::unavailable};
    bool verified{false};
    bool password_change_required{false};
    bool rehash_attempted{false};
    bool rehash_completed{false};
    // Exact verifier used successfully (or the successfully committed rehash).
    // This is proof binding for a connection-local password handoff, never a
    // recovered password and never an independent authorization grant.
    std::optional<CredentialEnvelope> verified_credential{};

    bool authenticated() const noexcept {
        return verified && (code == AuthenticationOperationCode::completed ||
            code == AuthenticationOperationCode::password_change_required);
    }
};

struct PasswordMutationResult {
    AuthenticationOperationCode code{AuthenticationOperationCode::unavailable};
    PasswordPolicyCode policy_code{PasswordPolicyCode::accepted};
    std::optional<std::int64_t> account_revision;
    bool credential_committed{false};
    bool revocation_completed{false};
    std::optional<CredentialEnvelope> committed_credential;

    bool succeeded() const noexcept { return code == AuthenticationOperationCode::completed; }
};

struct VoluntaryPasswordChangeRequest {
    AccountId account_id{0};
    std::int64_t expected_account_revision{0};
    std::string_view current_password;
    std::string_view new_password;
    PasswordPolicyContext policy;
    Identifier128 current_session_id;
    AuthenticationAuditContext audit;
};

struct TemporaryPasswordIssueOperation {
    AccountId account_id{0};
    std::int64_t expected_account_revision{0};
    std::string_view temporary_password;
    PasswordPolicyContext policy;
    AuthenticationAuditContext audit;
};

struct TemporaryPasswordCancelOperation {
    AccountId account_id{0};
    std::int64_t expected_reset_revision{0};
    AuthenticationAuditContext audit;
};

struct TemporaryPasswordReplacementOperation {
    AccountId account_id{0};
    std::int64_t expected_account_revision{0};
    std::int64_t expected_reset_revision{0};
    std::string_view temporary_password;
    std::string_view permanent_password;
    PasswordPolicyContext policy;
    Identifier128 current_session_id;
    AuthenticationAuditContext consumed_audit;
    AuthenticationAuditContext changed_audit;
};

struct SystemPasswordReplacementOperation {
    std::int64_t expected_system_revision{0};
    std::string_view new_password;
    PasswordPolicyContext policy;
    AuthenticationAuditContext audit;
};

class AuthenticationSecurityService final {
public:
    AuthenticationSecurityService(
        IAccountStorage& accounts,
        IAuthenticationStorage& authentication_storage,
        IPasswordKdf& kdf,
        const IPasswordBlocklist* blocklist,
        ISessionRevocationSink& revocations) noexcept;

    PasswordAuthenticationResult verify_account_password(
        AccountId account_id,
        std::string_view candidate,
        std::int64_t now_utc_seconds,
        const AuthenticationAuditContext& maintenance_audit,
        const CancellationToken& cancellation);
    PasswordAuthenticationResult verify_system_password(
        std::string_view candidate,
        std::int64_t now_utc_seconds,
        const AuthenticationAuditContext& rehash_audit,
        const CancellationToken& cancellation);

    PasswordMutationResult create_password_credential(
        std::string_view password,
        const PasswordPolicyContext& policy,
        std::int64_t created_utc_seconds,
        bool password_change_required,
        const CancellationToken& cancellation,
        CredentialEnvelope& output);

    PasswordMutationResult change_account_password(
        const VoluntaryPasswordChangeRequest& request,
        const CancellationToken& cancellation);
    PasswordMutationResult issue_temporary_password(
        const TemporaryPasswordIssueOperation& request,
        const CancellationToken& cancellation,
        const std::function<bool()>& still_authorized = {});
    PasswordMutationResult cancel_temporary_password(
        const TemporaryPasswordCancelOperation& request);
    PasswordMutationResult replace_temporary_password(
        const TemporaryPasswordReplacementOperation& request,
        const CancellationToken& cancellation);
    PasswordMutationResult replace_system_password(
        const SystemPasswordReplacementOperation& request,
        const CancellationToken& cancellation);

private:
    PasswordMutationResult prepare_password(
        std::string_view password,
        const PasswordPolicyContext& policy,
        std::int64_t created_utc_seconds,
        bool password_change_required,
        const CancellationToken& cancellation,
        CredentialEnvelope& output);
    PasswordAuthenticationResult verify_and_rehash_account(
        const AccountRecord& account,
        const CredentialEnvelope& credential,
        std::string_view candidate,
        bool temporary,
        std::int64_t now_utc_seconds,
        const AuthenticationAuditContext& rehash_audit,
        const CancellationToken& cancellation);

    IAccountStorage& accounts_;
    IAuthenticationStorage& authentication_storage_;
    IPasswordKdf& kdf_;
    const IPasswordBlocklist* blocklist_;
    ISessionRevocationSink& revocations_;
};

}  // namespace sfx::application
