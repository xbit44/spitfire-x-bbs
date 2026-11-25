#include "sfx/application/scheduler.hpp"
#include "sfx/application/external_events.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace app = sfx::application;

namespace {
unsigned checks = 0;
void require(const bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}

std::string run_id(const unsigned value) {
    const char digit = static_cast<char>('A' + (value % 6));
    return std::string(31, 'A') + digit;
}

app::NextDailyRun hourly() {
    return [](const std::int64_t after, const std::uint8_t hour,
              const std::uint8_t minute) -> std::optional<std::int64_t> {
        if (hour >= 24 || minute >= 60) return std::nullopt;
        return after + 3600;
    };
}
void external_event_checks() {
    struct Clock final : app::IClock {
        std::int64_t unix_seconds() const noexcept override { return 1794441600; }
        std::uint64_t monotonic_millis() const noexcept override { return 0; }
    } clock;
    struct PacificRules final : app::IBbsTimeZoneRuleProvider {
        std::optional<app::BbsTimeZoneYearRules> rules_for_year(std::string_view, std::int32_t) const override {
            return {};
        }
        bool provides_utc_offsets() const noexcept override { return true; }
        std::optional<std::int32_t> utc_offset_seconds(std::string_view, std::int64_t utc) const override {
            return utc >= 1772964000 && utc < 1793523600 ? -7*3600 : -8*3600;
        }
    } rules;
    app::BbsLocalTimeAdapters zone({"Pacific Standard Time"}, rules, clock);
    app::ExternalEvent event;
    require(event.enabled && !app::valid_external_event(event), "new external event enabled with blank schedule");
    event.name = "Door \"maintenance\"";
    event.program = R"(D:\Maintenance folder\event.bat)";
    event.directory = R"(D:\Maintenance folder)";
    event.months = app::kEveryEventMonth;
    event.dates = app::kEveryEventDate;
    event.weekdays = app::kEveryEventWeekday;
    event.minute = 150; // 02:30 local.
    require(app::external_event_slot(event,zone,1772960400) == 1773048600, "spring gap skips nonexistent 02:30");
    event.minute = 90;
    require(app::external_event_slot(event,zone,1793516400) == 1793521800, "fall runs the first copy of 01:30");
    require(app::external_event_slot(event,zone,1793521800) == 1793611800, "fall skips the second copy of 01:30");
    require(app::external_event_slot(event,zone,1793523600,true) == 1793521800, "fall catch-up references same civil slot");
    event.runs_per_day = 24;
    require(app::external_event_slot(event,zone,1794441637) == 1794445200, "24 runs are aligned to the next hour");
    event.runs_per_day = 0;
    event.minute = 0;
    event.dates = 1U << 12;
    event.weekdays = 1U << 5;
    require(app::external_event_slot(event,zone,clock.unix_seconds()) == 1794556800, "Friday the 13th requires both calendar matches");
    app::ExternalEventCatalog catalog;
    require(app::change_external_events(catalog,event,false,clock.unix_seconds()),"save a named event");
    const auto stored = app::serialize_external_events(catalog);
    const auto parsed = app::parse_external_events(stored);
    require(parsed && parsed->events.front().program == event.program && parsed->events.front().name == event.name,
        "paths and quotes round trip");
    require(!app::parse_external_events(stored + "garbage"),"malformed catalog refused");
    event = parsed->events.front();
    event.name = "Updated";
    require(app::change_external_events(catalog,event,false,clock.unix_seconds()+1) &&
        catalog.events.front().effective_utc == clock.unix_seconds(),"name change keeps original schedule");
    event.enabled = false;
    require(app::change_external_events(catalog,event,false,clock.unix_seconds()+2) &&
        catalog.events.front().effective_utc == clock.unix_seconds()+2 &&
        !app::external_event_slot(event,zone,clock.unix_seconds()),"disabled event has no scheduled run");
    require(app::event_limited_calling_seconds({}, 1300, 1000) == 300 &&
        app::event_limited_calling_seconds(40, 1300, 1000) == 40 &&
        !app::event_limited_calling_seconds({}, 0, 1000), "menu time is capped at the earlier deadline");
}
} // namespace

