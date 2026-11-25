#pragma once
#include "sfx/platform/linux/file_scanner.hpp"
#include <array>
#include <optional>

namespace sfx::platform::linux_host {
enum class LinuxClamAvUpdateMode { automatic, manual };
enum class LinuxClamAvDefinitionChange { unknown, initial, updated, current };
struct LinuxClamAvDatabaseVersion {
    std::uint32_t version{},signatures{};
    std::int64_t published{};
};
struct LinuxClamAvStatus {
    bool installed{},verified{},definitions_present{},definitions_verified{},history_available{},busy{};
    std::int64_t last_attempt{},last_success{},last_change{};
    LinuxClamAvUpdateMode last_mode{LinuxClamAvUpdateMode::automatic};
    LinuxClamAvDefinitionChange change{LinuxClamAvDefinitionChange::unknown};
    std::string version,diagnostic,last_result;
    std::array<std::optional<LinuxClamAvDatabaseVersion>,3> databases;
};
struct LinuxClamAvUpdateResult {
    bool attempted{},succeeded{},cancelled{},process_reaped{true},cleanup_complete{true};
    std::uint32_t exit_code{0xffffffffU},daily_version{};
    LinuxClamAvDefinitionChange change{LinuxClamAvDefinitionChange::unknown};
    std::string diagnostic;
};
// The trusted host owns fixed paths beneath one portable installation. No shell,
// daemon, global configuration, browser-supplied mirror or privileged operation.
// Status verifies saved fingerprints; a parsed CVD header alone proves nothing.
LinuxClamAvStatus inspect_linux_clamav(const std::filesystem::path&,
    const std::function<bool()>& cancelled={}) noexcept;
// Called only after the optional pinned component is present. Establish fixed
// private working/data directories; never recreate a lost initialized db set.
void initialize_linux_clamav_data(const std::filesystem::path&);
LinuxClamAvUpdateResult update_linux_clamav(const std::filesystem::path&,
    std::int64_t utc_seconds,LinuxClamAvUpdateMode=LinuxClamAvUpdateMode::automatic,
    const std::function<bool()>& cancelled={},
    const std::function<void(std::uint64_t)>& activity={},
    std::uint32_t deadline_milliseconds=180000) noexcept;
}
