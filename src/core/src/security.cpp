#include "sfx/core/security.hpp"

#include <algorithm>

namespace sfx::core {

SecurityLevel::SecurityLevel(const std::uint16_t value) noexcept : value_(value) {}

std::optional<SecurityLevel> SecurityLevel::create(const std::uint16_t value) noexcept {
    if (value < kMinimumSecurityLevel || value > kMaximumSecurityLevel) {
        return std::nullopt;
    }
    return SecurityLevel(value);
}

std::uint16_t SecurityLevel::value() const noexcept { return value_; }
bool operator==(const SecurityLevel left, const SecurityLevel right) noexcept { return left.value_ == right.value_; }
bool operator!=(const SecurityLevel left, const SecurityLevel right) noexcept { return !(left == right); }
bool operator<(const SecurityLevel left, const SecurityLevel right) noexcept { return left.value_ < right.value_; }
bool operator>=(const SecurityLevel left, const SecurityLevel right) noexcept { return !(left < right); }

SecurityPolicy::SecurityPolicy(
    std::vector<SecurityLevel> configured_levels,
    const SecurityLevel new_user_level,
    const SecurityLevel sysop_threshold)
    : configured_levels_(std::move(configured_levels)),
      new_user_level_(new_user_level),
      sysop_threshold_(sysop_threshold) {}

std::optional<SecurityPolicy> SecurityPolicy::create(
    std::vector<SecurityLevel> configured_levels,
    const SecurityLevel new_user_level,
    const SecurityLevel sysop_threshold) {
    if (configured_levels.empty()) {
        return std::nullopt;
    }
    std::sort(configured_levels.begin(), configured_levels.end());
    if (std::adjacent_find(configured_levels.begin(), configured_levels.end()) != configured_levels.end()) {
        return std::nullopt;
    }
    if (!std::binary_search(configured_levels.begin(), configured_levels.end(), new_user_level)) {
        return std::nullopt;
    }
    return SecurityPolicy(std::move(configured_levels), new_user_level, sysop_threshold);
}

SecurityPolicy SecurityPolicy::fresh_install() {
    const auto level10 = SecurityLevel::create(kFreshNewUserSecurityLevel).value();
    const auto level999 = SecurityLevel::create(kFreshSysopSecurityLevel).value();
    return SecurityPolicy({level10, level999}, level10, level999);
}

const std::vector<SecurityLevel>& SecurityPolicy::configured_levels() const noexcept { return configured_levels_; }
SecurityLevel SecurityPolicy::new_user_level() const noexcept { return new_user_level_; }
SecurityLevel SecurityPolicy::sysop_threshold() const noexcept { return sysop_threshold_; }

bool SecurityPolicy::is_configured(const SecurityLevel level) const noexcept {
    return std::binary_search(configured_levels_.begin(), configured_levels_.end(), level);
}

bool SecurityPolicy::is_sysop(const SecurityLevel level) const noexcept {
    return is_configured(level) && level >= sysop_threshold_;
}

bool SecurityPolicy::authorizes(const SecurityLevel caller_level, const SecurityRule& rule) const noexcept {
    if (!is_configured(caller_level)) {
        return false;
    }
    switch (rule.kind) {
    case SecurityRuleKind::minimum:
        return caller_level >= rule.first;
    case SecurityRuleKind::maximum:
        return rule.first >= caller_level;
    case SecurityRuleKind::exact:
        return caller_level == rule.first;
    case SecurityRuleKind::range:
        return caller_level >= rule.first && rule.second >= caller_level;
    case SecurityRuleKind::sysop_only:
        return is_sysop(caller_level);
    }
    return false;
}

}  // namespace sfx::core

