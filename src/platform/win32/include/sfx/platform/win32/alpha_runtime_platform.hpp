#pragma once

#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/operational_configuration.hpp"
#include "sfx/application/scheduler.hpp"
#include "sfx/application/storage.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace sfx::platform::win32 {

inline constexpr wchar_t kShippingInstallationRoot[] = L"C:\\SFX";

std::filesystem::path alpha_installation_root();

struct InstallationObjectNames {
    std::wstring pipe_name;
    std::wstring mutex_name;
    std::wstring configuration_pipe_name;
    std::wstring configuration_mutex_name;
};

InstallationObjectNames installation_object_names(
    const std::filesystem::path& installation_root);

enum class ConfigurationFileState {
    missing,
    loaded,
    invalid,
    io_failure,
};

struct ConfigurationFileReadResult {
    ConfigurationFileState state{ConfigurationFileState::io_failure};
    std::optional<application::OperationalConfiguration> configuration;
    std::string diagnostic_code;
};

template <typename Value>
struct SchedulerFileReadResult final {
    ConfigurationFileState state{ConfigurationFileState::io_failure};
    std::optional<Value> value;
    std::string diagnostic_code;
};

using SchedulerConfigurationFileReadResult =
    SchedulerFileReadResult<application::SchedulerConfiguration>;
using SchedulerStateFileReadResult =
    SchedulerFileReadResult<application::SchedulerState>;

application::StorageStatus provision_alpha_installation_tree(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid);
application::StorageStatus assess_alpha_bin_directory(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid);
// Read-only, after protected provisioning and the instance guard, before SQLite
// or host-key creation. Empty directories and UI preferences are not identity.
application::StorageResult<bool> alpha_installation_has_persistent_state(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const std::function<bool(const std::filesystem::path&)>& verified_component_metadata = {});
// Exact private Setup document-tracking receipts only. No adoption or repair.
bool stock_docs_bootstrap_metadata(const std::filesystem::path& root,
    const std::filesystem::path& candidate,const std::wstring& owner) noexcept;
ConfigurationFileReadResult read_operational_configuration_file(
    const std::filesystem::path& installation_root);
// Called only under Runtime/offline exclusivity with database-authoritative
// policy. Repairs missing/stale catalog snapshots and older binding revisions;
// same-revision disagreement, future revisions and invalid files refuse.
application::StorageStatus reconcile_security_snapshot(
    const std::filesystem::path& root, const std::wstring& owner,
    const application::LocalSecuritySnapshot& truth,
    application::OperationalConfiguration& configuration);
ConfigurationFileReadResult read_pending_operational_configuration_file(
    const std::filesystem::path& installation_root);
application::StorageStatus publish_operational_configuration_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const application::OperationalConfiguration& configuration);
application::StorageStatus publish_pending_operational_configuration_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const application::OperationalConfiguration& configuration);
application::StorageStatus remove_pending_operational_configuration_file(
    const std::filesystem::path& installation_root);
SchedulerConfigurationFileReadResult read_scheduler_configuration_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    std::string_view job_id=application::kBackupJobId);
SchedulerStateFileReadResult read_scheduler_state_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    std::string_view job_id=application::kBackupJobId);
application::StorageStatus publish_scheduler_configuration_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const application::SchedulerConfiguration& configuration);
// Host must hold OfflineInstallationGuard through the entire supported restore.
// The old restored clean result must never clear the current-store prerequisite.
application::StorageStatus invalidate_restored_message_verification(const std::filesystem::path& root,const std::wstring& owner);
application::StorageStatus publish_scheduler_state_file(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const application::SchedulerState& state);
std::optional<std::int64_t> next_local_daily_run(
    std::int64_t after_utc, std::uint8_t local_hour,
    std::uint8_t local_minute) noexcept;

std::optional<application::ControlPanelUiPreferences> read_control_panel_ui_preferences(
    const std::filesystem::path& installation_root) noexcept;
application::StorageStatus publish_control_panel_ui_preferences(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const application::ControlPanelUiPreferences& preferences);
bool read_node_one_enabled(const std::filesystem::path& installation_root) noexcept;
// Versioned 128-node state. Read v2's four flags without changing its choices.
// Missing new state upgrades the old stop-all flag;
// the old file is retained byte-for-byte for offline rollback. Invalid state is
// a provisioning failure, never an implicit enable/default.
application::StorageResult<application::NodeEnablement> load_alpha_node_controls(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid);
application::StorageStatus publish_alpha_node_controls(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    const application::NodeEnablement& enabled);
application::StorageStatus publish_node_one_enabled(
    const std::filesystem::path& installation_root,
    const std::wstring& installation_owner_sid,
    bool enabled);

class RuntimeInstanceGuard final {
public:
    RuntimeInstanceGuard() = default;
    ~RuntimeInstanceGuard();
    RuntimeInstanceGuard(const RuntimeInstanceGuard&) = delete;
    RuntimeInstanceGuard& operator=(const RuntimeInstanceGuard&) = delete;

    application::StorageStatus acquire(
        const std::filesystem::path& installation_root,
        const std::wstring& installation_owner_sid);
    bool acquired() const noexcept;

private:
    HANDLE mutex_{nullptr};
    HANDLE lock_file_{INVALID_HANDLE_VALUE};
};

// Configuration's stopped-system transaction guard.  It owns the same
// installation-scoped mutex as Runtime, but deliberately does not open the
// Data lock file so an authorized offline restore can exchange the Data tree.
class OfflineInstallationGuard final {
public:
    OfflineInstallationGuard() = default;
    ~OfflineInstallationGuard();
    OfflineInstallationGuard(const OfflineInstallationGuard&) = delete;
    OfflineInstallationGuard& operator=(const OfflineInstallationGuard&) = delete;

    application::StorageStatus acquire(
        const std::filesystem::path& installation_root,
        const std::wstring& installation_owner_sid);
    bool acquired() const noexcept;
private:
    HANDLE mutex_{nullptr};
};

class NamedPipeConnection final {
public:
    NamedPipeConnection() = default;
    explicit NamedPipeConnection(HANDLE handle) noexcept;
    ~NamedPipeConnection();
    NamedPipeConnection(const NamedPipeConnection&) = delete;
    NamedPipeConnection& operator=(const NamedPipeConnection&) = delete;
    NamedPipeConnection(NamedPipeConnection&& other) noexcept;
    NamedPipeConnection& operator=(NamedPipeConnection&& other) noexcept;

    bool valid() const noexcept;
    HANDLE native_handle() const noexcept;
    bool read_frame(application::LocalControlFrame& frame) const;
    bool write_frame(const application::LocalControlFrame& frame) const;
    void disconnect() noexcept;

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

NamedPipeConnection accept_local_control_client(
    const std::wstring& pipe_name,
    const std::wstring& installation_owner_sid,
    DWORD expected_client_process_id,
    DWORD timeout_millis,
    std::string* diagnostic_code = nullptr);
NamedPipeConnection connect_local_control_server(
    const std::wstring& pipe_name,
    DWORD expected_server_process_id,
    DWORD timeout_millis,
    const std::function<bool()>& cancelled = {});

bool fill_secure_random(void* destination, std::size_t bytes) noexcept;

}  // namespace sfx::platform::win32
