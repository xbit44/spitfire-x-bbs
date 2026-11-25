#pragma once
#include "linux_caller_engine.hpp"
#include "linux_clamav_worker.hpp"
#include "linux_ftn_engine.hpp"
#include "linux_runtime_jobs.hpp"
#include "external_event_runner.hpp"
#include "sfx/application/ftn_configuration_transport.hpp"
#include "sfx/application/ftn_held_control.hpp"
#include "sfx/application/ftn_hatch_catalog.hpp"
#include "sfx/platform/linux/file_administration.hpp"
#include "sfx/platform/linux/file_configuration.hpp"
#include "sfx/platform/linux/bbs_health.hpp"
#include "sfx/platform/linux/core_restore.hpp"
#include "sfx/platform/linux/file_backup.hpp"
#include "sfx/application/message_deletion.hpp"
#include <condition_variable>

namespace sfx::apps::runtime {
struct LinuxWebAuthenticationResult {bool verified{};std::uint32_t retry_seconds{};std::uint64_t epoch{};};
struct LinuxStopResult {
    application::AlphaControlResponse response;
    std::uint16_t callers{};
    std::uint64_t confirmation{};
};
struct LinuxLifecycleSnapshot {
    std::uint64_t run_sequence{},drain_remaining_milliseconds{};
    bool draining{};
    std::string diagnostic;
};
struct LinuxCompactionSnapshot {
    std::uint64_t operation{};
    bool active{},completed{},backup_verified{},reopened{};
    std::string stage,detail;
    std::filesystem::path backup,temporary;
    adapters::storage::SqliteCompactionResult result;
};
struct LinuxFileCountRulesSnapshot {
    application::LocalAccountResultCode code{application::LocalAccountResultCode::unavailable};
    application::FilePolicy policy;
    std::vector<application::SecurityLevelRecord> levels;
    std::vector<application::FileSecurityRule> rules;
};
enum class LinuxOfflineKind {health,prepare,restore,message_deletion,file_group_deletion,file_contents,ftn_history};
struct LinuxContentDestination {
    std::int64_t area{};
    std::filesystem::path path;
    std::uint64_t missing{},identical{},conflicts{};
};
struct LinuxOfflineSnapshot {
    std::uint64_t operation{};
    LinuxOfflineKind kind{LinuxOfflineKind::health};
    bool active{},awaiting_confirmation{},completed{},success{},ready{},management_reopened{};
    std::string stage,detail,diagnostic,confirmation;
    std::filesystem::path archive;
    std::uint64_t archive_files{};
    platform::linux_host::BbsHealthReport health;
    platform::linux_host::LinuxCoreRestoreResult restored;
    application::FtnHistoryRecoveryPreview ftn_history;
    application::FtnHistoryRecoveryResult ftn_history_result;
    bool awaiting_backup_choice{},backup_verified{},deletion_reconcile{};
    application::MessageDeletionTarget deletion_target;
    application::MessageDeletionPreview deletion_preview;
    application::StorageResult<application::MessageDeletionResult> deletion_result;
    std::filesystem::path deletion_backup;
    application::FileConfigurationChange file_deletion_change;
    application::LocalFileConfigurationResult file_deletion_preview,file_deletion_result;
    bool file_deletion_resume{},file_deletion_started{};
    bool awaiting_area_selection{};
    std::vector<archive::ContentArea> content_areas;
    std::vector<LinuxContentDestination> content_destinations;
    platform::linux_host::FileContentRestoreResult contents_restored;
};
struct LinuxSecurityRemovalPreview {
    application::LocalAccountResultCode code{application::LocalAccountResultCode::unavailable};
    application::LocalSecuritySnapshot security;
    std::vector<std::pair<application::AccountId,std::string>> accounts;
    std::string affected_hash;
    std::size_t total{};
};
struct LinuxBotGateRuleFile {
    unsigned file{};
    bool available{},editable{};
    std::string text,revision,detail;
};
// Serializes management transactions while caller threads keep their own
// lifetime. Browser/console closure is not a BBS stop operation.
class LinuxRuntimeHost final:public application::ISessionRevocationSink {
public:
    LinuxRuntimeHost(std::filesystem::path,application::IClock&,
        std::function<bool()> verified_starter_doors);
    ~LinuxRuntimeHost();
    application::AlphaControlResponse open();
    application::AlphaControlResponse start_bbs();
    LinuxStopResult stop_bbs(std::optional<std::uint64_t> confirmation={});
    LinuxStopResult restart_bbs(std::uint64_t expected_run,std::optional<std::uint64_t> confirmation={});
    application::AlphaControlResponse command_lifecycle(const application::AlphaLifecycleCommandRequest&,std::optional<std::uint64_t> expected_run={});
    LinuxLifecycleSnapshot lifecycle_controls();
    void tick_controls();
    application::AlphaControlResponse command_node(const application::AlphaNodeCommandRequest&,std::optional<application::NodeAssignmentIdentity> observed={});
    std::optional<application::AlphaNodeStatus> view_node(application::NodeAssignmentIdentity);
    void cancel_stop();
    bool bbs_owned()const;
    bool management_owned() const;
    bool management_work_active();
    application::AlphaControlResponse shutdown();
    application::AlphaRuntimeStatus status();
    application::AlphaRuntimeStatistics persisted_statistics();
    application::BotGateFeedStatus botgate_status();
    LinuxBotGateRuleFile botgate_rule_file(unsigned);
    application::AlphaControlResponse save_botgate_rule_file(unsigned,std::string_view revision,std::string_view text);
    application::AlphaControlResponse update_botgate_feed();
    application::AlphaRuntimeMetricSnapshot monitor_runtime();
    application::AlphaControlResponse bootstrap(application::AlphaBootstrapRequest&);
    application::AlphaControlResponse prepare_copied_file_bindings(const std::function<bool()>& cancelled={});
    std::optional<application::AlphaConfigurationSnapshot> configuration();
    application::AlphaConfigurationUpdateResult save_configuration(const application::AlphaConfigurationUpdateRequest&);
    application::LocalMessageConfigurationSnapshot message_configuration_snapshot(const application::LocalMessageConfigurationQuery&);
    application::LocalMessageConfigurationResult change_message_configuration(const application::MessageConfigurationChange&);
    application::LocalDoorSnapshot door_configuration_snapshot(const application::LocalDoorQuery&);
    std::optional<application::DoorCatalog> complete_door_catalog();
    application::LocalDoorResult change_door_configuration(const application::DoorConfigurationChange&);
    application::LocalDoorSnapshot door_bulletin_snapshot(const application::LocalDoorBulletinQuery&);
    std::optional<application::DoorBulletinCatalog> complete_door_bulletin_catalog();
    application::LocalDoorResult change_door_bulletin(const application::DoorBulletinChange&);
    application::DoorBulletinPreview preview_door_bulletin(application::DoorBulletinId,bool ansi,std::int64_t expected_revision=0);
    application::LocalDoorResult remove_door_with_placements(const application::DoorConfigurationChange&);
    application::LocalQwkPolicyResult qwk_policy_snapshot();
    application::LocalQwkPolicyResult change_qwk_policy(const application::QwkPolicyChange&);
    application::LocalClassicInformationSnapshot classic_information_snapshot(std::int64_t after);
    application::LocalAccountResult change_classic_information(const application::ClassicInformationChange&);
    application::LocalAccountSnapshot account_snapshot(const application::LocalAccountQuery&);
    application::LocalAccountResult administer_account(application::LocalAccountRequest&);
    LinuxWebAuthenticationResult authenticate_web(std::string& password,std::string_view source_ip);
    std::uint64_t web_authentication_epoch()const noexcept{return web_authentication_epoch_.load();}
    application::AlphaControlResponse clear_throttle(const application::LocalThrottleClearRequest&);
    application::LocalSecuritySnapshot security_snapshot(std::uint32_t offset);
    application::LocalSecuritySnapshot complete_security_snapshot();
    LinuxSecurityRemovalPreview security_removal_preview(std::uint16_t,std::uint32_t);
    application::LocalAccountResult change_security(const application::LocalSecurityChange&);
    application::ChatConfigurationResult chat_configuration();
    application::ChatConfigurationResult change_chat_configuration(const application::ChatConfiguration&);
    application::ChatOperatorSnapshot chat_operator(const application::ChatOperatorRequest&);
    application::ChatResult send_chat_notice(const application::ChatNoticeRequest&);
    std::optional<application::OperationalLogPage> log_page(application::OperationalLogCursor);
    std::optional<application::LocalControlFrame> control(application::LocalControlFrame&);
    bool revoke(const application::SessionRevocationCommand&)override;
    bool refresh_account_access(application::AccountId)override;
    bool disconnect(application::NodeAssignmentIdentity);
    application::AlphaLocalSessionStartResult reserve_local_node(std::uint16_t);
    bool cancel_local_node(application::NodeAssignmentIdentity);
    LinuxBrowserNodeStart open_browser_node(std::uint16_t);
    LinuxClamAvWorkerSnapshot clamav_status();
    std::optional<std::string> clamav_update_log();
    std::uint64_t request_clamav_update();
    bool cancel_clamav_update(std::uint64_t);
    void pause_clamav_automatic(bool);
    application::LocalFileConfigurationSnapshot file_configuration_snapshot(const application::LocalFileConfigurationQuery&);
    LinuxFileCountRulesSnapshot file_count_rules();
    application::LocalFileConfigurationResult change_file_configuration(const application::FileConfigurationChange&);
    application::FileManagementReadSnapshot query_file_management(const application::FileManagementReadRequest&);
    application::FileManagementJobStatus start_file_management(const application::FileManagementTask&);
    application::FileManagementJobStatus file_management_status(application::Identifier128);
    application::FileManagementJobStatus cancel_file_management(application::Identifier128);
    std::optional<application::FtnHatchCatalog> file_hatch_choices();
    application::FtnConfigurationResponse ftn_configuration(const application::FtnConfigurationRequest&);
    application::FtnConfigurationResponse ftn_hatch_catalog(const application::FtnConfigurationRequest&);
    application::FtnRuntimeResponse ftn_runtime(const application::FtnRuntimeRequest&);
    application::FtnRuntimeResponse ftn_runtime_checked(const application::FtnRuntimeRequest&,application::Identifier128,std::int64_t);
    application::FtnHeldControlResponse ftn_held(const application::FtnHeldControlRequest&);
    application::AlphaControlResponse configure_scheduler(const application::AlphaSchedulerSettingsRequest&,std::optional<std::uint32_t> retained={});
    LinuxScheduledJobSnapshot scheduled_job(std::string_view);
    std::uint64_t request_compaction();
    LinuxCompactionSnapshot compaction_status();
    bool cancel_compaction(std::uint64_t);
    std::uint64_t request_offline(LinuxOfflineKind,const std::filesystem::path& archive={},std::function<void()> before_restore={});
    LinuxOfflineSnapshot offline_status();
    bool select_restore_contents(std::uint64_t,std::string_view,const std::vector<std::int64_t>&);
    bool cancel_offline(std::uint64_t);
    bool confirm_restore(std::uint64_t,std::string_view);
    std::uint64_t request_message_deletion(const application::MessageDeletionTarget&);
    bool choose_deletion_backup(std::uint64_t,std::string_view,bool);
    bool confirm_message_deletion(std::uint64_t,std::string_view);
    std::uint64_t request_file_group_deletion(application::FileGroupId,std::int64_t revision,std::int64_t catalog_revision);
    bool confirm_file_group_deletion(std::uint64_t,std::string_view,bool delete_disk_files);
    application::AlphaControlResponse run_scheduler_now(const application::AlphaSchedulerRunRequest&);
    application::SoundPollSnapshot sound_poll(const application::SoundPollRequest&);
    application::MessageMaintenanceStatus message_maintenance_status(application::MessageMaintenanceKind);
    application::AlphaControlResponse configure_message_schedule(const application::MessageMaintenanceScheduleRequest&);
    application::AlphaControlResponse run_message_maintenance_now(application::MessageMaintenanceKind);
private:
    std::shared_ptr<application::EventSoundStream> sounds_;
    application::Identifier128 sound_epoch_;
    std::atomic<std::uint64_t> web_authentication_epoch_{1};
    const std::filesystem::path root_;
    application::IClock& clock_;
    mutable std::recursive_mutex operations_;
    std::mutex compaction_lifecycle_,compaction_state_mutex_;
    std::thread compaction_thread_;
    std::atomic_bool compaction_cancel_{false};
    bool compaction_shutdown_{};
    LinuxCompactionSnapshot compaction_;
    void compact_worker() noexcept;
    std::mutex offline_state_mutex_;
    std::condition_variable offline_confirmation_;
    std::thread offline_thread_;
    std::atomic_bool offline_cancel_{false};
    bool offline_confirmed_{};
    std::optional<std::vector<std::int64_t>> content_selection_;
    std::optional<bool> deletion_backup_choice_;
    LinuxOfflineSnapshot offline_;
    std::function<void()> before_restore_;
    void offline_worker() noexcept;
    void message_deletion_worker() noexcept;
    void file_group_deletion_worker() noexcept;
    application::AlphaControlResponse close_stopped_management();
    LinuxRuntimeStore store_;
    std::shared_ptr<LinuxCallerEngine> engine_;
    std::unique_ptr<ExternalEventRunner> external_events_;
    std::optional<application::OperationalConfiguration> active_configuration_,pending_configuration_;
    application::AlphaConfigurationActivation configuration_activation_{application::AlphaConfigurationActivation::saved_next_start};
    std::string configuration_detail_{"BBS stopped"};
    std::shared_ptr<LinuxFtnEngine> ftn_engine_;
    std::unique_ptr<application::FtnConfigurationPublisher> ftn_publisher_;
    std::unique_ptr<application::FtnReadOnlyPublisher> ftn_hatch_publisher_;
    std::string ftn_initialization_error_{"ftn_bbs_stopped"};
    application::Identifier128 epoch_;
    std::uint64_t sequence_{},ticket_sequence_{},pending_ticket_{};
    std::vector<application::NodeAssignmentIdentity> pending_callers_;
    bool pending_restart_{};
    std::uint64_t run_sequence_{};
    std::optional<std::uint64_t> bbs_started_boot_ms_;
    application::ControlPanelSqliteRateTracker sqlite_rates_;
    application::ControlPanelSqliteMetricSnapshot sqlite_sample_;
    std::optional<std::uint64_t> sqlite_sample_ms_;
    std::uint64_t sqlite_sample_generation_{};
    std::optional<std::uint64_t> drain_deadline_;
    std::string lifecycle_diagnostic_;
    application::NodeEnablement node_enablement_{application::default_node_enablement()};
    LinuxStopResult stop_request(std::optional<std::uint64_t>,bool restart);
    LinuxStopResult stop_owned_bbs();
    application::AlphaStatusSnapshotCache status_cache_;
    application::LocalDoorSnapshotCache door_snapshot_cache_;
    application::LocalDoorBulletinSnapshotCache door_bulletin_snapshot_cache_;
    std::shared_ptr<AlphaOperationalJournal> journal_;
    std::shared_ptr<AlphaDurableJournal> durable_;
    std::unique_ptr<LinuxClamAvWorker> clamav_;
    std::unique_ptr<LinuxRuntimeJobs> jobs_;
    std::unique_ptr<platform::linux_host::LinuxFileAdministration> file_administration_;
    std::string journal_fault_;
    std::array<std::uint64_t,application::kAlphaNodePoolCapacity> generations_{};
    void retain_generations(const LinuxCallerListenerStatus&);
    bool ensure_journals();
    void ensure_clamav();
    void start_ftn();
    void stop_ftn();
    void ensure_jobs();
    void stop_jobs();
    bool store_ready() const noexcept;
    bool activate_configuration(const application::OperationalConfiguration&,const std::function<bool()>& publish);
    void apply_pending_configuration();
};
} // namespace sfx::apps::runtime
