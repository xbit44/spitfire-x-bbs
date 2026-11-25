#include "sfx/application/external_events.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace sfx::application {
namespace {
bool event_text(std::string_view text, std::size_t limit, bool optional = false) noexcept {
    return (optional || text.find_first_not_of(' ') != text.npos) && text.size() <= limit &&
        std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
}

bool valid_external_event(const ExternalEvent& event) noexcept {
    constexpr std::array<unsigned, 12> maximum_dates{31,29,31,30,31,30,31,31,30,31,30,31};
    if (!event_text(event.name, 80) || !event_text(event.program, 1024) ||
        !event_text(event.directory, 1024) || !event_text(event.arguments, 2048, true) ||
        !event.months || event.months > kEveryEventMonth ||
        !event.dates || event.dates > kEveryEventDate ||
        !event.weekdays || event.weekdays > kEveryEventWeekday ||
        event.minute >= 1440 || event.runs_per_day > 1440 || (event.end_calls && !event.exclusive) ||
        (event.exclusive && !event.wait_minutes)) return false;
    for (unsigned month = 0; month < 12; ++month) {
        if ((event.months & (1U << month)) && (event.dates & ((1U << maximum_dates[month]) - 1))) return true;
    }
    return false;
}

bool same_event_schedule(const ExternalEvent& a, const ExternalEvent& b) noexcept {
    return a.enabled == b.enabled && a.months == b.months && a.dates == b.dates &&
        a.weekdays == b.weekdays && a.runs_per_day == b.runs_per_day &&
        (a.runs_per_day || a.minute == b.minute);
}

bool change_external_events(ExternalEventCatalog& catalog, ExternalEvent event, bool erase, std::int64_t now) {
    if (catalog.revision == UINT64_MAX || now <= 0) return false;
    const auto existing = std::find_if(catalog.events.begin(), catalog.events.end(),
        [&](const auto& value) { return value.id == event.id; });
    if (erase) {
        if (existing == catalog.events.end()) return false;
        catalog.events.erase(existing);
    } else {
        if (!valid_external_event(event)) return false;
        if (!event.id) {
            if (catalog.next_id == UINT64_MAX) return false;
            event.id = catalog.next_id++;
            event.effective_utc = now;
            catalog.events.push_back(std::move(event));
        } else {
            if (existing == catalog.events.end()) return false;
            event.effective_utc = same_event_schedule(event, *existing) ? existing->effective_utc : now;
            *existing = std::move(event);
        }
    }
    ++catalog.revision;
    return true;
}

std::string serialize_external_events(const ExternalEventCatalog& catalog) {
    if (!catalog.revision || !catalog.next_id) return {};
    std::ostringstream out;
    out << "SFX-EXTERNAL-EVENTS-1 " << catalog.revision << ' ' << catalog.next_id << '\n';
    std::set<std::uint64_t> ids;
    for (const auto& event : catalog.events) {
        if (!valid_external_event(event) || !event.id || event.id >= catalog.next_id ||
            !valid_utc_seconds(event.effective_utc) || event.effective_utc <= 0 || !ids.insert(event.id).second) return {};
        out << event.id << ' ' << event.effective_utc << ' ' << event.enabled << ' ' << event.months << ' '
            << event.dates << ' ' << event.weekdays << ' ' << event.minute << ' ' << event.runs_per_day << ' '
            << event.exclusive << ' ' << event.end_calls << ' ' << event.at_start << ' ' << event.run_missed << ' '
            << event.wait_minutes << ' ' << event.warn_minutes << ' ' << std::quoted(event.name) << ' '
            << std::quoted(event.program) << ' ' << std::quoted(event.directory) << ' '
            << std::quoted(event.arguments) << '\n';
    }
    auto bytes = out.str();
    return bytes.size() <= kExternalEventFileLimit ? bytes : std::string{};
}

std::optional<ExternalEventCatalog> parse_external_events(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kExternalEventFileLimit) return {};
    std::istringstream in{std::string(bytes)};
    std::string header;
    ExternalEventCatalog catalog;
    if (!(in >> header >> catalog.revision >> catalog.next_id) || header != "SFX-EXTERNAL-EVENTS-1") return {};
    for (in >> std::ws; !in.eof(); in >> std::ws) {
        ExternalEvent event;
        if (!(in >> event.id >> event.effective_utc >> event.enabled >> event.months >> event.dates >>
                event.weekdays >> event.minute >> event.runs_per_day >> event.exclusive >> event.end_calls >>
                event.at_start >> event.run_missed >> event.wait_minutes >> event.warn_minutes >>
                std::quoted(event.name) >> std::quoted(event.program) >> std::quoted(event.directory) >>
                std::quoted(event.arguments))) return {};
        catalog.events.push_back(std::move(event));
    }
    return serialize_external_events(catalog) == bytes ? std::optional{std::move(catalog)} : std::nullopt;
}

std::optional<std::int64_t> event_limited_calling_seconds(std::optional<std::int64_t> allowance,
    std::int64_t cutoff_utc, std::int64_t now_utc) noexcept {
    if (!cutoff_utc) return allowance;
    const auto before_cutoff = std::max<std::int64_t>(0, cutoff_utc - now_utc);
    return allowance ? std::min(*allowance, before_cutoff) : before_cutoff;
}

} // namespace sfx::application
