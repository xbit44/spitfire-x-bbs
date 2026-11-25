#include "sfx/adapters/authentication/phase3a_authentication_adapters.hpp"

#include "sfx/adapters/authentication/sodium_authentication.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sfx::adapters::authentication {
namespace {

using namespace sfx::application;

AuthenticationOperationCode map_kdf_code(const PasswordKdfCode code) noexcept {
    switch (code) {
        case PasswordKdfCode::completed: return AuthenticationOperationCode::completed;
        case PasswordKdfCode::malformed_credential:
            return AuthenticationOperationCode::malformed_record;
        case PasswordKdfCode::unsupported_credential:
            return AuthenticationOperationCode::unsupported_record;
        case PasswordKdfCode::backpressure: return AuthenticationOperationCode::backpressure;
        case PasswordKdfCode::cancelled: return AuthenticationOperationCode::cancelled;
        case PasswordKdfCode::unavailable: return AuthenticationOperationCode::unavailable;
    }
    return AuthenticationOperationCode::unavailable;
}

bool hash_present(const RateLimitKey& key) noexcept {
    return std::any_of(key.subject_hash.begin(), key.subject_hash.end(),
        [](const std::uint8_t value) { return value != 0; });
}

bool key_less(const RateLimitKey& left, const RateLimitKey& right) noexcept {
    if (left.scope != right.scope) return left.scope < right.scope;
    return left.subject_hash < right.subject_hash;
}

std::optional<std::vector<RateLimitKey>> keys_for(
    const Phase3aRateScopeSet& scopes) {
    std::vector<RateLimitKey> keys;
    if (scopes.account_id.has_value()) {
        if (*scopes.account_id <= 0) return std::nullopt;
        RateLimitKey key{RateLimitScope::account,
            rate_limit_subject_hash(RateLimitScope::account,
                std::to_string(*scopes.account_id))};
        if (!hash_present(key)) return std::nullopt;
        keys.push_back(key);
    }
    if (scopes.source) {
        if (!valid_source_ip_text(scopes.source_ip)) return std::nullopt;
        RateLimitKey key{RateLimitScope::source,
            rate_limit_subject_hash(RateLimitScope::source, scopes.source_ip)};
        if (!hash_present(key)) return std::nullopt;
        keys.push_back(key);
    } else if (!scopes.source_ip.empty()) {
        return std::nullopt;
    }
    if (scopes.global) {
        RateLimitKey key{RateLimitScope::global,
            rate_limit_subject_hash(RateLimitScope::global, "global")};
        if (!hash_present(key)) return std::nullopt;
        keys.push_back(key);
    }
    if (scopes.system_password) {
        RateLimitKey key{RateLimitScope::system_password,
            rate_limit_subject_hash(RateLimitScope::system_password, "system-password")};
        if (!hash_present(key)) return std::nullopt;
        keys.push_back(key);
    }
    if (keys.empty()) return std::nullopt;
    std::sort(keys.begin(), keys.end(), key_less);
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

StorageStatus unavailable(const char* operation) {
    return {StorageErrorCode::internal, operation};
}

}  // namespace

std::unique_ptr<Phase3aCredentialOperationsBridge>
Phase3aCredentialOperationsBridge::create(
    application::AuthenticationSecurityService& service,
    application::IPasswordKdf& kdf,
    const std::int64_t created_utc_seconds,
    const application::CancellationToken& cancellation) {
    if (!application::valid_utc_seconds(created_utc_seconds) ||
        cancellation.is_cancelled()) return {};
    constexpr std::string_view dummy_seed =
        "SFX fixed public dummy verification credential v1";
    auto created = kdf.create(dummy_seed, created_utc_seconds, false, cancellation);
    if (!created.succeeded() ||
        application::decode_argon2id_credential_envelope(*created.credential).code !=
            application::CredentialEnvelopeCode::valid) return {};
    return std::unique_ptr<Phase3aCredentialOperationsBridge>(
        new Phase3aCredentialOperationsBridge(service, kdf,
            std::move(*created.credential)));
}

Phase3aCredentialOperationsBridge::Phase3aCredentialOperationsBridge(
    application::AuthenticationSecurityService& service,
    application::IPasswordKdf& kdf,
    application::CredentialEnvelope dummy_credential) noexcept
    : service_(service), kdf_(kdf), dummy_credential_(std::move(dummy_credential)) {}

application::PasswordAuthenticationResult
Phase3aCredentialOperationsBridge::verify_existing_account(
    const application::AccountId account_id,
    const std::string_view candidate,
    const std::int64_t now_utc_seconds,
    const application::AuthenticationAuditContext& maintenance_audit,
    const application::CancellationToken& cancellation) {
    return service_.verify_account_password(account_id, candidate, now_utc_seconds,
        maintenance_audit, cancellation);
}

application::PasswordAuthenticationResult
Phase3aCredentialOperationsBridge::perform_dummy_verification(
    const std::string_view candidate,
    const std::int64_t,
    const application::CancellationToken& cancellation) {
    const auto checked = kdf_.verify(dummy_credential_, candidate, cancellation);
    if (!checked.completed()) {
        return {map_kdf_code(checked.code), false, false, false, false};
    }
    return {application::AuthenticationOperationCode::rejected,
        false, false, false, false};
}

application::IPhase3aCredentialOperations::RegistrationCredentialResult
Phase3aCredentialOperationsBridge::create_registration_credential(
    const std::string_view candidate,
    const application::PasswordPolicyContext& policy,
    const std::int64_t created_utc_seconds,
    const application::CancellationToken& cancellation) {
    application::CredentialEnvelope credential;
    const auto result = service_.create_password_credential(candidate, policy,
        created_utc_seconds, false, cancellation, credential);
    RegistrationCredentialResult output;
    output.code = result.code;
    output.policy_code = result.policy_code;
    if (result.succeeded()) output.credential = std::move(credential);
    return output;
}

application::PasswordMutationResult
Phase3aCredentialOperationsBridge::replace_temporary_password(
    const application::TemporaryPasswordReplacementOperation& request,
    const application::CancellationToken& cancellation) {
    return service_.replace_temporary_password(request, cancellation);
}

application::PasswordAuthenticationResult
Phase3aCredentialOperationsBridge::verify_system_password(
    const std::string_view candidate,
    const std::int64_t now_utc_seconds,
    const application::AuthenticationAuditContext& maintenance_audit,
    const application::CancellationToken& cancellation) {
    return service_.verify_system_password(candidate, now_utc_seconds,
        maintenance_audit, cancellation);
}

class Phase3aRateControlCoordinator::Impl final {
public:
    Impl(application::IAuthenticationStorage& source,
         const std::size_t maximum)
        : storage(source), coordinator(application::provisional_rate_profile_a(), maximum),
          maximum_subjects(maximum) {}

