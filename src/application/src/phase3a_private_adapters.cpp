#include "sfx/application/phase3a_private_adapters.hpp"
#include "sfx/application/external_events.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sfx::application {
namespace {

struct IdentifierLess {
    bool operator()(const Identifier128& left, const Identifier128& right) const noexcept {
        return left.bytes < right.bytes;
    }
};

bool valid_admission_request(const AccountSessionAdmissionRequest& request) noexcept {
    if (!request.lease_id.valid() || request.account_id <= 0 ||
        !request.intended_session_id.valid() || request.diagnostic_node > 128) {
        return false;
    }
    ConnectionLimitPolicy policy;
    policy.maximum_sessions_per_account = request.maximum_sessions_per_account;
    return valid_connection_limit_policy(policy);
}

constexpr bool leap_year(const std::int32_t year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

constexpr std::uint8_t days_in_month(
    const std::int32_t year, const std::uint8_t month) noexcept {
    constexpr std::array<std::uint8_t, 12> days{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    return month == 2 && leap_year(year) ? 29 : days[month - 1U];
}

// Howard Hinnant's civil-calendar algorithms, shifted to the Unix epoch.
constexpr std::int64_t days_from_civil(
    std::int32_t year, const std::uint8_t month, const std::uint8_t day) noexcept {
    year -= month <= 2;
    const std::int32_t era = (year >= 0 ? year : year - 399) / 400;
    const std::uint32_t yoe = static_cast<std::uint32_t>(year - era * 400);
    const std::uint32_t adjusted_month = month > 2
        ? static_cast<std::uint32_t>(month - 3U)
        : static_cast<std::uint32_t>(month + 9U);
    const std::uint32_t doy = (153U * adjusted_month + 2U) / 5U + day - 1U;
    const std::uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return static_cast<std::int64_t>(era) * 146097LL + doe - 719468LL;
}

std::tuple<std::int32_t, std::uint8_t, std::uint8_t> civil_from_days(
    const std::int64_t unix_days) noexcept {
    const std::int64_t shifted = unix_days + 719468LL;
    const std::int64_t era = (shifted >= 0 ? shifted : shifted - 146096LL) / 146097LL;
    const std::uint32_t doe = static_cast<std::uint32_t>(shifted - era * 146097LL);
    const std::uint32_t yoe =
        (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    std::int32_t year = static_cast<std::int32_t>(yoe) +
        static_cast<std::int32_t>(era * 400LL);
    const std::uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    const std::uint32_t mp = (5U * doy + 2U) / 153U;
    const std::uint8_t day = static_cast<std::uint8_t>(
        doy - (153U * mp + 2U) / 5U + 1U);
    const std::uint8_t month = static_cast<std::uint8_t>(
        mp < 10U ? mp + 3U : mp - 9U);
    year += month <= 2;
    return {year, month, day};
}

std::int64_t floor_div(const std::int64_t value, const std::int64_t divisor) noexcept {
    std::int64_t quotient = value / divisor;
    const std::int64_t remainder = value % divisor;
    if (remainder < 0) --quotient;
    return quotient;
}

std::uint8_t weekday(const std::int64_t unix_days) noexcept {
    std::int64_t value = (unix_days + 4LL) % 7LL;
    if (value < 0) value += 7LL;
    return static_cast<std::uint8_t>(value);
}

bool valid_transition(const BbsTimeZoneTransition& value) noexcept {
    if (value.month == 0) {
        return value.week == 0 && value.day_of_week == 0 && value.hour == 0 &&
            value.minute == 0 && value.second == 0 && value.milliseconds == 0;
    }
    return value.month <= 12 && value.week >= 1 && value.week <= 5 &&
        value.day_of_week <= 6 && value.hour <= 23 && value.minute <= 59 &&
        value.second <= 59 && value.milliseconds <= 999;
}

bool valid_rules(const BbsTimeZoneYearRules& value) noexcept {
    constexpr std::int32_t maximum_bias_minutes = 24 * 60;
    if (value.bias_minutes < -maximum_bias_minutes ||
        value.bias_minutes > maximum_bias_minutes ||
        value.standard_bias_minutes < -maximum_bias_minutes ||
        value.standard_bias_minutes > maximum_bias_minutes ||
        value.daylight_bias_minutes < -maximum_bias_minutes ||
        value.daylight_bias_minutes > maximum_bias_minutes ||
        !valid_transition(value.standard_transition) ||
        !valid_transition(value.daylight_transition)) {
        return false;
    }
    return (value.standard_transition.month == 0) ==
        (value.daylight_transition.month == 0);
}

bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::size_t continuation = 0;
        std::uint32_t code_point = 0;
        if (lead <= 0x7fU) {
            code_point = lead;
        } else if (lead >= 0xc2U && lead <= 0xdfU) {
            continuation = 1;
            code_point = lead & 0x1fU;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            continuation = 2;
            code_point = lead & 0x0fU;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            continuation = 3;
            code_point = lead & 0x07U;
        } else {
            return false;
        }
        if (index + continuation >= value.size()) return false;
        for (std::size_t offset = 1; offset <= continuation; ++offset) {
            const auto byte = static_cast<unsigned char>(value[index + offset]);
            if ((byte & 0xc0U) != 0x80U) return false;
            code_point = (code_point << 6U) | (byte & 0x3fU);
        }
        if ((continuation == 1 && code_point < 0x80U) ||
            (continuation == 2 && code_point < 0x800U) ||
            (continuation == 3 && code_point < 0x10000U) ||
            code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) return false;
        index += continuation + 1U;
    }
    return true;
}

std::optional<std::int64_t> transition_local_seconds(
    const std::int32_t year,
    const BbsTimeZoneTransition& transition) noexcept {
    if (transition.month == 0 || transition.milliseconds != 0) return std::nullopt;
    const std::uint8_t maximum = days_in_month(year, transition.month);
    const std::int64_t first_day = days_from_civil(year, transition.month, 1);
    const std::uint8_t first_weekday = weekday(first_day);
    std::uint8_t day = static_cast<std::uint8_t>(
        1U + (transition.day_of_week + 7U - first_weekday) % 7U +
        7U * (transition.week - 1U));
    if (transition.week == 5 && day > maximum) day = static_cast<std::uint8_t>(day - 7U);
    if (day < 1 || day > maximum) return std::nullopt;
    const std::int64_t days = days_from_civil(year, transition.month, day);
    return days * 86400LL + transition.hour * 3600LL +
        transition.minute * 60LL + transition.second;
}

std::optional<std::int64_t> local_seconds_for_rules(
    const std::int64_t utc_seconds,
    const std::int32_t rule_year,
    const BbsTimeZoneYearRules& rules) noexcept {
    if (!valid_rules(rules)) return std::nullopt;
    bool daylight = false;
    if (rules.daylight_transition.month != 0) {
        const auto daylight_local = transition_local_seconds(
            rule_year, rules.daylight_transition);
        const auto standard_local = transition_local_seconds(
            rule_year, rules.standard_transition);
        if (!daylight_local.has_value() || !standard_local.has_value()) {
            return std::nullopt;
        }
        const std::int64_t daylight_utc = *daylight_local +
            static_cast<std::int64_t>(rules.bias_minutes +
                rules.standard_bias_minutes) * 60LL;
        const std::int64_t standard_utc = *standard_local +
            static_cast<std::int64_t>(rules.bias_minutes +
                rules.daylight_bias_minutes) * 60LL;
        daylight = daylight_utc < standard_utc
            ? utc_seconds >= daylight_utc && utc_seconds < standard_utc
            : utc_seconds >= daylight_utc || utc_seconds < standard_utc;
    }
    const std::int64_t effective_bias = static_cast<std::int64_t>(
        rules.bias_minutes) +
        (daylight ? rules.daylight_bias_minutes : rules.standard_bias_minutes);
    if ((effective_bias < 0 &&
            utc_seconds > std::numeric_limits<std::int64_t>::max() +
                effective_bias * 60LL) ||
        (effective_bias > 0 &&
            utc_seconds < std::numeric_limits<std::int64_t>::min() +
                effective_bias * 60LL)) {
        return std::nullopt;
    }
    const std::int64_t local_seconds = utc_seconds - effective_bias * 60LL;
    return valid_utc_seconds(local_seconds)
        ? std::optional<std::int64_t>(local_seconds) : std::nullopt;
}

}  // namespace

class HostIndependentAccountSessionAdmissionCoordinator::Impl final {
public:
    explicit Impl(const std::size_t maximum) : maximum_active_leases(maximum) {}

