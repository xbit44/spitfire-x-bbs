#pragma once
#include <cstdint>
#include <string_view>

namespace sfx::application {
inline constexpr std::string_view kCallerInactivityWarning =
    "\r\nYou have another minute to execute\r\na command or you will be logged off.\r\n";

// Created only after completed authentication. Only decoded caller keys invoke
// keyboard(); transport traffic, output and redraws never reset this clock.
class CallerInactivity {
public:
    enum class Action { none, warn, disconnect };
    CallerInactivity(std::uint64_t now, std::uint16_t minutes, bool authenticated_sysop)
        : last_key_(now), limit_(static_cast<std::uint64_t>(minutes) * 60000),
          exempt_(authenticated_sysop) {}
    void keyboard(std::uint64_t now) noexcept { last_key_ = now; warned_ = false; }
    void set_sysop_exemption(bool value) noexcept { exempt_ = value; }
    // Binary traffic must not masquerade as a keyboard event or extend AFK.
    std::uint64_t absolute_deadline() const noexcept { return exempt_?0:last_key_+limit_; }
    Action poll(std::uint64_t now) noexcept {
        if (exempt_ || now < last_key_) return Action::none;
        const auto elapsed = now - last_key_;
        if (elapsed >= limit_) return Action::disconnect;
        if (!warned_ && elapsed >= limit_ - 60000) {
            warned_ = true;
            return Action::warn;
        }
        return Action::none;
    }
private:
    std::uint64_t last_key_, limit_;
    bool exempt_, warned_{};
};
} // namespace sfx::application
