#include "sfx/platform/win32/offline_account_administration.hpp"
#include "../../../apps/product_version.h"
#include "sfx/application/door_bulletin_preview_context.hpp"
#include "sfx/platform/win32/door_bulletin_sources.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/other_board_text_file.hpp"
#include "sfx/platform/win32/durable_journal_files.hpp"
#include "sfx/platform/win32/bbs_log_timestamp.hpp"
#include "sfx/application/durable_journal.hpp"
#include "sfx/platform/win32/backup_collections.hpp"
#include "sfx/platform/win32/bbs_time_zone_provider.hpp"
#include "sfx/platform/win32/file_configuration.hpp"
#include "sfx/platform/win32/file_backup.hpp"
#include "sfx/platform/win32/ftn_native_session.hpp"
#include "../../common/ftn_history_recovery.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/adapters/authentication/sodium_authentication.hpp"
#include "sfx/adapters/authentication/phase3a_authentication_adapters.hpp"
#include <chrono>
#include <algorithm>

namespace sfx::platform::win32 {
namespace {
namespace app = application;
class Clock final : public app::IClock {
public:
    std::int64_t unix_seconds() const noexcept override {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    std::uint64_t monotonic_millis() const noexcept override { return GetTickCount64(); }
};

class OfflineSession final : public app::ISessionRevocationSink, public app::ISecurityPolicyPublication {
public:
    OfflineSession(const std::filesystem::path& root, const std::wstring& owner,
        app::LocalAccountAuthority authority = app::LocalAccountAuthority::installation_owner)
        : path_policy_(owner), authority_(authority), root_(root), owner_(owner) {
        if (authority == app::LocalAccountAuthority::elevated_administrator) {
            const auto receipt = installation::verify_configuration_recovery_entry(root);
            installation::require(receipt.owner == owner, "offline_recovery_owner_mismatch");
        } else {
            installation::require(owner == current_process_user_sid_string(), "offline_account_owner_mismatch");
            installation::verify_runtime_installation(root, owner);
        }
        installation::require(guard_.acquire(root, owner).succeeded(), "offline_account_runtime_active");
        (void)configure_portable_file_bindings(root);
        const auto loaded = read_operational_configuration_file(root);
        installation::require(loaded.state == ConfigurationFileState::loaded && loaded.configuration.has_value(),
            "offline_account_configuration_unavailable");
        WindowsStorageDaclPolicy security(owner);
        installation::require(security.assess(root / L"Config" / L"SFX.conf",
            StorageDaclObjectKind::file).status.succeeded(), "offline_account_configuration_security");
        configuration_ = *loaded.configuration;
        // No database creation, replacement or zero-byte adoption during recovery.
        // Hold the existing regular file against replacement until SQLite closes.
        database_pin_ = installation::open_object(root / L"Data" / L"SFX.DB", false,
            FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE);
        LARGE_INTEGER size{};
        installation::require(GetFileSizeEx(database_pin_.get(), &size) && size.QuadPart > 0,
            "offline_account_database_missing_or_empty");
        adapters::storage::SqliteStorageSettings settings;
        settings.data_directory = root / L"Data";
        initialize_other_board_text(root,owner);
        settings.other_board_text=std::make_shared<WindowsOtherBoardTextStorage>(root,owner,
            [this](const app::OtherBoardLogRecord& record) {
                WindowsDurableJournalFiles files(root_,owner_);WindowsBbsLogTimestamp timestamp;
                if(!timestamp.configure(configuration_.bbs_time_zone) || !files.configure(configuration_.logging))return false;
                app::DurableJournalPublisher publisher(*storage_,files,[&](std::int64_t utc){return timestamp.format(utc);});
                (void)publisher.configure(configuration_.logging);return publisher.publish_other_board(record);
            });
        storage_ = std::make_unique<adapters::storage::SqliteStorage>(settings,
            path_policy_, adapters::storage::sfx_storage_migrations());
        installation::require(storage_->start().succeeded(), "offline_account_storage_unavailable");
        if (authority_ == app::LocalAccountAuthority::installation_owner)
            installation::require(reconcile_security_snapshot(root,owner,security_snapshot(),configuration_).succeeded(),
                "offline_security_snapshot_unavailable");
        const auto policy = storage_->security_policy();
        installation::require(policy && policy.value &&
            policy.value->revision == configuration_.security_policy_revision &&
            policy.value->policy_hash == configuration_.security_policy_hash &&
            policy.value->published_revision == policy.value->revision &&
            policy.value->published_policy_hash == policy.value->policy_hash,
            "offline_account_policy_binding_invalid");
        authentication_ = std::make_unique<app::AuthenticationSecurityService>(
            *storage_, *storage_, kdf_, &password_blocklist_, *this);
    }
    ~OfflineSession() {
        authentication_.reset();
        if (storage_) (void)storage_->shutdown();
        kdf_.shutdown();
    }
    app::LocalAccountAdministration service() {
        return {*storage_, *storage_, *authentication_, *this, identifiers_, clock_,
            configuration_, authority_};
    }
    app::LocalSecuritySnapshot security_snapshot() {
        return app::LocalSecurityAdministration(*storage_,*this,identifiers_,clock_,authority_).snapshot();
    }
    app::LocalClassicInformationSnapshot classic_information_snapshot(std::int64_t after) {
        return app::LocalClassicInformationAdministration(*storage_,identifiers_,clock_,authority_).snapshot(after);
    }
    app::LocalMessageConfigurationSnapshot message_configuration_snapshot(const app::LocalMessageConfigurationQuery& query) {
        return app::LocalMessageAdministration(*storage_,authority_).snapshot(query);
    }
    app::LocalMessageConfigurationResult change_message_configuration(const app::MessageConfigurationChange& change) {
        if(!guard_.acquired())return {app::LocalAccountResultCode::unauthorized};
        if(change.action==app::MessageConfigurationAction::delete_echo || change.action==app::MessageConfigurationAction::delete_group)return {app::LocalAccountResultCode::unauthorized};
        return app::LocalMessageAdministration(*storage_,authority_).execute(change);
    }
    app::LocalQwkPolicyResult qwk_policy_snapshot(){return app::LocalQwkAdministration(*storage_,authority_).snapshot();}
    app::StorageResult<app::FtnConfiguration> ftn_configuration(){
        if(authority_!=app::LocalAccountAuthority::installation_owner||!guard_.acquired())return {{app::StorageErrorCode::permission_denied,"FTN configuration owner required"},{}};
        return storage_->ftn_configuration({true,{}});
    }
    app::FtnHeldControlResponse ftn_held(const app::FtnHeldControlRequest& request){
        if(authority_!=app::LocalAccountAuthority::installation_owner||!guard_.acquired())return {};
        return app::execute_ftn_held_control(*storage_,{true,{}},request);
    }
    app::StorageResult<app::FtnRetentionPreview> preview_ftn_retention(const app::FtnTransferToken& token,const app::FtnConfiguration& c){
        if(authority_!=app::LocalAccountAuthority::installation_owner||!guard_.acquired())return {{app::StorageErrorCode::permission_denied,"FTN retention review owner required"},{}};
        const auto current=storage_->ftn_configuration({true,{}});
        if(!current||!current.value||!valid_changed_ftn_credentials(root_.u8string(),*current.value,c))return {{app::StorageErrorCode::permission_denied,"Changed FTN credential unavailable"},{}};
        return storage_->preview_ftn_retention({true,{}},token,c);
    }
    app::StorageResult<app::FtnConfigurationChangeResult> change_ftn_configuration(const app::FtnTransferToken& token,const app::FtnConfiguration& c){
        if(authority_!=app::LocalAccountAuthority::installation_owner||!guard_.acquired())return {{app::StorageErrorCode::permission_denied,"FTN configuration credential authority"},{}};
        const auto current=storage_->ftn_configuration({true,{}});
        if(!current||!current.value||!valid_changed_ftn_credentials(root_.u8string(),*current.value,c))return {{app::StorageErrorCode::permission_denied,"Changed FTN credential unavailable"},{}};
        return storage_->change_ftn_configuration({true,{}},token,c);
    }
    app::ChatConfigurationResult chat_configuration(){
        if(authority_!=app::LocalAccountAuthority::installation_owner||!guard_.acquired())return {app::ChatResult::denied};
        const auto result=storage_->chat_configuration({true,{}});return result&&result.value?app::ChatConfigurationResult{app::ChatResult::ok,*result.value}:app::ChatConfigurationResult{};
    }
    app::ChatConfigurationResult change_chat_configuration(const app::ChatConfiguration& c){
        if(authority_!=app::LocalAccountAuthority::installation_owner||!guard_.acquired())return {app::ChatResult::denied};
        const auto result=storage_->change_chat_configuration({true,{}},c);return result&&result.value?app::ChatConfigurationResult{app::ChatResult::ok,*result.value}:app::ChatConfigurationResult{app::ChatResult::conflict};
    }
    app::StorageResult<app::DoorCatalog> door_configuration(){
        if(authority_!=app::LocalAccountAuthority::installation_owner||!guard_.acquired())return {{app::StorageErrorCode::permission_denied,"Door catalog owner required"},{}};
        return storage_->door_configuration({true,{}});
    }
    app::LocalDoorResult change_door_configuration(const app::DoorConfigurationChange& c){
        if(!guard_.acquired())return {app::LocalAccountResultCode::unauthorized};return app::administer_door(*storage_,authority_,c);
    }
    app::StorageResult<app::DoorBulletinCatalog> door_bulletin_configuration(){
        if(authority_!=app::LocalAccountAuthority::installation_owner||!guard_.acquired())return {{app::StorageErrorCode::permission_denied,"Door Bulletin owner required"},{}};return storage_->door_bulletin_configuration({true,{}});
    }
    app::LocalDoorResult change_door_bulletin(const app::DoorBulletinChange& c){if(!guard_.acquired())return {app::LocalAccountResultCode::unauthorized};return app::administer_door_bulletin(*storage_,authority_,c);}
    app::DoorBulletinPreview preview_door_bulletin(app::DoorBulletinId id,bool ansi,std::int64_t expected_revision=0){
        app::DoorBulletinPreview denied;denied.id=id;denied.title="Door Bulletin "+std::to_string(id.value);denied.diagnostic="Installation owner access is required.";if(authority_!=app::LocalAccountAuthority::installation_owner||!guard_.acquired())return denied;
        auto sources=door_bulletin_sources(root_,owner_,[this](app::DoorBulletinId id,std::string_view diagnostic){(void)preserved_work_attention("Door Bulletin "+std::to_string(id.value),std::string(diagnostic),app::AttentionKind::door_bulletin_unavailable);});
        auto request=app::door_bulletin_preview_request(id,ansi?app::GraphicsMode::ansi:app::GraphicsMode::ascii,configuration_.bbs_name,SFX_PRODUCT_DISPLAY_VERSION,configuration_.logical_nodes);return app::preview_door_bulletin(*storage_,id,std::move(request),sources,expected_revision);
    }
    app::LocalFileConfigurationSnapshot file_configuration_snapshot(const app::LocalFileConfigurationQuery& query){return app::LocalFileAdministration(*storage_,authority_).snapshot(query);}
    app::StorageResult<app::FileConfiguration> file_configuration_catalog(){if(authority_!=app::LocalAccountAuthority::installation_owner||!guard_.acquired())return {{app::StorageErrorCode::permission_denied,"file catalog owner required"},{}};return storage_->file_configuration({true,{}});}
    app::LocalFileConfigurationResult change_file_configuration(const app::FileConfigurationChange& change){
        if(!guard_.acquired())return {app::LocalAccountResultCode::unauthorized};
        WindowsFileConfigurationPaths paths(root_,owner_,true);return app::LocalFileAdministration(*storage_,authority_,&paths).execute(change);
    }
    app::LocalQwkPolicyResult change_qwk_policy(const app::QwkPolicyChange& c){if(!guard_.acquired())return {app::LocalAccountResultCode::unauthorized};return app::LocalQwkAdministration(*storage_,authority_).execute(c);}
    app::StorageResult<app::MessageDeletionPreview> deletion_preview(const app::MessageDeletionTarget& target,std::function<bool()> cancelled){
        if(authority_!=app::LocalAccountAuthority::installation_owner || !guard_.acquired())return {{app::StorageErrorCode::permission_denied,"offline deletion owner required"},{}};
        return storage_->preview_message_deletion({true,{}},target,std::move(cancelled));
    }
    app::StorageResult<app::MessageDeletionResult> deletion_commit(app::Identifier128 operation,const app::MessageDeletionPreview& preview,std::function<bool()> cancelled){
        if(authority_!=app::LocalAccountAuthority::installation_owner || !guard_.acquired())return {{app::StorageErrorCode::permission_denied,"offline deletion owner required"},{}};
        return storage_->commit_message_deletion({true,{}},operation,preview,std::move(cancelled));
    }
    app::Identifier128 operation(){return identifiers_.generate();}
    app::MessageMaintenanceStatus maintenance_status(app::MessageMaintenanceKind kind){
        app::MessageMaintenanceStatus result;
        if(kind>app::MessageMaintenanceKind::integrity){result.diagnostic="message_job_unknown";return result;}
        const auto* job=app::message_job_id(kind);result.kind=kind;
        result.configuration=app::default_message_scheduler_configuration(job);result.state=app::default_message_scheduler_state(job);
        const auto config=read_scheduler_configuration_file(root_,owner_,job);const auto state=read_scheduler_state_file(root_,owner_,job);
        if(config.value)result.configuration=*config.value;else if(config.state!=ConfigurationFileState::missing){result.diagnostic="message_configuration_unavailable";return result;}
        if(state.value)result.state=*state.value;else if(state.state!=ConfigurationFileState::missing){result.diagnostic="message_state_unavailable";return result;}
        const auto integrity=read_scheduler_state_file(root_,owner_,app::kMessageIntegrityJobId);
        result.shared_verification_required=integrity.value?integrity.value->requires_fresh_check:integrity.state!=ConfigurationFileState::missing;
        if(!integrity.value && read_scheduler_configuration_file(root_,owner_,app::kMessageIntegrityJobId).value)result.shared_verification_required=true;
        const auto holds=storage_->message_integrity_holds({true,{}});if(!holds || !holds.value){result.diagnostic="message_holds_unavailable";return result;}result.holds=*holds.value;
        const auto history=storage_->message_maintenance_history({true,{}},kind);if(!history || !history.value){result.diagnostic="message_reports_unavailable";return result;}
        if(!history.value->empty())result.latest=history.value->front();
        result.available=true;result.diagnostic=config.state==ConfigurationFileState::missing?"new_defaults_preview_no_run":"message_maintenance_offline";return result;
    }
    app::AlphaControlResponse configure_maintenance(const app::MessageMaintenanceScheduleRequest& request){
        if(authority_!=app::LocalAccountAuthority::installation_owner || !guard_.acquired() || app::encode_message_maintenance_schedule(request).empty())return {app::LocalControlResponseCode::invalid_request,"message_schedule_invalid_or_unauthorized"};
        const auto* job=app::message_job_id(request.kind);const auto config_file=read_scheduler_configuration_file(root_,owner_,job);const auto state_file=read_scheduler_state_file(root_,owner_,job);
        auto old=app::default_message_scheduler_configuration(job);auto old_state=app::default_message_scheduler_state(job);
        if(config_file.value)old=*config_file.value;else if(config_file.state!=ConfigurationFileState::missing)return {app::LocalControlResponseCode::configuration_failure,"message_configuration_unavailable"};
        if(state_file.value)old_state=*state_file.value;else if(state_file.state!=ConfigurationFileState::missing)return {app::LocalControlResponseCode::configuration_failure,"message_state_unavailable"};
        else{old_state.configuration_revision=old.revision;if(request.kind==app::MessageMaintenanceKind::integrity && config_file.value)old_state.requires_fresh_check=true;}
        if(old.revision!=request.expected_revision || old.revision==UINT64_MAX)return {app::LocalControlResponseCode::invalid_request,"message_schedule_revision_conflict"};
        auto config=old;++config.revision;config.enabled=request.enabled;config.local_hour=request.hour;config.local_minute=request.minute;config.local_weekday=request.weekday;
        const auto zone=configuration_.bbs_time_zone;
        app::RuntimeScheduler candidate(old,old_state,[zone](std::int64_t after,std::uint8_t h,std::uint8_t m){return next_bbs_daily_run(zone,after,h,m);},
            [zone](std::int64_t after,std::uint8_t h,std::uint8_t m,std::optional<std::uint8_t> day,std::int64_t slot){return next_bbs_maintenance_run(zone,after,h,m,day,slot);});
        if(!candidate.reconfigure(config,clock_.unix_seconds()))return {app::LocalControlResponseCode::configuration_failure,"message_schedule_time_unavailable"};
        const auto saved=publish_scheduler_configuration_file(root_,owner_,config);
        const auto state_saved=saved?publish_scheduler_state_file(root_,owner_,candidate.state()):app::StorageStatus{app::StorageErrorCode::io,"message_schedule_unconfirmed"};
        if(!saved || !state_saved){
            // Existing choices restored; genuinely absent new settings may retain
            // approved defaults. No purge, replay, new backup pool or enabling.
            const auto rolled_config=publish_scheduler_configuration_file(root_,owner_,old);
            const auto rolled_state=publish_scheduler_state_file(root_,owner_,old_state);
            return {app::LocalControlResponseCode::configuration_failure,!rolled_config || !rolled_state?"message_schedule_rollback_unconfirmed":"message_schedule_save_failed_defaults_preserved"};
        }
        return {app::LocalControlResponseCode::ok,"message_schedule_saved_no_purge"};
    }
    OfflineMaintenanceBackupResult maintenance_backup(std::function<bool()> cancelled){
        OfflineMaintenanceBackupResult result;
        if(authority_!=app::LocalAccountAuthority::installation_owner || !guard_.acquired()){result.status={app::StorageErrorCode::permission_denied,"exclusive offline backup required"};return result;}
        auto settings=read_scheduler_configuration_file(root_,owner_);
        if(settings.state==ConfigurationFileState::missing){
            // Only genuinely absent accepted backup settings: Off, ordinary
            // default Scheduled destination/retention. Never replace saved paths.
            const auto saved=publish_scheduler_configuration_file(root_,owner_,app::SchedulerConfiguration{});
            if(saved)settings=read_scheduler_configuration_file(root_,owner_);
        }
        if(!settings.value){result.status={app::StorageErrorCode::io,"saved backup settings unavailable; no fallback"};return result;}
        const auto token=installation::unique_id();std::string transaction;
        for(const auto ch:token){if(!((ch>=L'0' && ch<=L'9') || (ch>=L'A' && ch<=L'F'))){result.status={app::StorageErrorCode::internal,"maintenance backup identity"};return result;}transaction.push_back(static_cast<char>(ch));}
        const auto snapshot=root_/L"Work"/(L"message-maintenance-"+token+L".sqlite");
        std::unique_ptr<FileBackupSnapshot> snapshot_cleanup,identity_cleanup;
        struct AttentionOnFailure {
            OfflineSession& session;OfflineMaintenanceBackupResult& result;std::unique_ptr<FileBackupSnapshot>& snapshot;std::string operation;
            ~AttentionOnFailure(){if(snapshot&&!result.status)try{session.preserved_work_attention("Offline backup "+operation,"offline_backup_incomplete_work_preserved");}catch(...){}}
        } attention{*this,result,snapshot_cleanup,transaction};
        try{
            auto collection=open_backup_collection(root_,owner_,archive::BackupKind::manual,settings.value->backup_destination);
            const auto files=storage_->file_configuration({true,{}});installation::require(files&&files.value,"file_backup_configuration_unavailable");
            auto contents=FileBackupCapture::begin(root_,owner_,*files.value,collection,archive::BackupKind::manual,cancelled);
            if(cancelled && cancelled()){result.status={app::StorageErrorCode::busy,"maintenance backup cancelled"};return result;}
            if(GetFileAttributesW(snapshot.c_str())!=INVALID_FILE_ATTRIBUTES){result.status={app::StorageErrorCode::conflict,"maintenance snapshot collision; existing file retained"};return result;}
            const auto database=storage_->create_backup(snapshot);
            if(!database.status){result.status=database.status;return result;}
            snapshot_cleanup=std::make_unique<FileBackupSnapshot>(FilePayloadDirectory::private_store(root_/L"Work",owner_),snapshot.filename().u8string());
            const auto valid=storage_->validate_backup(snapshot);
            if(!valid){result.status=valid;return result;}
            archive::ManagedBackupRequest request;request.installation_root=root_;request.destination_directory=collection.directory;
            const auto receipt=installation::read_receipt(root_,owner_);
            if(receipt.ftn_layout)installation::verify_ftn_directory(receipt);
            request.ftn_layout=receipt.ftn_layout;
            request.kind=archive::BackupKind::manual;request.transaction_id=transaction;request.source_generation=std::to_string(receipt.generation);
            request.created=archive::current_timestamp();request.database_snapshot=snapshot;request.cancelled=cancelled;
            const auto current_files=storage_->file_configuration({true,{}});installation::require(current_files&&current_files.value,"file_backup_configuration_unavailable");contents->bind(request,*current_files.value);
            const auto identity_path=root_/L"Work"/(L"file-identities-"+token+L".snapshot");
            const auto identities=refresh_portable_file_snapshot(root_,owner_,false,identity_path,cancelled);
            installation::require(identities.published,"backup_file_identity_snapshot_incomplete_preserved");
            identity_cleanup=std::make_unique<FileBackupSnapshot>(FilePayloadDirectory::private_store(root_/L"Work",owner_),identity_path.filename().u8string());
            request.portable_identity_snapshot=identity_path;
            const auto saved=archive::create_managed_backup(request);
            if(!saved.result.ok()){result.status={app::StorageErrorCode::io,"maintenance backup archive failed; nothing deleted/compacted"};return result;}
            result.archive=saved.archive_path;
            WindowsStorageDaclPolicy policy(owner_);result.status=policy.apply(saved.archive_path,StorageDaclObjectKind::file);
            if(!result.status)return result;
            if(!archive::inspect_backup(saved.archive_path).result.ok()){result.status={app::StorageErrorCode::io,"maintenance backup verification failed; scratch preserved"};return result;}
            const bool content_cleanup=contents->cleanup(),removed=snapshot_cleanup->cleanup(),identities_removed=identity_cleanup->cleanup();
            if(!removed || !content_cleanup || !identities_removed){result.status={app::StorageErrorCode::io,"maintenance backup owned snapshot removal unconfirmed"};return result;}
            result.pruned=finish_backup_collection(root_,owner_,collection,saved.archive_path,archive::BackupKind::manual,settings.value->retained_backups,cancelled);
            result.status=app::StorageStatus::success();return result;
        }catch(...){result.status={app::StorageErrorCode::io,"maintenance backup failed; scratch preserved; requested backup prerequisite not met"};return result;}
    }
    OfflineCompactionResult compact(std::function<bool()> cancelled){
        OfflineCompactionResult result;
        const auto backup=maintenance_backup(cancelled);
        if(!backup.status){result.status=backup.status;return result;}
        result.verified_backup=backup.archive;
        if(cancelled && cancelled()){result.status={app::StorageErrorCode::busy,"compaction cancelled after backup; no compaction begun"};return result;}
        try{
        const auto temp=root_/L"Work"/(L"Compaction-"+installation::unique_id());
        auto descriptor=installation::mutable_descriptor(owner_);installation::create_directory(temp,descriptor.get());
        const auto work_pin=installation::open_object(root_/L"Work",true,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        auto temp_pin=installation::open_object(temp,true,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        const auto stopped=storage_->shutdown();if(!stopped){result.status=stopped;return result;}
        const auto compacted=storage_->compact_offline(temp,std::move(cancelled));
        result.status=compacted.status;result.bytes_before=compacted.bytes_before;result.bytes_after=compacted.bytes_after;
        result.logical_content_verified=compacted.logical_content_verified;result.physical_compaction_committed=compacted.physical_compaction_committed;
        temp_pin.reset();std::error_code error;
        if(std::filesystem::is_empty(temp,error) && !error)RemoveDirectoryW(temp.c_str()); // exact new owned empty folder only
        return result;
        }catch(...){result.status={app::StorageErrorCode::io,"compaction preparation failed; verified prerequisite backup retained"};return result;}
    }
    app::LocalAccountResult change_classic_information(const app::ClassicInformationChange& change) {
        if(!guard_.acquired())return {app::LocalAccountResultCode::unauthorized};
        return app::LocalClassicInformationAdministration(*storage_,identifiers_,clock_,authority_).execute(change);
    }
    app::LocalAccountResult change_security(const app::LocalSecurityChange& change) {
        if (authority_ != app::LocalAccountAuthority::installation_owner || !guard_.acquired())
            return {app::LocalAccountResultCode::unauthorized};
        return app::LocalSecurityAdministration(*storage_,*this,identifiers_,clock_,authority_).execute(change);
    }
    app::StorageStatus clear_throttle(const app::LocalThrottleClearRequest& request) {
        if (authority_ != app::LocalAccountAuthority::installation_owner || !guard_.acquired())
            return {app::StorageErrorCode::permission_denied,"owner-only offline throttle clear"};
        const auto now = clock_.unix_seconds();
        adapters::authentication::Phase3aRateControlCoordinator rates(*storage_,now,4095);
        return rates.clear_locally(request,{identifiers_.generate(),identifiers_.generate(),{},now});
    }
    std::string fingerprint(std::string_view definition) override {
        auto hash = installation::sha256({definition.begin(),definition.end()});
        std::transform(hash.begin(),hash.end(),hash.begin(),[](char value) {
            return value >= 'A' && value <= 'F' ? static_cast<char>(value-'A'+'a') : value;
        });
        return hash;
    }
    app::StorageStatus publish(const app::LocalSecuritySnapshot& truth) override {
        return reconcile_security_snapshot(root_,owner_,truth,configuration_);
    }
    bool revoke(const app::SessionRevocationCommand&) override {
        // Exclusive Runtime-named guard is retained through storage shutdown.
        // There are no live sessions; historical incomplete leases are untouched.
        return guard_.acquired();
    }
    bool preserved_work_attention(std::string resource,std::string diagnostic,app::AttentionKind kind=app::AttentionKind::work_preserved){
        WindowsDurableJournalFiles files(root_,owner_);WindowsBbsLogTimestamp timestamp;
        if(!timestamp.configure(configuration_.bbs_time_zone)||!files.configure(configuration_.logging))return false;
        app::DurableJournalPublisher publisher(*storage_,files,[&](std::int64_t utc){return timestamp.format(utc);});
        (void)publisher.configure(configuration_.logging);
        app::AttentionRecord notice;notice.kind=kind;notice.resource=std::move(resource);notice.diagnostic=std::move(diagnostic);notice.utc_seconds=clock_.unix_seconds();
        return publisher.publish_attention(notice);
    }
private:
    OfflineInstallationGuard guard_;
    installation::Handle database_pin_;
    WindowsLocalStoragePathPolicy path_policy_;
    std::unique_ptr<adapters::storage::SqliteStorage> storage_;
    adapters::authentication::SodiumPasswordKdf kdf_{1, 4};
    adapters::authentication::PinnedSecListsBlocklist password_blocklist_;
    std::unique_ptr<app::AuthenticationSecurityService> authentication_;
    WindowsSecureIdentifierGenerator identifiers_;
    Clock clock_;
    app::OperationalConfiguration configuration_;
    app::LocalAccountAuthority authority_;
    std::filesystem::path root_;
    std::wstring owner_;
};
}

#include "offline_message_maintenance.inc"

struct OfflineConfigurationReadSession::Impl {OfflineSession session;explicit Impl(const std::filesystem::path& root,const std::wstring& owner):session(root,owner){}};
OfflineConfigurationReadSession::OfflineConfigurationReadSession(const std::filesystem::path& root,const std::wstring& owner):impl_(std::make_unique<Impl>(root,owner)){}
OfflineConfigurationReadSession::~OfflineConfigurationReadSession()=default;
std::optional<app::LocalControlFrame> OfflineConfigurationReadSession::query(const app::LocalControlFrame& request){
    using T=app::LocalControlMessageType;app::LocalControlFrame response;response.request_id=request.request_id;
    auto& s=impl_->session;
    switch(request.type){
    case T::account_query_request:{const auto q=app::decode_account_query(request.payload);if(!q)return {};response.type=T::account_query_response;response.payload=app::encode_account_snapshot(s.service().snapshot(*q));break;}
    case T::security_query_request:{const auto q=app::decode_account_query(request.payload);if(!q||q->account_id||!q->name_prefix.empty())return {};auto all=s.security_snapshot();if(q->offset>all.levels.size())return {};const auto end=std::min<std::size_t>(all.levels.size(),q->offset+app::kLocalAccountPageSize);all.has_more=end<all.levels.size();all.levels=std::vector<app::SecurityLevelRecord>(all.levels.begin()+q->offset,all.levels.begin()+end);response.type=T::security_query_response;response.payload=app::encode_security_snapshot(all);break;}
    case T::classic_information_query_request:{const auto q=app::decode_classic_information_query(request.payload);if(!q)return {};response.type=T::classic_information_query_response;response.payload=app::encode_classic_information_snapshot(s.classic_information_snapshot(*q));break;}
    case T::message_configuration_query_request:{const auto q=app::decode_message_configuration_query(request.payload);if(!q)return {};response.type=T::message_configuration_query_response;response.payload=app::encode_message_configuration_snapshot(s.message_configuration_snapshot(*q));break;}
    case T::file_configuration_query_request:{const auto q=app::decode_file_configuration_query(request.payload);if(!q)return {};response.type=T::file_configuration_query_response;response.payload=app::encode_file_configuration_snapshot(s.file_configuration_snapshot(*q));break;}
    case T::qwk_policy_query_request:if(!request.payload.empty())return {};response.type=T::qwk_policy_query_response;response.payload=app::encode_qwk_policy_result(s.qwk_policy_snapshot());break;
    case T::chat_configuration_query_request:if(!request.payload.empty())return {};response.type=T::chat_configuration_query_response;response.payload=app::encode_chat_result(s.chat_configuration());break;
    case T::door_configuration_query_request:{const auto q=app::decode_door_configuration_query(request.payload);if(!q)return {};const auto catalog=s.door_configuration();if(!catalog||!catalog.value)return {};const auto bytes=app::encode_door_catalog(*catalog.value);if(bytes.empty()||q->offset>=bytes.size()||(q->revision&&q->revision!=catalog.value->revision))return {};app::LocalDoorSnapshot part;part.code=app::LocalAccountResultCode::ok;part.revision=catalog.value->revision;part.offset=q->offset;part.total=static_cast<std::uint32_t>(bytes.size());const auto end=std::min(bytes.size(),q->offset+app::kLocalDoorChunkBytes);part.bytes={bytes.begin()+q->offset,bytes.begin()+end};response.type=T::door_configuration_query_response;response.payload=app::encode_door_configuration_snapshot(part);break;}
#include "offline_door_bulletin_routes.inc"
    case T::ftn_configuration_request:{if(!request.payload.empty())return {};const auto c=s.ftn_configuration();if(!c||!c.value)return {};response.type=T::ftn_configuration_response;response.payload=app::encode_ftn_configuration(*c.value);break;}
    case T::ftn_held_request:{const auto q=app::decode_ftn_held_request(request.payload);if(!q||q->index()>1)return {};response.type=T::ftn_held_response;response.payload=app::encode_ftn_held_response(s.ftn_held(*q));break;}
    default:return {};
    }
    if(response.payload.empty())return {};return response;
}

application::LocalAccountSnapshot offline_account_snapshot(const std::filesystem::path& root,
    const std::wstring& owner, const application::LocalAccountQuery& query) {
    try { OfflineSession session(root, owner); return session.service().snapshot(query); }
    catch (...) { return {}; }
}
application::LocalAccountResult offline_administer_account(const std::filesystem::path& root,
    const std::wstring& owner, application::LocalAccountRequest& request) {
    struct Erase { application::LocalAccountRequest& value;
        ~Erase() { application::erase_local_account_secrets(value); } } erase{request};
    try {
        OfflineSession session(root, owner);
        application::CancellationSource cancellation;
        return session.service().execute(request, cancellation.token());
    } catch (...) { return {}; }
}
application::LocalAccountSnapshot elevated_system_password_snapshot(const std::filesystem::path& root) {
    try {
        const auto receipt = installation::verify_configuration_recovery_entry(root);
        OfflineSession session(root, receipt.owner, app::LocalAccountAuthority::elevated_administrator);
        return session.service().snapshot({});
    } catch (...) { return {}; }
}
application::LocalAccountResult elevated_replace_system_password(const std::filesystem::path& root,
    application::LocalAccountRequest& request) {
    struct Erase { application::LocalAccountRequest& value;
        ~Erase() { application::erase_local_account_secrets(value); } } erase{request};
    if (request.action != app::LocalAccountAction::replace_system_password)
        return {app::LocalAccountResultCode::unauthorized};
    try {
        const auto receipt = installation::verify_configuration_recovery_entry(root);
        OfflineSession session(root, receipt.owner, app::LocalAccountAuthority::elevated_administrator);
        application::CancellationSource cancellation;
        return session.service().execute(request,cancellation.token());
    } catch (...) { return {}; }
}
application::LocalSecuritySnapshot offline_security_snapshot(const std::filesystem::path& root,
    const std::wstring& owner) {
    try { OfflineSession session(root,owner); return session.security_snapshot(); }
    catch (...) { return {}; }
}
application::LocalAccountResult offline_change_security_policy(const std::filesystem::path& root,
    const std::wstring& owner, const application::LocalSecurityChange& change) {
    try { OfflineSession session(root,owner); return session.change_security(change); }
    catch (...) { return {}; }
}
application::StorageStatus offline_clear_throttle(const std::filesystem::path& root,
    const std::wstring& owner,const application::LocalThrottleClearRequest& request) {
    if (!app::valid_local_throttle_clear(request))
        return {app::StorageErrorCode::invalid_argument,"invalid local throttle clear"};
    try { OfflineSession session(root,owner); return session.clear_throttle(request); }
    catch (...) { return {app::StorageErrorCode::internal,"offline throttle clear unavailable"}; }
}
application::LocalClassicInformationSnapshot offline_classic_information_snapshot(const std::filesystem::path& root,const std::wstring& owner,std::int64_t after) {
    try { OfflineSession session(root,owner);return session.classic_information_snapshot(after); }catch(...){return {};}
}
application::LocalAccountResult offline_change_classic_information(const std::filesystem::path& root,const std::wstring& owner,const application::ClassicInformationChange& change) {
    try { OfflineSession session(root,owner);return session.change_classic_information(change); }catch(...){return {};}
}
application::LocalMessageConfigurationSnapshot offline_message_configuration_snapshot(const std::filesystem::path& root,const std::wstring& owner,const application::LocalMessageConfigurationQuery& query) {
    try {OfflineSession session(root,owner);return session.message_configuration_snapshot(query);}catch(...){return {};}
}
application::LocalMessageConfigurationResult offline_change_message_configuration(const std::filesystem::path& root,const std::wstring& owner,const application::MessageConfigurationChange& change) {
    try {OfflineSession session(root,owner);return session.change_message_configuration(change);}catch(...){return {};}
}
application::LocalQwkPolicyResult offline_qwk_policy_snapshot(const std::filesystem::path& root,const std::wstring& owner){try{OfflineSession session(root,owner);return session.qwk_policy_snapshot();}catch(...){return {};}}
application::StorageResult<application::FtnConfiguration> offline_ftn_configuration(const std::filesystem::path& root,const std::wstring& owner){try{OfflineSession session(root,owner);return session.ftn_configuration();}catch(...){return {{application::StorageErrorCode::io,"offline FTN configuration unavailable"},{}};}}
application::FtnHeldControlResponse offline_ftn_held(const std::filesystem::path& root,const std::wstring& owner,const application::FtnHeldControlRequest& request){try{OfflineSession session(root,owner);return session.ftn_held(request);}catch(...){return {};}}
application::StorageResult<application::FtnRetentionPreview> offline_preview_ftn_retention(const std::filesystem::path& root,const std::wstring& owner,const application::FtnTransferToken& token,const application::FtnConfiguration& c){try{OfflineSession session(root,owner);return session.preview_ftn_retention(token,c);}catch(...){return {{application::StorageErrorCode::io,"offline FTN retention review unavailable"},{}};}}
application::StorageResult<application::FtnConfigurationChangeResult> offline_change_ftn_configuration(const std::filesystem::path& root,const std::wstring& owner,const application::FtnTransferToken& token,const application::FtnConfiguration& c){try{OfflineSession session(root,owner);return session.change_ftn_configuration(token,c);}catch(...){return {{application::StorageErrorCode::io,"offline FTN configuration save unconfirmed"},{}};}}
application::ChatConfigurationResult offline_chat_configuration(const std::filesystem::path& root,const std::wstring& owner){try{OfflineSession session(root,owner);return session.chat_configuration();}catch(...){return {};}}
application::ChatConfigurationResult offline_change_chat_configuration(const std::filesystem::path& root,const std::wstring& owner,const application::ChatConfiguration& c){try{OfflineSession session(root,owner);return session.change_chat_configuration(c);}catch(...){return {};}}
application::StorageResult<application::DoorCatalog> offline_door_configuration(const std::filesystem::path& root,const std::wstring& owner){try{OfflineSession session(root,owner);return session.door_configuration();}catch(...){return {{application::StorageErrorCode::io,"offline Door catalog unavailable"},{}};}}
application::LocalDoorResult offline_change_door_configuration(const std::filesystem::path& root,const std::wstring& owner,const application::DoorConfigurationChange& c){try{OfflineSession session(root,owner);return session.change_door_configuration(c);}catch(...){return {};}}
application::StorageResult<application::DoorBulletinCatalog> offline_door_bulletin_configuration(const std::filesystem::path& root,const std::wstring& owner){try{OfflineSession session(root,owner);return session.door_bulletin_configuration();}catch(...){return {{application::StorageErrorCode::io,"offline Door Bulletin configuration unavailable"},{}};}}
application::DoorBulletinPreview offline_preview_door_bulletin(const std::filesystem::path& root,const std::wstring& owner,application::DoorBulletinId id,bool ansi,std::int64_t expected_revision){try{OfflineSession session(root,owner);return session.preview_door_bulletin(id,ansi,expected_revision);}catch(...){application::DoorBulletinPreview p;p.id=id;p.title="Door Bulletin "+std::to_string(id.value);p.diagnostic="Stopped BBS access or source validation failed. Reload and check BBS Health Check.";return p;}}
application::LocalDoorResult offline_change_door_bulletin(const std::filesystem::path& root,const std::wstring& owner,const application::DoorBulletinChange& c){try{OfflineSession session(root,owner);return session.change_door_bulletin(c);}catch(...){return {};}}
application::LocalFileConfigurationSnapshot offline_file_configuration_snapshot(const std::filesystem::path& root,const std::wstring& owner,const application::LocalFileConfigurationQuery& query){try{OfflineSession session(root,owner);return session.file_configuration_snapshot(query);}catch(...){return {};}}
application::StorageResult<application::FileConfiguration> offline_file_configuration_catalog(const std::filesystem::path& root,const std::wstring& owner){try{OfflineSession session(root,owner);return session.file_configuration_catalog();}catch(...){return {{application::StorageErrorCode::io,"offline file catalog unavailable"},{}};}}
application::LocalFileConfigurationResult offline_change_file_configuration(const std::filesystem::path& root,const std::wstring& owner,const application::FileConfigurationChange& change){try{OfflineSession session(root,owner);return session.change_file_configuration(change);}catch(...){return {};}}
application::LocalQwkPolicyResult offline_change_qwk_policy(const std::filesystem::path& root,const std::wstring& owner,const application::QwkPolicyChange& c){try{OfflineSession session(root,owner);return session.change_qwk_policy(c);}catch(...){return {};}}
OfflineMaintenanceBackupResult offline_create_backup(const std::filesystem::path& root,const std::wstring& owner,std::function<bool()> cancelled){
    try{OfflineSession session(root,owner);return session.maintenance_backup(std::move(cancelled));}
    catch(...){return {{app::StorageErrorCode::io,"exclusive offline backup unavailable"},{},0};}
}
struct OfflineFileContentRestore::Impl {
    OfflineSession session;BackupCollection collection;std::unique_ptr<FileContentRestore> restore;
    Impl(const std::filesystem::path& root,const std::wstring& owner,const std::filesystem::path& point,const std::vector<std::int64_t>& areas):session(root,owner){
        const auto config=session.file_configuration_catalog();installation::require(config&&config.value,"file_restore_configuration_unavailable");
        const auto kind=point.parent_path()==root/L"Backup"/L"Upgrades"?archive::BackupKind::setup_upgrade:archive::BackupKind::manual;
        collection=open_backup_collection(root,owner,kind,point.parent_path().u8string(),false);
        restore=FileContentRestore::preview(root,owner,*config.value,collection,point,areas);
    }
};
OfflineFileContentRestore::OfflineFileContentRestore(std::unique_ptr<Impl> p):impl_(std::move(p)){}
OfflineFileContentRestore::~OfflineFileContentRestore()=default;
std::unique_ptr<OfflineFileContentRestore> OfflineFileContentRestore::begin(const std::filesystem::path& root,const std::wstring& owner,const std::filesystem::path& point,const std::vector<std::int64_t>& areas,std::string& diagnostic){
    try{return std::unique_ptr<OfflineFileContentRestore>(new OfflineFileContentRestore(std::make_unique<Impl>(root,owner,point,areas)));}
    catch(const std::exception& e){diagnostic=e.what();return {};}
}
const FileContentRestore& OfflineFileContentRestore::preview()const noexcept{return *impl_->restore;}
FileContentRestoreResult OfflineFileContentRestore::execute(){
    const auto config=impl_->session.file_configuration_catalog();if(!config||!config.value)return {0,0,0,false,"file_restore_configuration_unavailable"};
    auto result=impl_->restore->execute(*config.value);
    if(!result.completed&&result.diagnostic.find("retained")!=std::string::npos){
        bool logged{};try{logged=impl_->session.preserved_work_attention("File contents restore","file_restore_incomplete_partial_preserved");}catch(...){}
        if(!logged)result.diagnostic+="; HEYSYSOP.LOG notice could not be confirmed";
    }
    return result;
}
struct OfflineFtnSerialRestore::Impl {
    std::filesystem::path root;std::wstring owner;const OfflineInstallationGuard& guard;
    installation::Receipt receipt;std::optional<app::FtnSerialState> prior;bool held{};
    Impl(const std::filesystem::path& path,const std::wstring& who,const OfflineInstallationGuard& lock):root(path),owner(who),guard(lock){
        installation::require(guard.acquired(),"ftn_restore_exclusive_guard_required");installation::verify_runtime_installation(root,owner);receipt=installation::read_receipt(root,owner);
        // Discovery is read-only; a missing/corrupt/experimental current store
        // is unknown, never a fabricated zero and never repaired here.
        try {
            WindowsStorageDaclPolicy security(owner);installation::require(security.assess(root/L"Data"/L"SFX.DB",StorageDaclObjectKind::file).status.succeeded(),"ftn_restore_current_security_unknown");
            auto pin=installation::open_object(root/L"Data"/L"SFX.DB",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);LARGE_INTEGER size{};installation::require(GetFileSizeEx(pin.get(),&size)&&size.QuadPart>0,"ftn_restore_current_history_unknown");
            WindowsLocalStoragePathPolicy paths(owner);adapters::storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";adapters::storage::SqliteStorage storage(settings,paths);
            const auto state=storage.verified_ftn_serial_history(root/L"Data"/L"SFX.DB");if(state&&state.value)prior=*state.value;
        }catch(const std::exception&){prior.reset();}
    }
    app::StorageStatus publish(){
        try {
            installation::require(guard.acquired(),"ftn_restore_exclusive_guard_lost");installation::verify_runtime_installation(root,owner);const auto current=installation::read_receipt(root,owner);installation::require(current.root_id==receipt.root_id&&current.volume==receipt.volume,"ftn_restore_installation_changed");
            WindowsStorageDaclPolicy security(owner);installation::require(security.assess(root/L"Data"/L"SFX.DB",StorageDaclObjectKind::file).status.succeeded(),"ftn_restore_candidate_security_invalid");
            auto pin=installation::open_object(root/L"Data"/L"SFX.DB",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);LARGE_INTEGER size{};installation::require(GetFileSizeEx(pin.get(),&size)&&size.QuadPart>0,"ftn_restore_candidate_missing");
            WindowsLocalStoragePathPolicy paths(owner);adapters::storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";adapters::storage::SqliteStorage storage(settings,paths);
            const auto version=storage.verified_schema_version(root/L"Data"/L"SFX.DB");if(!version)return version.status;
            const auto started=storage.start();if(!started)return started;
            auto result=storage.preserve_ftn_serial_history({true,{}},prior,*version.value<22);
            if(result){const auto state=storage.ftn_serial_state({true,{}});if(!state||!state.value)result=state.status;else held=state.value->recovery_required;}
            const auto stopped=storage.shutdown();return result?stopped:result;
        }catch(const std::exception&){return {app::StorageErrorCode::io,"FTN serial restore publication failed"};}
    }
};
OfflineFtnSerialRestore::OfflineFtnSerialRestore(std::unique_ptr<Impl> value):impl_(std::move(value)){}
OfflineFtnSerialRestore::~OfflineFtnSerialRestore()=default;
std::unique_ptr<OfflineFtnSerialRestore> OfflineFtnSerialRestore::capture(const std::filesystem::path& root,const std::wstring& owner,const OfflineInstallationGuard& guard){return std::unique_ptr<OfflineFtnSerialRestore>(new OfflineFtnSerialRestore(std::make_unique<Impl>(root,owner,guard)));}
app::StorageStatus OfflineFtnSerialRestore::publish(){return impl_->publish();}
bool OfflineFtnSerialRestore::recovery_required()const noexcept{return impl_->held;}
#include "ftn_history_recovery.inc"
} // namespace sfx::platform::win32
