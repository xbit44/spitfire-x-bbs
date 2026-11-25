#pragma once
#include "sfx/application/phase3a_private_adapters.hpp"
#include <filesystem>
#include <memory>
#include <vector>

namespace sfx::platform::linux_host {
class LinuxBbsTimeZoneRuleProvider final : public application::IBbsTimeZoneRuleProvider {
public:
    // Once per management/Runtime process, before creating any provider. Uses
    // private bundled text rules. It never mutates the host timezone database.
    static bool initialize(const std::filesystem::path& bundled_data) noexcept;
    static std::unique_ptr<LinuxBbsTimeZoneRuleProvider> create(std::string_view);
    static std::vector<std::string> available_zones();
    // Only exact CLDR territory-001 correspondences, never an offset-based guess.
    static std::optional<std::string> windows_key_for_zone(std::string_view);
    const application::BbsTimeZoneConfiguration& configuration() const noexcept { return configuration_; }
    const std::string& iana_zone() const noexcept { return iana_zone_; }
    bool provides_utc_offsets() const noexcept override { return true; }
    std::optional<std::int32_t> utc_offset_seconds(std::string_view,std::int64_t) const override;
    std::optional<application::BbsTimeZoneYearRules> rules_for_year(std::string_view,std::int32_t) const override { return {}; }
private:
    LinuxBbsTimeZoneRuleProvider(std::string key,std::string zone):configuration_{std::move(key)},iana_zone_(std::move(zone)){}
    application::BbsTimeZoneConfiguration configuration_;
    std::string iana_zone_;
};
std::optional<std::int64_t> next_bbs_daily_run(std::string_view,std::int64_t,std::uint8_t,std::uint8_t) noexcept;
std::optional<std::int64_t> next_bbs_maintenance_run(std::string_view,std::int64_t,std::uint8_t,std::uint8_t,
    std::optional<std::uint8_t>,std::int64_t) noexcept;
} // namespace sfx::platform::linux_host
