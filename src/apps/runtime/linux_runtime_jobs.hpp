#pragma once
#include "alpha_operational_journal.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/scheduler.hpp"
#include <atomic>
#include <condition_variable>
#include <thread>

namespace sfx::apps::runtime {
struct LinuxScheduledJobSnapshot {
    bool available{},paused{true},any_active{},queued{};
    application::SchedulerConfiguration configuration;
    application::SchedulerState state;
    std::string diagnostic;
};
// A single joined worker for backup and message jobs. The owning host keeps
// storage/journal alive until stop joins. Closing a browser does not stop it.
// Backup publication is an installation-native capability, never web input.
class LinuxRuntimeJobs final {
public:
    using Outcome=std::pair<application::ScheduledJobResult,std::string>;
    using Backup=std::function<Outcome(const application::ScheduledRun&,const std::function<bool()>&)>;
    using ValidateDestination=std::function<std::shared_ptr<void>(const std::string&)>;
    LinuxRuntimeJobs(std::filesystem::path,adapters::storage::SqliteStorage&,application::IIdentifierGenerator&,
        application::IClock&,std::string zone,std::shared_ptr<AlphaOperationalJournal>,Backup,ValidateDestination);
    ~LinuxRuntimeJobs();
    bool start(bool paused=true);
    void stop() noexcept;
    void pause(bool);
    bool busy() const;
    bool reconfigure_time_zone(const std::string&,const std::function<bool()>& publish);
    application::AlphaSchedulerStatus scheduler_status() const;
    LinuxScheduledJobSnapshot scheduled_job(std::string_view) const;
    application::MessageMaintenanceStatus message_maintenance_status(application::MessageMaintenanceKind);
    application::AlphaControlResponse configure_scheduler(const application::AlphaSchedulerSettingsRequest&,std::optional<std::uint32_t> retained={});
    application::AlphaControlResponse run_scheduler_now(const application::AlphaSchedulerRunRequest&);
    application::AlphaControlResponse configure_message_schedule(const application::MessageMaintenanceScheduleRequest&);
    application::AlphaControlResponse run_message_maintenance_now(application::MessageMaintenanceKind);
private:
    const std::filesystem::path installation_root_;
    adapters::storage::SqliteStorage* storage_;
    application::IIdentifierGenerator& identifiers_;
    application::IClock& clock_;
    std::string zone_;
    std::shared_ptr<AlphaOperationalJournal> journal_;
    Backup backup_;
    ValidateDestination validate_destination_;
    std::mutex lifecycle_mutex_;
    mutable std::mutex scheduler_mutex_;
    std::condition_variable scheduler_wake_;
    std::unique_ptr<application::RuntimeScheduler> scheduler_;
    std::array<std::unique_ptr<application::RuntimeScheduler>,2> message_schedulers_;
    std::array<std::string,2> message_scheduler_diagnostics_{{"message_jobs_not_initialized","message_jobs_not_initialized"}};
    std::optional<application::ScheduledRun> pending_scheduler_run_;
    std::thread scheduler_thread_;
    bool scheduler_stop_{false},ready_{false};
    std::atomic_bool scheduler_cancel_{false},scheduler_paused_{true};
    std::string scheduler_diagnostic_{"scheduler_not_initialized"},diagnostic_code_;
    bool initialize_scheduler_locked();
    void initialize_message_schedulers_locked();
    bool scheduler_has_work() const;
    bool scheduler_has_work_locked() const;
    application::RuntimeScheduler* scheduler_for_job(std::string_view) const;
    struct MessageJobOutcome {
        application::ScheduledJobResult result{application::ScheduledJobResult::failed};
        std::string detail;
        std::optional<bool> requires_fresh_check;
    };
    MessageJobOutcome execute_scheduler_message(const application::ScheduledRun&) noexcept;
    Outcome execute_scheduler_backup(const application::ScheduledRun&) noexcept;
    void scheduler_loop() noexcept;
    void stop_scheduler() noexcept;
    std::int64_t now_utc_seconds() const noexcept;
};
} // namespace sfx::apps::runtime
