#include "sfx/application/authentication_service.hpp"

namespace sfx::application {
namespace {

AuthenticationOperationCode map_kdf_code(const PasswordKdfCode code) noexcept {
    switch (code) {
        case PasswordKdfCode::completed: return AuthenticationOperationCode::completed;
        case PasswordKdfCode::malformed_credential: return AuthenticationOperationCode::malformed_record;
        case PasswordKdfCode::unsupported_credential: return AuthenticationOperationCode::unsupported_record;
        case PasswordKdfCode::backpressure: return AuthenticationOperationCode::backpressure;
        case PasswordKdfCode::cancelled: return AuthenticationOperationCode::cancelled;
        case PasswordKdfCode::unavailable: return AuthenticationOperationCode::unavailable;
    }
    return AuthenticationOperationCode::unavailable;
}

AuthenticationOperationCode map_storage_status(const StorageStatus& status) noexcept {
    if (status.succeeded()) return AuthenticationOperationCode::completed;
    if (status.code == StorageErrorCode::conflict) return AuthenticationOperationCode::conflict;
    if (status.code == StorageErrorCode::backpressure || status.code == StorageErrorCode::busy) {
        return AuthenticationOperationCode::backpressure;
    }
    return AuthenticationOperationCode::storage_failure;
}

bool valid_operation_audit(const AuthenticationAuditContext& audit) noexcept {
    return valid_authentication_audit_context(audit);
}

PasswordMutationResult mutation_result(const AuthenticationOperationCode code) noexcept {
    PasswordMutationResult result;
    result.code = code;
    return result;
}

}  // namespace

AuthenticationSecurityService::AuthenticationSecurityService(
    IAccountStorage& accounts,
    IAuthenticationStorage& authentication_storage,
    IPasswordKdf& kdf,
    const IPasswordBlocklist* const blocklist,
    ISessionRevocationSink& revocations) noexcept
    : accounts_(accounts),
      authentication_storage_(authentication_storage),
      kdf_(kdf),
      blocklist_(blocklist),
      revocations_(revocations) {}

PasswordMutationResult AuthenticationSecurityService::prepare_password(
    const std::string_view password,
    const PasswordPolicyContext& policy,
    const std::int64_t created_utc_seconds,
    const bool password_change_required,
    const CancellationToken& cancellation,
    CredentialEnvelope& output) {
    PasswordMutationResult result;
    result.policy_code = evaluate_password_policy(password, policy, blocklist_);
    if (result.policy_code != PasswordPolicyCode::accepted) {
        result.code = AuthenticationOperationCode::policy_rejected;
        return result;
    }
    if (!valid_utc_seconds(created_utc_seconds)) {
        result.code = AuthenticationOperationCode::storage_failure;
        return result;
    }
    PasswordHashResult created = kdf_.create(
        password, created_utc_seconds, password_change_required, cancellation);
    if (!created.succeeded()) {
        result.code = map_kdf_code(created.code);
        return result;
    }
    const PasswordVerifyResult self_check = kdf_.verify(*created.credential, password, cancellation);
    if (!self_check.completed() || !self_check.verified) {
        result.code = self_check.completed()
            ? AuthenticationOperationCode::unavailable
            : map_kdf_code(self_check.code);
        return result;
    }
    output = std::move(*created.credential);
    result.code = AuthenticationOperationCode::completed;
    return result;
}

PasswordAuthenticationResult AuthenticationSecurityService::verify_and_rehash_account(
    const AccountRecord& account,
    const CredentialEnvelope& credential,
    const std::string_view candidate,
    const bool temporary,
    const std::int64_t now_utc_seconds,
    const AuthenticationAuditContext& rehash_audit,
    const CancellationToken& cancellation) {
    const PasswordVerifyResult checked = kdf_.verify(credential, candidate, cancellation);
    if (!checked.completed()) return {map_kdf_code(checked.code), false, false, false, false};
    if (!checked.verified) return {AuthenticationOperationCode::rejected, false, false, false, false};
    PasswordAuthenticationResult result;
    result.code = temporary
        ? AuthenticationOperationCode::password_change_required
        : AuthenticationOperationCode::completed;
    result.verified = true;
    result.password_change_required = temporary || credential.password_change_required;
    result.verified_credential=credential;
    if (!checked.rehash_required) return result;

    result.rehash_attempted = true;
    CredentialEnvelope replacement;
    const PasswordHashResult created = kdf_.create(
        candidate, now_utc_seconds, credential.password_change_required, cancellation);
    if (!created.succeeded()) return result;
    const PasswordVerifyResult self_check = kdf_.verify(*created.credential, candidate, cancellation);
    if (!self_check.completed() || !self_check.verified) return result;
    replacement = std::move(*created.credential);
    RehashAccountCredentialRequest request;
    request.account_id = account.account_id;
    request.expected_account_revision = account.revision;
    request.credential = std::move(replacement);
    request.audit = rehash_audit;
    result.rehash_completed = authentication_storage_.rehash_account_credential(request).succeeded();
    if(result.rehash_completed)result.verified_credential=request.credential;
    return result;
}

PasswordAuthenticationResult AuthenticationSecurityService::verify_account_password(
    const AccountId account_id,
    const std::string_view candidate,
    const std::int64_t now_utc_seconds,
    const AuthenticationAuditContext& maintenance_audit,
    const CancellationToken& cancellation) {
    const auto account = accounts_.find_account_by_id(account_id);
    if (!account || !account.value.has_value() || account.value->state != AccountState::active) {
        return {AuthenticationOperationCode::rejected, false, false, false, false};
    }
    bool temporary = false;
    const auto reset = authentication_storage_.temporary_password(account_id);
    if (reset && reset.value.has_value() && reset.value->state == TemporaryPasswordState::active) {
        temporary = true;
        if (now_utc_seconds >= reset.value->expires_utc_seconds) {
            if (!valid_operation_audit(maintenance_audit) ||
                !authentication_storage_.transition_temporary_password({
                    account_id,
                    reset.value->revision,
                    TemporaryPasswordState::expired,
                    maintenance_audit}).succeeded()) {
                return {AuthenticationOperationCode::storage_failure, false, false, false, false};
            }
            return {AuthenticationOperationCode::rejected, false, false, false, false};
        }
    } else if (!reset.status.succeeded() && reset.status.code != StorageErrorCode::not_found) {
        return {AuthenticationOperationCode::storage_failure, false, false, false, false};
    }
    const auto credential = accounts_.load_credential(account_id);
    if (!credential || !credential.value.has_value()) {
        return {AuthenticationOperationCode::rejected, false, false, false, false};
    }
    if (credential.value->password_change_required != temporary) {
        return {AuthenticationOperationCode::malformed_record, false, false, false, false};
    }
    return verify_and_rehash_account(
        *account.value, *credential.value, candidate, temporary, now_utc_seconds,
        maintenance_audit, cancellation);
}

PasswordAuthenticationResult AuthenticationSecurityService::verify_system_password(
    const std::string_view candidate,
    const std::int64_t now_utc_seconds,
    const AuthenticationAuditContext& rehash_audit,
    const CancellationToken& cancellation) {
    const auto stored = authentication_storage_.system_password();
    if (!stored || !stored.value.has_value()) {
        return {AuthenticationOperationCode::rejected, false, false, false, false};
    }
    const PasswordVerifyResult checked = kdf_.verify(stored.value->credential, candidate, cancellation);
    if (!checked.completed()) return {map_kdf_code(checked.code), false, false, false, false};
    if (!checked.verified) return {AuthenticationOperationCode::rejected, false, false, false, false};
    PasswordAuthenticationResult result{AuthenticationOperationCode::completed, true, false, false, false};
    if (!checked.rehash_required) return result;
    result.rehash_attempted = true;
    const PasswordHashResult created = kdf_.create(
        candidate, now_utc_seconds, false, cancellation);
    if (!created.succeeded()) return result;
    const PasswordVerifyResult self_check = kdf_.verify(*created.credential, candidate, cancellation);
    if (!self_check.completed() || !self_check.verified) return result;
    RehashSystemCredentialRequest request;
    request.expected_revision = stored.value->revision;
    request.credential = std::move(*created.credential);
    request.audit = rehash_audit;
    result.rehash_completed = authentication_storage_.rehash_system_credential(request).succeeded();
    return result;
}

PasswordMutationResult AuthenticationSecurityService::create_password_credential(
    const std::string_view password,
    const PasswordPolicyContext& policy,
    const std::int64_t created_utc_seconds,
    const bool password_change_required,
    const CancellationToken& cancellation,
    CredentialEnvelope& output) {
    return prepare_password(password, policy, created_utc_seconds,
        password_change_required, cancellation, output);
}

PasswordMutationResult AuthenticationSecurityService::change_account_password(
    const VoluntaryPasswordChangeRequest& request,
    const CancellationToken& cancellation) {
    if (request.account_id <= 0 || request.expected_account_revision < 1 ||
        !request.current_session_id.valid() || !valid_operation_audit(request.audit)) {
        return mutation_result(AuthenticationOperationCode::storage_failure);
    }
    const auto current = accounts_.load_credential(request.account_id);
    if (!current || !current.value.has_value() || current.value->password_change_required) {
        return mutation_result(AuthenticationOperationCode::rejected);
    }
    const PasswordVerifyResult current_check = kdf_.verify(
        *current.value, request.current_password, cancellation);
    if (!current_check.completed()) return mutation_result(map_kdf_code(current_check.code));
    if (!current_check.verified) return mutation_result(AuthenticationOperationCode::rejected);
    CredentialEnvelope replacement;
    PasswordMutationResult result = prepare_password(
        request.new_password, request.policy, request.audit.occurred_utc_seconds,
        false, cancellation, replacement);
    if (!result.succeeded()) return result;
    ReplaceAccountPasswordRequest storage_request;
    storage_request.account_id = request.account_id;
    storage_request.expected_account_revision = request.expected_account_revision;
    storage_request.credential = std::move(replacement);
    storage_request.audit = request.audit;
    const MutationResult stored = authentication_storage_.replace_account_password(storage_request);
    result.code = map_storage_status(stored.status);
    result.account_revision = stored.revision;
    result.credential_committed = stored.succeeded();
    if (!stored.succeeded()) return result;
    result.committed_credential=storage_request.credential;
    result.revocation_completed = revocations_.revoke({
        SessionRevocationKind::account_except_current,
        request.account_id,
        request.current_session_id});
    if (!result.revocation_completed) result.code = AuthenticationOperationCode::revocation_failed;
    return result;
}

PasswordMutationResult AuthenticationSecurityService::issue_temporary_password(
    const TemporaryPasswordIssueOperation& request,
    const CancellationToken& cancellation,
    const std::function<bool()>& still_authorized) {
    if (request.account_id <= 0 || request.expected_account_revision < 1 ||
        !valid_operation_audit(request.audit)) {
        return mutation_result(AuthenticationOperationCode::storage_failure);
    }
    CredentialEnvelope replacement;
    PasswordMutationResult result = prepare_password(
        request.temporary_password, request.policy, request.audit.occurred_utc_seconds,
        true, cancellation, replacement);
    if (!result.succeeded()) return result;
    if (still_authorized && !still_authorized())
        return mutation_result(AuthenticationOperationCode::rejected);
    IssueTemporaryPasswordRequest storage_request;
    storage_request.account_id = request.account_id;
    storage_request.expected_account_revision = request.expected_account_revision;
    storage_request.credential = std::move(replacement);
    storage_request.expires_utc_seconds =
        request.audit.occurred_utc_seconds + kTemporaryPasswordLifetimeSeconds;
    storage_request.audit = request.audit;
    const MutationResult stored = authentication_storage_.issue_temporary_password(storage_request);
    result.code = map_storage_status(stored.status);
    result.account_revision = stored.revision;
    result.credential_committed = stored.succeeded();
    if (!stored.succeeded()) return result;
    result.revocation_completed = revocations_.revoke({
        SessionRevocationKind::account_all, request.account_id, std::nullopt});
    if (!result.revocation_completed) result.code = AuthenticationOperationCode::revocation_failed;
    return result;
}

PasswordMutationResult AuthenticationSecurityService::cancel_temporary_password(
    const TemporaryPasswordCancelOperation& request) {
    if (request.account_id <= 0 || request.expected_reset_revision < 1 ||
        !valid_operation_audit(request.audit)) {
        return mutation_result(AuthenticationOperationCode::storage_failure);
    }
    const StorageStatus stored = authentication_storage_.transition_temporary_password({
        request.account_id,
        request.expected_reset_revision,
        TemporaryPasswordState::cancelled,
        request.audit});
    PasswordMutationResult result;
    result.code = map_storage_status(stored);
    result.credential_committed = stored.succeeded();
    result.revocation_completed = stored.succeeded();
    return result;
}

PasswordMutationResult AuthenticationSecurityService::replace_temporary_password(
    const TemporaryPasswordReplacementOperation& request,
    const CancellationToken& cancellation) {
    if (request.account_id <= 0 || request.expected_account_revision < 1 ||
        request.expected_reset_revision < 1 || !request.current_session_id.valid() ||
        !valid_operation_audit(request.consumed_audit) ||
        !valid_operation_audit(request.changed_audit)) {
        return mutation_result(AuthenticationOperationCode::storage_failure);
    }
    const auto reset = authentication_storage_.temporary_password(request.account_id);
    if (!reset || !reset.value.has_value() || reset.value->state != TemporaryPasswordState::active ||
        reset.value->revision != request.expected_reset_revision ||
        request.consumed_audit.occurred_utc_seconds >= reset.value->expires_utc_seconds) {
        return mutation_result(AuthenticationOperationCode::rejected);
    }
    const auto current = accounts_.load_credential(request.account_id);
    if (!current || !current.value.has_value() || !current.value->password_change_required) {
        return mutation_result(AuthenticationOperationCode::rejected);
    }
    const PasswordVerifyResult temporary_check = kdf_.verify(
        *current.value, request.temporary_password, cancellation);
    if (!temporary_check.completed()) return mutation_result(map_kdf_code(temporary_check.code));
    if (!temporary_check.verified) return mutation_result(AuthenticationOperationCode::rejected);
    CredentialEnvelope replacement;
    PasswordMutationResult result = prepare_password(
        request.permanent_password, request.policy, request.changed_audit.occurred_utc_seconds,
        false, cancellation, replacement);
    if (!result.succeeded()) return result;
    CompleteTemporaryPasswordRequest storage_request;
    storage_request.account_id = request.account_id;
    storage_request.expected_account_revision = request.expected_account_revision;
    storage_request.expected_reset_revision = request.expected_reset_revision;
    storage_request.credential = std::move(replacement);
    storage_request.consumed_audit = request.consumed_audit;
    storage_request.changed_audit = request.changed_audit;
    const MutationResult stored = authentication_storage_.complete_temporary_password(storage_request);
    result.code = map_storage_status(stored.status);
    result.account_revision = stored.revision;
    result.credential_committed = stored.succeeded();
    if (!stored.succeeded()) return result;
    result.committed_credential=storage_request.credential;
    result.revocation_completed = true;
    return result;
}

PasswordMutationResult AuthenticationSecurityService::replace_system_password(
    const SystemPasswordReplacementOperation& request,
    const CancellationToken& cancellation) {
    if (request.expected_system_revision < 0 || !valid_operation_audit(request.audit)) {
        return mutation_result(AuthenticationOperationCode::storage_failure);
    }
    CredentialEnvelope replacement;
    PasswordMutationResult result = prepare_password(
        request.new_password, request.policy, request.audit.occurred_utc_seconds,
        false, cancellation, replacement);
    if (!result.succeeded()) return result;
    ReplaceSystemPasswordRequest storage_request;
    storage_request.expected_revision = request.expected_system_revision;
    storage_request.credential = std::move(replacement);
    storage_request.audit = request.audit;
    const StorageStatus stored = authentication_storage_.replace_system_password(storage_request);
    result.code = map_storage_status(stored);
    result.credential_committed = stored.succeeded();
    if (!stored.succeeded()) return result;
    result.revocation_completed = revocations_.revoke({
        SessionRevocationKind::sysop_level_all, std::nullopt, std::nullopt});
    if (!result.revocation_completed) result.code = AuthenticationOperationCode::revocation_failed;
    return result;
}

}  // namespace sfx::application
