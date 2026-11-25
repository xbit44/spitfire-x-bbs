#pragma once

#include <cstdint>
#include <string>

namespace sfx::application {

// Worker-owned state for the currently implemented ordinary SFX menu only.
// A zero deadline cancels the notice. Runtime, not acknowledgement, owns expiry.
class CallerMenuNotice {
public:
    enum class Change { none, show, redraw };
    Change update(std::uint64_t deadline, std::uint64_t now) noexcept {
        if (deadline == deadline_) return Change::none;
        const bool was_waiting = waiting_;
        deadline_ = deadline;
        waiting_ = deadline > now;
        return waiting_ ? Change::show : was_waiting ? Change::redraw : Change::none;
    }
    bool waiting() const noexcept { return waiting_; }
    bool acknowledge(unsigned char key) noexcept {
        if (!waiting_ || (key != '\r' && key != '\n')) return false;
        waiting_ = false;
        return true;
    }
    std::string text(std::uint64_t now) const {
        if (!waiting_ || now >= deadline_) return {};
        const auto seconds = (deadline_ - now + 999) / 1000;
        const bool minutes = seconds >= 60;
        const auto count = minutes ? (seconds + 59) / 60 : seconds;
        return "\r\nSpitfire-X BBS will shut down in " + std::to_string(count) +
            (minutes ? (count == 1 ? " minute" : " minutes") :
                       (count == 1 ? " second" : " seconds")) +
            ".\r\nPlease save your work and log off soon.\r\n<enter>\r\n";
    }
private:
    std::uint64_t deadline_{};
    bool waiting_{};
};

} // namespace sfx::application
