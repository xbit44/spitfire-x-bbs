#include "sfx/application/hostname_lookup.hpp"
#include <algorithm>
#include <charconv>
#include <limits>

namespace sfx::application {
namespace {
std::uint64_t after(std::uint64_t now, std::uint64_t delta) noexcept {
    return now > UINT64_MAX - delta ? UINT64_MAX : now + delta;
}
}
std::optional<HostnameHint> normalize_hostname_hint(std::string_view value) noexcept {
    if (!value.empty() && value.back() == '.') value.remove_suffix(1);
    if (value.empty() || value.size() > kHostnameMaximum) return {};
    HostnameHint hint;
    std::size_t label = 0;
    char previous = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '.') {
            if (!label || previous == '-') return {};
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') ||
                (!label && c == '-') || ++label > 63) return {};
        }
        hint.bytes[i] = c; previous = c;
    }
    if (!label || previous == '-') return {};
    hint.size = static_cast<std::uint16_t>(value.size());
    return hint;
}
std::optional<std::array<std::uint8_t, 4>> hostname_source_address(std::string_view ip) noexcept {
    std::array<std::uint8_t, 4> result{};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto end = ip.find('.'); const auto part = ip.substr(0, end);
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0') ||
            ((i == 3) != (end == ip.npos))) return {};
        unsigned n{}; const auto parsed = std::from_chars(part.data(), part.data() + part.size(), n);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || n > 255) return {};
        result[i] = static_cast<std::uint8_t>(n);
        if (i != 3) ip.remove_prefix(end + 1);
    }
    return result;
}
bool HostnameSubscription::valid() const noexcept {
    return lifetime && deadline && assignment.runtime_epoch.valid() &&
        assignment.node >= 1 && assignment.node <= kAlphaNodePoolCapacity && assignment.generation;
}
bool operator==(const HostnameSubscription& a, const HostnameSubscription& b) noexcept {
    return a.assignment == b.assignment && a.address == b.address &&
        a.lifetime == b.lifetime && a.deadline == b.deadline;
}
bool HostnameLookupModel::activate(std::uint64_t lifetime, bool enabled) noexcept {
    if (closed_ || !lifetime || lifetime <= lifetime_) return false;
    for (std::size_t i = 0; i < slots_.size(); ++i) uncertain(i);
    subscribers_ = {}; queue_ = {}; cache_ = {};
    // Rate budget and permanent quarantines are process-scoped, not reset here.
    lifetime_ = lifetime; enabled_ = enabled;
    return true;
}
void HostnameLookupModel::answer(std::array<std::uint8_t, 4> address,
    HostnameHint hint, std::uint64_t now) noexcept {
    for (auto& s : subscribers_) if (s.value.valid() && !s.answered &&
        s.value.lifetime == lifetime_ && s.value.address == address && now < s.value.deadline) {
        s.hint = hint; s.answered = true;
    }
}
void HostnameLookupModel::remember(std::array<std::uint8_t, 4> address,
    HostnameHint hint, std::uint64_t now) noexcept {
    Cache* selected = nullptr;
    for (auto& c : cache_) if (c.used && c.address == address) { selected = &c; break; }
    if (!selected) for (auto& c : cache_) if (!c.used || c.expiry <= now) { selected = &c; break; }
    if (!selected) selected = &*std::min_element(cache_.begin(), cache_.end(),
        [](const Cache& a, const Cache& b) { return a.used_at < b.used_at; });
    *selected = {address, hint, after(now, hint.size ? 300000 : 60000), now, true};
}
void HostnameLookupModel::synchronize(const std::array<HostnameSubscription, kAlphaNodePoolCapacity>& desired,
    std::uint64_t now) noexcept {
    if (!enabled_ || closed_) return;
    for (auto& c : cache_) if (c.used && now >= c.expiry) c = {};
    for (auto& j : queue_) if (j.used && now >= j.deadline) {
        remember(j.address, {}, now); answer(j.address, {}, now); j = {};
    }
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const auto s = slots_[i];
        if (s.state == HostnameSlotState::pending && now >= s.deadline) {
            uncertain(i); remember(s.address, {}, now); answer(s.address, {}, now);
        }
    }
    for (std::size_t i = 0; i < desired.size(); ++i) {
        auto& s = subscribers_[i]; const auto& d = desired[i];
        if (!d.valid() || d.assignment.node != i + 1 || d.lifetime != lifetime_) { s = {}; continue; }
        if (!(s.value == d)) s = {d, {}, false};
        if (s.answered) continue;
        if (now >= d.deadline || exhausted()) { s.answered = true; continue; }
        for (auto& c : cache_) if (c.used && c.address == d.address) {
            s.hint = c.hint; s.answered = true; c.used_at = now; break;
        }
        if (s.answered) continue;
        bool found = false;
        for (const auto& r : slots_) if (r.state == HostnameSlotState::pending && r.address == d.address) found = true;
        for (const auto& j : queue_) if (j.used && j.address == d.address) found = true;
        if (found) continue;
        for (auto& j : queue_) if (!j.used) { j = {d.address, d.deadline, true}; found = true; break; }
        if (!found) s.answered = true; // overload never waits or grows a queue
    }
    if (exhausted()) { queue_ = {}; for (auto& s : subscribers_) { s.hint = {}; s.answered = true; } }
}
std::optional<std::size_t> HostnameLookupModel::next(std::uint64_t now) noexcept {
    if (!enabled_ || closed_ || exhausted() || sequence_ == UINT64_MAX) return {};
    std::size_t count = 0;
    for (std::size_t i = 0; i < starts_count_; ++i)
        if (now < after(starts_[i], 1000)) starts_[count++] = starts_[i];
    starts_count_ = count;
    if (count == starts_.size()) return {};
    for (std::size_t i = 0; i < slots_.size(); ++i) if (slots_[i].state == HostnameSlotState::free) {
        Job* job = nullptr;
        for (auto& j : queue_) if (j.used && now < j.deadline && (!job || j.deadline < job->deadline)) job = &j;
        if (!job) return {};
        slots_[i] = {HostnameSlotState::pending, job->address, ++sequence_, lifetime_, job->deadline, 0};
        *job = {}; starts_[starts_count_++] = now; return i;
    }
    return {};
}
bool HostnameLookupModel::bind(std::size_t index, std::uint64_t request, std::uintptr_t handle) noexcept {
    if (index >= slots_.size() || !handle) return false;
    auto& r = slots_[index];
    if (r.state != HostnameSlotState::pending || r.request != request || r.handle) return false;
    r.handle = handle; return true;
}
bool HostnameLookupModel::complete(std::size_t index, std::uintptr_t handle,
    std::string_view name, std::uint64_t now) noexcept {
    if (index >= slots_.size() || !handle) return false;
    auto& r = slots_[index];
    if (r.state != HostnameSlotState::pending || r.handle != handle) return false;
    if (now >= r.deadline) { uncertain(index); return false; }
    r.state = HostnameSlotState::completed;
    if (!closed_ && enabled_ && r.lifetime == lifetime_) {
        const auto hint = normalize_hostname_hint(name).value_or(HostnameHint{});
        remember(r.address, hint, now); answer(r.address, hint, now);
    }
    return true;
}
void HostnameLookupModel::drained() noexcept {
    for (auto& s : slots_) if (s.state == HostnameSlotState::completed) s = {};
}
void HostnameLookupModel::uncertain(std::size_t index) noexcept {
    if (index < slots_.size() && slots_[index].state == HostnameSlotState::pending)
        slots_[index].state = HostnameSlotState::quarantined;
}
void HostnameLookupModel::close() noexcept {
    for (std::size_t i = 0; i < slots_.size(); ++i) uncertain(i);
    closed_ = true; enabled_ = false; subscribers_ = {}; queue_ = {}; cache_ = {};
}
std::array<HostnamePublication, kAlphaNodePoolCapacity> HostnameLookupModel::publications() const noexcept {
    std::array<HostnamePublication, kAlphaNodePoolCapacity> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = {subscribers_[i].value, subscribers_[i].hint};
    return result;
}
std::size_t HostnameLookupModel::quarantined() const noexcept {
    return static_cast<std::size_t>(std::count_if(slots_.begin(), slots_.end(),
        [](const HostnameRequest& s) { return s.state == HostnameSlotState::quarantined; }));
}
std::size_t HostnameLookupModel::queued() const noexcept {
    return static_cast<std::size_t>(std::count_if(queue_.begin(), queue_.end(), [](const Job& j) { return j.used; }));
}
std::size_t HostnameLookupModel::cached() const noexcept {
    return static_cast<std::size_t>(std::count_if(cache_.begin(), cache_.end(), [](const Cache& c) { return c.used; }));
}
} // namespace sfx::application
