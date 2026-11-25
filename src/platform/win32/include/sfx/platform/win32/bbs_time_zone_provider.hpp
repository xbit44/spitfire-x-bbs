#pragma once

#include "sfx/application/phase3a_private_adapters.hpp"

#include <memory>
#include <optional>
#include <cstdint>
#include <string>
#include <string_view>

namespace sfx::platform::win32 {

class WindowsRegistryBbsTimeZoneRuleProvider final
    : public application::IBbsTimeZoneRuleProvider {
public:
    static std::unique_ptr<WindowsRegistryBbsTimeZoneRuleProvider> create(
        std::string_view configured_utf8_registry_key);
    ~WindowsRegistryBbsTimeZoneRuleProvider() override;

    WindowsRegistryBbsTimeZoneRuleProvider(
        const WindowsRegistryBbsTimeZoneRuleProvider&) = delete;
    WindowsRegistryBbsTimeZoneRuleProvider& operator=(
        const WindowsRegistryBbsTimeZoneRuleProvider&) = delete;

    const application::BbsTimeZoneConfiguration& configuration() const noexcept;
    std::optional<application::BbsTimeZoneYearRules> rules_for_year(
        std::string_view canonical_windows_registry_key,
        std::int32_t utc_year) const override;

private:
    class Impl;
    explicit WindowsRegistryBbsTimeZoneRuleProvider(
        std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Returns the first real UTC minute after `after_utc` whose civil wall clock in
// the selected BBS zone equals hour:minute.  Scanning real UTC minutes handles
// daylight gaps and repeated civil times without inventing a run.
std::optional<std::int64_t> next_bbs_daily_run(
    std::string_view canonical_windows_registry_key,
    std::int64_t after_utc, std::uint8_t local_hour,
    std::uint8_t local_minute) noexcept;

// Maintenance consumes at most one slot per BBS-local date. A consumed civil
// date is excluded across a repeated DST time or backwards clock adjustment.
// Weekday is Sunday=0 through Saturday=6; null denotes daily retention.
std::optional<std::int64_t> next_bbs_maintenance_run(
    std::string_view canonical_windows_registry_key,std::int64_t after_utc,
    std::uint8_t local_hour,std::uint8_t local_minute,
    std::optional<std::uint8_t> local_weekday,std::int64_t consumed_slot_utc) noexcept;

}  // namespace sfx::platform::win32
