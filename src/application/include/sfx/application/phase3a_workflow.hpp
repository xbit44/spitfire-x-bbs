#pragma once

#include <memory>
#include <functional>
#include <string_view>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sfx/application/phase3a.hpp"
#include "sfx/core/session.hpp"
#include "sfx/application/new_caller_names.hpp"

namespace sfx::application {

// Optional native connection sink. The caller owns and erases its session-only
// secret; callbacks carry the exact credential proof and confer no authority.
using CallerPasswordObserver=std::function<void(AccountId,const CredentialEnvelope&,std::string_view)>;

enum class Phase3aRateGateCode : std::uint8_t {
    admitted,
    cancelled,
    timed_out,
    disconnected,
    shutdown_requested,
    unavailable
};

struct Phase3aRateScopeSet {
    std::optional<AccountId> account_id;
    std::string source_ip;
    bool source{false};
    bool global{false};
    bool system_password{false};
};

struct Phase3aRateGateResult {
    Phase3aRateGateCode code{Phase3aRateGateCode::unavailable};
    std::uint32_t applied_delay_seconds{0};
};

class IPhase3aRateControlPort {
public:
    virtual ~IPhase3aRateControlPort() = default;

    virtual Phase3aRateGateResult await_admission(
        const Phase3aRateScopeSet& scopes,
        std::int64_t now_utc_seconds,
        const CancellationToken& cancellation) = 0;
    virtual StorageStatus record_failure(
        const Phase3aRateScopeSet& scopes,
        std::int64_t now_utc_seconds) = 0;
    virtual StorageStatus record_success(
        const Phase3aRateScopeSet& scopes,
        std::int64_t now_utc_seconds) = 0;
};

class IBbsLocalTimestampFormatter {
public:
    virtual ~IBbsLocalTimestampFormatter() = default;
    virtual std::optional<std::string> format_utc_seconds(
        std::int64_t utc_seconds) const = 0;
};

class ICallerWelcomeStatisticsPort {
public:
    virtual ~ICallerWelcomeStatisticsPort() = default;

    // Called only after a durable successful-session commit. Implementations may
    // refresh an in-memory status cache while returning the authoritative BBS-day
    // count that includes the just-committed call. Never independently increment
    // a cache that can also be replaced by a database snapshot.
    virtual StorageResult<std::uint64_t>
        record_successful_call_and_count_today() = 0;
};

enum class Phase3aWorkflowCode : std::uint8_t {
    authenticated_caller_ready,
    registration_requested,
    registration_unavailable,
    caller_goodbye,
    authentication_denied,
    bootstrap_incomplete,
    cancelled,
    timed_out,
    disconnected,
    shutdown_requested,
    kdf_backpressure,
    storage_failure,
    temporarily_unavailable,
    terminal_failure,
    cleanup_failure,
    invalid_context,
    internal_failure
};

struct Phase3aRegistrationContinuation {
    std::string normalized_name;
    GraphicsMode graphics{GraphicsMode::ascii};
    std::uint8_t failed_authentication_attempts{0};

    bool valid() const noexcept;
};

enum class Phase3aPreAuthenticationEndReason : std::uint8_t {
    joker_name_rejected
};

struct Phase3aPreAuthenticationEnd {
    Phase3aPreAuthenticationEndReason reason{
        Phase3aPreAuthenticationEndReason::joker_name_rejected};
    // A normalized, bounded name claim only. This is never authenticated caller
    // identity and must remain separate at every downstream boundary.
    std::string attempted_name;
};

struct Phase3aWorkflowResult {
    Phase3aWorkflowCode code{Phase3aWorkflowCode::internal_failure};
    std::optional<Phase3aRegistrationContinuation> registration;
    std::optional<AuthenticatedCallerReadyHandoff> ready;
    std::optional<Phase3aPreAuthenticationEnd> preauthentication_end;

    bool caller_ready() const noexcept {
        return code == Phase3aWorkflowCode::authenticated_caller_ready &&
            ready.has_value();
    }
};

struct Phase3aExistingCallerDependencies {
    ICallerTerminal& terminal;
    ICallerNameClassificationPort& names;
    IPhase3aCredentialOperations& credentials;
    IAccountStorage& accounts;
    IAuthenticationStorage& authentication_storage;
    IPhase3aStorage& phase3a_storage;
    IAccountSessionAdmissionPort& admission;
    IAuthenticatedSessionCommitPort& session_commit;
    ITrustedDisplaySelectionPort& displays;
    IPhase3aRateControlPort& rates;
    IBbsLocalTimestampFormatter& local_time;
    ICallerWelcomeStatisticsPort& welcome_statistics;
    IClock& clock;
    IIdentifierGenerator& identifiers;
    ICallerStageObserver* stages{nullptr};
    std::shared_ptr<CallerCleanupHealth> cleanup_health{};
    const INewCallerNamePolicy* new_names{nullptr};
    CallerPasswordObserver caller_password{};
};

struct Phase3aExistingCallerRequest {
    std::string source_ip;
    Identifier128 correlation_id;
    struct ResumeState {
        GraphicsMode graphics{GraphicsMode::ascii};
        std::uint8_t failed_authentication_attempts{0};
        AccountId newuser_entitled_account_id{0};

