#pragma once

#include "sfx/application/operational_log.hpp"
#include <deque>

namespace sfx::application {

inline constexpr std::size_t kOperationalBacklogEntries = 10000;
inline constexpr std::size_t kOperationalBacklogBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kOperationalPageEntries = 128;
inline constexpr std::size_t kOperationalPageBytes = 60U * 1024U;
inline constexpr std::size_t kOperationalPageHeaderBytes = 51;

struct OperationalLogCursor {
    Identifier128 epoch{};
    std::uint64_t after{0}; // exclusive; zero requests the retained beginning
};
struct OperationalEncodedEntry {
    std::uint64_t sequence{0};
    std::string record; // complete, validated SFXLOG record, excluding CRLF
};
struct OperationalLogPage {
    OperationalLogCursor cursor;
    std::uint64_t oldest{0}, newest{0}, next{0};
    bool gap{false};
    std::vector<OperationalEncodedEntry> entries;
};
bool valid_operational_page(const OperationalLogPage& page) noexcept;

// Caller owns synchronization. Only encoded strings are retained: no second
// unbounded rendered/typed copy. Both bounds evict whole oldest records.
class OperationalBacklog final {
public:
    explicit OperationalBacklog(Identifier128 epoch = {}) : epoch_(epoch) {}
    bool append(OperationalEncodedEntry entry);
    std::optional<OperationalLogPage> page(OperationalLogCursor cursor) const;
    std::vector<AlphaOperationalLogEntry> tail(std::size_t count) const;
    std::uint64_t oldest() const noexcept { return entries_.empty() ? 0 : entries_.front().sequence; }
    std::uint64_t newest() const noexcept { return entries_.empty() ? 0 : entries_.back().sequence; }
    std::size_t bytes() const noexcept { return bytes_; }
    std::uint64_t last_origin() const noexcept { return last_origin_; }
    const std::deque<OperationalEncodedEntry>& entries() const noexcept { return entries_; }
private:
    Identifier128 epoch_;
    std::deque<OperationalEncodedEntry> entries_;
    std::size_t bytes_{0};
    std::uint64_t last_origin_{0};
};

// Presentation reset retains the authenticated epoch and response high-water
// mark. A new Runtime may be bound only by the authenticated hello's owner.
class OperationalLogCache final {
public:
    void bind(Identifier128 epoch);
    void clear_presentation();
    OperationalLogCursor cursor() const noexcept { return {epoch_, cursor_}; }
    bool accept(const OperationalLogPage& page);
    bool more() const noexcept { return cursor_ < high_water_; }
    bool gap() const noexcept { return gap_; }
    const OperationalBacklog& backlog() const noexcept { return backlog_; }
private:
    Identifier128 epoch_;
    OperationalBacklog backlog_;
    std::uint64_t cursor_{0}, high_water_{0};
    bool gap_{false};
};
} // namespace sfx::application
