#pragma once
#include "sfx/application/message_maintenance.hpp"
#include "sfx/application/message_deletion.hpp"
#include "sfx/application/qwk_reply_storage.hpp"
#include "sfx/application/caller_journal.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sfx/application/account_storage.hpp"
#include "sfx/application/authentication_storage.hpp"
#include "sfx/application/phase3a.hpp"
#include "sfx/application/phase3a_storage.hpp"
#include "sfx/application/menu_storage.hpp"
#include "sfx/application/storage.hpp"
#include "sfx/application/classic_information_storage.hpp"
#include "sfx/application/message_storage.hpp"
#include "sfx/application/qwk_storage.hpp"
#include "sfx/application/file_storage.hpp"
#include "sfx/application/door_storage.hpp"
#include "sfx/application/chat_storage.hpp"
#include "sfx/application/ftn_storage.hpp"
#include "sfx/application/ftn_work.hpp"

namespace sfx::adapters::storage {

struct StorageLocationAssessment {
    application::StorageStatus status;
    std::filesystem::path canonical_directory;
};

class IStoragePathPolicy {
public:
    virtual ~IStoragePathPolicy() = default;
    virtual StorageLocationAssessment assess(const std::filesystem::path& directory) const = 0;
    virtual application::StorageStatus create_database_file_exclusive(
        const std::filesystem::path& path) const = 0;
    virtual application::StorageStatus protect_and_verify_database_file(
        const std::filesystem::path& path) const = 0;
};

// Qualification defaults only. Product defaults remain subject to representative
// Windows/runtime measurements and may be revised without changing correctness.
struct SqliteQueueSettings {
    std::size_t maximum_operations{4096};
    std::size_t maximum_bytes{8U * 1024U * 1024U};
    std::size_t dequeue_batch_limit{64};
    std::chrono::milliseconds dequeue_batch_window{5};
    std::chrono::milliseconds enqueue_wait{1000};
};

// Bounded qualification defaults, not permanent product constants.
struct SqliteBusySettings {
    std::chrono::milliseconds sqlite_timeout{250};
    std::chrono::milliseconds transaction_deadline{1000};
    std::uint32_t maximum_attempts{3};
};

// Measured maintenance defaults, not permanent product constants.
struct SqliteCheckpointSettings {
    std::chrono::seconds passive_interval{30};
    std::uint32_t frame_target{1024};
    std::uint64_t wal_warning_bytes{64U * 1024U * 1024U};
};

struct SqliteReaderSettings {
    std::size_t pool_size{1};
    std::size_t maximum_pending_operations{4096};
    std::chrono::milliseconds enqueue_wait{1000};
};

struct SqliteReaderPoolHealth {
    bool failed{false};
    std::size_t pool_size{0};
    std::size_t queued_operations{0};
    std::size_t queue_high_water_operations{0};
    std::uint64_t completed_operations{0};
    std::uint64_t rejected_operations{0};
    std::uint64_t maximum_queue_wait_microseconds{0};
};

struct SqlitePagerCounterSnapshot {
    bool available{false};
    std::uint64_t connection_generation{0};
    std::uint64_t cache_hits{0};
    std::uint64_t cache_misses{0};
    std::uint64_t cache_writes{0};
};

enum class SqliteStorageLayoutProfile {
    canonical,
    linux_lowercase_v1,
};

struct SqliteStorageSettings {
    std::filesystem::path data_directory;
    std::shared_ptr<application::IOtherBoardTextStorage> other_board_text;
    SqliteQueueSettings queue;
    SqliteBusySettings busy;
    SqliteCheckpointSettings checkpoint;
    SqliteReaderSettings readers;
    std::function<std::int64_t()> utc_now;
    // Deterministic subprocess-crash qualification seam. Production leaves this empty.
    std::function<application::StorageStatus(std::string_view)> bootstrap_boundary_hook;
    // Private deterministic worker-failure qualification seam. Production leaves empty.
    std::function<void()> reader_dequeued_hook;
    // Trusted native-host selection, never an arbitrary filename or a migration
    // of established file-area paths. Windows/generic callers retain canonical.
    SqliteStorageLayoutProfile layout_profile{SqliteStorageLayoutProfile::canonical};
};

struct SqliteMigration {
    std::int32_t version{0};
    std::string name;
    std::string content_sha256;
    std::string sql;
};

std::vector<SqliteMigration> sfx_storage_migrations();

struct SqliteBackupResult {
    application::StorageStatus status;
    std::filesystem::path backup_file;
    std::uintmax_t database_bytes{0};
    std::int32_t schema_version{0};
};
struct SqliteCompactionResult {
    application::StorageStatus status;
    std::uintmax_t bytes_before{},bytes_after{};
    bool logical_content_verified{};
    bool physical_compaction_committed{};
    std::size_t private_temporary_files{};
};

class SqliteStorage final : public application::IStorageRuntime,
                            public application::IAccountStorage,
                            public application::IAuthenticationStorage,
                            public application::IPhase3aStorage,
                            public application::IMenuPreferenceStorage,
                            public application::IRegistrationCommitPort,
                            public application::ICallerNameClassificationPort,
                            public application::IAuthenticatedSessionCommitPort,
                            public application::ICallerJournalStorage,
                            public application::IClassicInformationStorage,
                            public application::IMessageStorage,
                            public application::IMessageMaintenanceStorage,
                            public application::IMessageDeletionStorage,
                            public application::IQwkStorage,
                            public application::IQwkReplyStorage,
                            public application::IFileStorage,
                            public application::IDoorStorage, public application::IChatStorage,
                            public application::IFtnStorage,public application::IFtnMailStorage {
public:
    SqliteStorage(
        SqliteStorageSettings settings,
        const IStoragePathPolicy& path_policy,
        std::vector<SqliteMigration> migrations = sfx_storage_migrations());
    ~SqliteStorage() override;

    SqliteStorage(const SqliteStorage&) = delete;
    SqliteStorage& operator=(const SqliteStorage&) = delete;

    application::StorageStatus start() override;
    application::StorageStatus checkpoint() override;
    application::StorageStatus shutdown() override;
    application::StorageHealth health() const override;
    SqliteReaderPoolHealth reader_pool_health() const;
    application::StorageResult<application::FtnConfiguration> ftn_configuration(const application::MessageConfigurationActor&) override;
    application::StorageResult<std::int64_t> ftn_configuration_receipt(const application::MessageConfigurationActor&,const application::FtnTransferToken&) override;
    application::StorageResult<application::FtnRetentionPreview> preview_ftn_retention(const application::MessageConfigurationActor&,const application::FtnTransferToken&,const application::FtnConfiguration&) override;
    application::StorageResult<std::vector<application::FtnRetainedOriginal>> ftn_retention_candidates(const application::MessageConfigurationActor&,const application::Identifier128&,std::size_t) override;
    application::StorageResult<application::FtnExpiryIntent> prepare_ftn_expiry(const application::MessageConfigurationActor&,const application::FtnExpiryIntent&) override;
    application::StorageResult<std::optional<application::FtnExpiryIntent>> pending_ftn_expiry(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageStatus reconcile_ftn_expiry(const application::MessageConfigurationActor&,const application::FtnExpiryIntent&,application::FtnExpiryOutcome) override;
    application::StorageStatus execute_ftn_expiry(const application::MessageConfigurationActor&,std::shared_ptr<application::FtnRetentionRemoval>) override;
    application::StorageResult<application::FtnConfigurationChangeResult> change_ftn_configuration(const application::MessageConfigurationActor&,const application::FtnTransferToken&,const application::FtnConfiguration&) override;
    application::StorageResult<application::FtnIngressReceipt> begin_ftn_ingress(const application::MessageConfigurationActor&,const application::FtnIngressReceipt&) override;
    application::StorageResult<application::FtnIngressReceipt> ftn_ingress_receipt(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageStatus index_ftn_tic(const application::MessageConfigurationActor&,const application::Identifier128&,std::int64_t,std::string_view,std::string_view) override;
    application::StorageResult<bool> claim_ftn_message(const application::MessageConfigurationActor&,const application::Identifier128&,std::int64_t) override;
    application::StorageResult<application::FtnHatchRecord> prepare_ftn_hatch(const application::MessageConfigurationActor&,const application::FtnHatchRequest&,const application::FilePayloadIdentity&,std::uint32_t) override;
    application::StorageResult<application::FtnHatchRecord> ftn_hatch(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageResult<application::FtnHatchRecord> bind_ftn_hatch_preparer(const application::MessageConfigurationActor&,const application::Identifier128&,const application::FileObjectIdentity&) override;
    application::StorageResult<std::vector<application::FtnHatchRecord>> pending_ftn_hatches(const application::MessageConfigurationActor&,const application::Identifier128&,std::size_t) override;
    application::StorageResult<std::vector<application::FtnHatchRecord>> ftn_hatches_for_file(const application::MessageConfigurationActor&,application::FileId,const application::Identifier128&,std::size_t) override;
    application::StorageResult<application::FtnHatchRecord> commit_ftn_hatch(const application::MessageConfigurationActor&,const application::Identifier128&,const std::vector<application::FtnOutboundJob>&) override;
    application::StorageStatus hold_ftn_hatch(const application::MessageConfigurationActor&,const application::Identifier128&,std::string_view) override;
    application::StorageResult<application::FtnSpoolObject> prepare_ftn_spool(const application::MessageConfigurationActor&,std::int64_t,const application::FtnOutboundJob&,const application::Identifier128& attempt={}) override;
    application::StorageResult<application::FtnSpoolObject> bind_ftn_spool(const application::MessageConfigurationActor&,const application::Identifier128&,const application::FileObjectIdentity&,const application::Identifier128& attempt={}) override;
    application::StorageResult<application::FtnSpoolObject> verify_ftn_spool(const application::MessageConfigurationActor&,const application::Identifier128&,const application::FilePayloadIdentity&,const application::Identifier128& attempt={}) override;
    application::StorageResult<application::FtnFilePublication> prepare_ftn_file(const application::MessageConfigurationActor&,const application::FtnFileRequest&,const std::optional<application::FtnFileForwardPlan>& plan={},const application::Identifier128& attempt={}) override;
    application::StorageResult<application::FtnFileAuthority> ftn_file_authority(const application::MessageConfigurationActor&,const application::Identifier128&,const application::Identifier128& attempt={}) override;
    application::StorageResult<application::FtnFileForwardPlan> ftn_file_forward_plan(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageResult<application::FtnFilePublication> ftn_file_publication(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageResult<std::vector<application::Identifier128>> pending_ftn_files(const application::MessageConfigurationActor&,const application::Identifier128&,std::size_t) override;
    application::StorageResult<application::FtnFilePublication> advance_ftn_file(const application::MessageConfigurationActor&,const application::Identifier128&,application::FtnFilePhase,const application::FilePayloadIdentity&,const application::Identifier128& attempt={}) override;
    application::StorageResult<application::FtnFilePublication> commit_ftn_file(const application::MessageConfigurationActor&,const application::Identifier128&,const std::vector<application::FtnOutboundJob>&,const application::Identifier128& attempt={}) override;
    application::StorageStatus hold_ftn_file(const application::MessageConfigurationActor&,const application::Identifier128&,std::string_view) override;
    application::StorageResult<std::optional<application::FilePayloadIdentity>> ftn_ingress_checkpoint(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageStatus checkpoint_ftn_ingress(const application::MessageConfigurationActor&,const application::Identifier128&,std::uint64_t,const application::FilePayloadIdentity&) override;
    application::StorageStatus advance_ftn_ingress(const application::MessageConfigurationActor&,const application::Identifier128&,std::uint64_t,std::uint64_t,std::optional<application::FtnDigest>,application::FtnIngressState,std::string_view) override;
    application::StorageResult<std::vector<application::FtnIngressReceipt>> pending_ftn_ingress(const application::MessageConfigurationActor&,std::size_t) override;
    application::StorageResult<std::vector<application::FtnIngressReceipt>> pending_ftn_ingress_after(const application::MessageConfigurationActor&,std::size_t,const application::Identifier128&) override;
    application::StorageResult<std::vector<application::FtnIngressReceipt>> ftn_payload_candidates(const application::MessageConfigurationActor&,const application::Identifier128&,std::string_view) override;
    application::StorageResult<std::vector<application::FtnScanIntent>> pending_ftn_scan(const application::MessageConfigurationActor&,std::string_view,std::int64_t,std::size_t) override;
    application::StorageStatus commit_ftn_scan(const application::MessageConfigurationActor&,std::int64_t,const application::FtnScanIntent&,const std::vector<application::FtnOutboundJob>&) override;
    application::StorageResult<application::FtnSerialState> ftn_serial_state(const application::MessageConfigurationActor&) override;
    application::StorageResult<std::vector<application::FtnOutboundJob>> ftn_jobs_for_operation(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageResult<application::FtnQueuePage> ftn_queue_page(const application::MessageConfigurationActor&,std::uint32_t offset) override;
    application::StorageResult<application::FtnQueuePage> sysop_ftn_queues(const application::MessageActor&,std::string_view,std::uint32_t) override;
    application::StorageResult<application::FtnHeldReview> sysop_ftn_held(const application::MessageActor&,std::string_view,application::Identifier128) override;
    application::StorageResult<bool> ftn_outbound_ready(const application::MessageConfigurationActor&,std::string_view,std::string_view) override;
    application::StorageResult<application::FtnAreaFixResult> apply_ftn_areafix(const application::MessageConfigurationActor&,const application::FtnAreaFixRequest&) override;
    application::StorageResult<std::vector<application::FtnControlIntent>> pending_ftn_control(const application::MessageConfigurationActor&,const application::Identifier128& after,std::size_t maximum) override;
    application::StorageResult<application::FtnImportResult> record_ftn_provider_response(const application::MessageConfigurationActor&,const application::FtnImportRequest&) override;
    application::StorageResult<std::optional<application::FtnRescanItem>> next_ftn_rescan(const application::MessageConfigurationActor&,const application::Identifier128& after) override;
    application::StorageStatus commit_ftn_rescan(const application::MessageConfigurationActor&,std::int64_t revision,const application::FtnRescanItem&,const std::optional<application::FtnOutboundJob>&) override;
    application::StorageStatus commit_ftn_control(const application::MessageConfigurationActor&,std::int64_t revision,const application::FtnControlIntent&,const application::FtnOutboundJob&) override;
    application::StorageStatus preserve_ftn_serial_history(const application::MessageConfigurationActor&,const std::optional<application::FtnSerialState>&,bool restored_predecessor=false) override;
    application::StorageResult<application::FtnSerialRecoveryReceipt> recover_ftn_serial_history(const application::MessageConfigurationActor&,const application::FtnSerialRecoveryRequest&);
    application::StorageResult<std::optional<application::FtnSerialRecoveryReceipt>> last_ftn_serial_recovery(const application::MessageConfigurationActor&);
    application::StorageResult<application::FtnHistoryRecoveryQueue> ftn_history_recovery_queue(const application::MessageConfigurationActor&);
    application::StorageStatus queue_ftn_jobs(const application::MessageConfigurationActor&,std::int64_t,const std::vector<application::FtnOutboundJob>&) override;
    application::StorageResult<std::vector<application::FtnOutboundJob>> claim_ftn_jobs(const application::MessageConfigurationActor&,std::string_view,std::string_view,const application::Identifier128&,const application::Identifier128&,std::size_t,bool) override;
    application::StorageStatus finish_ftn_job(const application::MessageConfigurationActor&,const application::Identifier128&,const application::Identifier128&,application::FtnOutboundState,std::string_view) override;
    application::StorageResult<std::vector<application::FtnExternalPublication>> prepare_ftn_external_handoff(const application::MessageConfigurationActor&,std::int64_t,const application::Identifier128&) override;
    application::StorageResult<std::vector<application::FtnExternalPublication>> pending_ftn_external_handoff(const application::MessageConfigurationActor&,const application::Identifier128& = {}) override;
    application::StorageStatus finish_ftn_external_handoff(const application::MessageConfigurationActor&,const application::Identifier128&,bool) override;
    application::StorageStatus release_ftn_external_handoff(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageStatus recover_ftn_claims(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageResult<std::vector<application::MessageNetworkChoice>> ftn_message_networks(const application::MessageActor&) override;
    application::StorageStatus reject_ftn_tic(const application::MessageConfigurationActor&,const application::FtnTicRejection&) override;
    application::StorageResult<std::optional<application::FtnTicReview>> review_ftn_tic(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageResult<application::FtnTicRetryRecord> request_ftn_tic_retry(const application::MessageConfigurationActor&,const application::FtnTicRetryRequest&) override;
    application::StorageResult<std::vector<application::FtnTicRetryRecord>> pending_ftn_tic_retries(const application::MessageConfigurationActor&,const application::Identifier128&,std::size_t) override;
    application::StorageResult<application::FtnTicReview> check_ftn_tic_retry(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageStatus finish_ftn_tic_retry(const application::MessageConfigurationActor&,const application::Identifier128&,bool,std::string_view) override;
    application::StorageResult<application::FtnReprocessRecord> request_ftn_reprocess(const application::MessageConfigurationActor&,const application::FtnReprocessRequest&) override;
    application::StorageResult<std::optional<application::FtnReprocessRecord>> latest_ftn_reprocess(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageResult<std::vector<application::FtnReprocessRecord>> pending_ftn_reprocess(const application::MessageConfigurationActor&,const application::Identifier128&,std::size_t) override;
    application::StorageResult<application::FtnIngressReceipt> check_ftn_reprocess(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageStatus finish_ftn_reprocess(const application::MessageConfigurationActor&,const application::Identifier128&,bool,std::string_view) override;
    application::StorageStatus record_ftn_held_message(const application::MessageConfigurationActor&,const application::FtnHeldMessage&) override;
    application::StorageResult<std::vector<application::FtnHeldMessage>> held_ftn_messages(const application::MessageConfigurationActor&,const application::Identifier128& after,std::size_t maximum) override;
    application::StorageResult<application::FtnRecipientResolution> resolve_ftn_recipient(const application::MessageConfigurationActor&,const application::FtnRecipientResolutionRequest&) override;
    application::StorageResult<application::FtnHeldReview> ftn_held_review(const application::MessageConfigurationActor&,const application::FtnHeldReviewQuery&) override;
    application::StorageResult<application::FtnHeldRecipients> ftn_held_recipients(const application::MessageConfigurationActor&,const application::FtnHeldRecipientQuery&) override;
    application::StorageResult<application::FtnImportResult> import_ftn_message(const application::MessageConfigurationActor&,const application::FtnImportRequest&) override;
    application::StorageResult<std::vector<application::FtnTossProgress>> ftn_toss_progress(const application::MessageConfigurationActor&,const application::Identifier128&) override;
    application::StorageResult<bool> known_ftn_message(const application::MessageConfigurationActor&,std::string_view,std::string_view,std::string_view,const application::FtnDigest&,const application::FtnDigest&) override;
    application::StorageResult<application::ChatConfiguration> chat_configuration(const application::MessageConfigurationActor&) override;
    application::StorageResult<application::ChatConfiguration> change_chat_configuration(const application::MessageConfigurationActor&,const application::ChatConfiguration&) override;
    application::StorageResult<application::ChatPreferences> chat_preferences(const application::MessageActor&) override;
    application::StorageResult<application::ChatPreferences> save_chat_preferences(const application::MessageActor&,const application::ChatPreferences&) override;
    application::StorageResult<application::PrivateChatAllowance> private_chat_allowance(const application::MessageActor&,std::string_view) override;
    application::StorageStatus persist_private_chat_usage(application::Identifier128,std::int64_t,
        const std::vector<std::pair<std::string,std::int64_t>>&) override;
    application::StorageResult<application::DoorCatalog> door_configuration(const application::MessageConfigurationActor&) override;
    application::StorageResult<application::DoorChangeOutcome> change_door_configuration(const application::MessageConfigurationActor&,const application::DoorConfigurationChange&) override;
    application::StorageResult<application::DoorCallerMenu> door_menu(const application::MessageActor&,application::DoorMenuId) override;
    application::StorageResult<application::DoorLaunch> door_launch(const application::MessageActor&,const application::DoorLaunchRequest&) override;
    application::StorageResult<application::DoorEventList> door_events(const application::MessageActor&,application::DoorAutomaticEvent) override;
    application::StorageResult<application::DoorLaunch> door_event_launch(const application::MessageActor&,const application::DoorEventLaunchRequest&) override;
    application::StorageResult<application::DoorBulletinCatalog> door_bulletin_configuration(const application::MessageConfigurationActor&) override;
    application::StorageResult<application::DoorChangeOutcome> change_door_bulletin(const application::MessageConfigurationActor&,const application::DoorBulletinChange&) override;
    application::StorageResult<application::DoorBulletinListing> door_bulletins(const application::MessageActor&) override;
    application::StorageResult<std::vector<application::DoorBulletinNotice>> door_bulletin_changes(const application::MessageActor&,std::int64_t) override;
    application::StorageResult<application::DoorBulletinSource> door_bulletin_source(const application::MessageConfigurationActor&,application::DoorBulletinId) override;
    application::StorageResult<application::DoorBulletinPublicationResult> publish_door_bulletin(const application::MessageConfigurationActor&,const application::DoorBulletinPublication&) override;
    application::StorageResult<application::MenuDoorCommands> menu_door_commands(const application::MessageActor&,application::MenuId) override;
    application::StorageResult<application::DoorLaunch> menu_door_launch(const application::MessageActor&,const application::MenuDoorSelection&) override;
    application::StorageResult<application::FileCatalog> file_catalog(const application::FileActor&) override;
    application::StorageResult<application::FileConfiguration> file_configuration(const application::FileConfigurationActor&) override;
    application::StorageResult<application::FileGroupDeletionState> file_group_deletion(const application::FileConfigurationActor&,application::FileGroupId) override;
    application::StorageResult<application::FileCallerExemption> file_caller_exemption(const application::FileConfigurationActor&,application::AccountId) override;
    application::StorageResult<std::uint64_t> file_content_duplicate_count(const application::FileConfigurationActor&,const application::FilePayloadIdentity&) override;
    application::StorageResult<application::FileChangeOutcome> change_file_configuration(const application::FileConfigurationActor&,const application::FileConfigurationChange&) override;
    application::StorageResult<application::FileCallerState> file_caller_state(const application::FileActor&) override;
    application::StorageResult<application::FileCallerState> save_file_caller_state(const application::FileActor&,const application::FileCallerState&) override;
    application::StorageResult<std::vector<application::FileScanArea>> file_scan_areas(const application::FileActor&) override;
    application::StorageResult<application::FileScanArea> save_file_scan_area(const application::FileActor&,const application::FileScanArea&) override;
    application::StorageStatus acknowledge_file_scan(const application::FileActor&,const std::vector<application::FileScanSeen>&) override;
    application::StorageStatus complete_file_scan(const application::FileActor&,const std::vector<application::FileAreaId>&,std::int64_t,std::int64_t) override;
    application::StorageResult<application::FilePage> file_page(const application::FileActor&,const application::FileQuery&,const application::FileQueryCursor&,std::size_t) override;
    application::StorageResult<application::FileEntry> file_information(const application::FileActor&,application::FileId) override;
    application::StorageResult<application::FileChangeOutcome> edit_own_file_description(const application::FileActor&,application::Identifier128,application::FileId,std::int64_t,const application::FileDescriptions&) override;
    application::StorageResult<application::FileEntry> register_file(const application::FileConfigurationActor&,const application::FileRegistration&) override;
    application::StorageResult<application::FileCallerStatistics> file_caller_statistics(const application::FileActor&,std::string_view) override;
    application::StorageResult<application::FileDownloadStart> reserve_file_download(const application::FileActor&,application::Identifier128,application::FileId,std::string_view) override;
    application::StorageResult<application::FileTransferOutcome> finish_file_download(const application::FileActor&,application::Identifier128,const application::FilePayloadIdentity&,bool) override;
    application::StorageStatus recover_file_transfers(const application::FileConfigurationActor&) override;
    application::StorageResult<application::FileIntake> begin_file_intake(const application::FileActor&,const application::FileIntakeStart&) override;
    application::StorageResult<application::FileIntake> file_intake(const application::FileConfigurationActor&,application::Identifier128) override;
    application::StorageStatus bind_file_intake_object(const application::FileActor&,application::Identifier128,bool,const application::FileObjectIdentity&) override;
    application::StorageStatus finish_file_intake_cleanup(const application::FileConfigurationActor&,application::Identifier128,const application::FileObjectIdentity&) override;
    application::StorageResult<application::FileIntake> record_file_intake(const application::FileActor&,const application::FileIntakeReceived&) override;
    application::StorageResult<application::FileIntake> prepare_file_intake(const application::FileActor&,application::Identifier128,const application::FileDescriptions&,bool) override;
    application::StorageResult<application::FileEntry> commit_file_intake(const application::FileActor&,application::Identifier128,const application::FilePayloadIdentity&) override;
    application::StorageResult<std::vector<application::FileIntake>> pending_file_intake(const application::FileConfigurationActor&,std::size_t,application::Identifier128 = {}) override;
    application::StorageStatus resolve_file_intake(const application::FileConfigurationActor&,application::Identifier128,const application::FilePayloadIdentity&,bool,std::string_view) override;
    application::StorageStatus recover_file_intake(const application::FileConfigurationActor&) override;
    application::StorageResult<application::FileIntakePurge> file_intake_purge(const application::FileConfigurationActor&,application::Identifier128) override;
    application::StorageStatus prepare_file_intake_purge(const application::FileConfigurationActor&,const application::FileIntakePurge&) override;
    application::StorageStatus finish_file_intake_purge(const application::FileConfigurationActor&,const application::FileIntakePurge&) override;
    application::StorageResult<application::ManagedFilePage> managed_file_page(const application::FileConfigurationActor&,const application::FileManagementQuery&,application::FileId,std::size_t) override;
    application::StorageResult<application::ManagedFile> managed_file(const application::FileConfigurationActor&,application::FileId) override;
    application::StorageResult<std::vector<application::FileNameStatus>> file_name_status(const application::FileConfigurationActor&,application::FileAreaId,const std::vector<std::string>&) override;
    application::StorageResult<application::FileChangeOutcome> set_file_reupload_block(const application::FileConfigurationActor&,application::Identifier128,application::FileId,std::int64_t,bool) override;
    application::StorageResult<application::FileOperation> prepare_file_operation(const application::FileConfigurationActor&,const application::FileOperationRequest&) override;
    application::StorageResult<application::FileOperation> file_operation(const application::FileConfigurationActor&,application::Identifier128) override;
    application::StorageStatus bind_file_operation_destination(const application::FileConfigurationActor&,application::Identifier128,const application::FileObjectIdentity&) override;
    application::StorageStatus cancel_file_operation(const application::FileConfigurationActor&,application::Identifier128,const application::FileOperationCompletion&) override;
    application::StorageResult<application::FileOperation> commit_file_operation(const application::FileConfigurationActor&,application::Identifier128,const application::FileOperationCompletion&) override;
    application::StorageStatus hold_file_operation(const application::FileConfigurationActor&,application::Identifier128,std::string_view) override;
    application::StorageResult<std::vector<application::FileOperation>> pending_file_operations(const application::FileConfigurationActor&,application::Identifier128,std::size_t) override;
    application::StorageResult<std::string> file_importer_name(const application::FileConfigurationActor&) override;
    application::StorageResult<application::FileImport> prepare_file_import(const application::FileConfigurationActor&,const application::FileImportRequest&) override;
    application::StorageResult<application::FileImport> file_import(const application::FileConfigurationActor&,application::Identifier128) override;
    application::StorageResult<std::vector<application::FileImport>> pending_file_imports(const application::FileConfigurationActor&,application::Identifier128,std::size_t) override;
    application::StorageStatus bind_file_import_destination(const application::FileConfigurationActor&,application::Identifier128,const application::FileObjectIdentity&) override;
    application::StorageResult<application::FileEntry> commit_file_import(const application::FileConfigurationActor&,application::Identifier128,const application::FileOperationCompletion&) override;
    application::StorageStatus hold_file_import(const application::FileConfigurationActor&,application::Identifier128,std::string_view) override;
    application::StorageStatus cancel_file_import(const application::FileConfigurationActor&,application::Identifier128,const application::FileOperationCompletion&) override;
    application::StorageResult<application::QwkPolicy> qwk_policy(const application::MessageConfigurationActor&) override;
    application::StorageResult<application::QwkPolicy> save_qwk_policy(const application::MessageConfigurationActor&,application::Identifier128,std::int64_t,std::string_view,std::uint32_t) override;
    application::StorageResult<application::QwkSavedPreferences> qwk_preferences(const application::MessageActor&) override;
    application::StorageResult<application::QwkSavedPreferences> save_qwk_preferences(const application::MessageActor&,std::int64_t,const application::QwkPreferences&) override;
    application::StorageResult<std::uint32_t> qwk_reference(const application::MessageActor&,application::MessageId) override;
    application::StorageResult<std::vector<std::uint32_t>> qwk_references(const application::MessageActor&,const std::vector<application::MessageId>&) override;
    application::StorageResult<application::QwkPacketPlan> begin_qwk_packet(const application::MessageActor&,application::Identifier128) override;
    application::StorageResult<bool> validate_qwk_packet_authority(const application::MessageActor&,const application::QwkPacketPlan&) override;
    application::StorageResult<std::optional<application::OpenedMessage>> next_qwk_message(const application::MessageActor&,const application::QwkPacketPlan&,std::size_t) override;
    application::StorageResult<std::optional<application::QwkMessageCandidate>> next_qwk_candidate(const application::MessageActor&,const application::QwkPacketPlan&,std::size_t) override;
    application::StorageResult<application::QwkQueueSelection<application::QwkMessageCandidate>> select_qwk_candidate(const application::MessageActor&,const application::QwkPacketPlan&,std::size_t,std::size_t) override;
    application::StorageResult<application::QwkQueueSelection<application::OpenedMessage>> select_qwk_message(const application::MessageActor&,const application::QwkPacketPlan&,std::size_t,std::size_t) override;
    application::StorageResult<application::QwkDeliveryOutcome> commit_qwk_delivery(const application::MessageActor&,const application::QwkDeliveryCommit&) override;
    application::StorageResult<std::optional<application::QwkDeliveryOutcome>> qwk_delivery_outcome(const application::MessageActor&,const application::QwkDeliveryCommit&) override;
    application::StorageResult<application::QwkPointerChange> preview_qwk_public_pointers(const application::MessageActor&,const application::QwkPointerChange&) override;
    application::StorageStatus change_qwk_public_pointers(const application::MessageActor&,const application::QwkPointerChange&) override;
    application::StorageResult<application::QwkPolicy> qwk_caller_policy(const application::MessageActor&) override;
    application::StorageResult<application::QwkImportOutcome> import_qwk_reply(const application::MessageActor&,std::shared_ptr<application::IQwkReplySource>) override;
    application::StorageResult<std::optional<application::QwkImportOutcome>> qwk_import_outcome(const application::MessageActor&,const application::QwkReplySourceInfo&) override;
    SqlitePagerCounterSnapshot pager_counters() const;
    application::StorageResult<std::vector<application::MessageGroup>> message_groups(const application::MessageActor&) override;
    application::StorageResult<application::MessageConfigurationSnapshot> message_configuration(const application::MessageConfigurationActor& a, std::optional<application::MessageGroupId>, const application::MessageCatalogCursor&) override;
    application::StorageResult<application::MessageConfigurationResult> change_message_configuration(const application::MessageConfigurationActor& a, const application::MessageConfigurationChange& c) override;
    application::StorageResult<application::MessageRecipientPage> resolve_message_recipient(const application::MessageActor& a, std::string_view q, const std::optional<application::MessageRecipient>& p = {}, bool name_only=false) override;
    application::StorageResult<application::MessageTarget> system_sysop_target(const application::MessageActor& a) override;
    application::StorageResult<std::optional<application::MessageArea>> saved_message_area(const application::MessageActor&) override;
    application::StorageStatus remember_message_area(const application::MessageActor&,application::LocalEchoId) override;
    application::StorageResult<application::MessageArea> message_area(const application::MessageActor& a,application::LocalEchoId e) override;
    application::StorageResult<application::MessageCallerStatistics> message_caller_statistics(const application::MessageActor& a,std::optional<application::LocalEchoId> e) override;
    application::StorageResult<application::MessageArrivalPage> message_arrival_page(const application::MessageActor& a,std::int64_t after,std::size_t limit) override;
    application::StorageResult<bool> message_arrival_eligible(const application::MessageActor& a,application::MessageId id) override;
    application::StorageResult<std::optional<std::string>> message_arrival_sender(const application::MessageActor&,application::MessageId) override;
    application::StorageResult<bool> has_addressed_unread(const application::MessageActor& a,bool personal) override;
    application::StorageResult<application::MessageLoginScanSummary> login_message_scan_summary(const application::MessageActor&,bool include_authored_echoes=false) override;
    application::StorageResult<application::MessageCommitOutcome> message_commit_outcome(const application::CommitMessageRequest&) override;
    application::StorageResult<application::MessageCatalogPage<application::MessageGroup>> message_group_page(const application::MessageActor&,const application::MessageCatalogCursor&) override;
    application::StorageResult<application::MessageCatalogPage<application::LocalEcho>> local_echo_page(const application::MessageActor&,application::MessageGroupId,const application::MessageCatalogCursor&) override;
    application::StorageResult<application::MessageQueryContext> begin_message_query(const application::MessageActor&,const application::MessageQueryOptions&) override;
    application::StorageResult<application::MessageHeader> message_number_header(const application::MessageActor&,const application::MessageQueryContext&,std::int64_t) override;
    application::StorageResult<application::OpenedMessage> open_query_message(const application::MessageActor&,const application::MessageQueryContext&,application::MessageId,bool,bool,bool preview=false) override;
    application::StorageResult<application::MessageGroupScanPreview> preview_group_scan_column(const application::MessageActor&,application::MessageGroupId,application::MessageScanColumn) override;
    application::StorageResult<bool> toggle_group_scan_column(const application::MessageActor&,application::Identifier128,const application::MessageGroupScanPreview&) override;
    application::StorageResult<application::MessageHeaderPage> query_message_headers(const application::MessageActor&,const application::MessageQueryContext&,const application::MessageQueryCursor&,std::size_t = application::kMessagePageDefault) override;
    application::StorageResult<application::MessageHeaderPage> query_message_list_headers(const application::MessageActor&,const application::MessageQueryContext&,const application::MessageQueryCursor&,std::size_t = application::kMessagePageDefault) override;
    application::StorageStatus set_message_list_deleted(const application::MessageActor&,const application::MessageQueryContext&,
        application::MessageId,bool expected_deleted,bool deleted,application::MessageListDeleteScope expected_scope) override;
    application::StorageResult<std::vector<application::LocalEcho>> local_echoes(const application::MessageActor&, application::MessageGroupId) override;
    application::StorageResult<application::ComposeContext> begin_message(const application::MessageActor&, const application::MessageTarget&, std::optional<application::MessageId> = {}) override;
    application::StorageResult<application::CommittedMessage> commit_message(const application::CommitMessageRequest&) override;
    application::StorageResult<application::CommittedMessage> reconcile_message(const application::CommitMessageRequest&) override;
    application::StorageResult<application::OpenedMessage> open_message(const application::MessageActor&, application::MessageId) override;
    application::StorageResult<application::OpenedMessage> quote_source(const application::MessageActor&, application::MessageId) override;
    application::StorageResult<application::MessageScanPreference> message_scan_preference(const application::MessageActor&, application::LocalEchoId) override;
    application::StorageResult<application::MessageScanPreference> set_message_scan_preference(const application::MessageActor&, application::LocalEchoId, std::int64_t, application::MessageScanColumn, bool) override;
    application::StorageStatus remove_private_inbox_item(const application::MessageActor&, application::MessageId) override;
    application::StorageResult<bool> message_delete_authority(const application::MessageActor&) override;
    application::StorageResult<bool> delete_message(const application::MessageActor&, application::Identifier128, application::MessageId) override;
    application::StorageResult<bool> message_permanent_state(const application::MessageActor&, application::MessageId) override;
    application::StorageResult<bool> set_message_permanent(const application::MessageActor&, application::Identifier128, application::MessageId, bool, bool) override;
    application::StorageResult<application::MessageMaintenanceRun> begin_message_retention(const application::MessageConfigurationActor&,application::Identifier128) override;
    application::StorageResult<application::MessageRetentionBatch> message_retention_batch(const application::MessageConfigurationActor&,application::Identifier128,std::int64_t,application::MessageMaintenanceStop) override;
    application::StorageResult<application::MessageMaintenanceRun> finish_message_retention(const application::MessageConfigurationActor&,application::Identifier128,application::MessageMaintenanceOutcome,application::MessageMaintenanceReason) override;
    application::StorageResult<application::MessageMaintenanceRun> message_maintenance_run(const application::MessageConfigurationActor&,application::Identifier128) override;
    application::StorageResult<std::vector<application::MessageMaintenanceRun>> message_maintenance_history(const application::MessageConfigurationActor&,application::MessageMaintenanceKind) override;
    application::StorageResult<application::MessageIntegrityResult> check_message_integrity(const application::MessageConfigurationActor&,application::Identifier128,application::MessageMaintenanceStop) override;
    application::StorageResult<std::vector<application::MessageIntegrityFinding>> message_integrity_holds(const application::MessageConfigurationActor&) override;
    application::StorageStatus recover_interrupted_message_maintenance(const application::MessageConfigurationActor&) override;
    application::StorageResult<application::MessageDeletionPreview> preview_message_deletion(const application::MessageConfigurationActor&,const application::MessageDeletionTarget&,std::function<bool()>) override;
    application::StorageResult<application::MessageDeletionResult> commit_message_deletion(const application::MessageConfigurationActor&,application::Identifier128,const application::MessageDeletionPreview&,std::function<bool()>) override;

    application::StorageResult<application::FirstRunState> first_run_state() override;
    application::MutationResult stage_initial_sysop(const application::StageInitialSysopRequest& request) override;
    application::MutationResult complete_first_run(const application::CompleteFirstRunRequest& request) override;
    application::MutationResult create_account(const application::CreateAccountRequest& request) override;
    application::MutationResult commit_registration(
        const application::RegistrationCommitRequest& request) override;
    application::StorageResult<application::CallerNameResolution> classify(
        std::string_view submitted_name) override;
    application::MutationResult commit_authenticated_session(
        const application::AuthenticatedSessionCommitRequest& request) override;
    application::MutationResult rename_account(const application::RenameAccountRequest& request) override;
    application::MutationResult update_profile(const application::UpdateProfileRequest& request) override;
    application::StorageResult<std::vector<application::CallerDirectoryEntry>> caller_directory(
        const application::CallerDirectoryQuery&) override;
    application::StorageResult<std::int64_t> caller_day_seconds(application::AccountId,std::string_view) override;
    application::MutationResult set_purge_allowed(const application::SetPurgeAllowedRequest& request) override;
    application::MutationResult assign_security_level(const application::AssignAccountSecurityLevelRequest& request) override;
    application::MutationResult save_account(const application::SaveAccountRequest& request) override;
    application::MutationResult replace_credential(const application::ReplaceCredentialRequest& request) override;
    application::MutationResult change_account_state(const application::ChangeAccountStateRequest& request) override;
    application::MutationResult tombstone_account(const application::TombstoneAccountRequest& request) override;
    application::StorageStatus publish_security_policy(const application::PublishSecurityPolicyRequest& request) override;
    application::StorageStatus publish_retention_policy(const application::PublishRetentionPolicyRequest& request) override;
    application::MutationResult record_session_started(const application::StartSessionRequest& request) override;
    application::StorageStatus persist_session_progress(const application::PersistSessionProgressRequest& request) override;
    application::MutationResult finalize_session(const application::FinalizeSessionRequest& request) override;
    application::StorageResult<std::vector<application::CallerJournalRecord>> pending_caller_journals(bool unfinished, std::size_t limit) override;
    application::StorageStatus append_caller_activity(application::Identifier128 session,
        const application::CallerActivity&, std::string_view hostname_hint) override;
    application::StorageStatus mark_caller_recovered(application::Identifier128 session) override;
    application::StorageResult<std::optional<application::CallerJournalPublication>> caller_publication() override;
    application::StorageStatus prepare_caller_publication(const application::CallerJournalPublication&) override;
    application::StorageStatus acknowledge_caller_publication(std::int64_t caller_number) override;
    application::StorageResult<application::AccountRecord> find_account_by_name(std::string_view candidate) override;
    application::StorageResult<application::AccountRecord> find_account_by_id(application::AccountId account_id) override;
    application::StorageResult<std::string> door_user_key(application::AccountId account_id);
    application::StorageResult<application::CredentialEnvelope> load_credential(application::AccountId account_id) override;
    application::StorageResult<std::vector<application::AccountRecord>> list_accounts(const application::AccountListQuery& query) override;
    application::StorageResult<std::vector<application::AccountRecord>> inactivity_candidates(const application::InactivityCandidateQuery& query) override;
    application::StorageResult<std::vector<application::SessionRecord>> session_history(const application::HistoryQuery& query) override;
    application::StorageResult<std::vector<application::AuditRecord>> audit_history(const application::AuditHistoryQuery& query) override;
    application::StorageResult<std::vector<application::SecurityLevelRecord>> security_levels() override;
    application::StorageResult<application::SecurityLevelRecord> security_level(std::uint16_t level) override;
    application::StorageResult<application::SecurityPolicyRecord> security_policy() override;
    application::StorageResult<application::RetentionPolicyRecord> retention_policy() override;

    application::MutationResult replace_account_password(const application::ReplaceAccountPasswordRequest& request) override;
    application::MutationResult rehash_account_credential(const application::RehashAccountCredentialRequest& request) override;
    application::MutationResult issue_temporary_password(const application::IssueTemporaryPasswordRequest& request) override;
    application::StorageStatus transition_temporary_password(const application::TransitionTemporaryPasswordRequest& request) override;
    application::MutationResult complete_temporary_password(const application::CompleteTemporaryPasswordRequest& request) override;
    application::StorageResult<application::TemporaryPasswordRecord> temporary_password(application::AccountId account_id) override;
    application::StorageStatus replace_system_password(const application::ReplaceSystemPasswordRequest& request) override;
    application::StorageStatus rehash_system_credential(const application::RehashSystemCredentialRequest& request) override;
    application::StorageResult<application::SystemCredentialRecord> system_password() override;
    application::StorageStatus record_successful_login_ip(const application::RecordSuccessfulLoginIpRequest& request) override;
    application::StorageResult<std::vector<application::SuccessfulLoginIpRecord>> successful_login_ips(application::AccountId account_id) override;
    application::StorageStatus store_rate_limit_snapshot(const application::RateLimitSnapshot& snapshot) override;
    application::StorageStatus remove_rate_limit_snapshot(const application::RateLimitKey& key) override;
    application::StorageStatus clear_rate_limit(const application::ClearRateLimitRequest& request) override;
    application::StorageResult<std::vector<application::RateLimitSnapshot>> active_rate_limit_snapshots(std::int64_t now_utc_seconds, std::size_t limit) override;
    application::StorageResult<std::vector<application::AuthenticationAuditRecord>> authentication_audit_history(const application::AuthenticationAuditQuery& query) override;

    application::StorageResult<application::TransferPreferenceRecord> transfer_preference(application::AccountId account_id) override;
    application::MutationResult update_transfer_preference(const application::UpdateTransferPreferenceRequest& request) override;
    application::StorageResult<application::MenuPreferenceRecord> menu_preferences(application::AccountId account_id) override;
    application::MutationResult update_menu_preferences(const application::UpdateMenuPreferenceRequest& request) override;
    application::StorageStatus enqueue_failed_login_notice(const application::EnqueueFailedLoginNoticeRequest& request) override;
    application::StorageResult<application::FailedLoginNoticeBatch> claim_failed_login_notices(const application::ClaimFailedLoginNoticesRequest& request) override;
    application::StorageStatus acknowledge_failed_login_notices(const application::AcknowledgeFailedLoginNoticesRequest& request) override;
    application::StorageResult<std::size_t> recover_abandoned_failed_login_claims(const application::RecoverFailedLoginClaimsRequest& request) override;

    SqliteBackupResult create_backup(const std::filesystem::path& destination_file);
    application::StorageResult<application::OtherBoardPage> other_boards(std::int64_t after_id) override;
    application::StorageResult<application::OpeningDateRecord> opening_date() override;
    application::StorageStatus change_classic_information(const application::ClassicInformationChange&,const application::ClassicInformationActor&) override;
    application::StorageResult<application::SystemInformationCounters> system_information_counters(std::int64_t day_begin,std::int64_t day_end) override;
    application::StorageStatus validate_backup(const std::filesystem::path& backup_file) const;
    // Read-only exact integrity/history preflight, including an older approved
    // prefix. Native administrative hosts compare the returned version BEFORE
    // start(), so a GUI cannot silently create/migrate an installation.
    application::StorageResult<std::int32_t> verified_schema_version(const std::filesystem::path&) const;
    application::StorageResult<application::FtnSerialState> verified_ftn_serial_history(const std::filesystem::path&) const;
    application::StorageStatus restore_backup(const std::filesystem::path& backup_file);
    // Component operation only: host must retain stopped exclusive ownership
    // and obtain its fresh verified managed backup before invoking this method.
    SqliteCompactionResult compact_offline(const std::filesystem::path& private_temp_directory,
        std::function<bool()> cancelled={});

    std::filesystem::path database_path() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sfx::adapters::storage