int main() {
    try {
        external_event_checks();
        app::SchedulerConfiguration configuration;
        const auto config_text = app::serialize_scheduler_configuration(configuration);
        const auto parsed_config = app::parse_scheduler_configuration(config_text);
        require(parsed_config && !parsed_config->enabled && parsed_config->format_version == 2 &&
                    parsed_config->backup_destination.empty() && parsed_config->retained_backups == 10 &&
                    parsed_config->local_hour == 2 && parsed_config->local_minute == 0,
                "canonical daily-local configuration round trip");
        require(!app::parse_scheduler_configuration(
                    config_text + "unknown=value\n"),
                "unknown scheduler configuration field refused");

        app::RuntimeScheduler off(configuration, {}, hourly());
        require(off.recover_on_start(1000) && off.state().next_run_utc == 0 &&
                    !off.begin_due(5000, run_id(1)), "new scheduling defaults Off");
        const auto off_manual = off.begin_manual(5000, run_id(1));
        require(off_manual && !off_manual->scheduled, "manual backup independent of scheduling Off");
        auto legacy = configuration;
        legacy.format_version = 1; legacy.enabled = true; legacy.local_hour = 5; legacy.revision = 9;
        const auto legacy_text = app::serialize_scheduler_configuration(legacy);
        const auto legacy_read = app::parse_scheduler_configuration(legacy_text);
        require(legacy_read && legacy_read->enabled && legacy_read->local_hour == 5 &&
                    legacy_read->revision == 9 && app::serialize_scheduler_configuration(*legacy_read) == legacy_text,
                "legacy saved enabled/time/revision preserved; no silent rewrite");
        auto alternate = configuration;
        alternate.backup_destination = u8"D:\\SFX Backup\\猫"; alternate.retained_backups = 3;
        const auto alternate_read = app::parse_scheduler_configuration(app::serialize_scheduler_configuration(alternate));
        require(alternate_read && alternate_read->backup_destination == alternate.backup_destination &&
                    alternate_read->retained_backups == 3, "saved alternate Unicode destination and retention round trip");
        for (const auto* unsafe : {"C:\\SFX\\Bin\\Backup", "C:\\", "C:\\a\\..\\b", "C:\\CON\\b",
                "C:\\a:stream", "C:\\a.\\b", "C:\\a \\b", "C:\\A~1\\b", "\\\\server\\backup", "C:/backup"})
            require(!app::valid_backup_destination_text(unsafe), "unsafe destination syntax refused");
        for (const auto count : {0U, 366U, 0xffffffffU}) {
            auto invalid = configuration; invalid.retained_backups = count;
            require(app::serialize_scheduler_configuration(invalid).empty(), "retention must be finite and bounded");
        }
        configuration.enabled = true; // Remaining legacy scheduler behavior controls.

        app::SchedulerState initial;
        app::RuntimeScheduler scheduler(configuration, initial, hourly());
        require(scheduler.recover_on_start(1000) &&
                    scheduler.state().next_run_utc == 4600,
                "first start calculates one future run");
        require(!scheduler.begin_due(4599, run_id(1)),
                "job not eligible before absolute next-run time");
        const auto scheduled = scheduler.begin_due(4600, run_id(2));
        require(scheduled && scheduled->scheduled &&
                    scheduler.state().last_result == app::ScheduledJobResult::running,
                "due job begins with stable typed identity");
        require(!scheduler.begin_manual(4600, run_id(3)),
                "same job cannot overlap manual and scheduled runs");
        const auto active_text = app::serialize_scheduler_state(scheduler.state());
        const auto active_state = app::parse_scheduler_state(active_text);
        require(active_state && active_state->active &&
                    active_state->active_run_id == scheduled->run_id,
                "active state persists before work begins");

        app::RuntimeScheduler restarted(configuration, *active_state, hourly());
        require(restarted.recover_on_start(5000) && !restarted.state().active &&
                    restarted.state().last_result == app::ScheduledJobResult::interrupted &&
                    restarted.state().history.size() == 1 &&
                    restarted.state().next_run_utc == 8600,
                "restart marks interrupted work and skips retry/missed occurrence");
        const auto next = restarted.begin_due(8600, run_id(4));
        require(next && restarted.complete(*next, 8610,
                    app::ScheduledJobResult::success,
                    u8"C:\\SFX\\Backup\\scheduled-猫.zip") &&
                    restarted.state().next_run_utc == 12210,
                "completion records Unicode result and schedules after finish");
        const auto persisted = app::serialize_scheduler_state(restarted.state());
        const auto parsed_state = app::parse_scheduler_state(persisted);
        require(parsed_state && parsed_state->last_result ==
                    app::ScheduledJobResult::success &&
                    parsed_state->last_detail == u8"C:\\SFX\\Backup\\scheduled-猫.zip",
                "completed state/history canonical round trip");

        const auto manual = restarted.begin_manual(9000, run_id(5));
        require(manual && !manual->scheduled,
                "manual run is available while scheduled job is enabled");
        app::RuntimeScheduler rezoned(restarted.configuration(), restarted.state(),
            [](const std::int64_t after, const std::uint8_t,
               const std::uint8_t) -> std::optional<std::int64_t> {
                return after + 7200;
            });
        require(rezoned.recompute_next(9001) && rezoned.state().active &&
                    rezoned.state().active_run_id == manual->run_id &&
                    rezoned.state().last_result == app::ScheduledJobResult::running &&
                    rezoned.state().history.size() == restarted.state().history.size() &&
                    rezoned.state().next_run_utc == 16201,
                "time-zone recompute preserves a running job and changes only future eligibility");
        auto disabled = configuration;
        disabled.revision = 2;
        disabled.enabled = false;
        require(restarted.reconfigure(disabled, 9001) &&
                    restarted.state().next_run_utc == 0,
                "new configuration applies to newly eligible work while run remains active");
        require(restarted.complete(*manual, 9010, app::ScheduledJobResult::success,
                                   "manual backup") &&
                    restarted.state().next_run_utc == 0,
                "captured manual run finishes under disabled replacement schedule");

        auto live_configuration = configuration;
        live_configuration.backup_destination = "C:\\Private\\Old";
        live_configuration.retained_backups = 12;
        app::RuntimeScheduler live_scheduler(live_configuration, initial, hourly());
        require(live_scheduler.recover_on_start(1000), "live destination fixture starts");
        const auto original_run = live_scheduler.begin_due(4600, run_id(80));
        require(original_run && original_run->backup_destination == "C:\\Private\\Old" &&
                    original_run->retained_backups == 12, "started backup captures original folder and retention");
        auto new_destination = live_configuration;
        ++new_destination.revision;
        new_destination.backup_destination = "C:\\Private\\New";
        const auto original_state = live_scheduler.state();
        require(live_scheduler.reconfigure(new_destination, 4601) &&
                    live_scheduler.state().active_run_id == original_state.active_run_id &&
                    live_scheduler.state().active_started_utc == original_state.active_started_utc &&
                    original_run->backup_destination == "C:\\Private\\Old" &&
                    original_run->retained_backups == 12, "live destination edit leaves active captured backup unchanged");
        require(!live_scheduler.begin_manual(4602, run_id(81)), "live destination edit does not permit overlapping jobs");
        require(live_scheduler.complete(*original_run, 4610, app::ScheduledJobResult::success, "original folder") &&
                    live_scheduler.state().history.back().detail == "original folder",
                    "original captured run finishes under replacement configuration");
        const auto future_run = live_scheduler.begin_manual(4611, run_id(82));
        require(future_run && future_run->backup_destination == "C:\\Private\\New" &&
                    future_run->retained_backups == 12, "future backup uses new folder without changing retention");
        require(live_scheduler.complete(*future_run, 4620, app::ScheduledJobResult::success, "new folder"),
                    "new destination backup completes normally");
        new_destination.revision++;
        new_destination.backup_destination.clear();
        require(live_scheduler.reconfigure(new_destination, 4621), "live explicit default selection saves");
        const auto default_run = live_scheduler.begin_manual(4622, run_id(83));
        require(default_run && default_run->backup_destination.empty(), "future default run captures installation-relative destination");

        for (unsigned index = 0; index < 40; ++index) {
            const auto run = restarted.begin_manual(10000 + index * 2, run_id(index));
            require(run && restarted.complete(*run, 10001 + index * 2,
                                               app::ScheduledJobResult::success,
                                               "bounded history"),
                    "bounded manual history run");
        }
        require(restarted.state().history.size() == app::kSchedulerHistoryLimit,
                "history drops oldest records at exact bound");
        require(app::parse_scheduler_state(
                    app::serialize_scheduler_state(restarted.state())).has_value(),
                "maximum bounded history persists and reloads");

        app::SchedulerState clock_state;
        app::RuntimeScheduler clock_scheduler(configuration, clock_state, hourly());
        require(clock_scheduler.recover_on_start(20000),
                "clock-jump fixture initializes");
        const auto clock_run = clock_scheduler.begin_manual(21000, run_id(5));
        require(clock_run && clock_scheduler.complete(*clock_run, 20500,
                    app::ScheduledJobResult::success, "clock moved backward") &&
                    clock_scheduler.state().last_run_utc == 21000,
                "backward wall-clock correction retains valid ordered run history");
        auto interrupted = clock_scheduler.state();
        interrupted.active = true;
        interrupted.active_scheduled = true;
        interrupted.active_run_id = run_id(4);
        interrupted.active_started_utc = 22000;
        interrupted.last_run_utc = 22000;
        interrupted.last_result = app::ScheduledJobResult::running;
        interrupted.last_detail = "running before clock correction";
        app::RuntimeScheduler recovered(configuration, interrupted, hourly());
        require(recovered.recover_on_start(21500) &&
                    recovered.state().last_result == app::ScheduledJobResult::interrupted &&
                    recovered.state().last_run_utc == 22000,
                "restart during backward correction records interruption without corrupt time ordering");

        for(const auto* job:{app::kMessageRetentionJobId,app::kMessageIntegrityJobId}){
            auto c=app::default_message_scheduler_configuration(job);auto s=app::default_message_scheduler_state(job);
            const auto encoded=app::serialize_scheduler_configuration(c);const auto decoded=app::parse_scheduler_configuration(encoded);
            require(decoded && decoded->enabled && decoded->local_hour==0 && decoded->local_minute==(c.job_id==app::kMessageIntegrityJobId?30:0) && decoded->local_weekday==c.local_weekday,"maintenance defaults: daily midnight and Sunday0030; backup independent");
            require(app::parse_scheduler_state(app::serialize_scheduler_state(s)).has_value(),"maintenance state2 strict roundtrip");
            require(!app::parse_scheduler_configuration(encoded+"unexpected=true\n"),"maintenance closed document rejects unknown fields");
            auto bad=c;bad.backup_destination="C:\\a\\backup";require(!app::valid_scheduler_configuration(bad),"maintenance cannot redirect backup or own another pool");
            bad=c;bad.local_weekday=std::uint8_t{7};require(!app::valid_scheduler_configuration(bad),"weekday bound closed");
            bad=c;bad.enabled=false;bad.local_hour=23;bad.local_minute=59;bad.revision=17;
            const auto saved=app::parse_scheduler_configuration(app::serialize_scheduler_configuration(bad));require(saved && !saved->enabled && saved->local_hour==23 && saved->local_minute==59 && saved->revision==17,"saved maintenance Off/custom choice preserved");
            std::int64_t observed_consumed{};
            const app::NextMaintenanceRun next_job=[&](std::int64_t after,std::uint8_t h,std::uint8_t m,std::optional<std::uint8_t> day,std::int64_t consumed)->std::optional<std::int64_t>{
                observed_consumed=consumed;if(h>23 || m>59 || day!=c.local_weekday)return std::nullopt;
                auto candidate=(after/86400+1)*86400+h*3600+m*60;
                if(candidate/86400==consumed/86400)candidate+=86400;
                if(day)candidate+=6*86400;
                return candidate;
            };
            app::RuntimeScheduler maintenance(c,s,hourly(),next_job);require(maintenance.recover_on_start(1000) && maintenance.state().next_run_utc>1000,"maintenance starts with only future eligibility, no purge");
            const auto due=maintenance.state().next_run_utc;const auto run=maintenance.begin_due(due,run_id(1));
            require(run && maintenance.state().last_consumed_slot_utc==due && !maintenance.begin_manual(due,run_id(2)),"due maintenance captures slot, prevents overlap");
            if(c.job_id==app::kMessageIntegrityJobId)require(maintenance.state().requires_fresh_check,"integrity guard persisted before checking");
            const auto pending=app::parse_scheduler_state(app::serialize_scheduler_state(maintenance.state()));require(pending && pending->active && pending->last_consumed_slot_utc==due,"uncertain check/run slot preserved");
            app::RuntimeScheduler recovered_job(c,*pending,hourly(),next_job);
            require(recovered_job.recover_on_start(due+5) && recovered_job.state().last_result==app::ScheduledJobResult::interrupted && observed_consumed==due,"missed/interrupted maintenance not retried; consumed slot reaches civil rule");
            if(c.job_id==app::kMessageIntegrityJobId)require(recovered_job.state().requires_fresh_check,"restart cannot turn incomplete check into clearance");
            require(maintenance.complete(*run,due+10,app::ScheduledJobResult::partial,"actual earlier committed batches retained") && observed_consumed==due,"partial result schedules next normal cycle, no immediate retry");
            require(maintenance.recompute_next(due-3600) && maintenance.state().next_run_utc>due,"backwards wall clock cannot replay consumed maintenance slot");
            const auto manual_job=maintenance.begin_manual(due-3600,run_id(3));require(manual_job && maintenance.complete(*manual_job,due-3590,app::ScheduledJobResult::skipped,"held storage"),"manual maintenance remains explicit, skipped distinct from success");
            require(app::parse_scheduler_state(app::serialize_scheduler_state(maintenance.state())).has_value(),"partial/skipped history roundtrip");
        }
        std::cout << "SCHEDULER PASS checks=" << checks
                  << " failed=0 skipped=0; persistence, non-overlap, restart, "
                     "clock eligibility, live reconfiguration, manual run and history\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SCHEDULER FAIL checks=" << checks
                  << " diagnostic=" << error.what() << '\n';
        return 1;
    }
}
