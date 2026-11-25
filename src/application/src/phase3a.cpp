#include "sfx/application/phase3a.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "sfx/core/node.hpp"
#include "sfx/core/security.hpp"

namespace sfx::application {
namespace {

bool valid_registration_field_mode(const RegistrationFieldMode value) noexcept {
    return value == RegistrationFieldMode::off || value == RegistrationFieldMode::required;
}

bool valid_email_mode(const EmailRegistrationMode value) noexcept {
    return value == EmailRegistrationMode::off || value == EmailRegistrationMode::ask ||
        value == EmailRegistrationMode::required;
}

bool valid_password_mode(const PasswordMode value) noexcept {
    return value == PasswordMode::standard || value == PasswordMode::strong;
}

bool valid_transfer_preference(const TransferProtocolPreference value) noexcept {
    switch (value) {
        case TransferProtocolPreference::xmodem:
        case TransferProtocolPreference::ymodem:
        case TransferProtocolPreference::ymodem_g:
        case TransferProtocolPreference::zmodem:
        case TransferProtocolPreference::select_at_transfer:
            return true;
    }
    return false;
}

bool bounded_printable(
    const std::string_view value,
    const std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum || value.front() == ' ' ||
        value.back() == ' ') {
        return false;
    }
    for (const unsigned char character : value) {
        if (character < 0x20U || character > 0x7eU) return false;
    }
    return true;
}

void add_error(
    ConfigurationValidation& validation,
    std::string field,
    std::string code) {
    validation.errors.push_back({std::move(field), std::move(code)});
}

std::vector<RegistrationField> registration_fields(
    const Phase3aConfigurationValues& values) {
    std::vector<RegistrationField> fields;
    fields.push_back(RegistrationField::caller_name);
    fields.push_back(RegistrationField::password);
    if (values.collect_optional_real_name && !values.real_names_required) {
        fields.push_back(RegistrationField::real_name);
    }
    if (values.location_mode == RegistrationFieldMode::required) {
        fields.push_back(RegistrationField::location);
    }
    if (values.birth_date_mode == RegistrationFieldMode::required) {
        fields.push_back(RegistrationField::birth_date);
    }
    if (values.sex_mode == RegistrationFieldMode::required) {
        fields.push_back(RegistrationField::sex);
    }
    if (values.email_mode != EmailRegistrationMode::off) {
        fields.push_back(RegistrationField::email);
    }
    if (values.transfer_protocol_question_enabled) {
        fields.push_back(RegistrationField::transfer_protocol);
    }
    fields.push_back(RegistrationField::final_review);
    return fields;
}

bool leap_year(const std::int32_t year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

}  // namespace

Phase3aConfigurationSnapshot::Phase3aConfigurationSnapshot(
    Phase3aConfigurationValues values,
    std::vector<RegistrationField> enabled_fields)
    : values_(std::move(values)), enabled_fields_(std::move(enabled_fields)) {}

Phase3aConfigurationResult Phase3aConfigurationSnapshot::create(
    Phase3aConfigurationValues values) {
    Phase3aConfigurationResult result;
    const ConfigurationValidation foundation_validation = validate(values.foundation);
    for (const ConfigurationError& error : foundation_validation.errors) {
        add_error(result.validation, "foundation." + error.field, error.code);
    }

    if (!valid_registration_field_mode(values.location_mode)) {
        add_error(result.validation, "registration.location_mode", "invalid_mode");
    }
    if (!valid_registration_field_mode(values.birth_date_mode)) {
        add_error(result.validation, "registration.birth_date_mode", "invalid_mode");
    }
    if (!valid_registration_field_mode(values.sex_mode)) {
        add_error(result.validation, "registration.sex_mode", "invalid_mode");
    }
    if (!valid_email_mode(values.email_mode)) {
        add_error(result.validation, "registration.email_mode", "invalid_mode");
    }
    if (!valid_password_mode(values.password_mode)) {
        add_error(result.validation, "security.password_mode", "invalid_mode");
    }
    if (values.real_names_required && values.collect_optional_real_name) {
        add_error(result.validation, "registration.real_name", "duplicate_real_name_prompt");
    }
    if (values.minimum_age.has_value()) {
        if (values.birth_date_mode != RegistrationFieldMode::required) {
            add_error(result.validation, "registration.minimum_age", "requires_birth_date");
        }
        if (*values.minimum_age < kPhase3aMinimumPlausibleAge ||
            *values.minimum_age > kPhase3aMaximumPlausibleAge) {
            add_error(result.validation, "registration.minimum_age", "outside_10_100");
        }
    }
    if (values.allow_sex_x && values.sex_mode != RegistrationFieldMode::required) {
        add_error(result.validation, "registration.allow_sex_x", "requires_sex_field");
    }

    std::array<bool, 5> seen_transfer{};
    for (const TransferProtocolPreference choice : values.enabled_transfer_choices) {
        if (!valid_transfer_preference(choice)) {
            add_error(result.validation, "registration.transfer_choices", "invalid_choice");
            continue;
        }
        const auto index = static_cast<std::size_t>(choice);
        if (seen_transfer[index]) {
            add_error(result.validation, "registration.transfer_choices", "duplicate_choice");
        }
        seen_transfer[index] = true;
    }

    if (values.security_policy.revision < 1) {
        add_error(result.validation, "security.policy_revision", "outside_positive_range");
    }
    if (!valid_sha256_text(values.security_policy.policy_hash)) {
        add_error(result.validation, "security.policy_hash", "invalid_sha256");
    }
    if (!valid_connection_limit_policy(values.connection_limits)) {
        add_error(result.validation, "security.connection_limits", "invalid_policy");
    } else if (values.connection_limits.maximum_logical_nodes !=
        values.foundation.logical_nodes) {
        add_error(result.validation, "security.connection_limits.logical_nodes",
            "foundation_mismatch");
    }
    const auto new_user_level =
        core::SecurityLevel::create(values.security_policy.new_user_security_level);
    const auto sysop_threshold =
        core::SecurityLevel::create(values.security_policy.sysop_security_threshold);
    if (!new_user_level.has_value()) {
        add_error(result.validation, "security.new_user_level", "outside_1_999");
    } else if (!values.foundation.security.is_configured(*new_user_level) ||
        values.foundation.security.new_user_level() != *new_user_level) {
        add_error(result.validation, "security.new_user_level", "foundation_mismatch");
    }
    if (!sysop_threshold.has_value()) {
        add_error(result.validation, "security.sysop_threshold", "outside_1_999");
    } else if (!values.foundation.security.is_configured(*sysop_threshold) ||
        values.foundation.security.sysop_threshold() != *sysop_threshold) {
        add_error(result.validation, "security.sysop_threshold", "foundation_mismatch");
    }
    if (values.maximum_authentication_attempts != kFailedAttemptsPerConnection) {
        add_error(result.validation, "security.maximum_authentication_attempts",
            "must_equal_locked_three");
    }
    if (!bounded_printable(
            values.prelogin_identity.bbs_name, kPhase3aBbsNameMaximum)) {
        add_error(result.validation, "prelogin.bbs_name", "invalid_bounded_printable");
    }
    if (!bounded_printable(
            values.prelogin_identity.sysop_name, kPhase3aSysopNameMaximum)) {
        add_error(result.validation, "prelogin.sysop_name", "invalid_bounded_printable");
    }
    if (!bounded_printable(
            values.prelogin_identity.version_label, kPhase3aVersionLabelMaximum)) {
        add_error(result.validation, "prelogin.version_label", "invalid_bounded_printable");
    }

    if (result.validation.valid()) {
        std::vector<RegistrationField> enabled_fields = registration_fields(values);
        Phase3aConfigurationSnapshot snapshot(
            std::move(values), std::move(enabled_fields));
        result.snapshot.emplace(std::move(snapshot));
    }
    return result;
}

const FoundationConfiguration& Phase3aConfigurationSnapshot::foundation() const noexcept {
    return values_.foundation;
}
bool Phase3aConfigurationSnapshot::bootstrap_complete() const noexcept {
    return values_.bootstrap_complete;
}
bool Phase3aConfigurationSnapshot::caller_entry_allowed() const noexcept {
    return values_.bootstrap_complete;
}
bool Phase3aConfigurationSnapshot::allow_new_users() const noexcept {
    return values_.allow_new_users;
}
bool Phase3aConfigurationSnapshot::real_names_required() const noexcept {
    return values_.real_names_required;
}
bool Phase3aConfigurationSnapshot::collect_optional_real_name() const noexcept {
    return values_.collect_optional_real_name;
}
RegistrationFieldMode Phase3aConfigurationSnapshot::location_mode() const noexcept {
    return values_.location_mode;
}
RegistrationFieldMode Phase3aConfigurationSnapshot::birth_date_mode() const noexcept {
    return values_.birth_date_mode;
}
const std::optional<std::uint8_t>& Phase3aConfigurationSnapshot::minimum_age() const noexcept {
    return values_.minimum_age;
}
RegistrationFieldMode Phase3aConfigurationSnapshot::sex_mode() const noexcept {
    return values_.sex_mode;
}
bool Phase3aConfigurationSnapshot::allow_sex_x() const noexcept {
    return values_.allow_sex_x;
}
EmailRegistrationMode Phase3aConfigurationSnapshot::email_mode() const noexcept {
    return values_.email_mode;
}
bool Phase3aConfigurationSnapshot::transfer_protocol_question_enabled() const noexcept {
    return values_.transfer_protocol_question_enabled;
}
const std::vector<TransferProtocolPreference>&
Phase3aConfigurationSnapshot::enabled_transfer_choices() const noexcept {
    return values_.enabled_transfer_choices;
}
TransferProtocolPreference
Phase3aConfigurationSnapshot::unavailable_transfer_fallback() const noexcept {
    return TransferProtocolPreference::select_at_transfer;
}
PasswordMode Phase3aConfigurationSnapshot::password_mode() const noexcept {
    return values_.password_mode;
}
const Phase3aSecurityPolicyReference&
Phase3aConfigurationSnapshot::security_policy() const noexcept {
    return values_.security_policy;
}
const ConnectionLimitPolicy&
Phase3aConfigurationSnapshot::connection_limits() const noexcept {
    return values_.connection_limits;
}
std::uint8_t Phase3aConfigurationSnapshot::maximum_authentication_attempts() const noexcept {
    return values_.maximum_authentication_attempts;
}
const BuiltinPreloginIdentity&
Phase3aConfigurationSnapshot::prelogin_identity() const noexcept {
    return values_.prelogin_identity;
}
const std::vector<RegistrationField>&
Phase3aConfigurationSnapshot::enabled_registration_fields() const noexcept {
    return enabled_fields_;
}

bool valid_civil_date(const CivilDate& value) noexcept {
    if (value.year < 1 || value.year > 9999 || value.month < 1 || value.month > 12 ||
        value.day < 1) {
        return false;
    }
    static constexpr std::array<std::uint8_t, 12> days{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    std::uint8_t maximum = days[static_cast<std::size_t>(value.month - 1U)];
    if (value.month == 2 && leap_year(value.year)) maximum = 29;
    return value.day <= maximum;
}

bool CallerNameResolution::valid() const noexcept {
    const auto submitted_key = account_name_comparison_key(normalized_name);
    if (!submitted_key.has_value()) return false;
    switch (classification) {
        case CallerNameClassification::existing: {
            if (!saved_display_name.has_value()) return false;
            const auto saved_key =
                account_name_comparison_key(*saved_display_name);
            return account_id.has_value() && *account_id > 0 &&
                saved_key.has_value() && *saved_key == *submitted_key &&
                security_level.has_value() && *security_level >= core::kMinimumSecurityLevel &&
                *security_level <= core::kMaximumSecurityLevel;
        }
        case CallerNameClassification::registerable:
        case CallerNameClassification::unavailable:
            return !account_id.has_value() && !saved_display_name.has_value() &&
                !security_level.has_value();
    }
    return false;
}

AccountSessionAdmissionLeaseOwner::AccountSessionAdmissionLeaseOwner(
    AccountSessionAdmissionLease lease,
    IAccountSessionAdmissionPort& port,
    std::shared_ptr<CallerCleanupHealth> health) noexcept
    : lease_(std::move(lease)), port_(&port), health_(std::move(health)) {}

AccountSessionAdmissionLeaseOwner::AccountSessionAdmissionLeaseOwner(
    AccountSessionAdmissionLeaseOwner&& other) noexcept
    : lease_(std::move(other.lease_)), port_(other.port_), health_(std::move(other.health_)) {
    other.lease_.reset();
    other.port_ = nullptr;
}

AccountSessionAdmissionLeaseOwner&
AccountSessionAdmissionLeaseOwner::operator=(
    AccountSessionAdmissionLeaseOwner&& other) noexcept {
    if (this != &other) {
        release();
        lease_ = std::move(other.lease_);
        port_ = other.port_;
        health_ = std::move(other.health_);
        other.lease_.reset();
        other.port_ = nullptr;
    }
    return *this;
}

AccountSessionAdmissionLeaseOwner::~AccountSessionAdmissionLeaseOwner() {
    release();
}

std::optional<AccountSessionAdmissionLeaseOwner>
AccountSessionAdmissionLeaseOwner::create(
    AccountSessionAdmissionLease lease,
    IAccountSessionAdmissionPort& port,
    std::shared_ptr<CallerCleanupHealth> health) noexcept {
    if (!lease.lease_id.valid() || lease.account_id == 0 ||
        !lease.intended_session_id.valid()) {
        return std::nullopt;
    }
    return AccountSessionAdmissionLeaseOwner(std::move(lease), port, std::move(health));
}

const AccountSessionAdmissionLease*
AccountSessionAdmissionLeaseOwner::lease() const noexcept {
    return lease_.has_value() ? &*lease_ : nullptr;
}

bool AccountSessionAdmissionLeaseOwner::active() const noexcept {
    return lease_.has_value() && port_ != nullptr;
}

bool AccountSessionAdmissionLeaseOwner::release() noexcept {
    if (!active()) return false;
    AccountSessionAdmissionLease lease = std::move(*lease_);
    IAccountSessionAdmissionPort* port = port_;
    lease_.reset();
    port_ = nullptr;
    const bool released = port->release(lease);
    if (!released && health_) health_->failed.store(true);
    return released;
}

bool valid_authenticated_session_commit_request(
    const AuthenticatedSessionCommitRequest& request) noexcept {
    const auto name_key = account_name_comparison_key(
        request.expected_account_name_key);
    const auto source = canonicalize_source_ip(request.successful_source.source_ip);
    const bool valid_protocol =
        request.successful_source.protocol == AuthenticationProtocol::telnet ||
        request.successful_source.protocol == AuthenticationProtocol::ssh;
    return request.session.session_id.valid() &&
        request.session.correlation_id.valid() && request.session.account_id > 0 &&
        request.session.node_id >= 1 &&
        request.session.node_id <= core::kMaximumLogicalNodes &&
        request.session.started_utc_seconds >= 0 &&
        request.successful_source.event_id.valid() &&
        request.successful_source.account_id == request.session.account_id &&
        source.has_value() && valid_protocol &&
        request.successful_source.succeeded_utc_seconds ==
            request.session.started_utc_seconds &&
        request.expected_account_revision >= 1 && name_key.has_value() &&
        *name_key == request.expected_account_name_key &&
        request.expected_security_level >= core::kMinimumSecurityLevel &&
        request.expected_security_level <= core::kMaximumSecurityLevel &&
        request.expected_security_policy_revision >= 1 &&
        valid_sha256_text(request.expected_security_policy_hash);
}

bool valid_registration_commit_request(
    const RegistrationCommitRequest& request) noexcept {
    const auto normalized = normalize_account_name(request.account.display_name);
    const bool empty_statistics =
        request.initial_statistics.successful_login_count == 0 &&
        request.initial_statistics.authenticated_session_seconds == 0 &&
        !request.initial_statistics.last_successful_login_utc_seconds.has_value() &&
        !request.initial_statistics.last_completed_session_utc_seconds.has_value() &&
        !request.initial_statistics.last_session_outcome.has_value();
    return normalized.has_value() &&
        *normalized == request.account.display_name &&
        valid_profile(request.account.profile) &&
        valid_credential_envelope(request.account.credential) &&
        valid_audit_context(request.account.audit) &&
        request.account.audit.reason == AuditReasonCode::registration &&
        !request.account.audit.actor_account_id.has_value() &&
        request.security_level >= core::kMinimumSecurityLevel &&
        request.security_level <= core::kMaximumSecurityLevel &&
        valid_transfer_preference(request.transfer_preference) &&
        empty_statistics && request.expected_security_policy_revision >= 1 &&
        valid_sha256_text(request.expected_security_policy_hash);
}

AuthenticatedCallerReady& AuthenticatedCallerReady::operator=(AuthenticatedCallerReady&& other) noexcept {
    if (this != &other) {
        durable_cleanup.reset(); // must precede release even when overwriting a live value
        admission_lease.reset();
        account_id = other.account_id; durable_session_id = other.durable_session_id;
        correlation_id = other.correlation_id; node_id = other.node_id;
        security_level = other.security_level; graphics = other.graphics;
        display_name = std::move(other.display_name);
        previous_login_utc = other.previous_login_utc;
        admission_lease = std::move(other.admission_lease);
        durable_cleanup = std::move(other.durable_cleanup);
    }
    return *this;
}

bool valid_authenticated_caller_ready(
    const AuthenticatedCallerReady& value) noexcept {
    const auto normalized = normalize_account_name(value.display_name);
    const AccountSessionAdmissionLease* lease = value.admission_lease.has_value()
        ? value.admission_lease->lease()
        : nullptr;
    return value.account_id > 0 && value.durable_session_id.valid() &&
        value.correlation_id.valid() && value.node_id >= 1 &&
        value.node_id <= core::kMaximumLogicalNodes &&
        value.security_level >= core::kMinimumSecurityLevel &&
        value.security_level <= core::kMaximumSecurityLevel &&
        (value.graphics == GraphicsMode::ascii || value.graphics == GraphicsMode::ansi) &&
        normalized.has_value() && *normalized == value.display_name &&
        lease != nullptr && lease->account_id == value.account_id &&
        lease->intended_session_id == value.durable_session_id;
}

AuthenticatedCallerReadyHandoff::AuthenticatedCallerReadyHandoff(
    AuthenticatedCallerReady value)
    : value_(std::move(value)) {}

AuthenticatedCallerReadyHandoff::AuthenticatedCallerReadyHandoff(
    AuthenticatedCallerReadyHandoff&& other) noexcept
    : value_(std::move(other.value_)) {
    other.value_.reset();
}

AuthenticatedCallerReadyHandoff& AuthenticatedCallerReadyHandoff::operator=(
    AuthenticatedCallerReadyHandoff&& other) noexcept {
    if (this != &other) {
        value_ = std::move(other.value_);
        other.value_.reset();
    }
    return *this;
}

std::optional<AuthenticatedCallerReadyHandoff>
AuthenticatedCallerReadyHandoff::create(AuthenticatedCallerReady value) {
    if (!valid_authenticated_caller_ready(value)) return std::nullopt;
    return AuthenticatedCallerReadyHandoff(std::move(value));
}

std::optional<AuthenticatedCallerReady> AuthenticatedCallerReadyHandoff::take() noexcept {
    if (!value_.has_value()) return std::nullopt;
    std::optional<AuthenticatedCallerReady> result(std::move(value_));
    value_.reset();
    return result;
}

bool AuthenticatedCallerReadyHandoff::consumed() const noexcept {
    return !value_.has_value();
}

}  // namespace sfx::application
