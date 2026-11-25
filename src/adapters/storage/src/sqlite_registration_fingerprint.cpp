#include "sqlite_registration_fingerprint.hpp"

#include <sodium.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace sfx::adapters::storage::internal {
namespace {

constexpr std::string_view kFingerprintDomain =
    "SFX/Phase3A/RegistrationCommitRequest/v1";

class Sha256CanonicalWriter final {
public:
    Sha256CanonicalWriter() noexcept {
        ready_ = sodium_init() >= 0 && crypto_hash_sha256_init(&state_) == 0;
    }

    ~Sha256CanonicalWriter() {
        sodium_memzero(&state_, sizeof(state_));
    }

    Sha256CanonicalWriter(const Sha256CanonicalWriter&) = delete;
    Sha256CanonicalWriter& operator=(const Sha256CanonicalWriter&) = delete;

    bool ready() const noexcept { return ready_; }

    void byte(const std::uint8_t value) noexcept {
        update(&value, sizeof(value));
    }

    void unsigned16(const std::uint16_t value) noexcept {
        const std::array<unsigned char, 2> bytes{
            static_cast<unsigned char>((value >> 8U) & 0xffU),
            static_cast<unsigned char>(value & 0xffU)};
        update(bytes.data(), bytes.size());
    }

    void unsigned32(const std::uint32_t value) noexcept {
        const std::array<unsigned char, 4> bytes{
            static_cast<unsigned char>((value >> 24U) & 0xffU),
            static_cast<unsigned char>((value >> 16U) & 0xffU),
            static_cast<unsigned char>((value >> 8U) & 0xffU),
            static_cast<unsigned char>(value & 0xffU)};
        update(bytes.data(), bytes.size());
    }

    void signed64(const std::int64_t value) noexcept {
        const std::uint64_t encoded = static_cast<std::uint64_t>(value);
        const std::array<unsigned char, 8> bytes{
            static_cast<unsigned char>((encoded >> 56U) & 0xffU),
            static_cast<unsigned char>((encoded >> 48U) & 0xffU),
            static_cast<unsigned char>((encoded >> 40U) & 0xffU),
            static_cast<unsigned char>((encoded >> 32U) & 0xffU),
            static_cast<unsigned char>((encoded >> 24U) & 0xffU),
            static_cast<unsigned char>((encoded >> 16U) & 0xffU),
            static_cast<unsigned char>((encoded >> 8U) & 0xffU),
            static_cast<unsigned char>(encoded & 0xffU)};
        update(bytes.data(), bytes.size());
    }

    void identifier(const application::Identifier128& value) noexcept {
        update(value.bytes.data(), value.bytes.size());
    }

    void text(const std::string_view value) noexcept {
        length(value.size());
        update(reinterpret_cast<const unsigned char*>(value.data()), value.size());
    }

    void binary(const std::vector<std::uint8_t>& value) noexcept {
        length(value.size());
        update(value.data(), value.size());
    }

    void optional_text(const std::optional<std::string>& value) noexcept {
        byte(value.has_value() ? 1U : 0U);
        if (value.has_value()) text(*value);
    }

    void optional_character(const std::optional<char> value) noexcept {
        byte(value.has_value() ? 1U : 0U);
        if (value.has_value()) byte(static_cast<std::uint8_t>(*value));
    }

    void optional_account_id(
        const std::optional<application::AccountId> value) noexcept {
        byte(value.has_value() ? 1U : 0U);
        if (value.has_value()) signed64(*value);
    }

    void optional_timestamp(const std::optional<std::int64_t> value) noexcept {
        byte(value.has_value() ? 1U : 0U);
        if (value.has_value()) signed64(*value);
    }

    std::optional<RegistrationRequestFingerprint> finish() noexcept {
        if (!ready_) return std::nullopt;
        RegistrationRequestFingerprint digest{};
        if (crypto_hash_sha256_final(&state_, digest.data()) != 0) {
            sodium_memzero(digest.data(), digest.size());
            ready_ = false;
            return std::nullopt;
        }
        ready_ = false;
        return digest;
    }

private:
    void length(const std::size_t value) noexcept {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            ready_ = false;
            return;
        }
        unsigned32(static_cast<std::uint32_t>(value));
    }

    void update(const unsigned char* const bytes, const std::size_t size) noexcept {
        if (!ready_ || size == 0U) return;
        if (bytes == nullptr ||
            size > std::numeric_limits<unsigned long long>::max() ||
            crypto_hash_sha256_update(
                &state_, bytes, static_cast<unsigned long long>(size)) != 0) {
            ready_ = false;
        }
    }

