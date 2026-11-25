#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace sfx::core {

inline constexpr std::uint16_t kMinimumSecurityLevel = 1;
inline constexpr std::uint16_t kMaximumSecurityLevel = 999;
inline constexpr std::uint16_t kFreshNewUserSecurityLevel = 10;
inline constexpr std::uint16_t kFreshSysopSecurityLevel = 999;

class SecurityLevel final {
public:
    static std::optional<SecurityLevel> create(std::uint16_t value) noexcept;

    std::uint16_t value() const noexcept;

    friend bool operator==(SecurityLevel left, SecurityLevel right) noexcept;
    friend bool operator!=(SecurityLevel left, SecurityLevel right) noexcept;
    friend bool operator<(SecurityLevel left, SecurityLevel right) noexcept;
    friend bool operator>=(SecurityLevel left, SecurityLevel right) noexcept;

private:
    explicit SecurityLevel(std::uint16_t value) noexcept;
    std::uint16_t value_;
};

enum class SecurityRuleKind : std::uint8_t {
    minimum,
    maximum,
    exact,
    range,
    sysop_only
};

struct SecurityRule {
    SecurityRuleKind kind{SecurityRuleKind::minimum};
    SecurityLevel first;
    SecurityLevel second;
};

class SecurityPolicy final {
public:
    static std::optional<SecurityPolicy> create(
        std::vector<SecurityLevel> configured_levels,
        SecurityLevel new_user_level,
        SecurityLevel sysop_threshold);
    static SecurityPolicy fresh_install();

    const std::vector<SecurityLevel>& configured_levels() const noexcept;
    SecurityLevel new_user_level() const noexcept;
    SecurityLevel sysop_threshold() const noexcept;
    bool is_configured(SecurityLevel level) const noexcept;
    bool is_sysop(SecurityLevel level) const noexcept;
    bool authorizes(SecurityLevel caller_level, const SecurityRule& rule) const noexcept;

private:
    SecurityPolicy(
        std::vector<SecurityLevel> configured_levels,
        SecurityLevel new_user_level,
        SecurityLevel sysop_threshold);

    std::vector<SecurityLevel> configured_levels_;
    SecurityLevel new_user_level_;
    SecurityLevel sysop_threshold_;
};

}  // namespace sfx::core

