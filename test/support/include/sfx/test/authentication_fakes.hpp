#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sfx/application/authentication_security.hpp"
#include "sfx/application/authentication_storage.hpp"

namespace sfx::test {

class FakePasswordBlocklist final : public application::IPasswordBlocklist {
public:
    bool available{true};
    std::unordered_set<std::string> entries;

    bool ready() const noexcept override { return available; }
    bool contains_complete_case_insensitive(const std::string_view candidate) const override {
        std::string key(candidate);
        std::transform(key.begin(), key.end(), key.begin(), [](const unsigned char value) {
            return value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')
                ? static_cast<char>(value + 0x20U)
                : static_cast<char>(value);
        });
        return entries.find(key) != entries.end();
    }
};

class DeterministicPasswordKdf final : public application::IPasswordKdf {
public:
    application::PasswordKdfCode create_code{application::PasswordKdfCode::completed};
    application::PasswordKdfCode verify_code{application::PasswordKdfCode::completed};
    bool force_rehash{false};
    std::uint8_t salt_sequence{1};
    std::uint64_t create_calls{0};
    std::uint64_t verify_calls{0};
    bool stopped{false};

    application::PasswordHashResult create(
        const std::string_view password,
        const std::int64_t created_utc_seconds,
        const bool password_change_required,
        const application::CancellationToken& cancellation) override {
        ++create_calls;
        if (cancellation.is_cancelled()) {
            return {application::PasswordKdfCode::cancelled, std::nullopt};
        }
        if (stopped || create_code != application::PasswordKdfCode::completed) {
            return {stopped ? application::PasswordKdfCode::unavailable : create_code, std::nullopt};
        }
        std::vector<std::uint8_t> salt(application::kArgon2idSaltBytes, salt_sequence++);
        std::vector<std::uint8_t> verifier = verifier_for(password, salt);
        return {application::PasswordKdfCode::completed,
            application::make_argon2id_credential_envelope(
                {}, std::move(salt), std::move(verifier), created_utc_seconds,
                password_change_required)};
    }

    application::PasswordVerifyResult verify(
        const application::CredentialEnvelope& credential,
        const std::string_view candidate,
        const application::CancellationToken& cancellation) override {
        ++verify_calls;
        if (cancellation.is_cancelled()) {
            return {application::PasswordKdfCode::cancelled, false, false};
        }
        if (stopped || verify_code != application::PasswordKdfCode::completed) {
            return {stopped ? application::PasswordKdfCode::unavailable : verify_code, false, false};
        }
        const auto decoded = application::decode_argon2id_credential_envelope(credential);
        if (!decoded.valid()) {
            return {application::PasswordKdfCode::malformed_credential, false, false};
        }
        const bool matched = verifier_for(candidate, credential.salt) == credential.verifier;
        return {application::PasswordKdfCode::completed, matched, matched && force_rehash};
    }

    application::PasswordKdfHealth health() const override {
        application::PasswordKdfHealth result;
        result.maximum_active_jobs = application::kMaximumConcurrentKdfJobs;
        result.accepting = !stopped;
        result.completed_jobs = create_calls + verify_calls;
        return result;
    }
    void shutdown() noexcept override { stopped = true; }

private:
    static std::vector<std::uint8_t> verifier_for(
        const std::string_view password,
        const std::vector<std::uint8_t>& salt) {
        std::uint64_t state = 1469598103934665603ULL;
        for (const unsigned char value : password) {
            state ^= value;
            state *= 1099511628211ULL;
        }
        for (const std::uint8_t value : salt) {
            state ^= value;
            state *= 1099511628211ULL;
        }
        std::vector<std::uint8_t> verifier(application::kArgon2idVerifierBytes);
        for (std::size_t index = 0; index < verifier.size(); ++index) {
            state ^= state >> 12U;
            state ^= state << 25U;
            state ^= state >> 27U;
            verifier[index] = static_cast<std::uint8_t>(state & 0xffU);
        }
        return verifier;
    }
};

class FakeSessionRevocationSink final : public application::ISessionRevocationSink {
public:
    bool result{true};
    std::vector<application::SessionRevocationCommand> commands;
    std::vector<application::AccountId> access_refreshes;
    bool refresh_account_access(application::AccountId id) override {access_refreshes.push_back(id);return result;}

