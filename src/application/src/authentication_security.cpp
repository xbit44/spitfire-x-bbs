#include "sfx/application/authentication_security.hpp"
#include "sfx/application/phase3a_storage.hpp"
#include <charconv>

#include <algorithm>
#include <stdexcept>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

namespace sfx::application {
namespace {

constexpr std::uint16_t envelope_version = 1;
constexpr std::uint16_t parameter_version = 1;
constexpr std::string_view scheme_identifier = "argon2id-v1.3";
constexpr std::size_t parameter_bytes = 20;

void append_u16(std::vector<std::uint8_t>& output, const std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& input, const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(input[offset]) << 8U) |
        static_cast<std::uint16_t>(input[offset + 1U]));
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& input, const std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(input[offset]) << 24U) |
        (static_cast<std::uint32_t>(input[offset + 1U]) << 16U) |
        (static_cast<std::uint32_t>(input[offset + 2U]) << 8U) |
        static_cast<std::uint32_t>(input[offset + 3U]);
}

bool printable_ascii(const std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return character >= 0x20U && character <= 0x7eU;
    });
}

std::string simple_context_key(const std::string_view value) {
    std::string key;
    key.reserve(value.size());
    for (const unsigned char character : value) {
        if (character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('Z'))
            key.push_back(static_cast<char>(character + 0x20U));
        else if ((character >= static_cast<unsigned char>('a') && character <= static_cast<unsigned char>('z')) ||
                 (character >= static_cast<unsigned char>('0') && character <= static_cast<unsigned char>('9')))
            key.push_back(static_cast<char>(character));
    }
    return key;
}

bool all_decimal(const std::string_view value) noexcept {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return character >= static_cast<unsigned char>('0') && character <= static_cast<unsigned char>('9');
    });
}

bool derived_from(const std::string& candidate, const std::string_view context) {
    const std::string base = simple_context_key(context);
    if (base.empty()) return false;
    if (candidate == base || candidate == base + "1" || candidate == base + "123" ||
        candidate == base + "2026") {
        return true;
    }
    if (candidate.size() > base.size() && candidate.size() <= base.size() + 12U &&
        candidate.compare(0, base.size(), base) == 0) {
        if (all_decimal(std::string_view(candidate).substr(base.size()))) return true;
    }
    if (candidate.size() >= base.size() * 2U && candidate.size() % base.size() == 0U) {
        for (std::size_t offset = 0; offset < candidate.size(); offset += base.size()) {
            if (candidate.compare(offset, base.size(), base) != 0) return false;
        }
        return true;
    }
    return false;
}

const RateScopePolicy& policy_for(const RateLimitProfile& profile, const RateLimitScope scope) {
    switch (scope) {
        case RateLimitScope::account: return profile.account;
        case RateLimitScope::source: return profile.source;
        case RateLimitScope::system_password: return profile.system_password;
        case RateLimitScope::global: return profile.global;
    }
    return profile.global;
}

bool valid_scope_policy(const RateScopePolicy& policy) noexcept {
    return policy.burst_allowance > 0 && policy.base_delay_seconds > 0 &&
        policy.maximum_delay_seconds >= policy.base_delay_seconds &&
        policy.maximum_delay_seconds <= 300 && policy.decay_interval_seconds > 0 &&
        policy.state_ttl_seconds >= policy.decay_interval_seconds &&
        policy.state_ttl_seconds <= 86'400;
}

std::uint32_t progressive_delay(
    const RateScopePolicy& policy,
    const std::uint32_t failure_count) noexcept {
    if (failure_count <= policy.burst_allowance) return 0;
    const std::uint32_t exponent = std::min<std::uint32_t>(
        failure_count - policy.burst_allowance - 1U, 30U);
    std::uint64_t delay = policy.base_delay_seconds;
    delay <<= exponent;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(delay, policy.maximum_delay_seconds));
}

