#include "sfx/application/chat_notice.hpp"
#pragma once

#include "alpha_telnet_listener.hpp"
#include "alpha_botgate_feed.hpp"
#include "alpha_ftn_engine.hpp"
#include "external_event_runner.hpp"

#include "sfx/adapters/authentication/sodium_authentication.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/authentication_service.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/lifecycle.hpp"
#include "sfx/core/node.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/file_administration.hpp"

#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sfx::apps::runtime {

class AlphaRuntimeHost final {
public:
    explicit AlphaRuntimeHost(std::filesystem::path installation_root, AlphaCallerInterlock interlock = {},
        std::wstring service_owner_sid = {});
    ~AlphaRuntimeHost();
    AlphaRuntimeHost(const AlphaRuntimeHost&) = delete;
    AlphaRuntimeHost& operator=(const AlphaRuntimeHost&) = delete;

    application::AlphaControlResponse start();
    application::AlphaRuntimeStatus status();
    application::SoundPollSnapshot sound_poll(const application::SoundPollRequest&);
    application::Identifier128 runtime_epoch() const noexcept { return runtime_epoch_; }
    application::AlphaRuntimeMetricSnapshot metrics();
    std::optional<application::OperationalLogPage> log_page(application::OperationalLogCursor cursor);
    application::AlphaControlResponse bootstrap(
        application::AlphaBootstrapRequest& request);
    application::AlphaControlResponse command_node(
        const application::AlphaNodeCommandRequest& request);
    application::AlphaLocalSessionStartResult start_local_session(
        const application::AlphaLocalSessionStartRequest& request);
    application::AlphaControlResponse cancel_local_session(
        const application::AlphaLocalSessionCancelRequest& request);
    application::AlphaControlResponse stage_settings(
        const application::AlphaSettingsRequest& request);
    std::optional<application::AlphaConfigurationSnapshot> configuration_snapshot();
    application::BotGateFeedStatus botgate_feed_status();
    application::AlphaControlResponse update_botgate_feed();
    application::AlphaConfigurationUpdateResult update_configuration(
        const application::AlphaConfigurationUpdateRequest& request);
    application::LocalAccountSnapshot account_snapshot(const application::LocalAccountQuery& query);
    application::LocalAccountResult administer_account(application::LocalAccountRequest& request);
    application::LocalSecuritySnapshot security_snapshot(std::uint32_t offset = 0);
    application::LocalClassicInformationSnapshot classic_information_snapshot(std::int64_t after);
    application::LocalAccountResult change_classic_information(const application::ClassicInformationChange&);
    application::LocalMessageConfigurationSnapshot message_configuration_snapshot(const application::LocalMessageConfigurationQuery&);
    application::LocalMessageConfigurationResult change_message_configuration(const application::MessageConfigurationChange&);
    application::LocalQwkPolicyResult qwk_policy_snapshot();
    application::LocalFileConfigurationSnapshot file_configuration_snapshot(const application::LocalFileConfigurationQuery&);
    application::LocalFileConfigurationResult change_file_configuration(const application::FileConfigurationChange&);
    application::ChatConfigurationResult chat_configuration();
    application::FtnConfigurationResponse ftn_configuration(const application::FtnConfigurationRequest&);
    application::FtnConfigurationResponse ftn_hatch_catalog(const application::FtnConfigurationRequest&);
    AlphaFtnSnapshot ftn_status();
    application::FtnRuntimeResponse ftn_runtime(const application::FtnRuntimeRequest&);
    application::FtnHeldControlResponse ftn_held(const application::FtnHeldControlRequest&);
    bool ftn_poll(std::string_view network,std::string_view link,bool include_hold);
    application::ChatResult send_chat_notice(const application::ChatNoticeRequest&);
    application::ChatOperatorSnapshot chat_operator(const application::ChatOperatorRequest&);
    application::ChatConfigurationResult change_chat_configuration(const application::ChatConfiguration&);
    application::LocalDoorSnapshot door_configuration_snapshot(const application::LocalDoorQuery&);
    application::LocalDoorResult change_door_configuration(const application::DoorConfigurationChange&);
    application::LocalDoorSnapshot door_bulletin_snapshot(const application::LocalDoorBulletinQuery&);
    application::LocalDoorResult change_door_bulletin(const application::DoorBulletinChange&);
    application::AlphaControlResponse manual_clamav_notice(const application::ManualClamAvNotice&);
    application::FileManagementJobStatus start_file_management(const application::FileManagementTask&);
    application::FileManagementJobStatus file_management_status(application::Identifier128);
    application::FileManagementJobStatus cancel_file_management(application::Identifier128);
    application::FileManagementReadSnapshot query_file_management(const application::FileManagementReadRequest&);
    application::LocalQwkPolicyResult change_qwk_policy(const application::QwkPolicyChange&);
    application::MessageMaintenanceStatus message_maintenance_status(application::MessageMaintenanceKind);
    application::AlphaControlResponse configure_message_schedule(const application::MessageMaintenanceScheduleRequest&);
    application::AlphaControlResponse run_message_maintenance_now(application::MessageMaintenanceKind);
    application::LocalAccountResult change_security(const application::LocalSecurityChange& request);
    application::AlphaControlResponse clear_throttle(const application::LocalThrottleClearRequest& request);
    application::AlphaControlResponse configure_scheduler(
        const application::AlphaSchedulerSettingsRequest& request);
    application::AlphaControlResponse run_scheduler_now(
        const application::AlphaSchedulerRunRequest& request);
    application::AlphaControlResponse shutdown();
    application::AlphaControlResponse command_lifecycle(
        const application::AlphaLifecycleCommandRequest& request);

private:
    class RuntimeRevocationSink final : public application::ISessionRevocationSink {
    public:
        bool revoke(const application::SessionRevocationCommand& command) override;
        bool refresh_account_access(application::AccountId account_id) override;
        void attach(const std::shared_ptr<AlphaTelnetListener>& listener);
    private:
        std::mutex mutex;
        std::weak_ptr<AlphaTelnetListener> target;
        bool bootstrap_only{true};
    };

