#pragma once

#include <cstdint>

namespace sfx::application {

class IClock {
public:
    virtual ~IClock() = default;
    virtual std::int64_t unix_seconds() const noexcept = 0;
    virtual std::uint64_t monotonic_millis() const noexcept = 0;
};

struct Deadline {
    std::uint64_t monotonic_millis{0};

    bool reached(std::uint64_t now_millis) const noexcept {
        return now_millis >= monotonic_millis;
    }
};

}  // namespace sfx::application

