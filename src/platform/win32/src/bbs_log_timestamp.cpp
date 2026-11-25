#include "sfx/platform/win32/bbs_log_timestamp.hpp"

namespace sfx::platform::win32 {
bool WindowsBbsLogTimestamp::configure(std::string_view key) noexcept {
    try {
        if (key == key_) return static_cast<bool>(formatter_);
        formatter_.reset(); rules_.reset(); key_ = key;
        if (key.empty()) return false;
        rules_ = WindowsRegistryBbsTimeZoneRuleProvider::create(key);
        if (!rules_ || rules_->configuration().canonical_windows_registry_key != key) {
            rules_.reset(); return false;
        }
        formatter_ = std::make_unique<application::BbsLocalTimeAdapters>(rules_->configuration(), *rules_, clock_);
        return true;
    } catch (...) { formatter_.reset(); rules_.reset(); return false; }
}
std::optional<std::string> WindowsBbsLogTimestamp::format(std::int64_t utc) const noexcept {
    try { return formatter_ ? formatter_->format_utc_seconds(utc) : std::nullopt; }
    catch (...) { return std::nullopt; }
}
std::optional<application::BbsLocalDateTime> WindowsBbsLogTimestamp::project(std::int64_t utc) const noexcept {
    try { return formatter_ ? formatter_->project_utc_seconds(utc) : std::nullopt; }
    catch (...) { return std::nullopt; }
}
} // namespace sfx::platform::win32
