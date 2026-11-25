#include "sfx/platform/win32/bbs_time_zone_provider.hpp"
#include "sfx/platform/win32/bbs_log_timestamp.hpp"
#include "sfx/application/operational_log.hpp"
#include "sfx/test/foundation_fakes.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace app = sfx::application;
namespace platform = sfx::platform::win32;
namespace test = sfx::test;

std::size_t assertions = 0;

void require(const bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

void resolution_and_read_only_rules() {
    auto pacific = platform::WindowsRegistryBbsTimeZoneRuleProvider::create(
        "pacific standard time");
    require(pacific != nullptr &&
            pacific->configuration().canonical_windows_registry_key ==
                "Pacific Standard Time",
        "configured registry key resolves case-insensitively to installed canonical casing");
    const auto old_rules = pacific->rules_for_year("Pacific Standard Time", 2006);
    const auto new_rules = pacific->rules_for_year("Pacific Standard Time", 2007);
    const auto current_rules = pacific->rules_for_year("Pacific Standard Time", 2026);
    require(old_rules.has_value() && new_rules.has_value() &&
            current_rules.has_value(),
        "installed dynamic DST rules resolve for historical and current represented years");
    require(old_rules->daylight_transition.month == 4 &&
            new_rules->daylight_transition.month == 3,
        "pre-2007 and post-2007 installed Pacific dynamic rules remain distinct");
    require(!pacific->rules_for_year("pacific standard time", 2026).has_value() &&
            !pacific->rules_for_year("Pacific Standard Time", 0).has_value(),
        "provider requires its resolved canonical key and representable year");
}

void installed_utc_and_fail_closed_inputs() {
    auto utc = platform::WindowsRegistryBbsTimeZoneRuleProvider::create("utc");
    require(utc != nullptr &&
            utc->configuration().canonical_windows_registry_key == "UTC",
        "UTC registry key resolves without using the host current zone");
    const auto rules = utc->rules_for_year("UTC", 2026);
    require(rules.has_value() && rules->bias_minutes == 0 &&
            rules->standard_transition.month == 0 &&
            rules->daylight_transition.month == 0,
        "non-DST installed zone returns an explicit zero-transition rule");
    test::FakeClock clock;
    clock.wall_seconds = 1'788'300'000LL;
    app::BbsLocalTimeAdapters local(utc->configuration(), *utc, clock);
    require(local.format_utc_seconds(clock.wall_seconds).has_value() &&
            app::valid_civil_date(local.today()),
        "resolved installed provider composes with both BBS-local adapters");

    require(platform::WindowsRegistryBbsTimeZoneRuleProvider::create("") == nullptr,
        "empty time-zone identifier fails closed");
    require(platform::WindowsRegistryBbsTimeZoneRuleProvider::create(
                "Not An Installed SFX Time Zone") == nullptr,
        "unavailable time-zone identifier fails closed without fallback");
    const std::string invalid_utf8{"\xC3\x28", 2};
    require(platform::WindowsRegistryBbsTimeZoneRuleProvider::create(invalid_utf8) == nullptr,
        "malformed UTF-8 time-zone identifier fails closed");
    const std::string embedded_nul{"UTC\0ignored", 11};
    require(platform::WindowsRegistryBbsTimeZoneRuleProvider::create(embedded_nul) == nullptr,
        "embedded-NUL registry-key alias fails closed");
}

void l1_log_presentation() {
    platform::WindowsBbsLogTimestamp display;
    require(!display.format(1768478400), "unconfigured log formatter has no machine-time fallback");
    require(display.configure("Pacific Standard Time"), "log formatter accepts configured canonical BBS zone");
    require(display.format(1768478400) == "2026-01-15 04:00:00", "winter UTC to BBS Pacific standard time");
    require(display.format(1784116800) == "2026-07-15 05:00:00", "summer UTC to BBS Pacific daylight time");
    require(display.format(1772963999) == "2026-03-08 01:59:59" &&
        display.format(1772964000) == "2026-03-08 03:00:00", "spring transition has no fabricated nonexistent hour");
    require(display.format(1793521800) == "2026-11-01 01:30:00" &&
        display.format(1793525400) == "2026-11-01 01:30:00", "fall-back local repetition does not change authoritative UTC identity");
    auto event = app::OperationalEvent{}; event.kind = app::OperationalKind::runtime_stopping;
    event.context.epoch.bytes[0] = 1; event.context.origin_sequence = 1;
    const auto entry = app::make_operational_entry(1, 1793521800, event);
    const auto before = app::encode_operational_record(entry);
    require(app::render_operational_line(entry, display.format(entry.utc_seconds)) ==
        "2026-11-01 01:30:00 [INFO] Runtime shutdown requested", "normal Live Logs line is BBS local, not labeled UTC");
    require(app::encode_operational_record(entry) == before && entry.utc_seconds == 1793521800,
        "presentation never rewrites the structured UTC timestamp");
    require(display.configure("India Standard Time") && display.format(1768478400) == "2026-01-15 17:30:00",
        "configured half-hour zone works independently of Windows operator zone");
    require(!display.configure("Not An Installed SFX Time Zone") && !display.format(1768478400),
        "failed reconfiguration cannot retain previous-zone labels");
    require(!display.configure("") && !display.format(1768478400), "empty BBS zone fails closed");
    require(display.configure("UTC") && display.format(1768478400) == "2026-01-15 12:00:00",
        "explicit configured UTC is supported, not an implicit fallback");
    require(!display.format(INT64_MAX), "unrepresentable timestamp yields explicit unavailable presentation");
    std::cout << "PASS L1 BBS-local log presentation: winter/summer/spring/fall/half-hour/failure; diagnostic UTC unchanged\n";
}

void scheduler_eligibility_uses_selected_bbs_zone() {
    require(platform::next_bbs_daily_run(
                "UTC", 1768478400, 12, 1) == 1768478460,
        "UTC next daily minute is strictly after the supplied instant");
    require(platform::next_bbs_daily_run(
                "Pacific Standard Time", 1772962200, 2, 30) == 1773048600,
        "spring daylight gap skips the nonexistent local run and selects the next real day");
    require(platform::next_bbs_daily_run(
                "Pacific Standard Time", 1793520000, 1, 30) == 1793521800,
        "fall repetition selects the first real matching local minute");
    require(platform::next_bbs_daily_run(
                "Pacific Standard Time", 1793521800, 1, 30) == 1793525400,
        "fall repetition selects the second real matching minute after the first passes");
    require(!platform::next_bbs_daily_run("UTC", 1768478400, 24, 0) &&
            !platform::next_bbs_daily_run("Not An Installed SFX Time Zone",
                1768478400, 2, 0),
        "invalid schedule inputs and unavailable zones fail closed");
    std::cout << "PASS CR-3 scheduler eligibility: selected BBS zone and DST gap/repetition\n";
    require(platform::next_bbs_maintenance_run("Pacific Standard Time",1793520000,1,30,{},0)==1793521800,"maintenance first real fall slot");
    require(platform::next_bbs_maintenance_run("Pacific Standard Time",1793521800,1,30,{},1793521800)==1793611800,"maintenance excludes second DST-fold execution on consumed civil date");
    require(platform::next_bbs_maintenance_run("Pacific Standard Time",1793520000-3600,1,30,{},1793521800)==1793611800,"maintenance clock reversal never replays consumed date");
    require(platform::next_bbs_maintenance_run("Pacific Standard Time",1793521800,1,30,std::uint8_t{0},1793521800)==1794130200,"weekly Sunday selects next real week, not second fold");
    require(platform::next_bbs_maintenance_run("Pacific Standard Time",1772962200,2,30,{},0)==1773048600,"maintenance gap never invents missing daily instant");
    require(!platform::next_bbs_maintenance_run("UTC",1768478400,0,0,std::uint8_t{7},0),"maintenance invalid weekday refused");
}

}  // namespace

int main() {
    try {
        resolution_and_read_only_rules();
        installed_utc_and_fail_closed_inputs();
        l1_log_presentation();
        scheduler_eligibility_uses_selected_bbs_zone();
        std::cout << "all SFX Phase 3A Window 6C Win32 time-zone tests passed assertions="
                  << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Window 6C Win32 time-zone test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
