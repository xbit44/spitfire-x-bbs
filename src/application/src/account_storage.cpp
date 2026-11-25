#include "sfx/application/account_storage.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace sfx::application {
namespace {

bool printable_ascii(const std::string_view value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return character >= 0x20U && character <= 0x7eU;
    });
}

std::optional<std::string> trim_ascii_spaces(
    const std::string_view candidate,
    const std::size_t maximum,
    const bool collapse_spaces) {
    std::size_t begin = 0;
    while (begin < candidate.size() && candidate[begin] == ' ') {
        ++begin;
    }
    std::size_t end = candidate.size();
    while (end > begin && candidate[end - 1U] == ' ') {
        --end;
    }

    std::string result;
    result.reserve(end - begin);
    bool previous_space = false;
    for (std::size_t index = begin; index < end; ++index) {
        const unsigned char character = static_cast<unsigned char>(candidate[index]);
        if (character < 0x20U || character > 0x7eU) {
            return std::nullopt;
        }
        if (collapse_spaces && character == 0x20U && previous_space) {
            continue;
        }
        result.push_back(static_cast<char>(character));
        previous_space = character == 0x20U;
        if (result.size() > maximum) {
            return std::nullopt;
        }
    }
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

bool leap_year(const int year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int decimal_pair(const std::string_view value, const std::size_t offset) noexcept {
    return (value[offset] - '0') * 10 + (value[offset + 1U] - '0');
}

bool valid_optional_profile_field(
    const std::optional<std::string>& value,
    const std::size_t maximum,
    const bool collapse) noexcept {
    if (!value.has_value()) {
        return true;
    }
    const std::optional<std::string> normalized = trim_ascii_spaces(*value, maximum, collapse);
    return normalized.has_value() && *normalized == *value;
}

}  // namespace

bool Identifier128::valid() const noexcept {
    return std::any_of(bytes.begin(), bytes.end(), [](const std::uint8_t value) { return value != 0; });
}

bool operator==(const Identifier128& left, const Identifier128& right) noexcept {
    return left.bytes == right.bytes;
}

bool operator!=(const Identifier128& left, const Identifier128& right) noexcept { return !(left == right); }

std::string_view storage_code(const AccountState value) noexcept {
    switch (value) {
        case AccountState::active: return "active";
        case AccountState::locked: return "locked";
        case AccountState::deletion_scheduled: return "deletion_scheduled";
        case AccountState::tombstoned: return "tombstoned";
    }
    return {};
}

std::string_view storage_code(const SecurityLevelState value) noexcept {
    switch (value) {
        case SecurityLevelState::active: return "active";
        case SecurityLevelState::retired: return "retired";
    }
    return {};
}

std::string_view storage_code(const AuditEventKind value) noexcept {
    switch (value) {
        case AuditEventKind::account_created: return "account.created";
        case AuditEventKind::account_renamed: return "account.renamed";
        case AuditEventKind::account_state_changed: return "account.state_changed";
        case AuditEventKind::account_profile_changed: return "account.profile_changed";
        case AuditEventKind::account_credential_replaced: return "account.credential_replaced";
        case AuditEventKind::account_tombstoned: return "account.tombstoned";
        case AuditEventKind::security_policy_changed: return "security.policy_changed";
        case AuditEventKind::retention_policy_changed: return "retention.policy_changed";
        case AuditEventKind::first_run_completed: return "first_run.completed";
        case AuditEventKind::session_completed: return "session.completed";
    }
    return {};
}

std::string_view storage_code(const AuditReasonCode value) noexcept {
    switch (value) {
        case AuditReasonCode::initial_setup: return "initial_setup";
        case AuditReasonCode::registration: return "registration";
        case AuditReasonCode::sysop_action: return "sysop_action";
        case AuditReasonCode::inactivity_policy: return "inactivity_policy";
        case AuditReasonCode::security_policy: return "security_policy";
        case AuditReasonCode::retention_policy: return "retention_policy";
        case AuditReasonCode::session_finalization: return "session_finalization";
        case AuditReasonCode::caller_action: return "caller_action";
    }
    return {};
}

std::string_view storage_code(const AuditResultCode value) noexcept {
    switch (value) {
        case AuditResultCode::succeeded: return "succeeded";
    }
    return {};
}

std::string_view storage_code(const SessionOutcomeCode value) noexcept {
    switch (value) {
        case SessionOutcomeCode::normal: return "normal";
        case SessionOutcomeCode::disconnected: return "disconnected";
        case SessionOutcomeCode::timed_out: return "timed_out";
        case SessionOutcomeCode::shutdown: return "shutdown";
        case SessionOutcomeCode::runtime_error: return "runtime_error";
    }
    return {};
}

std::optional<std::string> normalize_account_name(const std::string_view candidate) {
    return trim_ascii_spaces(candidate, kAccountNameMaximum, true);
}

std::optional<std::string> account_name_comparison_key(const std::string_view normalized_name) {
    const auto normalized = normalize_account_name(normalized_name);
    if (!normalized.has_value() || *normalized != normalized_name) {
        return std::nullopt;
    }
    std::string key(normalized_name);
    for (char& character : key) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return key;
}

std::optional<std::string> normalize_real_name(const std::string_view candidate) {
    return trim_ascii_spaces(candidate, kRealNameMaximum, true);
}

std::optional<std::string> normalize_optional_profile_text(
    const std::string_view candidate,
    const std::size_t maximum_length) {
    return trim_ascii_spaces(candidate, maximum_length, false);
}

bool valid_iso_civil_date(const std::string_view candidate) noexcept {
    if (candidate.size() != 10U || candidate[4] != '-' || candidate[7] != '-') {
        return false;
    }
    for (const std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
        if (candidate[index] < '0' || candidate[index] > '9') {
            return false;
        }
    }
    const int year = (candidate[0] - '0') * 1000 + (candidate[1] - '0') * 100 +
        (candidate[2] - '0') * 10 + (candidate[3] - '0');
    const int month = decimal_pair(candidate, 5);
    const int day = decimal_pair(candidate, 8);
    if (year < 1 || month < 1 || month > 12 || day < 1) {
        return false;
    }
    static constexpr std::array<int, 12> days = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int maximum_day = month == 2 && leap_year(year) ? 29 : days[static_cast<std::size_t>(month - 1)];
    return day <= maximum_day;
}

bool valid_utc_seconds(const std::int64_t value) noexcept {
    return value >= kMinimumUtcSeconds && value <= kMaximumUtcSeconds;
}

bool valid_profile(const AccountProfile& profile) {
    if (!valid_optional_profile_field(profile.real_name, kRealNameMaximum, true) ||
        !valid_optional_profile_field(profile.location, kLocationMaximum, false) ||
        !valid_optional_profile_field(profile.phone, kPhoneMaximum, false) ||
        !valid_optional_profile_field(profile.email, kEmailMaximum, false)) {
        return false;
    }
    if (profile.birth_date.has_value() && !valid_iso_civil_date(*profile.birth_date)) {
        return false;
    }
    return !profile.sex.has_value() || *profile.sex == 'M' || *profile.sex == 'F' || *profile.sex == 'X';
}

bool valid_typed_code(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kTypedCodeMaximum || !printable_ascii(value)) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '_' || character == '.' || character == '-';
    });
}

