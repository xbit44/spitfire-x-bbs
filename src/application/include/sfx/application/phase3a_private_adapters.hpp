#pragma once

#include "sfx/application/phase3a_workflow.hpp"
#include "sfx/application/terminal_stream.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sfx::application {

class HostIndependentAccountSessionAdmissionCoordinator final
    : public IAccountSessionAdmissionPort {
public:
    explicit HostIndependentAccountSessionAdmissionCoordinator(
        std::size_t maximum_active_leases = 4096);
    ~HostIndependentAccountSessionAdmissionCoordinator() override;

    HostIndependentAccountSessionAdmissionCoordinator(
        const HostIndependentAccountSessionAdmissionCoordinator&) = delete;
    HostIndependentAccountSessionAdmissionCoordinator& operator=(
        const HostIndependentAccountSessionAdmissionCoordinator&) = delete;

    AccountSessionAdmissionResult acquire(
        const AccountSessionAdmissionRequest& request,
        const CancellationToken& cancellation) override;
    bool release(const AccountSessionAdmissionLease& lease) noexcept override;
    std::size_t active_leases() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct BbsTimeZoneTransition {
    std::uint8_t month{0};
    std::uint8_t week{0};
    std::uint8_t day_of_week{0};
    std::uint8_t hour{0};
    std::uint8_t minute{0};
    std::uint8_t second{0};
    std::uint16_t milliseconds{0};
};

struct BbsTimeZoneYearRules {
    std::int32_t bias_minutes{0};
    std::int32_t standard_bias_minutes{0};
    std::int32_t daylight_bias_minutes{0};
    BbsTimeZoneTransition standard_transition;
    BbsTimeZoneTransition daylight_transition;
};

struct BbsTimeZoneConfiguration {
    std::string canonical_windows_registry_key;

    bool valid() const noexcept;
};

class IBbsTimeZoneRuleProvider {
public:
    virtual ~IBbsTimeZoneRuleProvider() = default;
    virtual std::optional<BbsTimeZoneYearRules> rules_for_year(
        std::string_view canonical_windows_registry_key,
        std::int32_t utc_year) const = 0;
    // IANA providers use exact UTC transitions, including years with more than
    // two changes. A failed exact lookup must not fall back to Windows rules.
    // Existing providers retain their original year-rule behavior by default.
    virtual bool provides_utc_offsets() const noexcept { return false; }
    virtual std::optional<std::int32_t> utc_offset_seconds(
        std::string_view, std::int64_t) const { return std::nullopt; }
};

struct BbsLocalDateTime {
    CivilDate date;
    std::uint8_t hour{}, minute{}, second{};
    std::int32_t offset_minutes{};
};

class BbsLocalTimeAdapters final : public IBbsLocalTimestampFormatter,
                                   public IBbsLocalDateProvider {
public:
    BbsLocalTimeAdapters(
        BbsTimeZoneConfiguration configuration,
        const IBbsTimeZoneRuleProvider& rules,
        const IClock& clock);

    std::optional<std::string> format_utc_seconds(
        std::int64_t utc_seconds) const override;
    std::optional<BbsLocalDateTime> project_utc_seconds(std::int64_t utc_seconds) const;
    CivilDate today() const noexcept override;
    const BbsTimeZoneConfiguration& configuration() const noexcept;

private:
    const BbsTimeZoneConfiguration configuration_;
    const IBbsTimeZoneRuleProvider& rules_;
    const IClock& clock_;
};

struct Phase3aPrivateCompositionDependencies {
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

class Phase3aNonNetworkComposition final {
public:
    explicit Phase3aNonNetworkComposition(
        Phase3aPrivateCompositionDependencies dependencies) noexcept;

    Phase3aWorkflowResult run_existing(
        IInteractiveSessionByteStream& admitted_stream,
        const Phase3aConfigurationSnapshot& configuration,
        core::Session& session,
        const Phase3aExistingCallerRequest& request,
        const CancellationToken& cancellation,
        StreamCallerTerminalOptions terminal_options = {});
    Phase3aWorkflowResult run_registration(
        IInteractiveSessionByteStream& admitted_stream,
        const Phase3aConfigurationSnapshot& configuration,
        core::Session& session,
        const Phase3aRegistrationRequest& request,
        const CancellationToken& cancellation,
        StreamCallerTerminalOptions terminal_options = {});

private:
    Phase3aPrivateCompositionDependencies dependencies_;
};

}  // namespace sfx::application
