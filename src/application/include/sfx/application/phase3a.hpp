#pragma once
#include "sfx/application/caller_lifetime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sfx/application/account_storage.hpp"
#include "sfx/application/authentication_security.hpp"
#include "sfx/application/authentication_service.hpp"
#include "sfx/application/authentication_storage.hpp"
#include "sfx/application/configuration.hpp"
#include "sfx/application/display.hpp"
#include "sfx/application/phase3a_storage.hpp"
#include "sfx/application/presentation.hpp"
#include "sfx/application/terminal.hpp"
#include "sfx/application/time.hpp"

namespace sfx::application {

inline constexpr std::size_t kPhase3aBbsNameMaximum = 64;
inline constexpr std::size_t kPhase3aSysopNameMaximum = 64;
inline constexpr std::size_t kPhase3aVersionLabelMaximum = 32;
inline constexpr std::uint8_t kPhase3aMinimumPlausibleAge = 10;
inline constexpr std::uint8_t kPhase3aMaximumPlausibleAge = 100;

enum class RegistrationFieldMode : std::uint8_t { off, required };
enum class EmailRegistrationMode : std::uint8_t { off, ask, required };
enum class RegistrationField : std::uint8_t {
    caller_name,
    password,
    real_name,
    location,
    birth_date,
    sex,
    email,
    transfer_protocol,
    final_review
};

struct Phase3aSecurityPolicyReference {
    std::int64_t revision{0};
    std::string policy_hash;
    std::uint16_t new_user_security_level{0};
    std::uint16_t sysop_security_threshold{0};
};

struct BuiltinPreloginIdentity {
    std::string bbs_name;
    std::string sysop_name;
    std::string version_label;
};

struct Phase3aConfigurationValues {
    FoundationConfiguration foundation;
    bool bootstrap_complete{false};
    bool allow_new_users{true};
    bool real_names_required{false};
    bool collect_optional_real_name{true};
    RegistrationFieldMode location_mode{RegistrationFieldMode::off};
    RegistrationFieldMode birth_date_mode{RegistrationFieldMode::off};
    std::optional<std::uint8_t> minimum_age;
    RegistrationFieldMode sex_mode{RegistrationFieldMode::off};
    bool allow_sex_x{false};
    EmailRegistrationMode email_mode{EmailRegistrationMode::off};
    bool transfer_protocol_question_enabled{true};
    std::vector<TransferProtocolPreference> enabled_transfer_choices{
        TransferProtocolPreference::xmodem,
        TransferProtocolPreference::ymodem,
        TransferProtocolPreference::ymodem_g,
        TransferProtocolPreference::zmodem};
    PasswordMode password_mode{PasswordMode::standard};
    Phase3aSecurityPolicyReference security_policy;
    ConnectionLimitPolicy connection_limits;
    std::uint8_t maximum_authentication_attempts{kFailedAttemptsPerConnection};
    BuiltinPreloginIdentity prelogin_identity;
};

struct Phase3aConfigurationResult;

class Phase3aConfigurationSnapshot final {
public:
    Phase3aConfigurationSnapshot(const Phase3aConfigurationSnapshot&) = default;
    Phase3aConfigurationSnapshot(Phase3aConfigurationSnapshot&&) noexcept = default;
    Phase3aConfigurationSnapshot& operator=(const Phase3aConfigurationSnapshot&) = delete;
    Phase3aConfigurationSnapshot& operator=(Phase3aConfigurationSnapshot&&) = delete;

    static Phase3aConfigurationResult create(Phase3aConfigurationValues values);

    const FoundationConfiguration& foundation() const noexcept;
    bool bootstrap_complete() const noexcept;
    bool caller_entry_allowed() const noexcept;
    bool allow_new_users() const noexcept;
    bool real_names_required() const noexcept;
    bool collect_optional_real_name() const noexcept;
    RegistrationFieldMode location_mode() const noexcept;
    RegistrationFieldMode birth_date_mode() const noexcept;
    const std::optional<std::uint8_t>& minimum_age() const noexcept;
    RegistrationFieldMode sex_mode() const noexcept;
    bool allow_sex_x() const noexcept;
    EmailRegistrationMode email_mode() const noexcept;
    bool transfer_protocol_question_enabled() const noexcept;
    const std::vector<TransferProtocolPreference>& enabled_transfer_choices() const noexcept;
    TransferProtocolPreference unavailable_transfer_fallback() const noexcept;
    PasswordMode password_mode() const noexcept;
    const Phase3aSecurityPolicyReference& security_policy() const noexcept;
    const ConnectionLimitPolicy& connection_limits() const noexcept;
    std::uint8_t maximum_authentication_attempts() const noexcept;
    const BuiltinPreloginIdentity& prelogin_identity() const noexcept;
    const std::vector<RegistrationField>& enabled_registration_fields() const noexcept;

private:
    Phase3aConfigurationSnapshot(
        Phase3aConfigurationValues values,
        std::vector<RegistrationField> enabled_fields);