    void refresh_state_locked();
    bool ensure_ssh_backend_locked();
    bool start_listener_locked(bool fault_on_failure = true);
    bool retire_listener_locked();
    bool activate_saved_configuration_locked(
        const application::OperationalConfiguration& candidate,
        std::string& diagnostic);
    void activate_pending_configuration_if_safe_locked();
    application::AlphaControlResponse fail_recovery_locked(std::string diagnostic);
    application::AuditContext account_audit_locked(
        application::AuditReasonCode reason,
        std::optional<application::AccountId> actor = std::nullopt);
    application::AuthenticationAuditContext authentication_audit_locked(
        std::optional<application::AccountId> actor = std::nullopt);
    std::int64_t now_utc_seconds() const noexcept;
    bool initialize_scheduler_locked();
    void initialize_ftn_locked();
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
    void scheduler_loop() noexcept;
    void stop_scheduler() noexcept;
    std::pair<application::ScheduledJobResult, std::string> execute_scheduler_backup(
        const application::ScheduledRun& run) noexcept;
    application::AlphaSchedulerStatus scheduler_status() const;
    void lifecycle_loop() noexcept;
    bool initialize_clamav_locked();
    void clamav_loop() noexcept;
    void stop_clamav() noexcept;

    const std::filesystem::path installation_root_;
    const std::wstring service_owner_sid_;
    const AlphaCallerInterlock caller_interlock_;
    std::wstring owner_sid_;
    std::mutex mutex_;
    application::RuntimeLifecycle lifecycle_;
    application::AlphaRuntimePresentationState presentation_state_{
        application::AlphaRuntimePresentationState::starting};
    std::string diagnostic_code_{"starting"};
    std::optional<application::OperationalConfiguration> configuration_;
    bool fresh_setup_allowed_{false};
    std::optional<application::SecurityPolicyRecord> security_policy_;
    application::AlphaRuntimeStatistics statistics_;
    application::AdministrationReportingCache administration_reporting_;
    platform::win32::RuntimeInstanceGuard instance_guard_;
    std::unique_ptr<platform::win32::WindowsLocalStoragePathPolicy> path_policy_;
    std::unique_ptr<adapters::storage::SqliteStorage> storage_;
    application::LocalDoorSnapshotCache door_snapshot_cache_;
    application::LocalDoorBulletinSnapshotCache door_bulletin_snapshot_cache_;
    std::unique_ptr<application::FtnConfigurationPublisher> ftn_publisher_;
    std::unique_ptr<application::FtnReadOnlyPublisher> ftn_hatch_publisher_;
    std::optional<application::FtnConfiguration> ftn_snapshot_;
    std::shared_ptr<AlphaFtnEngine> ftn_engine_;
    std::string ftn_initialization_error_{"ftn_runtime_not_ready"};
    std::shared_ptr<platform::win32::WindowsFileAdministration> file_administration_;
    adapters::authentication::SodiumPasswordKdf kdf_;
    adapters::authentication::PinnedSecListsBlocklist password_blocklist_;
    RuntimeRevocationSink revocations_;
    std::unique_ptr<application::AuthenticationSecurityService> authentication_;
    platform::win32::WindowsSecureIdentifierGenerator identifiers_;
    const application::Identifier128 runtime_epoch_;
    std::shared_ptr<application::EventSoundStream> sounds_;
    bool starting_sounds_{};
    std::uint64_t status_sequence_{0};
    std::optional<std::uint64_t> started_tick_;
    std::array<std::uint64_t, application::kAlphaNodePoolCapacity> previous_generations_{};
    std::shared_ptr<const AlphaSshBackend> ssh_backend_;
    std::shared_ptr<AlphaTelnetListener> telnet_listener_;
    std::unique_ptr<ExternalEventRunner> external_events_;
    bool external_events_started_{};
    std::shared_ptr<AlphaOperationalJournal> journal_;
    std::shared_ptr<AlphaBotGateFeed> botgate_feed_;
    std::shared_ptr<class AlphaDurableJournal> durable_journal_;
    std::vector<application::AlphaOperationalLogEntry> retained_logs_;
    application::NodeEnablement nodes_enabled_{application::default_node_enablement()};
    std::optional<application::AlphaSettingsRequest> pending_settings_;
    std::optional<application::OperationalConfiguration> pending_configuration_;
    application::ControlPanelSqliteRateTracker sqlite_rate_tracker_;
    mutable std::mutex scheduler_mutex_;
    std::condition_variable scheduler_wake_;
    std::unique_ptr<application::RuntimeScheduler> scheduler_;
    std::array<std::unique_ptr<application::RuntimeScheduler>,2> message_schedulers_;
    std::array<std::string,2> message_scheduler_diagnostics_{{"message_jobs_not_initialized","message_jobs_not_initialized"}};
    std::optional<application::ScheduledRun> pending_scheduler_run_;
    std::thread scheduler_thread_;
    bool scheduler_stop_{false};
    std::atomic_bool scheduler_cancel_{false};
    std::string scheduler_diagnostic_{"scheduler_not_initialized"};
    std::string scheduler_source_generation_{"unknown"};
    std::atomic_bool scheduler_paused_{false};
    std::condition_variable lifecycle_wake_;
    std::thread lifecycle_thread_;
    bool lifecycle_worker_stop_{false}; // guarded by mutex_
    std::thread clamav_thread_;
    std::atomic_bool clamav_stop_{false};
    std::mutex clamav_mutex_;
    std::condition_variable clamav_wake_;
    std::wstring clamav_installation_id_;
    std::mutex clamav_notice_mutex_;
    std::vector<application::ManualClamAvNotice> clamav_notices_;
};

int run_runtime_process(void* inherited_owner_handle);

}  // namespace sfx::apps::runtime