struct RateLimitKeyLess {
    bool operator()(const RateLimitKey& left, const RateLimitKey& right) const noexcept {
        if (left.scope != right.scope) {
            return static_cast<std::uint8_t>(left.scope) < static_cast<std::uint8_t>(right.scope);
        }
        return left.subject_hash < right.subject_hash;
    }
};

}  // namespace

bool operator==(const Argon2idProfile& left, const Argon2idProfile& right) noexcept {
    return left.memory_kib == right.memory_kib && left.iterations == right.iterations &&
        left.parallelism == right.parallelism && left.salt_bytes == right.salt_bytes &&
        left.verifier_bytes == right.verifier_bytes;
}

bool operator!=(const Argon2idProfile& left, const Argon2idProfile& right) noexcept {
    return !(left == right);
}

PasswordPolicyCode evaluate_password_policy(
    const std::string_view candidate,
    const PasswordPolicyContext& context,
    const IPasswordBlocklist* const blocklist) noexcept {
    const std::size_t minimum = context.mode == PasswordMode::strong
        ? kPasswordMinimumStrong
        : kPasswordMinimumStandard;
    if (candidate.size() < minimum) return PasswordPolicyCode::too_short;
    if (candidate.size() > kPasswordMaximum) return PasswordPolicyCode::too_long;
    if (!printable_ascii(candidate)) return PasswordPolicyCode::non_printable_ascii;
    if (context.mode == PasswordMode::standard) return PasswordPolicyCode::accepted;
    if (blocklist == nullptr || !blocklist->ready()) return PasswordPolicyCode::blocklist_unavailable;
    if (blocklist->contains_complete_case_insensitive(candidate)) return PasswordPolicyCode::blocklisted;

    const std::string candidate_key = simple_context_key(candidate);
    if (derived_from(candidate_key, context.account_name) ||
        derived_from(candidate_key, context.bbs_name) ||
        derived_from(candidate_key, "Spitfire") ||
        derived_from(candidate_key, "SFX")) {
        return PasswordPolicyCode::context_derived;
    }
    return PasswordPolicyCode::accepted;
}

CredentialEnvelope make_argon2id_credential_envelope(
    const Argon2idProfile& profile,
    std::vector<std::uint8_t> salt,
    std::vector<std::uint8_t> verifier,
    const std::int64_t created_utc_seconds,
    const bool password_change_required) {
    CredentialEnvelope envelope;
    envelope.envelope_format_version = envelope_version;
    envelope.scheme_identifier = std::string(scheme_identifier);
    envelope.parameter_encoding_version = parameter_version;
    envelope.canonical_parameters.reserve(parameter_bytes);
    envelope.canonical_parameters.insert(
        envelope.canonical_parameters.end(), {'S', '5', 'C', '1'});
    append_u32(envelope.canonical_parameters, profile.memory_kib);
    append_u32(envelope.canonical_parameters, profile.iterations);
    append_u32(envelope.canonical_parameters, profile.parallelism);
    append_u16(envelope.canonical_parameters, profile.salt_bytes);
    append_u16(envelope.canonical_parameters, profile.verifier_bytes);
    envelope.salt = std::move(salt);
    envelope.verifier = std::move(verifier);
    envelope.pepper_key_identifier.reset();
    envelope.created_utc_seconds = created_utc_seconds;
    envelope.password_change_required = password_change_required;
    return envelope;
}

