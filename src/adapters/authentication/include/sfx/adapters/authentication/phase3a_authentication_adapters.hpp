#pragma once

#include "sfx/application/phase3a_workflow.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>

namespace sfx::adapters::authentication {

class Phase3aCredentialOperationsBridge final
    : public application::IPhase3aCredentialOperations {
public:
    static std::unique_ptr<Phase3aCredentialOperationsBridge> create(
        application::AuthenticationSecurityService& service,
        application::IPasswordKdf& kdf,
        std::int64_t created_utc_seconds,
        const application::CancellationToken& cancellation);

    application::PasswordAuthenticationResult verify_existing_account(
        application::AccountId account_id,
        std::string_view candidate,
        std::int64_t now_utc_seconds,
        const application::AuthenticationAuditContext& maintenance_audit,
        const application::CancellationToken& cancellation) override;
    application::PasswordAuthenticationResult perform_dummy_verification(
        std::string_view candidate,
        std::int64_t now_utc_seconds,
        const application::CancellationToken& cancellation) override;
    RegistrationCredentialResult create_registration_credential(
        std::string_view candidate,
        const application::PasswordPolicyContext& policy,
        std::int64_t created_utc_seconds,
        const application::CancellationToken& cancellation) override;
    application::PasswordMutationResult replace_temporary_password(
        const application::TemporaryPasswordReplacementOperation& request,
        const application::CancellationToken& cancellation) override;
    application::PasswordAuthenticationResult verify_system_password(
        std::string_view candidate,
        std::int64_t now_utc_seconds,
        const application::AuthenticationAuditContext& maintenance_audit,
        const application::CancellationToken& cancellation) override;

private:
    Phase3aCredentialOperationsBridge(
        application::AuthenticationSecurityService& service,
        application::IPasswordKdf& kdf,
        application::CredentialEnvelope dummy_credential) noexcept;

    application::AuthenticationSecurityService& service_;
    application::IPasswordKdf& kdf_;
    application::CredentialEnvelope dummy_credential_;
};

class Phase3aRateControlCoordinator final
    : public application::IPhase3aRateControlPort {
public:
    Phase3aRateControlCoordinator(
        application::IAuthenticationStorage& storage,
        std::int64_t restore_utc_seconds,
        std::size_t maximum_subjects = 4096,std::function<void()> activated = {});
    ~Phase3aRateControlCoordinator() override;

    Phase3aRateControlCoordinator(const Phase3aRateControlCoordinator&) = delete;
    Phase3aRateControlCoordinator& operator=(const Phase3aRateControlCoordinator&) = delete;

    bool ready() const noexcept;
    // HTTP management reports the existing throttle instead of sleeping while
    // holding Runtime's operation boundary. Uses the same durable rate state.
    application::Phase3aRateGateResult try_admission(
        const application::Phase3aRateScopeSet&,std::int64_t now_utc_seconds);
    // Called only behind verified local owner authority. Serializes with every
    // failure/success publication; audit and durable deletion commit first.
    application::StorageStatus clear_locally(
        const application::LocalThrottleClearRequest& request,
        const application::AuthenticationAuditContext& audit);
    application::Phase3aRateGateResult await_admission(
        const application::Phase3aRateScopeSet& scopes,
        std::int64_t now_utc_seconds,
        const application::CancellationToken& cancellation) override;
    application::StorageStatus record_failure(
        const application::Phase3aRateScopeSet& scopes,
        std::int64_t now_utc_seconds) override;
    application::StorageStatus record_success(
        const application::Phase3aRateScopeSet& scopes,
        std::int64_t now_utc_seconds) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sfx::adapters::authentication