        bool valid() const noexcept;
    };
    std::optional<ResumeState> resume;
};

class Phase3aExistingCallerWorkflow final {
public:
    explicit Phase3aExistingCallerWorkflow(
        Phase3aExistingCallerDependencies dependencies) noexcept;

    Phase3aWorkflowResult run(
        const Phase3aConfigurationSnapshot& configuration,
        core::Session& session,
        const Phase3aExistingCallerRequest& request,
        const CancellationToken& cancellation);

private:
    Phase3aExistingCallerDependencies dependencies_;
};

enum class TransportPasswordResult : std::uint8_t { accepted, rejected, disconnect };

// Private staged entry for password-authenticating transports. An accepted
// password is not a caller-ready handoff or Sysop authority. Eligible unknown
// names receive only a provisional registration continuation, never a verified
// account identity; their throwaway password is discarded before acceptance.
// This non-copyable
// object retains the verified identity and original attempt budget until the
// one shell continuation performs the existing remaining authentication and
// durable admission path. Workflow-owned input copies are erased immediately;
// an optional native connection sink can retain a verified ordinary password
// for automatic outbound login until that connection ends. A temporary password
// is retained only until its mandatory replacement and is never forwarded.
class Phase3aTransportPasswordLogin final {
public:
    Phase3aTransportPasswordLogin(Phase3aExistingCallerDependencies dependencies,
        const Phase3aConfigurationSnapshot& configuration, core::Session& session,
        Phase3aExistingCallerRequest request, CancellationToken cancellation);
    ~Phase3aTransportPasswordLogin();
    Phase3aTransportPasswordLogin(const Phase3aTransportPasswordLogin&) = delete;
    Phase3aTransportPasswordLogin& operator=(const Phase3aTransportPasswordLogin&) = delete;

    TransportPasswordResult authenticate(std::string_view username,
        std::string_view password) noexcept;
    // Classification only, never account authentication or caller authority.
    // The continuation revalidates registration policy/name after the gate.
    bool new_registration_pending() const noexcept;
    Phase3aWorkflowResult continue_on_shell();
    std::string_view registration_refusal_notice() const noexcept;
    const Phase3aPreAuthenticationEnd* preauthentication_end() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct Phase3aRegistrationDependencies {
    ICallerTerminal& terminal;
    ICallerNameClassificationPort& names;
    IPhase3aCredentialOperations& credentials;
    IAccountStorage& accounts;
    IAuthenticationStorage& authentication_storage;
    IPhase3aStorage& phase3a_storage;
    IRegistrationCommitPort& registration_commit;
    IAccountSessionAdmissionPort& admission;
    IAuthenticatedSessionCommitPort& session_commit;
    ITrustedDisplaySelectionPort& displays;
    IPhase3aRateControlPort& rates;
    IBbsLocalTimestampFormatter& local_time;
    ICallerWelcomeStatisticsPort& welcome_statistics;
    IBbsLocalDateProvider& local_date;
    IClock& clock;
    IIdentifierGenerator& identifiers;
    ICallerStageObserver* stages{nullptr};
    std::shared_ptr<CallerCleanupHealth> cleanup_health{};
    const INewCallerNamePolicy* new_names{nullptr};
    CallerPasswordObserver caller_password{};
};

struct Phase3aRegistrationRequest {
    Phase3aRegistrationContinuation continuation;
    std::string source_ip;
    Identifier128 correlation_id;
};

class Phase3aRegistrationWorkflow final {
public:
    explicit Phase3aRegistrationWorkflow(
        Phase3aRegistrationDependencies dependencies) noexcept;

    Phase3aWorkflowResult run(
        const Phase3aConfigurationSnapshot& configuration,
        core::Session& session,
        const Phase3aRegistrationRequest& request,
        const CancellationToken& cancellation);

private:
    Phase3aRegistrationDependencies dependencies_;
};

}  // namespace sfx::application