    Phase3aConfigurationValues values_;
    std::vector<RegistrationField> enabled_fields_;
};

struct Phase3aConfigurationResult {
    ConfigurationValidation validation;
    std::optional<Phase3aConfigurationSnapshot> snapshot;

    bool valid() const noexcept {
        return validation.valid() && snapshot.has_value();
    }
};

struct CivilDate {
    std::int32_t year{0};
    std::uint8_t month{0};
    std::uint8_t day{0};
};

bool valid_civil_date(const CivilDate& value) noexcept;

class IBbsLocalDateProvider {
public:
    virtual ~IBbsLocalDateProvider() = default;
    virtual CivilDate today() const noexcept = 0;
};

enum class CallerNameClassification : std::uint8_t {
    existing,
    registerable,
    unavailable
};

struct CallerNameResolution {
    CallerNameClassification classification{CallerNameClassification::unavailable};
    std::string normalized_name;
    std::optional<AccountId> account_id;
    std::optional<std::string> saved_display_name;
    std::optional<std::uint16_t> security_level;

    bool valid() const noexcept;
};

class ICallerNameClassificationPort {
public:
    virtual ~ICallerNameClassificationPort() = default;
    virtual StorageResult<CallerNameResolution> classify(
        std::string_view normalized_name) = 0;
};

class IPhase3aCredentialOperations {
public:
    virtual ~IPhase3aCredentialOperations() = default;

    virtual PasswordAuthenticationResult verify_existing_account(
        AccountId account_id,
        std::string_view candidate,
        std::int64_t now_utc_seconds,
        const AuthenticationAuditContext& maintenance_audit,
        const CancellationToken& cancellation) = 0;
    virtual PasswordAuthenticationResult perform_dummy_verification(
        std::string_view candidate,
        std::int64_t now_utc_seconds,
        const CancellationToken& cancellation) = 0;
    struct RegistrationCredentialResult {
        AuthenticationOperationCode code{AuthenticationOperationCode::unavailable};
        PasswordPolicyCode policy_code{PasswordPolicyCode::accepted};
        std::optional<CredentialEnvelope> credential;

        bool succeeded() const noexcept {
            return code == AuthenticationOperationCode::completed &&
                policy_code == PasswordPolicyCode::accepted && credential.has_value();
        }
    };

    virtual RegistrationCredentialResult create_registration_credential(
        std::string_view candidate,
        const PasswordPolicyContext& policy,
        std::int64_t created_utc_seconds,
        const CancellationToken& cancellation) = 0;
    virtual PasswordMutationResult replace_temporary_password(
        const TemporaryPasswordReplacementOperation& request,
        const CancellationToken& cancellation) = 0;
    virtual PasswordAuthenticationResult verify_system_password(
        std::string_view candidate,
        std::int64_t now_utc_seconds,
        const AuthenticationAuditContext& maintenance_audit,
        const CancellationToken& cancellation) = 0;
};

enum class AccountSessionAdmissionCode : std::uint8_t {
    acquired,
    account_limit,
    cancelled,
    unavailable
};

struct AccountSessionAdmissionRequest {
    Identifier128 lease_id;
    AccountId account_id{0};
    Identifier128 intended_session_id;
    std::uint16_t maximum_sessions_per_account{1};
    std::uint16_t diagnostic_node{0}; // correlation only; never admission authority
};

struct AccountSessionAdmissionLease {
    Identifier128 lease_id;
    AccountId account_id{0};
    Identifier128 intended_session_id;
    std::uint16_t diagnostic_node{0};
};

struct AccountSessionAdmissionResult {
    AccountSessionAdmissionCode code{AccountSessionAdmissionCode::unavailable};
    std::optional<AccountSessionAdmissionLease> lease;
    std::uint16_t existing_node{0}; // captured under the same admission lock

