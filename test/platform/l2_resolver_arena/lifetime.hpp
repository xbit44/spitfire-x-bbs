// TEST ONLY: finite lifetime ledger, no production dependency or resolver I/O.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fixture {
constexpr std::size_t capacity = 4;
struct Identity {
    std::array<std::uint64_t, 2> epoch{};
    std::uint64_t generation{};
    std::uint32_t address{};
    unsigned node{};
};
inline bool same(const Identity& a, const Identity& b) noexcept {
    return a.epoch == b.epoch && a.generation == b.generation &&
        a.address == b.address && a.node == b.node;
}
enum class State { unused, pending, completed, quarantined };
struct Ticket {
    std::uint64_t request{}, lifetime{};
    std::size_t slot = capacity;
};
struct Slot {
    State state = State::unused;
    Ticket ticket{};
    Identity identity{};
    std::uintptr_t handle{};
    std::uint64_t deadline{};
    bool subscribed{};
};
class Ledger {
public:
    // Exactly one ledger per process arena. Logical start never resets slots.
    bool start(std::uint64_t lifetime) noexcept {
        if (active_ || closed_ || lifetime <= lifetime_) return false;
        lifetime_ = lifetime; active_ = true; return true;
    }
    Ticket begin(const Identity& identity, std::uint64_t now) noexcept {
        if (!active_ || closed_ || identity.node < 1 || identity.node > capacity ||
            identity.generation == 0 || (identity.epoch[0] == 0 && identity.epoch[1] == 0) ||
            next_ == std::numeric_limits<std::uint64_t>::max() ||
            now > std::numeric_limits<std::uint64_t>::max() - 2000) return {};
        // At most one subscription per exact caller node in this lifetime.
        for (const auto& s : slots_) if (s.state == State::pending &&
            s.subscribed && s.identity.node == identity.node) return {};
        for (std::size_t i = 0; i < capacity; ++i) {
            if (slots_[i].state != State::unused) continue;
            auto& s = slots_[i];
            s = Slot{}; s.state = State::pending;
            s.ticket = Ticket{++next_, lifetime_, i}; s.identity = identity;
            s.deadline = now + 2000; s.subscribed = true;
            return s.ticket;
        }
        return {}; // no heap growth, wait, native call, or new generation
    }
    bool bind(Ticket t, std::uintptr_t handle) noexcept {
        auto* s = matching(t);
        if (!s || s->state != State::pending || s->handle || !handle) return false;
        s->handle = handle; return true;
    }
    bool completion(Ticket t, std::uintptr_t handle, const Identity& current) noexcept {
        auto* s = matching(t);
        if (!s || s->state != State::pending || !handle || handle != s->handle) return false;
        s->state = State::completed;
        const bool publish = active_ && !closed_ && t.lifetime == lifetime_ &&
            s->subscribed && same(s->identity, current);
        s->subscribed = false;
        return publish;
    }
    // Only after receipt of the matching completion AND draining that message
    // slot. A native cancellation return is deliberately not an input here.
    bool retire_completed(Ticket t, bool queue_drained) noexcept {
        auto* s = matching(t);
        if (!s || s->state != State::completed || !queue_drained) return false;
        *s = Slot{}; return true;
    }
    void unsubscribe(Ticket t) noexcept {
        if (auto* s = matching(t)) s->subscribed = false;
    }
    void uncertain(Ticket t) noexcept {
        if (auto* s = matching(t)) if (s->state == State::pending) {
            s->subscribed = false; s->state = State::quarantined;
        }
    }
    void expire(std::uint64_t now) noexcept {
        for (auto& s : slots_) if (s.state == State::pending && now >= s.deadline)
            uncertain(s.ticket);
    }
    void stop() noexcept {
        active_ = false;
        for (auto& s : slots_) {
            s.subscribed = false;
            if (s.state == State::pending) s.state = State::quarantined;
        }
    }
    void close_process_coordinator() noexcept { stop(); closed_ = true; }
    std::size_t quarantined() const noexcept {
        std::size_t n = 0;
        for (const auto& s : slots_) if (s.state == State::quarantined) ++n;
        return n;
    }
    const Slot& slot(std::size_t i) const noexcept { return slots_[i]; }
private:
    Slot* matching(Ticket t) noexcept {
        if (t.slot >= capacity || t.request == 0) return nullptr;
        auto& s = slots_[t.slot];
        return s.ticket.request == t.request && s.ticket.lifetime == t.lifetime ? &s : nullptr;
    }
    std::array<Slot, capacity> slots_{};
    std::uint64_t lifetime_{}, next_{};
    bool active_{}, closed_{};
};
unsigned model_tests();
}
