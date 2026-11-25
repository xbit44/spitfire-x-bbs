#pragma once
#include "sfx/application/chat.hpp"
#include "sfx/application/ftn_storage.hpp"
#include "sfx/application/ftn_history_recovery.hpp"

#include "sfx/application/local_account_administration.hpp"
#include "sfx/application/local_classic_information.hpp"
#include "sfx/application/local_message_administration.hpp"
#include "sfx/application/message_deletion.hpp"
#include "sfx/application/message_maintenance_control.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/platform/win32/file_backup.hpp"
#include <memory>
#include <filesystem>
#include <string>

namespace sfx::platform::win32 {
// One worker-owned read batch. Holds the ordinary exclusive offline guard and
// existing storage for the batch, then releases both before editing/saving.
// Only the explicitly supported query frames are accepted; no write dispatch.
class OfflineConfigurationReadSession final {
public:
    OfflineConfigurationReadSession(const std::filesystem::path&,const std::wstring&);
    ~OfflineConfigurationReadSession();
    std::optional<application::LocalControlFrame> query(const application::LocalControlFrame&);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
application::ChatConfigurationResult offline_chat_configuration(const std::filesystem::path&,const std::wstring&);
application::StorageResult<application::FtnConfiguration> offline_ftn_configuration(const std::filesystem::path&,const std::wstring&);
application::FtnHeldControlResponse offline_ftn_held(const std::filesystem::path&,const std::wstring&,const application::FtnHeldControlRequest&);
application::StorageResult<application::FtnRetentionPreview> offline_preview_ftn_retention(const std::filesystem::path&,const std::wstring&,const application::FtnTransferToken&,const application::FtnConfiguration&);
application::StorageResult<application::FtnConfigurationChangeResult> offline_change_ftn_configuration(const std::filesystem::path&,const std::wstring&,const application::FtnTransferToken&,const application::FtnConfiguration&);
application::ChatConfigurationResult offline_change_chat_configuration(const std::filesystem::path&,const std::wstring&,const application::ChatConfiguration&);
application::StorageResult<application::DoorCatalog> offline_door_configuration(const std::filesystem::path&,const std::wstring&);
application::LocalDoorResult offline_change_door_configuration(const std::filesystem::path&,const std::wstring&,const application::DoorConfigurationChange&);
application::StorageResult<application::DoorBulletinCatalog> offline_door_bulletin_configuration(const std::filesystem::path&,const std::wstring&);
application::DoorBulletinPreview offline_preview_door_bulletin(const std::filesystem::path&,const std::wstring&,application::DoorBulletinId,bool ansi,std::int64_t expected_revision=0);
application::LocalDoorResult offline_change_door_bulletin(const std::filesystem::path&,const std::wstring&,const application::DoorBulletinChange&);

// Typed, exclusive stopped-installation operations. Each call revalidates the
// protected installation and owner; it never starts or impersonates Runtime.
application::LocalAccountSnapshot offline_account_snapshot(
    const std::filesystem::path& root, const std::wstring& owner,
    const application::LocalAccountQuery& query);
application::LocalAccountResult offline_administer_account(
    const std::filesystem::path& root, const std::wstring& owner,
    application::LocalAccountRequest& request);
application::LocalAccountSnapshot elevated_system_password_snapshot(const std::filesystem::path& root);
application::LocalAccountResult elevated_replace_system_password(
    const std::filesystem::path& root, application::LocalAccountRequest& request);
application::LocalSecuritySnapshot offline_security_snapshot(
    const std::filesystem::path& root, const std::wstring& owner);
application::LocalAccountResult offline_change_security_policy(
    const std::filesystem::path& root, const std::wstring& owner,
    const application::LocalSecurityChange& change);
application::StorageStatus offline_clear_throttle(const std::filesystem::path& root,
    const std::wstring& owner,const application::LocalThrottleClearRequest& request);
application::LocalClassicInformationSnapshot offline_classic_information_snapshot(const std::filesystem::path&,const std::wstring&,std::int64_t after);
application::LocalAccountResult offline_change_classic_information(const std::filesystem::path&,const std::wstring&,const application::ClassicInformationChange&);
application::LocalMessageConfigurationSnapshot offline_message_configuration_snapshot(const std::filesystem::path&,const std::wstring&,const application::LocalMessageConfigurationQuery&);
application::LocalMessageConfigurationResult offline_change_message_configuration(const std::filesystem::path&,const std::wstring&,const application::MessageConfigurationChange&);
application::LocalFileConfigurationSnapshot offline_file_configuration_snapshot(const std::filesystem::path&,const std::wstring&,const application::LocalFileConfigurationQuery&);
application::LocalFileConfigurationResult offline_change_file_configuration(const std::filesystem::path&,const std::wstring&,const application::FileConfigurationChange&);
application::StorageResult<application::FileConfiguration> offline_file_configuration_catalog(const std::filesystem::path&,const std::wstring&);

struct OfflineMaintenanceBackupResult {
    application::StorageStatus status;
    std::filesystem::path archive;
    std::size_t pruned{};
};
struct OfflineCompactionResult {
    application::StorageStatus status;
    std::filesystem::path verified_backup;
    std::uintmax_t bytes_before{},bytes_after{};
    bool logical_content_verified{},physical_compaction_committed{};
};
OfflineMaintenanceBackupResult offline_create_backup(const std::filesystem::path&,const std::wstring&,std::function<bool()> cancelled={});
class OfflineFileContentRestore final {
public:
    static std::unique_ptr<OfflineFileContentRestore> begin(const std::filesystem::path&,const std::wstring&,
        const std::filesystem::path&,const std::vector<std::int64_t>&,std::string& diagnostic);
    ~OfflineFileContentRestore();
    const FileContentRestore& preview()const noexcept;
    FileContentRestoreResult execute();
private:
    struct Impl;explicit OfflineFileContentRestore(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
OfflineCompactionResult offline_compact_messages(const std::filesystem::path&,const std::wstring&,std::function<bool()> cancelled={});
// Retains the genuine installation guard and database pin through preview,
// optional backup, final Yes/No dialog and exact commit/reconciliation. Dropping
// the object cancels without deleting configuration or message content.
class OfflineMessageDeletion final {
public:
    static std::unique_ptr<OfflineMessageDeletion> begin(const std::filesystem::path&,const std::wstring&,
        const application::MessageDeletionTarget&,std::string& diagnostic,std::function<bool()> cancelled={});
    ~OfflineMessageDeletion();
    OfflineMessageDeletion(const OfflineMessageDeletion&)=delete;
    const application::MessageDeletionPreview& preview()const noexcept;
    OfflineMaintenanceBackupResult backup(std::function<bool()> cancelled={});
    application::StorageResult<application::MessageDeletionResult> commit(std::function<bool()> cancelled={});
private:
    class Impl;explicit OfflineMessageDeletion(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
application::MessageMaintenanceStatus offline_message_maintenance_status(const std::filesystem::path&,const std::wstring&,application::MessageMaintenanceKind);
application::AlphaControlResponse offline_configure_message_schedule(const std::filesystem::path&,const std::wstring&,const application::MessageMaintenanceScheduleRequest&);
application::LocalQwkPolicyResult offline_qwk_policy_snapshot(const std::filesystem::path&,const std::wstring&);
application::LocalQwkPolicyResult offline_change_qwk_policy(const std::filesystem::path&,const std::wstring&,const application::QwkPolicyChange&);

class OfflineInstallationGuard;
std::unique_ptr<application::IFtnHistoryRecovery> preview_ftn_history_recovery(const std::filesystem::path&,const std::wstring&);
class OfflineFtnSerialRestore final {
public:
    // Caller retains its existing exclusive guard throughout normal restore.
    static std::unique_ptr<OfflineFtnSerialRestore> capture(const std::filesystem::path&,
        const std::wstring&,const OfflineInstallationGuard&);
    ~OfflineFtnSerialRestore();
    application::StorageStatus publish();
    bool recovery_required() const noexcept;
private:
    struct Impl;explicit OfflineFtnSerialRestore(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace sfx::platform::win32
