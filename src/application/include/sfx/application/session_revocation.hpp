#pragma once

#include "sfx/application/authentication_security.hpp"
#include <array>
#include <cstdint>

namespace sfx::application {

// Runtime-owned, non-secret authorization history for the short interval
// between connection admission and authenticated handoff. Caller/mutation
// paths use the same external lock as publishing the active session identity.
// A delayed handoff cannot escape a concurrent reset/revoke operation.
class SessionRevocationLedger final {
public:
    static constexpr std::size_t capacity = 512;
    std::uint64_t checkpoint() const noexcept { return sequence_; }
    bool record(const SessionRevocationCommand& command) noexcept;
    bool permits(std::uint64_t checkpoint, AccountId account, bool sysop,
        const Identifier128& session) const noexcept;
    static bool matches(const SessionRevocationCommand& command,
        AccountId account, bool sysop, const Identifier128& session) noexcept;
private:
    struct Entry { std::uint64_t sequence{}; SessionRevocationCommand command; };
    std::array<Entry, capacity> entries_{};
    std::uint64_t sequence_{};
    bool exhausted_{};
};

} // namespace sfx::application
