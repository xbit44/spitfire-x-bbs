#pragma once

#include "sfx/platform/win32/bbs_time_zone_provider.hpp"

namespace sfx::platform::win32 {

// Configured BBS timezone cache. No system/current-user timezone fallback, and no
// conversion or replacement of the authoritative UTC value in an event.
class WindowsBbsLogTimestamp final {
public:
    bool configure(std::string_view canonical_zone) noexcept;
    std::optional<std::string> format(std::int64_t utc) const noexcept;
    std::optional<application::BbsLocalDateTime> project(std::int64_t utc) const noexcept;
private:
    class UnusedClock final : public application::IClock {
        std::int64_t unix_seconds() const noexcept override { return 0; }
        std::uint64_t monotonic_millis() const noexcept override { return 0; }
    } clock_;
    std::string key_;
    std::unique_ptr<WindowsRegistryBbsTimeZoneRuleProvider> rules_;
    std::unique_ptr<application::BbsLocalTimeAdapters> formatter_;
};
} // namespace sfx::platform::win32
