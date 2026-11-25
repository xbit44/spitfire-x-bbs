#pragma once

#include "sfx/application/phase3a_private_adapters.hpp"

namespace sfx::application {

inline constexpr std::size_t kExternalEventFileLimit = 1024 * 1024;
inline constexpr auto kEveryEventMonth = 0x0fffU;
inline constexpr auto kEveryEventDate = 0x7fffffffU;
inline constexpr auto kEveryEventWeekday = 0x7fU;

struct ExternalEvent {
    std::uint64_t id{};
    std::string name, program, directory, arguments;
    bool enabled{true};
    unsigned months{}, dates{}, weekdays{};
    unsigned minute{}, runs_per_day{};
    bool exclusive{}, end_calls{}, at_start{}, run_missed{};
    unsigned wait_minutes{}, warn_minutes{};
    std::int64_t effective_utc{};
};

struct ExternalEventCatalog {
    std::uint64_t revision{1}, next_id{1};
    std::vector<ExternalEvent> events;
};

bool valid_external_event(const ExternalEvent&) noexcept;
bool same_event_schedule(const ExternalEvent&, const ExternalEvent&) noexcept;
bool change_external_events(ExternalEventCatalog&, ExternalEvent, bool erase, std::int64_t now);
std::string serialize_external_events(const ExternalEventCatalog&);
std::optional<ExternalEventCatalog> parse_external_events(std::string_view);
std::optional<std::int64_t> event_limited_calling_seconds(std::optional<std::int64_t> allowance,
    std::int64_t cutoff_utc, std::int64_t now_utc) noexcept;

// Earliest UTC copy of each civil slot. A missing civil slot has no UTC copy.
// `previous` looks for the latest slot at or before the supplied instant.
std::optional<std::int64_t> external_event_slot(const ExternalEvent&, const BbsLocalTimeAdapters&,
    std::int64_t utc, bool previous = false);

} // namespace sfx::application