DecodedCredentialEnvelope decode_argon2id_credential_envelope(
    const CredentialEnvelope& envelope) noexcept {
    std::size_t remaining = kCredentialEnvelopeMaximum;
    const auto consume = [&remaining](const std::size_t size) noexcept {
        if (size > remaining) return false;
        remaining -= size;
        return true;
    };
    if (!consume(5U) || !consume(envelope.scheme_identifier.size()) ||
        !consume(envelope.canonical_parameters.size()) || !consume(envelope.salt.size()) ||
        !consume(envelope.verifier.size()) ||
        (envelope.pepper_key_identifier.has_value() &&
         !consume(envelope.pepper_key_identifier->size()))) {
        return {CredentialEnvelopeCode::oversized, {}};
    }
    if (envelope.envelope_format_version != envelope_version) {
        return {CredentialEnvelopeCode::unsupported_version, {}};
    }
    if (envelope.scheme_identifier != scheme_identifier) {
        return {CredentialEnvelopeCode::unsupported_scheme, {}};
    }
    if (envelope.parameter_encoding_version != parameter_version) {
        return {CredentialEnvelopeCode::unsupported_version, {}};
    }
    if (envelope.pepper_key_identifier.has_value() ||
        envelope.canonical_parameters.size() != parameter_bytes ||
        envelope.canonical_parameters[0] != 'S' || envelope.canonical_parameters[1] != '5' ||
        envelope.canonical_parameters[2] != 'C' || envelope.canonical_parameters[3] != '1' ||
        !valid_utc_seconds(envelope.created_utc_seconds)) {
        return {CredentialEnvelopeCode::malformed, {}};
    }
    Argon2idProfile profile;
    profile.memory_kib = read_u32(envelope.canonical_parameters, 4);
    profile.iterations = read_u32(envelope.canonical_parameters, 8);
    profile.parallelism = read_u32(envelope.canonical_parameters, 12);
    profile.salt_bytes = read_u16(envelope.canonical_parameters, 16);
    profile.verifier_bytes = read_u16(envelope.canonical_parameters, 18);
    if (profile.memory_kib < 8U * 1024U || profile.memory_kib > 64U * 1024U ||
        profile.iterations < 1 || profile.iterations > 10 || profile.parallelism != 1 ||
        profile.salt_bytes != kArgon2idSaltBytes ||
        profile.verifier_bytes != kArgon2idVerifierBytes ||
        envelope.salt.size() != profile.salt_bytes ||
        envelope.verifier.size() != profile.verifier_bytes) {
        return {CredentialEnvelopeCode::unsupported_parameters, profile};
    }
    return {CredentialEnvelopeCode::valid, profile};
}

bool credential_needs_rehash(
    const CredentialEnvelope& envelope,
    const Argon2idProfile& current_profile) noexcept {
    const DecodedCredentialEnvelope decoded = decode_argon2id_credential_envelope(envelope);
    return decoded.valid() && decoded.profile != current_profile;
}

SysopSequenceAction next_sysop_sequence_action(
    const bool account_password_verified,
    const std::uint16_t account_security_level,
    const std::uint16_t configured_sysop_threshold) noexcept {
    if (!account_password_verified || account_security_level < 1 || account_security_level > 999 ||
        configured_sysop_threshold < 1 || configured_sysop_threshold > 999 ||
        account_security_level < configured_sysop_threshold) {
        return SysopSequenceAction::reject;
    }
    return SysopSequenceAction::request_system_password;
}

bool sysop_authority_granted(
    const SysopSequenceAction prior_action,
    const bool system_password_verified) noexcept {
    return prior_action == SysopSequenceAction::request_system_password && system_password_verified;
}

bool valid_connection_limit_policy(const ConnectionLimitPolicy& policy) noexcept {
    return policy.maximum_connections_per_source >= 1 &&
        policy.maximum_connections_per_source <= 128 &&
        policy.maximum_sessions_per_account >= 1 &&
        policy.maximum_sessions_per_account <= 128 &&
        policy.maximum_logical_nodes >= 1 && policy.maximum_logical_nodes <= 128;
}

bool valid_local_throttle_clear(const LocalThrottleClearRequest& request) {
    switch (request.scope) {
        case RateLimitScope::source: {
            const auto canonical = canonicalize_source_ip(request.subject);
            return canonical && canonical->text == request.subject;
        }
        case RateLimitScope::account: {
            if (request.subject.empty() || request.subject.front() == '0' || request.subject.size() > 19) return false;
            std::int64_t id{};
            const auto parsed = std::from_chars(request.subject.data(),request.subject.data()+request.subject.size(),id);
            return parsed.ec == std::errc{} && parsed.ptr == request.subject.data()+request.subject.size() && id > 0;
        }
        case RateLimitScope::system_password:
        case RateLimitScope::global: return request.subject.empty();
    }
    return false;
}