    const std::size_t maximum_active_leases;
    mutable std::mutex mutex;
    std::map<Identifier128, AccountSessionAdmissionLease, IdentifierLess> leases;
    std::map<AccountId, std::uint16_t> account_counts;
    std::set<Identifier128, IdentifierLess> intended_sessions;
};

HostIndependentAccountSessionAdmissionCoordinator::
HostIndependentAccountSessionAdmissionCoordinator(const std::size_t maximum_active_leases)
    : impl_(std::make_unique<Impl>(maximum_active_leases)) {
    if (maximum_active_leases == 0 || maximum_active_leases > 65'536) {
        throw std::invalid_argument("invalid bounded account-session admission capacity");
    }
}

HostIndependentAccountSessionAdmissionCoordinator::
~HostIndependentAccountSessionAdmissionCoordinator() = default;

AccountSessionAdmissionResult
HostIndependentAccountSessionAdmissionCoordinator::acquire(
    const AccountSessionAdmissionRequest& request,
    const CancellationToken& cancellation) {
    if (cancellation.is_cancelled()) return {AccountSessionAdmissionCode::cancelled, std::nullopt};
    if (!valid_admission_request(request)) {
        return {AccountSessionAdmissionCode::unavailable, std::nullopt};
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (cancellation.is_cancelled()) return {AccountSessionAdmissionCode::cancelled, std::nullopt};
    if (impl_->leases.size() >= impl_->maximum_active_leases ||
        impl_->leases.find(request.lease_id) != impl_->leases.end() ||
        impl_->intended_sessions.find(request.intended_session_id) !=
            impl_->intended_sessions.end()) {
        return {AccountSessionAdmissionCode::unavailable, std::nullopt};
    }
    const auto count = impl_->account_counts.find(request.account_id);
    const std::uint16_t active = count == impl_->account_counts.end() ? 0 : count->second;
    ConnectionLimitPolicy policy;
    policy.maximum_sessions_per_account = request.maximum_sessions_per_account;
    if (evaluate_account_session_admission(active, policy) ==
        ConnectionAdmissionCode::account_session_limit) {
        std::uint16_t existing{};
        for (const auto& item : impl_->leases) if (item.second.account_id == request.account_id) {
            existing = item.second.diagnostic_node; break;
        }
        return {AccountSessionAdmissionCode::account_limit, std::nullopt, existing};
    }
    AccountSessionAdmissionLease lease{
        request.lease_id, request.account_id, request.intended_session_id, request.diagnostic_node};
    // Keep all three ownership indexes transactional even if allocation fails.
    // No partially published lease may survive a failed admission attempt.
    bool new_account = false, new_session = false;
    try {
        const auto account = impl_->account_counts.emplace(lease.account_id, active);
        new_account = account.second;
        new_session = impl_->intended_sessions.insert(lease.intended_session_id).second;
        impl_->leases.emplace(lease.lease_id, lease);
        account.first->second = static_cast<std::uint16_t>(active + 1U);
    } catch (...) {
        if (new_session) impl_->intended_sessions.erase(lease.intended_session_id);
        if (new_account) impl_->account_counts.erase(lease.account_id);
        return {AccountSessionAdmissionCode::unavailable, std::nullopt};
    }
    return {AccountSessionAdmissionCode::acquired, lease};
}

bool HostIndependentAccountSessionAdmissionCoordinator::release(
    const AccountSessionAdmissionLease& lease) noexcept {
    if (!lease.lease_id.valid() || lease.account_id <= 0 ||
        !lease.intended_session_id.valid()) return false;
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto found = impl_->leases.find(lease.lease_id);
        if (found == impl_->leases.end() || found->second.account_id != lease.account_id ||
            found->second.intended_session_id != lease.intended_session_id) return false;
        impl_->intended_sessions.erase(found->second.intended_session_id);
        const auto count = impl_->account_counts.find(found->second.account_id);
        if (count == impl_->account_counts.end() || count->second == 0) return false;
        if (--count->second == 0) impl_->account_counts.erase(count);
        impl_->leases.erase(found);
        return true;
    } catch (...) {
        return false;
    }
}

std::size_t HostIndependentAccountSessionAdmissionCoordinator::active_leases() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->leases.size();
    } catch (...) {
        return 0;
    }
}