    crypto_hash_sha256_state state_{};
    bool ready_{false};
};

std::uint8_t audit_reason_code(
    const application::AuditReasonCode value) noexcept {
    switch (value) {
        case application::AuditReasonCode::initial_setup: return 1U;
        case application::AuditReasonCode::registration: return 2U;
        case application::AuditReasonCode::sysop_action: return 3U;
        case application::AuditReasonCode::inactivity_policy: return 4U;
        case application::AuditReasonCode::security_policy: return 5U;
        case application::AuditReasonCode::retention_policy: return 6U;
        case application::AuditReasonCode::session_finalization: return 7U;
        case application::AuditReasonCode::caller_action: return 0U;
    }
    return 0U;
}

std::uint8_t transfer_code(
    const application::TransferProtocolPreference value) noexcept {
    switch (value) {
        case application::TransferProtocolPreference::xmodem: return 1U;
        case application::TransferProtocolPreference::ymodem: return 2U;
        case application::TransferProtocolPreference::ymodem_g: return 3U;
        case application::TransferProtocolPreference::zmodem: return 4U;
        case application::TransferProtocolPreference::select_at_transfer: return 5U;
    }
    return 0U;
}

std::uint8_t session_outcome_code(
    const application::SessionOutcomeCode value) noexcept {
    switch (value) {
        case application::SessionOutcomeCode::normal: return 1U;
        case application::SessionOutcomeCode::disconnected: return 2U;
        case application::SessionOutcomeCode::timed_out: return 3U;
        case application::SessionOutcomeCode::shutdown: return 4U;
        case application::SessionOutcomeCode::runtime_error: return 5U;
    }
    return 0U;
}

}  // namespace

std::optional<RegistrationRequestFingerprint> registration_request_fingerprint_v1(
    const application::RegistrationCommitRequest& request) noexcept {
    if (!application::valid_registration_commit_request(request)) {
        return std::nullopt;
    }

    Sha256CanonicalWriter writer;
    if (!writer.ready()) return std::nullopt;

    // Fingerprint-v1 is a streaming canonical encoding; no complete canonical
    // request buffer is constructed or retained.
    writer.text(kFingerprintDomain);
    writer.unsigned16(
        static_cast<std::uint16_t>(kRegistrationFingerprintFormatVersion));

    writer.text(request.account.display_name);

    writer.optional_text(request.account.profile.real_name);
    writer.optional_text(request.account.profile.location);
    writer.optional_text(request.account.profile.phone);
    writer.optional_text(request.account.profile.email);
    writer.optional_text(request.account.profile.birth_date);
    writer.optional_character(request.account.profile.sex);

    writer.unsigned16(request.account.credential.envelope_format_version);
    writer.text(request.account.credential.scheme_identifier);
    writer.unsigned16(request.account.credential.parameter_encoding_version);
    writer.binary(request.account.credential.canonical_parameters);
    writer.binary(request.account.credential.salt);
    writer.binary(request.account.credential.verifier);
    writer.optional_text(request.account.credential.pepper_key_identifier);
    writer.signed64(request.account.credential.created_utc_seconds);
    writer.byte(request.account.credential.password_change_required ? 1U : 0U);

    writer.identifier(request.account.audit.event_id);
    writer.identifier(request.account.audit.correlation_id);
    writer.optional_account_id(request.account.audit.actor_account_id);
    writer.signed64(request.account.audit.occurred_utc_seconds);
    writer.byte(audit_reason_code(request.account.audit.reason));

    writer.unsigned16(request.security_level);
    writer.byte(transfer_code(request.transfer_preference));

    writer.signed64(request.initial_statistics.successful_login_count);
    writer.signed64(request.initial_statistics.authenticated_session_seconds);
    writer.optional_timestamp(
        request.initial_statistics.last_successful_login_utc_seconds);
    writer.optional_timestamp(
        request.initial_statistics.last_completed_session_utc_seconds);
    writer.byte(request.initial_statistics.last_session_outcome.has_value() ? 1U : 0U);
    if (request.initial_statistics.last_session_outcome.has_value()) {
        writer.byte(session_outcome_code(
            *request.initial_statistics.last_session_outcome));
    }

    writer.signed64(request.expected_security_policy_revision);
    writer.text(request.expected_security_policy_hash);
    return writer.finish();
}

bool registration_request_fingerprints_equal(
    const RegistrationRequestFingerprint& left,
    const RegistrationRequestFingerprint& right) noexcept {
    return sodium_memcmp(left.data(), right.data(), left.size()) == 0;
}

}  // namespace sfx::adapters::storage::internal
