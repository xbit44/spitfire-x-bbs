#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sfx::application {

enum class StorageErrorCode : std::uint8_t {
    none,
    invalid_argument,
    not_found,
    conflict,
    not_due,
    busy,
    backpressure,
    invalid_location,
    permission_denied,
    full,
    constraint,
    corrupt,
    incompatible_schema,
    migration_failed,
    io,
    shutting_down,
    internal,
    system_sysop_protected
};

struct StorageStatus {
    StorageErrorCode code{StorageErrorCode::none};
    std::string operation;

    bool succeeded() const noexcept { return code == StorageErrorCode::none; }
    explicit operator bool() const noexcept { return succeeded(); }

    static StorageStatus success() { return {}; }
};

enum class StorageRuntimeState : std::uint8_t {
    stopped,
    starting,
    ready,
    degraded,
    recovery_required,
    stopping
};

struct StorageHealth {
    StorageRuntimeState state{StorageRuntimeState::stopped};
    StorageErrorCode last_error{StorageErrorCode::none};
    std::string last_operation;
    std::string engine_version;
    std::string engine_source_id;
    std::int32_t schema_version{0};
    std::size_t queued_operations{0};
    std::size_t queued_bytes{0};
    std::size_t queue_high_water_operations{0};
    std::size_t queue_high_water_bytes{0};
    std::uint64_t committed_operations{0};
    std::uint64_t committed_batches{0};
    std::size_t maximum_committed_batch_operations{0};
    std::uint64_t rejected_operations{0};
    std::uint64_t busy_retries{0};
    std::uint64_t checkpoints{0};
    std::uint64_t incomplete_checkpoints{0};
    std::int32_t last_checkpoint_log_frames{0};
    std::int32_t last_checkpointed_frames{0};
};

class IStorageRuntime {
public:
    virtual ~IStorageRuntime() = default;
    virtual StorageStatus start() = 0;
    virtual StorageStatus checkpoint() = 0;
    virtual StorageStatus shutdown() = 0;
    virtual StorageHealth health() const = 0;
};

}  // namespace sfx::application