bool valid_source_connection_limits(const std::vector<SourceConnectionLimit>& exceptions) {
    if (exceptions.size() > kMaximumSourceConnectionExceptions) return false;
    std::string_view previous;
    for (const auto& value : exceptions) {
        const auto canonical = canonicalize_source_ip(value.address);
        if (!canonical || canonical->text != value.address || value.address <= previous ||
            value.maximum_connections < 1 || value.maximum_connections > 128) return false;
        previous = value.address;
    }
    return true;
}

std::optional<std::vector<SourceConnectionLimit>> parse_source_connection_limits(std::string_view serialized) {
    std::vector<SourceConnectionLimit> result;
    if (serialized == "-") return result;
    if (serialized.empty() || serialized.size() > 2048) return std::nullopt;
    while (!serialized.empty()) {
        const auto comma = serialized.find(',');
        const auto item = serialized.substr(0,comma);
        const auto separator = item.find('|');
        if (separator == std::string_view::npos) return std::nullopt;
        const auto count_text = item.substr(separator+1);
        std::uint16_t count = 0;
        const auto parsed = std::from_chars(count_text.data(),count_text.data()+count_text.size(),count);
        if (parsed.ec != std::errc{} || parsed.ptr != count_text.data()+count_text.size() ||
            count_text != std::to_string(count)) return std::nullopt;
        result.push_back({std::string(item.substr(0,separator)),count});
        if (result.size() > kMaximumSourceConnectionExceptions) return std::nullopt;
        if (comma == std::string_view::npos) break;
        serialized.remove_prefix(comma+1);
        if (serialized.empty()) return std::nullopt;
    }
    return valid_source_connection_limits(result) ? std::optional<std::vector<SourceConnectionLimit>>(std::move(result)) : std::nullopt;
}

std::string serialize_source_connection_limits(const std::vector<SourceConnectionLimit>& exceptions) {
    if (!valid_source_connection_limits(exceptions)) return {};
    if (exceptions.empty()) return "-";
    std::string result;
    for (const auto& value : exceptions) {
        if (!result.empty()) result += ',';
        result += value.address + "|" + std::to_string(value.maximum_connections);
    }
    return result;
}

ConnectionAttemptAction record_failed_account_attempt(ConnectionAttemptState& state) noexcept {
    if (state.failed_account_attempts < kFailedAttemptsPerConnection) {
        ++state.failed_account_attempts;
    }
    return state.failed_account_attempts >= kFailedAttemptsPerConnection
        ? ConnectionAttemptAction::disconnect
        : ConnectionAttemptAction::continue_login;
}

ConnectionAdmissionCode evaluate_prelogin_admission(
    const std::uint16_t active_source_connections,
    const std::uint16_t active_logical_nodes,
    const ConnectionLimitPolicy& policy) noexcept {
    if (!valid_connection_limit_policy(policy)) return ConnectionAdmissionCode::source_connection_limit;
    if (active_source_connections >= policy.maximum_connections_per_source) {
        return ConnectionAdmissionCode::source_connection_limit;
    }
    if (active_logical_nodes >= policy.maximum_logical_nodes) {
        return ConnectionAdmissionCode::logical_node_limit;
    }
    return ConnectionAdmissionCode::admitted;
}

ConnectionAdmissionCode evaluate_account_session_admission(
    const std::uint16_t active_account_sessions,
    const ConnectionLimitPolicy& policy) noexcept {
    if (!valid_connection_limit_policy(policy) ||
        active_account_sessions >= policy.maximum_sessions_per_account) {
        return ConnectionAdmissionCode::account_session_limit;
    }
    return ConnectionAdmissionCode::admitted;
}

bool operator==(const RateLimitKey& left, const RateLimitKey& right) noexcept {
    return left.scope == right.scope && left.subject_hash == right.subject_hash;
}

