#pragma once

#include "sfx/application/caller_lifetime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <array>
#include <vector>

namespace sfx::application {

inline constexpr std::size_t kOperationalRecordMaximum = 1280;
inline constexpr std::size_t kOperationalPresentationMaximum = 640;
inline constexpr std::uint64_t kOperationalFileMaximum = 10U * 1024U * 1024U; // default, configurable

enum class OperationalKind : std::uint8_t {
    listener_ready, listener_stopping, listener_stopped, connection_assigned,
    capacity_refused, source_refused, runtime_refused, socket_failed,
    ssh_setup_ended, stage_changed, authentication_denied, duplicate_denied,
    registration_started, registration_committed, registration_abandoned,
    authenticated, message_area_unavailable, connection_ended, assignment_released,
    assignment_quarantined, node_enabled, node_disabled, settings_pending,
    runtime_stopping, statistics_unavailable, session_failed, runtime_failed, listener_failed,
    session_persisted, hostname_exhausted, durable_journal_degraded,
    local_session_requested, local_session_reserved, local_session_cancelled,
    local_session_launch_failed, local_session_timed_out,
    account_administration, throttle_cleared, drain_started, drain_cancelled,
    backup_started, backup_completed, backup_failed, configuration_saved,
    botgate_passed, botgate_failed, botgate_timed_out, botgate_bypassed,
    botgate_blocked, feed_update_started, feed_update_completed, feed_update_failed,
    flood_aggregate, runtime_started, runtime_stopped,
    message_maintenance, message_maintenance_problem,
    file_maintenance, file_maintenance_problem,
    external_event_started, external_event_finished, external_event_problem
};
enum class OperationalTransport : std::uint8_t { none, telnet, ssh };
enum class OperationalOutcome : std::uint8_t {
    none, normal, cancelled, timed_out, disconnected, shutdown, denied,
    unavailable, storage_failure, terminal_failure, internal_failure,
    registration_closed
};
enum class OperationalClass : std::uint8_t {
    runtime, connection, authentication, registration, activity, cleanup,
    configuration, statistics, botgate
};

// Values are captured at emission, never reconstructed from the current node.
// Authenticated names/AccountIds are populated only after the complete ready
// handoff. committed_account_id separately records non-login registration truth.
struct OperationalContext {
    Identifier128 epoch;
    std::uint64_t origin_sequence{0};
    std::uint16_t node{0};
    std::uint64_t generation{0}, attempt{0};
    OperationalTransport transport{OperationalTransport::none};
    std::string source_ip;
    std::string hostname; // untrusted display hint, immutable at event emission
    std::uint16_t peer_port{0}, local_port{0};
    AccountId account_id{0}, committed_account_id{0}, verified_account_id{0};
    Identifier128 durable_session_id, correlation_id;
    std::string caller;
    // A normalized, bounded pre-authentication claim, populated only when a
    // confirmed JOKER.DAT match ends the connection. Never caller identity.
    std::string attempted_name;
    CallerStage stage{CallerStage::transport};
    std::uint64_t duration_seconds{0};
    std::uint16_t columns{80}, rows{25};
    // Reserved typed activity target for future independently authorized sinks.
    // No Doors/Utilities/event engine is activated by this context. The exact
    // configured name belongs here, never recovered from a rendered sentence.
    std::uint64_t resource_id{0};
    std::string resource_name;
    std::string diagnostic_code;
};
struct OperationalEvent {
    OperationalKind kind{OperationalKind::connection_assigned};
    OperationalOutcome outcome{OperationalOutcome::none};
    OperationalContext context;
};

struct AlphaOperationalLogEntry {
    std::uint64_t sequence{0}; // current delivery stream, not persistent identity
    std::int64_t utc_seconds{0};
    std::string level, component, message; // derived for new typed records
    std::optional<OperationalEvent> event{}; // absent only for legacy history
};

enum class OperationalCounter : std::size_t {
    connections, capacity_refusals, source_refusals, authentication_denials,
    duplicate_denials, account_changes, throttle_clears, drains_started,
    drains_cancelled, backups_completed, backups_failed, configuration_saves, count
};
inline constexpr std::size_t kRecentOperatingEvents = 32;
struct OperationalReportingSnapshot {
    bool available{false};
    std::array<std::uint64_t,static_cast<std::size_t>(OperationalCounter::count)> counts{};
    std::vector<AlphaOperationalLogEntry> recent;
};
std::string_view operational_counter_label(OperationalCounter counter) noexcept;
bool operating_event(OperationalKind kind) noexcept;
void observe_operational_reporting(OperationalReportingSnapshot& snapshot,const AlphaOperationalLogEntry& entry);

// Each future sink receives the SAME immutable, fully correlated record, before
// presentation. No registry, plugin, journal file or new durability promise here.
// Sink implementations must contain failures and must not mutate caller state.
class IOperationalEventSink {
public:
    virtual ~IOperationalEventSink() = default;
    virtual void observe(const AlphaOperationalLogEntry& entry) noexcept = 0;
};

enum OperationalLogHealth : std::uint8_t {
    operational_log_healthy = 0,
    operational_log_full = 1,
    operational_log_write_failed = 2,
    operational_log_history_invalid = 4,
    operational_log_event_rejected = 8,
    operational_durable_journal_failed = 16
};
std::string_view operational_log_health_text(std::uint8_t health) noexcept;
std::string_view caller_stage_label(CallerStage stage) noexcept;
OperationalClass operational_class(OperationalKind kind) noexcept;
std::string_view operational_kind_label(OperationalKind kind) noexcept;
// 1 Connections, 2 Callers, 3 BotGate, 4 System; legacy entries are System.
unsigned operational_filter_group(const AlphaOperationalLogEntry&) noexcept;
bool operational_sysop_attention(OperationalKind kind) noexcept;
std::string_view operational_level(OperationalKind kind) noexcept;
std::string operational_message(const OperationalEvent& event);
AlphaOperationalLogEntry make_operational_entry(std::uint64_t delivery_sequence,
    std::int64_t utc_seconds, OperationalEvent event);
bool valid_operational_entry(const AlphaOperationalLogEntry& entry) noexcept;
// File records exclude CR/LF; append owns one CRLF. SFXLOG6 adds BotGate/feed
// and bounded flood-summary kinds. SFXLOG5 adds bounded
// administration/lifecycle/backup kinds. SFXLOG4 added attempted-name claims;
// SFXLOG3 added hostname hints. Strict SFXLOG2/3/4 and legacy reading remains.
std::string encode_operational_record(const AlphaOperationalLogEntry& entry);
std::optional<AlphaOperationalLogEntry> decode_operational_record(
    std::string_view record, std::uint64_t delivery_sequence);
std::string render_operational_line(const AlphaOperationalLogEntry& entry,
    const std::optional<std::string>& bbs_local_timestamp);

} // namespace sfx::application