bool valid_sha256_text(const std::string_view value) noexcept {
    return value.size() == 64U && std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool valid_credential_envelope(const CredentialEnvelope& credential) noexcept {
    if (credential.envelope_format_version == 0 || credential.parameter_encoding_version == 0 ||
        !valid_typed_code(credential.scheme_identifier) || credential.verifier.empty() ||
        !valid_utc_seconds(credential.created_utc_seconds)) {
        return false;
    }
    if (credential.pepper_key_identifier.has_value() &&
        !valid_typed_code(*credential.pepper_key_identifier)) {
        return false;
    }
    const std::size_t pepper_size = credential.pepper_key_identifier.has_value()
        ? credential.pepper_key_identifier->size()
        : 0U;
    const std::size_t fixed_size = sizeof(credential.envelope_format_version) +
        sizeof(credential.parameter_encoding_version) + 1U;
    if (credential.canonical_parameters.size() > kCredentialEnvelopeMaximum ||
        credential.salt.size() > kCredentialEnvelopeMaximum ||
        credential.verifier.size() > kCredentialEnvelopeMaximum) {
        return false;
    }
    const std::size_t payload_size = fixed_size + credential.scheme_identifier.size() +
        credential.canonical_parameters.size() + credential.salt.size() +
        credential.verifier.size() + pepper_size;
    return payload_size <= kCredentialEnvelopeMaximum;
}

bool valid_audit_context(const AuditContext& audit) noexcept {
    return audit.event_id.valid() && audit.correlation_id.valid() &&
        (!audit.actor_account_id.has_value() || *audit.actor_account_id > 0) &&
        valid_utc_seconds(audit.occurred_utc_seconds) &&
        !storage_code(audit.reason).empty();
}

bool valid_retention_policy(const RetentionPolicyRecord& policy) noexcept {
    return policy.revision >= 1 &&
        policy.audit_online_months >= 1 &&
        policy.audit_total_months >= policy.audit_online_months &&
        policy.audit_total_months <= kMaximumRetentionMonths &&
        policy.session_online_months >= 1 &&
        policy.session_total_months >= policy.session_online_months &&
        policy.session_total_months <= kMaximumRetentionMonths &&
        valid_utc_seconds(policy.modified_utc_seconds);
}

RetentionDisposition retention_disposition(
    const std::int64_t record_utc_seconds,
    const std::int64_t online_cutoff_utc_seconds,
    const std::int64_t total_cutoff_utc_seconds,
    const bool verified_archive_membership) noexcept {
    if (!valid_utc_seconds(record_utc_seconds) || !valid_utc_seconds(online_cutoff_utc_seconds) ||
        !valid_utc_seconds(total_cutoff_utc_seconds) || total_cutoff_utc_seconds > online_cutoff_utc_seconds) {
        return RetentionDisposition::keep_online;
    }
    if (record_utc_seconds <= total_cutoff_utc_seconds && verified_archive_membership) {
        return RetentionDisposition::prune_eligible;
    }
    if (record_utc_seconds <= online_cutoff_utc_seconds) {
        return RetentionDisposition::archive_required;
    }
    return RetentionDisposition::keep_online;
}

}  // namespace sfx::application