bool valid_rate_limit_profile(const RateLimitProfile& profile) noexcept {
    return valid_scope_policy(profile.account) && valid_scope_policy(profile.source) &&
        valid_scope_policy(profile.system_password) && valid_scope_policy(profile.global);
}

RateLimitProfile provisional_rate_profile_a() noexcept {
    return {
        {3, 1, 8, 60, 900},
        {8, 1, 8, 60, 900},
        {3, 2, 16, 120, 1800},
        {64, 1, 4, 30, 300}};
}

RateLimitProfile provisional_rate_profile_b() noexcept {
    return {
        {3, 2, 16, 90, 1200},
        {6, 2, 16, 90, 1200},
        {3, 4, 32, 180, 2400},
        {32, 2, 8, 60, 600}};
}

class RateControlCoordinator::Impl final {
public:
    Impl(RateLimitProfile profile, const std::size_t maximum_subjects)
        : profile_(std::move(profile)), maximum_subjects_(maximum_subjects) {}

    void decay_or_expire(
        std::map<RateLimitKey, RateLimitSnapshot, RateLimitKeyLess>::iterator& entry,
        const std::int64_t now) {
        if (now >= entry->second.expires_utc_seconds) {
            entry = states_.erase(entry);
            ++health_.expired_states;
            return;
        }
        const RateScopePolicy& policy = policy_for(profile_, entry->first.scope);
        if (now > entry->second.last_failure_utc_seconds && policy.decay_interval_seconds > 0) {
            const std::uint64_t elapsed = static_cast<std::uint64_t>(
                now - entry->second.last_failure_utc_seconds);
            const std::uint32_t steps = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(elapsed / policy.decay_interval_seconds,
                    entry->second.failure_count));
            entry->second.failure_count -= steps;
            entry->second.last_failure_utc_seconds +=
                static_cast<std::int64_t>(steps) * policy.decay_interval_seconds;
            if (entry->second.failure_count == 0) {
                entry = states_.erase(entry);
                ++health_.expired_states;
            }
        }
    }

    RateLimitProfile profile_;
    std::size_t maximum_subjects_;
    mutable std::mutex mutex_;
    std::map<RateLimitKey, RateLimitSnapshot, RateLimitKeyLess> states_;
    RateLimitHealth health_;
};

RateControlCoordinator::RateControlCoordinator(
    RateLimitProfile profile,
    const std::size_t maximum_subjects)
    : impl_(std::make_unique<Impl>(std::move(profile), maximum_subjects)) {
    if (!valid_rate_limit_profile(impl_->profile_) || maximum_subjects == 0 || maximum_subjects > 65'536) {
        throw std::invalid_argument("invalid bounded rate-control profile");
    }
}

RateControlCoordinator::~RateControlCoordinator() = default;

RateLimitDecision RateControlCoordinator::assess(
    const RateLimitKey& key,
    const std::int64_t now_utc_seconds) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    auto entry = impl_->states_.find(key);
    if (entry == impl_->states_.end()) {
        ++impl_->health_.admitted_attempts;
        return {};
    }
    impl_->decay_or_expire(entry, now_utc_seconds);
    if (entry == impl_->states_.end()) {
        ++impl_->health_.admitted_attempts;
        return {};
    }
    if (entry->second.blocked_until_utc_seconds > now_utc_seconds) {
        ++impl_->health_.delayed_attempts;
        return {false,
            static_cast<std::uint32_t>(entry->second.blocked_until_utc_seconds - now_utc_seconds),
            entry->second.failure_count};
    }
    ++impl_->health_.admitted_attempts;
    return {true, 0, entry->second.failure_count};
}