    StorageStatus persist_after_assess(
        const RateLimitKey& key, const std::int64_t now) {
        const auto snapshots = coordinator.snapshots(now);
        const auto found = std::find_if(snapshots.begin(), snapshots.end(),
            [&key](const RateLimitSnapshot& snapshot) { return snapshot.key == key; });
        return found == snapshots.end()
            ? storage.remove_rate_limit_snapshot(key)
            : storage.store_rate_limit_snapshot(*found);
    }

    application::IAuthenticationStorage& storage;
    application::RateControlCoordinator coordinator;
    const std::size_t maximum_subjects;
    mutable std::mutex mutex;
    bool healthy{false};
    std::function<void()> activated;
};

Phase3aRateControlCoordinator::Phase3aRateControlCoordinator(
    application::IAuthenticationStorage& storage,
    const std::int64_t restore_utc_seconds,
    const std::size_t maximum_subjects,std::function<void()> activated)
    : impl_(std::make_unique<Impl>(storage, maximum_subjects)) {
    impl_->activated=std::move(activated);
    if (!application::valid_utc_seconds(restore_utc_seconds) ||
        maximum_subjects == 0 || maximum_subjects > 65'536) return;
    auto stored = storage.active_rate_limit_snapshots(
        restore_utc_seconds, maximum_subjects + 1U);
    if (!stored || !stored.value.has_value() ||
        stored.value->size() > maximum_subjects) return;
    for (const auto& snapshot : *stored.value) {
        if (!impl_->coordinator.restore(snapshot, restore_utc_seconds)) return;
    }
    impl_->healthy = true;
}

Phase3aRateControlCoordinator::~Phase3aRateControlCoordinator() = default;

bool Phase3aRateControlCoordinator::ready() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->healthy;
    } catch (...) {
        return false;
    }
}

application::StorageStatus Phase3aRateControlCoordinator::clear_locally(
    const application::LocalThrottleClearRequest& request,
    const application::AuthenticationAuditContext& audit) {
    if (!application::valid_local_throttle_clear(request) || !application::valid_authentication_audit_context(audit))
        return {application::StorageErrorCode::invalid_argument,"validate local throttle clear"};
    const auto subject = request.scope == RateLimitScope::global ? "global" :
        request.scope == RateLimitScope::system_password ? "system-password" : request.subject;
    const RateLimitKey key{request.scope,rate_limit_subject_hash(request.scope,subject)};
    if (!hash_present(key)) return unavailable("hash local throttle subject");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->healthy) return unavailable("use unavailable Phase 3A rate coordinator");
    const auto result = impl_->storage.clear_rate_limit({key,audit});
    if (!result) {
        impl_->healthy = false; // uncertain persistence cannot silently reopen admission
        return result;
    }
    impl_->coordinator.clear(key);
    return application::StorageStatus::success();
}

