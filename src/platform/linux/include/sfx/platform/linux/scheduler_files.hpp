#pragma once
#include "sfx/application/account_storage.hpp"
#include "sfx/application/scheduler.hpp"
#include <filesystem>

namespace sfx::platform::linux_host {
enum class SchedulerFileState { missing, loaded, invalid, io_failure };
template<class T> struct SchedulerFileReadResult {
    SchedulerFileState state{SchedulerFileState::io_failure};
    std::optional<T> value;
    std::string diagnostic_code;
};
using SchedulerConfigurationFileReadResult=SchedulerFileReadResult<application::SchedulerConfiguration>;
using SchedulerStateFileReadResult=SchedulerFileReadResult<application::SchedulerState>;
// Same bounded formats and names as Windows. The installation owner holds
// exclusive management ownership. No implicit repair or symlink following.
SchedulerConfigurationFileReadResult read_scheduler_configuration_file(const std::filesystem::path&,
    std::string_view job=application::kBackupJobId);
SchedulerStateFileReadResult read_scheduler_state_file(const std::filesystem::path&,
    std::string_view job=application::kBackupJobId);
application::StorageStatus publish_scheduler_configuration_file(const std::filesystem::path&,
    const application::SchedulerConfiguration&);
application::StorageStatus publish_scheduler_state_file(const std::filesystem::path&,
    const application::SchedulerState&);
} // namespace sfx::platform::linux_host