RateLimitDecision RateControlCoordinator::record_failure(
    const RateLimitKey& key,
    const std::int64_t now_utc_seconds) {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    auto entry = impl_->states_.find(key);
    if (entry != impl_->states_.end()) {
        impl_->decay_or_expire(entry, now_utc_seconds);
    }
    if (entry == impl_->states_.end()) {
        if (impl_->states_.size() >= impl_->maximum_subjects_) {
            const RateScopePolicy& policy = policy_for(impl_->profile_, key.scope);
            ++impl_->health_.delayed_attempts;
            return {false, policy.maximum_delay_seconds, policy.burst_allowance + 1U};
        }
        RateLimitSnapshot snapshot;
        snapshot.key = key;
        snapshot.window_started_utc_seconds = now_utc_seconds;
        snapshot.last_failure_utc_seconds = now_utc_seconds;
        entry = impl_->states_.emplace(key, snapshot).first;
        impl_->health_.tracked_high_water = std::max(
            impl_->health_.tracked_high_water, impl_->states_.size());
    }
    const RateScopePolicy& policy = policy_for(impl_->profile_, key.scope);
    if (entry->second.failure_count < std::numeric_limits<std::uint32_t>::max()) {
        ++entry->second.failure_count;
    }
    entry->second.last_failure_utc_seconds = now_utc_seconds;
    entry->second.expires_utc_seconds = now_utc_seconds + policy.state_ttl_seconds;
    const std::uint32_t delay = progressive_delay(policy, entry->second.failure_count);
    entry->second.blocked_until_utc_seconds = now_utc_seconds + delay;
    if (delay > 0) ++impl_->health_.delayed_attempts;
    else ++impl_->health_.admitted_attempts;
    return {delay == 0, delay, entry->second.failure_count};
}

void RateControlCoordinator::record_success(const RateLimitKey& key) noexcept { clear(key); }

void RateControlCoordinator::clear(const RateLimitKey& key) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    impl_->states_.erase(key);
}

bool RateControlCoordinator::restore(
    const RateLimitSnapshot& snapshot,
    const std::int64_t now_utc_seconds) {
    if (!valid_utc_seconds(now_utc_seconds) || snapshot.failure_count == 0 ||
        !valid_utc_seconds(snapshot.window_started_utc_seconds) ||
        !valid_utc_seconds(snapshot.last_failure_utc_seconds) ||
        !valid_utc_seconds(snapshot.blocked_until_utc_seconds) ||
        !valid_utc_seconds(snapshot.expires_utc_seconds) ||
        snapshot.window_started_utc_seconds > snapshot.last_failure_utc_seconds ||
        snapshot.last_failure_utc_seconds > snapshot.expires_utc_seconds ||
        snapshot.blocked_until_utc_seconds < snapshot.window_started_utc_seconds ||
        snapshot.blocked_until_utc_seconds > snapshot.expires_utc_seconds ||
        snapshot.expires_utc_seconds <= now_utc_seconds) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    if (impl_->states_.size() >= impl_->maximum_subjects_ &&
        impl_->states_.find(snapshot.key) == impl_->states_.end()) {
        return false;
    }
    impl_->states_[snapshot.key] = snapshot;
    impl_->health_.tracked_high_water = std::max(
        impl_->health_.tracked_high_water, impl_->states_.size());
    return true;
}

std::vector<RateLimitSnapshot> RateControlCoordinator::snapshots(
    const std::int64_t now_utc_seconds) const {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    std::vector<RateLimitSnapshot> output;
    output.reserve(impl_->states_.size());
    for (const auto& entry : impl_->states_) {
        if (entry.second.expires_utc_seconds > now_utc_seconds) output.push_back(entry.second);
    }
    return output;
}

bool RateControlCoordinator::temporarily_blocked(const RateLimitKey& key,const std::int64_t now) const noexcept {
    try{
        std::lock_guard<std::mutex> lock(impl_->mutex_);
        const auto item=impl_->states_.find(key);
        return item!=impl_->states_.end()&&item->second.expires_utc_seconds>now&&item->second.blocked_until_utc_seconds>now;
    }catch(...){return false;}
}

RateLimitHealth RateControlCoordinator::health() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex_);
    RateLimitHealth result = impl_->health_;
    result.tracked_subjects = impl_->states_.size();
    return result;
}

}  // namespace sfx::application
