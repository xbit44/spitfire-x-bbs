#pragma once

#include "sfx/application/message_maintenance.hpp"
#include "sfx/application/scheduler.hpp"

namespace sfx::application {

inline const char* message_job_id(MessageMaintenanceKind kind) noexcept {
    return kind==MessageMaintenanceKind::retention?kMessageRetentionJobId:kMessageIntegrityJobId;
}
struct MessageMaintenanceScheduleRequest {
    MessageMaintenanceKind kind{MessageMaintenanceKind::retention};
    std::uint64_t expected_revision{};
    bool enabled{};
    std::uint8_t hour{},minute{};
    std::optional<std::uint8_t> weekday;
};
struct MessageMaintenanceStatus {
    MessageMaintenanceKind kind{MessageMaintenanceKind::retention};
    bool available{};
    bool shared_verification_required{};
    std::string diagnostic;
    SchedulerConfiguration configuration{default_message_scheduler_configuration(kMessageRetentionJobId)};
    SchedulerState state{default_message_scheduler_state(kMessageRetentionJobId)};
    std::optional<MessageMaintenanceRun> latest;
    std::vector<MessageIntegrityFinding> holds;
};

} // namespace sfx::application