bool BbsTimeZoneConfiguration::valid() const noexcept {
    if (canonical_windows_registry_key.empty() ||
        canonical_windows_registry_key.size() > 128 ||
        !valid_utf8(canonical_windows_registry_key)) return false;
    return std::all_of(canonical_windows_registry_key.begin(),
        canonical_windows_registry_key.end(), [](const unsigned char value) {
            return value >= 0x20U && value != 0x7fU;
        });
}

BbsLocalTimeAdapters::BbsLocalTimeAdapters(
    BbsTimeZoneConfiguration configuration,
    const IBbsTimeZoneRuleProvider& rules,
    const IClock& clock)
    : configuration_(std::move(configuration)), rules_(rules), clock_(clock) {
    if (!configuration_.valid()) throw std::invalid_argument("invalid BBS time-zone configuration");
}

std::optional<BbsLocalDateTime> BbsLocalTimeAdapters::project_utc_seconds(
    const std::int64_t utc_seconds) const {
    if (!valid_utc_seconds(utc_seconds)) return std::nullopt;
    const std::int64_t utc_days = floor_div(utc_seconds, 86400LL);
    const auto [utc_year, utc_month, utc_day] = civil_from_days(utc_days);
    (void)utc_month;
    (void)utc_day;
    std::optional<std::int64_t> local_seconds;
    std::int32_t selected_rule_year = utc_year;
    if (rules_.provides_utc_offsets()) {
        const auto offset=rules_.utc_offset_seconds(configuration_.canonical_windows_registry_key,utc_seconds);
        if(!offset || *offset < -86400 || *offset > 86400 || !valid_utc_seconds(utc_seconds+*offset))
            return std::nullopt;
        local_seconds=utc_seconds+*offset;
        selected_rule_year=std::get<0>(civil_from_days(floor_div(*local_seconds,86400LL)));
    } else {
        const auto rules = rules_.rules_for_year(configuration_.canonical_windows_registry_key, utc_year);
        if (!rules.has_value() || !valid_rules(*rules)) return std::nullopt;
        local_seconds = local_seconds_for_rules(utc_seconds, utc_year, *rules);
        if (!local_seconds.has_value()) return std::nullopt;
        const auto [initial_local_year, ignored_month, ignored_day] =
            civil_from_days(floor_div(*local_seconds, 86400LL));
        (void)ignored_month;
        (void)ignored_day;
        if (initial_local_year != utc_year) {
            const auto boundary_rules = rules_.rules_for_year(
                configuration_.canonical_windows_registry_key, initial_local_year);
            if (!boundary_rules.has_value()) return std::nullopt;
            selected_rule_year = initial_local_year;
            local_seconds = local_seconds_for_rules(utc_seconds, initial_local_year, *boundary_rules);
            if (!local_seconds.has_value()) return std::nullopt;
        }
    }
    const std::int64_t local_days = floor_div(*local_seconds, 86400LL);
    std::int64_t second_of_day = *local_seconds - local_days * 86400LL;
    const auto [year, month, day] = civil_from_days(local_days);
    BbsLocalDateTime result;
    result.date = {year, month, day};
    result.hour = static_cast<std::uint8_t>(second_of_day / 3600LL);
    second_of_day %= 3600LL;
    result.minute = static_cast<std::uint8_t>(second_of_day / 60LL);
    result.second = static_cast<std::uint8_t>(second_of_day % 60LL);
    result.offset_minutes = static_cast<std::int32_t>((*local_seconds - utc_seconds) / 60);
    if (!valid_civil_date(result.date) || result.date.year != selected_rule_year) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> BbsLocalTimeAdapters::format_utc_seconds(
    const std::int64_t utc_seconds) const {
    const auto local = project_utc_seconds(utc_seconds);
    if (!local.has_value()) return std::nullopt;
    std::array<char, 19> output{
        '0', '0', '0', '0', '-', '0', '0', '-', '0', '0', ' ',
        '0', '0', ':', '0', '0', ':', '0', '0'};
    const auto write_digits = [&output](std::size_t offset,
        std::uint32_t value, const std::size_t width) noexcept {
        for (std::size_t index = 0; index < width; ++index) {
            output[offset + width - index - 1U] =
                static_cast<char>('0' + value % 10U);
            value /= 10U;
        }
    };
    write_digits(0, static_cast<std::uint32_t>(local->date.year), 4);
    write_digits(5, local->date.month, 2);
    write_digits(8, local->date.day, 2);
    write_digits(11, local->hour, 2);
    write_digits(14, local->minute, 2);
    write_digits(17, local->second, 2);
    return std::string(output.begin(), output.end());
}

CivilDate BbsLocalTimeAdapters::today() const noexcept {
    try {
        const auto local = project_utc_seconds(clock_.unix_seconds());
        return local.has_value() ? local->date : CivilDate{};
    } catch (...) {
        return {};
    }
}

const BbsTimeZoneConfiguration& BbsLocalTimeAdapters::configuration() const noexcept {
    return configuration_;
}

Phase3aNonNetworkComposition::Phase3aNonNetworkComposition(
    Phase3aPrivateCompositionDependencies dependencies) noexcept
    : dependencies_(dependencies) {}

Phase3aWorkflowResult Phase3aNonNetworkComposition::run_existing(
    IInteractiveSessionByteStream& admitted_stream,
    const Phase3aConfigurationSnapshot& configuration,
    core::Session& session,
    const Phase3aExistingCallerRequest& request,
    const CancellationToken& cancellation,
    const StreamCallerTerminalOptions terminal_options) {
    StreamCallerTerminal terminal(admitted_stream, terminal_options);
    Phase3aExistingCallerWorkflow workflow({terminal, dependencies_.names,
        dependencies_.credentials, dependencies_.accounts,
        dependencies_.authentication_storage, dependencies_.phase3a_storage,
        dependencies_.admission, dependencies_.session_commit, dependencies_.displays,
        dependencies_.rates, dependencies_.local_time, dependencies_.welcome_statistics,
        dependencies_.clock,
        dependencies_.identifiers, dependencies_.stages, dependencies_.cleanup_health, dependencies_.new_names,dependencies_.caller_password});
    return workflow.run(configuration, session, request, cancellation);
}

Phase3aWorkflowResult Phase3aNonNetworkComposition::run_registration(
    IInteractiveSessionByteStream& admitted_stream,
    const Phase3aConfigurationSnapshot& configuration,
    core::Session& session,
    const Phase3aRegistrationRequest& request,
    const CancellationToken& cancellation,
    const StreamCallerTerminalOptions terminal_options) {
    StreamCallerTerminal terminal(admitted_stream, terminal_options);
    Phase3aRegistrationWorkflow workflow({terminal, dependencies_.names,
        dependencies_.credentials, dependencies_.accounts,
        dependencies_.authentication_storage, dependencies_.phase3a_storage,
        dependencies_.registration_commit, dependencies_.admission,
        dependencies_.session_commit, dependencies_.displays, dependencies_.rates,
        dependencies_.local_time, dependencies_.welcome_statistics,
        dependencies_.local_date, dependencies_.clock,
        dependencies_.identifiers, dependencies_.stages, dependencies_.cleanup_health, dependencies_.new_names,dependencies_.caller_password});
    return workflow.run(configuration, session, request, cancellation);
}

std::optional<std::int64_t> external_event_slot(const ExternalEvent& event, const BbsLocalTimeAdapters& zone,
    std::int64_t utc, bool previous) {
    if (!event.enabled || !valid_external_event(event)) return {};
    const auto start = zone.project_utc_seconds(utc);
    if (!start) return {};
    const auto start_day = days_from_civil(start->date.year, start->date.month, start->date.day);
    const unsigned count = event.runs_per_day ? event.runs_per_day : 1;
    // One Gregorian cycle covers every combination of calendar filters.
    for (int step = 0; step <= 146097; ++step) {
        const auto day = start_day + (previous ? -step : step);
        const auto day_start = day * 86400;
        if (previous && day_start + 172800 <= event.effective_utc) break;
        const auto [year, month, date] = civil_from_days(day);
        if (!(event.months & (1U << (month - 1))) ||
            !(event.dates & (1U << (date - 1))) ||
            !(event.weekdays & (1U << weekday(day)))) continue;
        std::array<std::int32_t, 4> offsets{};
        std::size_t used{};
        for (int sample : {-1, 0, 1, 2}) {
            const auto local = zone.project_utc_seconds(day_start + sample * 86400);
            if (!local) continue;
            const auto end = offsets.begin() + used;
            if (std::find(offsets.begin(), end, local->offset_minutes) == end)
                offsets[used++] = local->offset_minutes;
        }
        if (!used) return {};
        for (unsigned step_in_day = 0; step_in_day < count; ++step_in_day) {
            const auto index = previous ? count - 1 - step_in_day : step_in_day;
            const auto minute = event.runs_per_day ? index * 1440 / count : event.minute;
            std::optional<std::int64_t> earliest;
            for (std::size_t i = 0; i < used; ++i) {
                const auto candidate = day_start + static_cast<std::int64_t>(minute) * 60 - offsets[i] * 60;
                const auto actual = zone.project_utc_seconds(candidate);
                if (actual && actual->date.year == year && actual->date.month == month &&
                    actual->date.day == date && actual->hour == minute / 60 &&
                    actual->minute == minute % 60 && actual->second == 0 &&
                    (!earliest || candidate < *earliest)) earliest = candidate;
            }
            if (earliest && *earliest > event.effective_utc && (previous ? *earliest <= utc : *earliest > utc))
                return earliest;
        }
    }
    return {};
}

}  // namespace sfx::application
