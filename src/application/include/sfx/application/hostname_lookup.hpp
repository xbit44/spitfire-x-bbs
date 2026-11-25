#pragma once

#include "sfx/application/node_pool.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sfx::application {
inline constexpr std::size_t kHostnameMaximum = 253;
inline constexpr std::string_view kHostnameExhaustedWarning =
    "Hostname lookup is unavailable. Connections will continue using numeric IP addresses. "
    "Use Restart BBS Now to restore hostname lookup.";

// Fixed-size display-only values. No credentials, caller ownership or authority.
struct HostnameHint {
    std::array<char, kHostnameMaximum + 1> bytes{};
    std::uint16_t size{};
    std::string_view view() const noexcept { return {bytes.data(), size}; }
};
std::optional<HostnameHint> normalize_hostname_hint(std::string_view value) noexcept;
std::optional<std::array<std::uint8_t, 4>> hostname_source_address(std::string_view ip) noexcept;

struct HostnameSubscription {
    NodeAssignmentIdentity assignment{};
    std::array<std::uint8_t, 4> address{};
    std::uint64_t lifetime{}, deadline{};
    bool valid() const noexcept;
    friend bool operator==(const HostnameSubscription& a, const HostnameSubscription& b) noexcept;
};
// Future forward-confirmed evaluation can consume this exact numeric-address
// correlation with a separate typed result/cache and its own evidence/expiry.
// This PTR hint is NOT confirmation and can never confer BotGate authority.
struct HostnamePublication { HostnameSubscription subscription; HostnameHint hint; };
enum class HostnameSlotState { free, pending, completed, quarantined };
struct HostnameRequest {
    HostnameSlotState state{HostnameSlotState::free};
    std::array<std::uint8_t, 4> address{};
    std::uint64_t request{}, lifetime{}, deadline{};
    std::uintptr_t handle{};
};

// Single coordinator-thread engine; fixed storage, no I/O, waits, callbacks or
// allocation. Native result buffers belong to the process, NOT to this object.
// Mirrors the owner-qualified R2 completion/drain/permanent-quarantine ledger.
class HostnameLookupModel final {
public:
    bool activate(std::uint64_t lifetime, bool enabled) noexcept;
    void synchronize(const std::array<HostnameSubscription, kAlphaNodePoolCapacity>& desired, std::uint64_t now) noexcept;
    std::optional<std::size_t> next(std::uint64_t now) noexcept;
    bool bind(std::size_t slot, std::uint64_t request, std::uintptr_t handle) noexcept;
    bool complete(std::size_t slot, std::uintptr_t handle, std::string_view name,
        std::uint64_t now) noexcept;
    void drained() noexcept;
    void uncertain(std::size_t slot) noexcept;
    void close() noexcept;
    std::array<HostnamePublication, kAlphaNodePoolCapacity> publications() const noexcept;
    const HostnameRequest& slot(std::size_t index) const noexcept { return slots_[index]; }
    std::size_t quarantined() const noexcept;
    bool exhausted() const noexcept { return quarantined() == slots_.size(); }
    std::size_t queued() const noexcept;
    std::size_t cached() const noexcept;
    std::uint64_t submissions() const noexcept { return sequence_; }
private:
    struct Subscriber { HostnameSubscription value; HostnameHint hint; bool answered{}; };
    struct Job { std::array<std::uint8_t, 4> address{}; std::uint64_t deadline{}; bool used{}; };
    struct Cache { std::array<std::uint8_t, 4> address{}; HostnameHint hint;
        std::uint64_t expiry{}, used_at{}; bool used{}; };
    void remember(std::array<std::uint8_t, 4> address, HostnameHint hint, std::uint64_t now) noexcept;
    void answer(std::array<std::uint8_t, 4> address, HostnameHint hint, std::uint64_t now) noexcept;
    std::array<HostnameRequest, 4> slots_{};
    std::array<Subscriber, kAlphaNodePoolCapacity> subscribers_{};
    std::array<Job, 8> queue_{};
    std::array<Cache, 256> cache_{};
    std::array<std::uint64_t, 4> starts_{};
    std::size_t starts_count_{};
    std::uint64_t lifetime_{}, sequence_{};
    bool enabled_{}, closed_{};
};
} // namespace sfx::application
