#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {

inline constexpr std::size_t kSchedulerHistoryLimit = 32;
inline constexpr std::size_t kSchedulerDocumentMaximumBytes = 65536;
inline constexpr const char* kBackupJobId = "builtin.backup";
inline constexpr const char* kMessageRetentionJobId = "builtin.message-retention";
inline constexpr const char* kMessageIntegrityJobId = "builtin.message-integrity";

enum class ScheduledJobResult : std::uint8_t {
    never,
    running,
    success,
    failed,
    interrupted,
    partial,
    skipped,
};

struct SchedulerConfiguration final {
    std::uint32_t format_version{2};
    std::uint64_t revision{1};
    std::string job_id{kBackupJobId};
    bool enabled{false};
    std::uint8_t local_hour{2};
    std::uint8_t local_minute{0};
    // Empty is the installation-relative Backup/Scheduled default. A saved
    // nonempty destination is an absolute local folder, never a fallback list.
    std::string backup_destination;
    std::uint32_t retained_backups{10};
    // Format 3 is reserved for the two maintenance jobs. Sunday is zero;
    // retention has no weekday, integrity requires one. Backup v1/v2 unchanged.
    std::optional<std::uint8_t> local_weekday;
};

struct ScheduledJobHistory final {
    std::string run_id;
    std::int64_t started_utc{0};
    std::int64_t finished_utc{0};
    ScheduledJobResult result{ScheduledJobResult::never};
    std::string detail;
};

struct SchedulerState final {
    std::uint32_t format_version{1};
    std::string job_id{kBackupJobId};
    std::uint64_t configuration_revision{1};
    std::int64_t next_run_utc{0};
    bool active{false};
    bool active_scheduled{false};
    std::string active_run_id;
    std::int64_t active_started_utc{0};
    std::int64_t last_run_utc{0};
    ScheduledJobResult last_result{ScheduledJobResult::never};
    std::string last_detail;
    std::vector<ScheduledJobHistory> history;
    std::int64_t last_consumed_slot_utc{0};
    // Protected external check guard also survives a failure to write DB holds.
    // Only a fresh complete current check publication can release this guard.
    bool requires_fresh_check{false};
};

struct ScheduledRun final {
    std::string job_id;
    std::string run_id;
    std::uint64_t configuration_revision{0};
    std::int64_t started_utc{0};
    bool scheduled{false};
    bool consumes_due_run{false};
    std::string backup_destination;
    std::uint32_t retained_backups{10};
};

using NextDailyRun = std::function<std::optional<std::int64_t>(
    std::int64_t after_utc, std::uint8_t local_hour, std::uint8_t local_minute)>;
using NextMaintenanceRun = std::function<std::optional<std::int64_t>(
    std::int64_t after_utc, std::uint8_t local_hour, std::uint8_t local_minute,
    std::optional<std::uint8_t> local_weekday, std::int64_t consumed_slot_utc)>;

bool message_maintenance_job(std::string_view job) noexcept;
SchedulerConfiguration default_message_scheduler_configuration(std::string_view job);
SchedulerState default_message_scheduler_state(std::string_view job);

bool valid_scheduler_configuration(const SchedulerConfiguration& value) noexcept;
bool valid_backup_destination_text(std::string_view value) noexcept;
bool valid_scheduler_state(const SchedulerState& value) noexcept;
std::string serialize_scheduler_configuration(const SchedulerConfiguration& value);
std::optional<SchedulerConfiguration> parse_scheduler_configuration(std::string_view value);
std::string serialize_scheduler_state(const SchedulerState& value);
std::optional<SchedulerState> parse_scheduler_state(std::string_view value);

class RuntimeScheduler final {
public:
    RuntimeScheduler(SchedulerConfiguration configuration, SchedulerState state,
                     NextDailyRun next_daily_run, NextMaintenanceRun next_maintenance_run = {});

    bool recover_on_start(std::int64_t now_utc);
    bool recompute_next(std::int64_t now_utc);
    bool reconfigure(SchedulerConfiguration configuration, std::int64_t now_utc);
    std::optional<ScheduledRun> begin_due(std::int64_t now_utc, std::string run_id);
    std::optional<ScheduledRun> begin_manual(std::int64_t now_utc, std::string run_id);
    bool complete(const ScheduledRun& run, std::int64_t finished_utc,
                  ScheduledJobResult result, std::string detail);

    const SchedulerConfiguration& configuration() const noexcept { return configuration_; }
    const SchedulerState& state() const noexcept { return state_; }
    void replace_state(SchedulerState state);

private:
    std::optional<ScheduledRun> begin(std::int64_t now_utc, std::string run_id,
                                      bool scheduled, bool consumes_due_run);
    bool schedule_next(std::int64_t after_utc);
    void append_history(ScheduledJobHistory record);

    SchedulerConfiguration configuration_;
    SchedulerState state_;
    NextDailyRun next_daily_run_;
    NextMaintenanceRun next_maintenance_run_;
};

} // namespace sfx::application