    bool revoke(const application::SessionRevocationCommand& command) override {
        commands.push_back(command);
        return result;
    }
};

class FakeAuthenticationStorage final : public application::IAuthenticationStorage {
public:
    application::MutationResult mutation_result;
    application::StorageStatus status;
    application::StorageResult<application::TemporaryPasswordRecord> temporary_result;
    application::StorageResult<application::SystemCredentialRecord> system_result;
    application::StorageResult<std::vector<application::SuccessfulLoginIpRecord>> ip_result;
    application::StorageResult<std::vector<application::RateLimitSnapshot>> rate_result;
    application::StorageResult<std::vector<application::AuthenticationAuditRecord>> audit_result;
    std::vector<std::string> calls;
    std::optional<application::ReplaceAccountPasswordRequest> replace_account_request;
    std::optional<application::RehashAccountCredentialRequest> rehash_account_request;
    std::optional<application::IssueTemporaryPasswordRequest> issue_temporary_request;
    std::optional<application::TransitionTemporaryPasswordRequest> transition_temporary_request;
    std::optional<application::CompleteTemporaryPasswordRequest> complete_temporary_request;
    std::optional<application::ReplaceSystemPasswordRequest> replace_system_request;
    std::vector<application::RateLimitSnapshot> stored_rate_snapshots;
    std::vector<application::RateLimitKey> removed_rate_keys;
    std::optional<application::RehashSystemCredentialRequest> rehash_system_request;
    std::optional<application::SessionRevocationCommand> unused_revocation_placeholder;

    application::MutationResult replace_account_password(
        const application::ReplaceAccountPasswordRequest& request) override {
        calls.emplace_back("replace_account_password");
        replace_account_request = request;
        return mutation_result;
    }
    application::MutationResult rehash_account_credential(
        const application::RehashAccountCredentialRequest& request) override {
        calls.emplace_back("rehash_account_credential");
        rehash_account_request = request;
        return mutation_result;
    }
    application::MutationResult issue_temporary_password(
        const application::IssueTemporaryPasswordRequest& request) override {
        calls.emplace_back("issue_temporary_password");
        issue_temporary_request = request;
        return mutation_result;
    }
    application::StorageStatus transition_temporary_password(
        const application::TransitionTemporaryPasswordRequest& request) override {
        calls.emplace_back("transition_temporary_password");
        transition_temporary_request = request;
        return status;
    }
    application::MutationResult complete_temporary_password(
        const application::CompleteTemporaryPasswordRequest& request) override {
        calls.emplace_back("complete_temporary_password");
        complete_temporary_request = request;
        return mutation_result;
    }
    application::StorageResult<application::TemporaryPasswordRecord> temporary_password(
        application::AccountId) override {
        calls.emplace_back("temporary_password");
        return temporary_result;
    }
    application::StorageStatus replace_system_password(
        const application::ReplaceSystemPasswordRequest& request) override {
        calls.emplace_back("replace_system_password");
        replace_system_request = request;
        return status;
    }
    application::StorageStatus rehash_system_credential(
        const application::RehashSystemCredentialRequest& request) override {
        calls.emplace_back("rehash_system_credential");
        rehash_system_request = request;
        return status;
    }
    application::StorageResult<application::SystemCredentialRecord> system_password() override {
        calls.emplace_back("system_password");
        return system_result;
    }
    application::StorageStatus record_successful_login_ip(
        const application::RecordSuccessfulLoginIpRequest&) override {
        calls.emplace_back("record_successful_login_ip");
        return status;
    }
    application::StorageResult<std::vector<application::SuccessfulLoginIpRecord>>
    successful_login_ips(application::AccountId) override {
        calls.emplace_back("successful_login_ips");
        return ip_result;
    }
    application::StorageStatus store_rate_limit_snapshot(
        const application::RateLimitSnapshot& snapshot) override {
        calls.emplace_back("store_rate_limit_snapshot");
        const auto found = std::find_if(stored_rate_snapshots.begin(),
            stored_rate_snapshots.end(), [&snapshot](const auto& value) {
                return value.key == snapshot.key;
            });
        if (found == stored_rate_snapshots.end()) stored_rate_snapshots.push_back(snapshot);
        else *found = snapshot;
        return status;
    }
    application::StorageStatus remove_rate_limit_snapshot(
        const application::RateLimitKey& key) override {
        calls.emplace_back("remove_rate_limit_snapshot");
        removed_rate_keys.push_back(key);
        stored_rate_snapshots.erase(std::remove_if(stored_rate_snapshots.begin(),
            stored_rate_snapshots.end(), [&key](const auto& value) {
                return value.key == key;
            }), stored_rate_snapshots.end());
        return status;
    }
    application::StorageStatus clear_rate_limit(
        const application::ClearRateLimitRequest&) override {
        calls.emplace_back("clear_rate_limit");
        return status;
    }
    application::StorageResult<std::vector<application::RateLimitSnapshot>>
    active_rate_limit_snapshots(std::int64_t, std::size_t) override {
        calls.emplace_back("active_rate_limit_snapshots");
        return rate_result;
    }
    application::StorageResult<std::vector<application::AuthenticationAuditRecord>>
    authentication_audit_history(const application::AuthenticationAuditQuery&) override {
        calls.emplace_back("authentication_audit_history");
        return audit_result;
    }
};

}  // namespace sfx::test