application::Phase3aRateGateResult Phase3aRateControlCoordinator::try_admission(
    const application::Phase3aRateScopeSet& scopes,const std::int64_t now_utc_seconds) {
    const auto keys=keys_for(scopes);
    if(!application::valid_utc_seconds(now_utc_seconds)||!keys)return {application::Phase3aRateGateCode::unavailable,0};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(!impl_->healthy)return {application::Phase3aRateGateCode::unavailable,0};
    std::uint32_t delay{};
    for(const auto& key:*keys){
        delay=std::max(delay,impl_->coordinator.assess(key,now_utc_seconds).delay_seconds);
        if(!impl_->persist_after_assess(key,now_utc_seconds)){impl_->healthy=false;return {application::Phase3aRateGateCode::unavailable,0};}
    }
    return {delay?application::Phase3aRateGateCode::timed_out:application::Phase3aRateGateCode::admitted,delay};
}

application::Phase3aRateGateResult Phase3aRateControlCoordinator::await_admission(
    const application::Phase3aRateScopeSet& scopes,
    const std::int64_t now_utc_seconds,
    const application::CancellationToken& cancellation) {
    if (cancellation.is_cancelled()) {
        return {application::Phase3aRateGateCode::cancelled, 0};
    }
    if (!application::valid_utc_seconds(now_utc_seconds)) {
        return {application::Phase3aRateGateCode::unavailable, 0};
    }
    const auto keys = keys_for(scopes);
    if (!keys.has_value()) return {application::Phase3aRateGateCode::unavailable, 0};
    std::uint32_t delay = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->healthy) return {application::Phase3aRateGateCode::unavailable, 0};
        for (const auto& key : *keys) {
            const auto decision = impl_->coordinator.assess(key, now_utc_seconds);
            delay = std::max(delay, decision.delay_seconds);
            const auto status = impl_->persist_after_assess(key, now_utc_seconds);
            if (!status) {
                impl_->healthy = false;
                return {application::Phase3aRateGateCode::unavailable, 0};
            }
        }
    }
    if (delay == 0) return {application::Phase3aRateGateCode::admitted, 0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(delay);
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancellation.is_cancelled()) {
            return {application::Phase3aRateGateCode::cancelled, delay};
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::min(remaining,
            std::chrono::steady_clock::duration(std::chrono::milliseconds(20))));
    }
    if (cancellation.is_cancelled()) {
        return {application::Phase3aRateGateCode::cancelled, delay};
    }
    return {application::Phase3aRateGateCode::admitted, delay};
}

application::StorageStatus Phase3aRateControlCoordinator::record_failure(
    const application::Phase3aRateScopeSet& scopes,
    const std::int64_t now_utc_seconds) {
    const auto keys = keys_for(scopes);
    if (!application::valid_utc_seconds(now_utc_seconds) || !keys.has_value()) {
        return {application::StorageErrorCode::invalid_argument,
            "validate Phase 3A rate failure"};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->healthy) return unavailable("use unavailable Phase 3A rate coordinator");
    bool activation{};
    for (const auto& key : *keys) {
        const bool blocked=impl_->activated&&impl_->coordinator.temporarily_blocked(key,now_utc_seconds);
        const auto decision = impl_->coordinator.record_failure(key, now_utc_seconds);
        activation=activation||(!blocked&&decision.delay_seconds>0);
        const auto snapshots = impl_->coordinator.snapshots(now_utc_seconds);
        if (std::none_of(snapshots.begin(), snapshots.end(),
                [&key](const application::RateLimitSnapshot& value) {
                    return value.key == key;
                })) {
            (void)decision;
            return {application::StorageErrorCode::backpressure,
                "enforce bounded Phase 3A rate capacity"};
        }
        const auto status = impl_->persist_after_assess(key, now_utc_seconds);
        if (!status) {
            impl_->healthy = false;
            return status;
        }
    }
    try{if(activation&&impl_->activated)impl_->activated();}catch(...){/* sound cannot change admission */}
    return application::StorageStatus::success();
}

application::StorageStatus Phase3aRateControlCoordinator::record_success(
    const application::Phase3aRateScopeSet& scopes,
    const std::int64_t now_utc_seconds) {
    const auto keys = keys_for(scopes);
    if (!application::valid_utc_seconds(now_utc_seconds) || !keys.has_value()) {
        return {application::StorageErrorCode::invalid_argument,
            "validate Phase 3A rate success"};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->healthy) return unavailable("use unavailable Phase 3A rate coordinator");
    for (const auto& key : *keys) {
        const auto status = impl_->storage.remove_rate_limit_snapshot(key);
        if (!status) {
            impl_->healthy = false;
            return status;
        }
        impl_->coordinator.record_success(key);
    }
    return application::StorageStatus::success();
}

}  // namespace sfx::adapters::authentication
