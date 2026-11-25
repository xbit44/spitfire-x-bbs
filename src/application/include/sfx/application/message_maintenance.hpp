#pragma once

#include "sfx/application/message_storage.hpp"
#include <functional>

namespace sfx::application {

inline constexpr std::size_t kMessageRetentionBatchLimit = 32;
inline constexpr std::size_t kMessageMaintenanceHistoryLimit = 32;
inline constexpr std::uint64_t kMessageRetentionSqlBudgetMillis = 50;
inline constexpr std::uint64_t kMessageIntegritySqlBudgetMillis = 5000;
enum class MessageMaintenanceKind : std::uint8_t { retention, integrity };
enum class MessageMaintenanceOutcome : std::uint8_t { running, success, partial, failed, skipped, interrupted };
enum class MessageMaintenanceReason : std::uint8_t {
    none, corruption_hold, busy, policy_changed, cancelled, deadline, corrupt,
    storage_failure, stale, incomplete, runtime_restart
};
struct MessageMaintenanceEchoResult {
    LocalEchoId echo;
    std::int64_t removed{}, protected_overage{};
};
struct MessageMaintenanceRun {
    Identifier128 id;
    MessageMaintenanceKind kind{MessageMaintenanceKind::retention};
    std::int64_t started_utc{}, finished_utc{};
    MessageMaintenanceOutcome outcome{MessageMaintenanceOutcome::running};
    MessageMaintenanceReason reason{MessageMaintenanceReason::none};
    std::int64_t removed_public{}, expired_unread{}, committed_batches{};
    std::vector<MessageMaintenanceEchoResult> echoes;
    bool replayed{};
};
struct MessageRetentionBatch {
    MessageMaintenanceRun run;
    std::int64_t sequence{}, removed_public{}, expired_unread{};
    bool complete{}, replayed{}, policy_changed{};
};
// Scope zero is a shared-store fault, -1 is private mail, positive is a stable
// public EchoId. There is no claim of independent physical echo databases.
struct MessageIntegrityFinding {
    std::int64_t scope{}, count{};
};
struct MessageIntegrityResult {
    MessageMaintenanceRun run;
    bool complete{}, clean{}, cleared_hold{};
    std::vector<MessageIntegrityFinding> findings;
};
using MessageMaintenanceStop = std::function<bool()>;

// Trusted host-created authority only, never decoded from terminal/config data.
// Implementations own all SQL, current actor checks, stable operation receipts,
// bounded transactions and actual committed totals. Callers cannot supply limits,
// eligibility flags, source rows, counts or a different area to a batch retry.
class IMessageMaintenanceStorage {
public:
    virtual ~IMessageMaintenanceStorage() = default;
    virtual StorageResult<MessageMaintenanceRun> begin_message_retention(
        const MessageConfigurationActor&, Identifier128 run) = 0;
    virtual StorageResult<MessageRetentionBatch> message_retention_batch(
        const MessageConfigurationActor&, Identifier128 run, std::int64_t expected_sequence,
        MessageMaintenanceStop stop = {}) = 0;
    virtual StorageResult<MessageMaintenanceRun> finish_message_retention(
        const MessageConfigurationActor&, Identifier128 run, MessageMaintenanceOutcome,
        MessageMaintenanceReason) = 0;
    virtual StorageResult<MessageMaintenanceRun> message_maintenance_run(
        const MessageConfigurationActor&, Identifier128 run) = 0;
    virtual StorageResult<std::vector<MessageMaintenanceRun>> message_maintenance_history(
        const MessageConfigurationActor&, MessageMaintenanceKind) = 0;
    virtual StorageResult<MessageIntegrityResult> check_message_integrity(
        const MessageConfigurationActor&, Identifier128 run, MessageMaintenanceStop stop = {}) = 0;
    virtual StorageResult<std::vector<MessageIntegrityFinding>> message_integrity_holds(
        const MessageConfigurationActor&) = 0;
    virtual StorageStatus recover_interrupted_message_maintenance(
        const MessageConfigurationActor&) = 0;
};

} // namespace sfx::application