    bool acquired() const noexcept {
        return code == AccountSessionAdmissionCode::acquired && lease.has_value();
    }
};

class IAccountSessionAdmissionPort {
public:
    virtual ~IAccountSessionAdmissionPort() = default;
    virtual AccountSessionAdmissionResult acquire(
        const AccountSessionAdmissionRequest& request,
        const CancellationToken& cancellation) = 0;
    virtual bool release(const AccountSessionAdmissionLease& lease) noexcept = 0;
};

class AccountSessionAdmissionLeaseOwner final {
public:
    AccountSessionAdmissionLeaseOwner(const AccountSessionAdmissionLeaseOwner&) = delete;
    AccountSessionAdmissionLeaseOwner& operator=(
        const AccountSessionAdmissionLeaseOwner&) = delete;
    AccountSessionAdmissionLeaseOwner(
        AccountSessionAdmissionLeaseOwner&& other) noexcept;
    AccountSessionAdmissionLeaseOwner& operator=(
        AccountSessionAdmissionLeaseOwner&& other) noexcept;
    ~AccountSessionAdmissionLeaseOwner();

    static std::optional<AccountSessionAdmissionLeaseOwner> create(
        AccountSessionAdmissionLease lease,
        IAccountSessionAdmissionPort& port,
        std::shared_ptr<CallerCleanupHealth> health = {}) noexcept;

    const AccountSessionAdmissionLease* lease() const noexcept;
    bool active() const noexcept;
    bool release() noexcept;

private:
    AccountSessionAdmissionLeaseOwner(
        AccountSessionAdmissionLease lease,
        IAccountSessionAdmissionPort& port,
        std::shared_ptr<CallerCleanupHealth> health) noexcept;

    std::optional<AccountSessionAdmissionLease> lease_;
    IAccountSessionAdmissionPort* port_{nullptr};
    std::shared_ptr<CallerCleanupHealth> health_;
};

struct RegistrationCommitRequest {
    CreateAccountRequest account;
    std::uint16_t security_level{0};
    TransferProtocolPreference transfer_preference{
        TransferProtocolPreference::select_at_transfer};
    AccountStatistics initial_statistics;
    std::int64_t expected_security_policy_revision{0};
    std::string expected_security_policy_hash;
};

bool valid_registration_commit_request(
    const RegistrationCommitRequest& request) noexcept;

class IRegistrationCommitPort {
public:
    virtual ~IRegistrationCommitPort() = default;
    virtual MutationResult commit_registration(
        const RegistrationCommitRequest& request) = 0;
};

struct AuthenticatedSessionCommitRequest {
    StartSessionRequest session;
    RecordSuccessfulLoginIpRequest successful_source;
    std::int64_t expected_account_revision{0};
    std::string expected_account_name_key;
    std::uint16_t expected_security_level{0};
    std::int64_t expected_security_policy_revision{0};
    std::string expected_security_policy_hash;
};

bool valid_authenticated_session_commit_request(
    const AuthenticatedSessionCommitRequest& request) noexcept;

class IAuthenticatedSessionCommitPort {
public:
    virtual ~IAuthenticatedSessionCommitPort() = default;
    virtual MutationResult commit_authenticated_session(
        const AuthenticatedSessionCommitRequest& request) = 0;
};

struct AuthenticatedCallerReady {
    AuthenticatedCallerReady() = default;
    AuthenticatedCallerReady(AuthenticatedCallerReady&&) noexcept = default;
    AuthenticatedCallerReady& operator=(AuthenticatedCallerReady&& other) noexcept;
    AccountId account_id{0};
    Identifier128 durable_session_id;
    Identifier128 correlation_id;
    std::uint16_t node_id{0};
    std::uint16_t security_level{0};
    GraphicsMode graphics{GraphicsMode::ascii};
    std::string display_name;
    std::optional<AccountSessionAdmissionLeaseOwner> admission_lease;
    // Declared after the account lease: abandoned handoffs finalize durably first.
    std::optional<DurableSessionCleanup> durable_cleanup{};
    std::optional<std::int64_t> previous_login_utc{};
};

bool valid_authenticated_caller_ready(
    const AuthenticatedCallerReady& value) noexcept;

class AuthenticatedCallerReadyHandoff final {
public:
    AuthenticatedCallerReadyHandoff(const AuthenticatedCallerReadyHandoff&) = delete;
    AuthenticatedCallerReadyHandoff& operator=(const AuthenticatedCallerReadyHandoff&) = delete;
    AuthenticatedCallerReadyHandoff(AuthenticatedCallerReadyHandoff&& other) noexcept;
    AuthenticatedCallerReadyHandoff& operator=(
        AuthenticatedCallerReadyHandoff&& other) noexcept;

    static std::optional<AuthenticatedCallerReadyHandoff> create(
        AuthenticatedCallerReady value);

    std::optional<AuthenticatedCallerReady> take() noexcept;
    bool consumed() const noexcept;

private:
    AuthenticatedCallerReadyHandoff() = default;

    std::optional<AuthenticatedCallerReady> value_;
};

}  // namespace sfx::application
