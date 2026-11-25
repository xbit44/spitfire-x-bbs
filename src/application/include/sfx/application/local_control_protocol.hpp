#pragma once

#include "sfx/application/lifecycle.hpp"
#include "sfx/application/control_panel.hpp"
#include "sfx/application/operational_configuration.hpp"
#include "sfx/application/storage.hpp"
#include "sfx/application/node_pool.hpp"
#include "sfx/application/operational_log.hpp"
#include "sfx/application/administration_reporting.hpp"
#include "sfx/application/operational_backlog.hpp"
#include "sfx/application/scheduler.hpp"
#include "sfx/application/local_account_administration.hpp"
#include "sfx/application/local_classic_information.hpp"
#include "sfx/application/local_message_administration.hpp"
#include "sfx/application/message_maintenance_control.hpp"
#include "sfx/application/local_qwk_administration.hpp"
#include "sfx/application/local_file_administration.hpp"
#include "sfx/application/local_door_administration.hpp"
#include "sfx/application/local_door_bulletin_administration.hpp"
#include "sfx/application/ftn_configuration_transport.hpp"
#include "sfx/application/ftn_runtime.hpp"
#include "sfx/application/ftn_held_control.hpp"
#include "sfx/application/file_management.hpp"
#include "sfx/core/node.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {

inline constexpr std::uint32_t kLocalControlFrameMagic = 0x31584653U;
inline constexpr std::size_t kLocalControlFrameHeaderBytes = 20;
inline constexpr std::size_t kLocalControlMaximumPayloadBytes = 65536;
inline constexpr std::size_t kLocalControlNonceBytes = 32;
inline constexpr ControlProtocolVersion kAlphaLocalControlVersion{50, 0};
// A logical status snapshot is transferred in at most five unchanged 64-KiB
// frames. Only the selected node carries its terminal cells.
inline constexpr std::size_t kRuntimeStatusSnapshotMaximumBytes = 256U * 1024U;
inline constexpr std::size_t kRuntimeStatusChunkHeaderBytes = 16;
inline constexpr std::size_t kRuntimeStatusChunkBytes = kLocalControlMaximumPayloadBytes - kRuntimeStatusChunkHeaderBytes;

enum class LocalControlMessageType : std::uint16_t {
    hello = 1,
    hello_response = 2,
    status_request = 3,
    status_response = 4,
    bootstrap_request = 5,
    bootstrap_response = 6,
    shutdown_request = 7,
    shutdown_response = 8,
    node_command_request = 9,
    node_command_response = 10,
    settings_request = 11,
    settings_response = 12,
    metrics_request = 13,
    metrics_response = 14,
    log_page_request = 15,
    log_page_response = 16,
    local_session_start_request = 17,
    local_session_start_response = 18,
    local_session_cancel_request = 19,
    local_session_cancel_response = 20,
    scheduler_settings_request = 21,
    scheduler_settings_response = 22,
    scheduler_run_request = 23,
    scheduler_run_response = 24,
    configuration_snapshot_request = 25,
    configuration_snapshot_response = 26,
    configuration_update_request = 27,
    configuration_update_response = 28,
    account_query_request = 29,
    account_query_response = 30,
    account_command_request = 31,
    account_command_response = 32,
    lifecycle_command_request = 33,
    lifecycle_command_response = 34,
    throttle_clear_request = 35,
    throttle_clear_response = 36,
    botgate_feed_status_request = 37,
    botgate_feed_status_response = 38,
    botgate_feed_update_request = 39,
    botgate_feed_update_response = 40,
    security_query_request = 41,
    security_query_response = 42,
    security_change_request = 43,
    security_change_response = 44,
    classic_information_query_request = 45,
    classic_information_query_response = 46,
    classic_information_change_request = 47,
    classic_information_change_response = 48,
    message_configuration_query_request = 49,
    message_configuration_query_response = 50,
    message_configuration_change_request = 51,
    message_configuration_change_response = 52,
    message_maintenance_query_request = 53,
    message_maintenance_query_response = 54,
    message_maintenance_schedule_request = 55,
    message_maintenance_schedule_response = 56,
    message_maintenance_run_request = 57,
    message_maintenance_run_response = 58,
    qwk_policy_query_request = 59,
    qwk_policy_query_response = 60,
    qwk_policy_change_request = 61,
    qwk_policy_change_response = 62,
    file_configuration_query_request = 63,
    file_configuration_query_response = 64,
    file_configuration_change_request = 65,
    file_configuration_change_response = 66,
    file_management_start_request = 67,
    file_management_start_response = 68,
    file_management_status_request = 69,
    file_management_status_response = 70,
    file_management_cancel_request = 71,
    file_management_cancel_response = 72,
    file_management_query_request = 73,
    file_management_query_response = 74,
    clamav_manual_notice_request = 75,
    clamav_manual_notice_response = 76,
    door_configuration_query_request = 77,
    door_configuration_query_response = 78,
    door_configuration_change_request = 79,
    door_configuration_change_response = 80,
    chat_configuration_query_request = 81,
    chat_configuration_query_response = 82,
    chat_configuration_change_request = 83,
    chat_configuration_change_response = 84,
    chat_notice_request = 85,
    chat_notice_response = 86,
    chat_operator_request = 87,
    chat_operator_response = 88,
    sound_poll_request = 89,
    sound_poll_response = 90,
    ftn_configuration_request = 91,
    ftn_configuration_response = 92,
    ftn_hatch_catalog_request = 93,
    ftn_hatch_catalog_response = 94,
    ftn_runtime_request = 95,
    ftn_runtime_response = 96,
    ftn_held_request = 97,
    ftn_held_response = 98,
    door_bulletin_query_request = 99,
    door_bulletin_query_response = 100,
    door_bulletin_change_request = 101,
    door_bulletin_change_response = 102,
};

std::vector<std::uint8_t> encode_sound_poll_request(const SoundPollRequest&);
std::optional<SoundPollRequest> decode_sound_poll_request(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_sound_poll_snapshot(const SoundPollSnapshot&);
std::optional<SoundPollSnapshot> decode_sound_poll_snapshot(const std::vector<std::uint8_t>&);

// Authenticated Config-to-Runtime activity summaries, not raw vendor output.
// A successful result is independently checked against native saved history.
enum class ManualClamAvPhase : std::uint8_t { started, verified, failed, cancelled };
struct ManualClamAvNotice {
    Identifier128 operation;
    ManualClamAvPhase phase{ManualClamAvPhase::started};
    std::int64_t attempt_utc{};
    std::string diagnostic;
};
std::vector<std::uint8_t> encode_manual_clamav_notice(const ManualClamAvNotice&);
std::optional<ManualClamAvNotice> decode_manual_clamav_notice(const std::vector<std::uint8_t>&);

inline constexpr bool file_management_control_request(LocalControlMessageType type) noexcept {
    return type==LocalControlMessageType::file_management_start_request||type==LocalControlMessageType::file_management_status_request||type==LocalControlMessageType::file_management_cancel_request;
}
inline constexpr bool file_manager_control_request(LocalControlMessageType type) noexcept {
    return type==LocalControlMessageType::ftn_hatch_catalog_request||type==LocalControlMessageType::file_configuration_query_request||type==LocalControlMessageType::file_management_query_request||file_management_control_request(type);
}

std::vector<std::uint8_t> encode_file_configuration_query(const LocalFileConfigurationQuery&);
std::optional<LocalFileConfigurationQuery> decode_file_configuration_query(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_file_configuration_snapshot(const LocalFileConfigurationSnapshot&);
std::optional<LocalFileConfigurationSnapshot> decode_file_configuration_snapshot(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_file_configuration_change(const FileConfigurationChange&);
std::optional<FileConfigurationChange> decode_file_configuration_change(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_file_configuration_result(const LocalFileConfigurationResult&);
std::optional<LocalFileConfigurationResult> decode_file_configuration_result(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_door_configuration_query(const LocalDoorQuery&);
std::vector<std::uint8_t> encode_door_bulletin_query(const LocalDoorBulletinQuery&);
std::optional<LocalDoorBulletinQuery> decode_door_bulletin_query(const std::vector<std::uint8_t>&);
std::optional<LocalDoorQuery> decode_door_configuration_query(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_door_configuration_snapshot(const LocalDoorSnapshot&);
std::optional<LocalDoorSnapshot> decode_door_configuration_snapshot(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_door_configuration_change(const DoorConfigurationChange&);
std::optional<DoorConfigurationChange> decode_door_configuration_change(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_door_configuration_result(const LocalDoorResult&);
std::optional<LocalDoorResult> decode_door_configuration_result(const std::vector<std::uint8_t>&);

std::vector<std::uint8_t> encode_message_maintenance_kind(MessageMaintenanceKind);
std::optional<MessageMaintenanceKind> decode_message_maintenance_kind(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_message_maintenance_schedule(const MessageMaintenanceScheduleRequest&);
std::optional<MessageMaintenanceScheduleRequest> decode_message_maintenance_schedule(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_message_maintenance_status(const MessageMaintenanceStatus&);
std::optional<MessageMaintenanceStatus> decode_message_maintenance_status(const std::vector<std::uint8_t>&);

std::vector<std::uint8_t> encode_security_snapshot(const LocalSecuritySnapshot& value);
std::optional<LocalSecuritySnapshot> decode_security_snapshot(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_security_change(const LocalSecurityChange& value);
std::optional<LocalSecurityChange> decode_security_change(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_classic_information_query(std::int64_t after);
std::optional<std::int64_t> decode_classic_information_query(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_classic_information_snapshot(const LocalClassicInformationSnapshot& value);
std::optional<LocalClassicInformationSnapshot> decode_classic_information_snapshot(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_classic_information_change(const ClassicInformationChange& value);
std::optional<ClassicInformationChange> decode_classic_information_change(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_message_configuration_query(const LocalMessageConfigurationQuery&);
std::optional<LocalMessageConfigurationQuery> decode_message_configuration_query(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_message_configuration_snapshot(const LocalMessageConfigurationSnapshot&);
std::optional<LocalMessageConfigurationSnapshot> decode_message_configuration_snapshot(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_message_configuration_change(const MessageConfigurationChange&);
std::optional<MessageConfigurationChange> decode_message_configuration_change(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_message_configuration_result(const LocalMessageConfigurationResult&);
std::optional<LocalMessageConfigurationResult> decode_message_configuration_result(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_qwk_policy_change(const QwkPolicyChange&);
std::optional<QwkPolicyChange> decode_qwk_policy_change(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_qwk_policy_result(const LocalQwkPolicyResult&);
std::optional<LocalQwkPolicyResult> decode_qwk_policy_result(const std::vector<std::uint8_t>&);

enum class LocalControlResponseCode : std::uint8_t {
    ok,
    setup_required,
    recovery_required,
    invalid_request,
    unauthorized,
    incompatible_version,
    storage_failure,
    configuration_failure,
    shutting_down,
    internal_failure,
};

enum class AlphaRuntimePresentationState : std::uint8_t {
    starting,
    setup_required,
    ready,
    recovery_required,
    stopping,
    faulted,
    draining,
    stopped,
};

struct LocalControlFrame {
    ControlProtocolVersion version{kAlphaLocalControlVersion};
    LocalControlMessageType type{LocalControlMessageType::status_request};
    std::uint32_t request_id{0};
    std::vector<std::uint8_t> payload;
};

enum class LocalControlFrameDecodeCode : std::uint8_t {
    complete,
    invalid_header,
    incompatible_version,
    unknown_message,
    oversized,
    length_mismatch,
};

struct LocalControlFrameDecodeResult {
    LocalControlFrameDecodeCode code{LocalControlFrameDecodeCode::invalid_header};
    std::optional<LocalControlFrame> frame;
};

struct AlphaNodeStatus {
    std::uint16_t number{0};
    core::NodeState state{core::NodeState::offline};
    bool available{false};
    bool enabled{false};
    std::string caller_handle;
    std::string connection;
    std::string activity;
    std::string details;
    std::string source_ip;
    std::string hostname;
    std::uint64_t connected_seconds{0};
    std::uint64_t assignment_sequence{0};
    DoorId door_id{};
    std::uint64_t door_launch_sequence{};
    // Transport truth, independent of login and screen presentation. Activity
    // sequences count successful payload operations, never bytes or secrets.
    bool caller_connected{false};
    bool ssh_encrypted{false};
    std::uint64_t receive_activity{0}, send_activity{0};
    bool screen_active{false};
    bool screen_included{true};
    std::uint16_t terminal_columns{80};
    std::uint16_t terminal_rows{25};
    ControlPanelTerminalSnapshot screen;
    NodeAssignmentIdentity screen_owner{};
    AccountId account_id{}; // Authenticated Runtime identity, never inferred from a displayed handle.
};

struct AlphaStatisticValue {
    bool available{false};
    std::uint64_t today{0};
    std::uint64_t total{0};
};

struct AlphaRuntimeStatistics {
    AlphaStatisticValue successful_logons;
    AlphaStatisticValue caller_seconds;
    AlphaStatisticValue new_users;
};

struct AlphaSchedulerStatus {
    bool configuration_loaded{false};
    std::string job_id{kBackupJobId};
    bool enabled{true};
    std::uint64_t configuration_revision{1};
    std::uint8_t local_hour{2};
    std::uint8_t local_minute{0};
    std::int64_t next_run_utc{0};
    bool active{false};
    bool active_scheduled{false};
    bool queued{false};
    std::string active_run_id;
    std::int64_t active_started_utc{0};
    std::int64_t last_run_utc{0};
    ScheduledJobResult last_result{ScheduledJobResult::never};
    std::string last_detail;
    std::vector<ScheduledJobHistory> history;
    std::string diagnostic_code{"scheduler_not_initialized"};
};

inline constexpr std::size_t kSchedulerStatusHistoryDetailMaximum = 128;
std::string bounded_scheduler_status_history_detail(std::string_view value);

struct AlphaSchedulerSettingsRequest {
    std::string job_id{kBackupJobId};
    std::uint64_t expected_revision{0};
    bool enabled{true};
    std::uint8_t local_hour{2};
    std::uint8_t local_minute{0};
    // Absent preserves the existing destination for schedule-only callers.
    // Present empty selects the installation-relative Scheduled default.
    // Runtime validates and publishes it; Config never writes live settings.
    std::optional<std::string> backup_destination;
};

struct AlphaSchedulerRunRequest {
    std::string job_id{kBackupJobId};
};

struct AlphaSettingsRequest {
    std::string bbs_name;
    std::string telnet_bind;
    std::uint16_t telnet_port{0};
    bool ssh_enabled{true};
    std::string ssh_bind{"0.0.0.0"};
    std::uint16_t ssh_port{2222};
    bool allow_new_users{true};
    bool hostname_lookup{true};
};

enum class AlphaConfigurationActivation : std::uint8_t {
    applied,
    saved_next_start,
    pending_safe_boundary,
    restart_required,
    refused,
};

struct AlphaConfigurationSnapshot {
    OperationalConfiguration active;
    OperationalConfiguration saved;
    AlphaConfigurationActivation activation{AlphaConfigurationActivation::applied};
    std::string detail{"Applied"};
};

struct AlphaConfigurationUpdateRequest {
    std::uint64_t expected_revision{0};
    OperationalConfiguration configuration;
};

struct AlphaConfigurationUpdateResult {
    LocalControlResponseCode code{LocalControlResponseCode::internal_failure};
    std::string diagnostic_code;
    AlphaConfigurationActivation activation{AlphaConfigurationActivation::refused};
    std::uint64_t active_revision{0};
    std::uint64_t saved_revision{0};
    std::string detail{"Refused"};
};

struct AlphaRuntimeStatus {
    Identifier128 runtime_epoch{};
    std::uint64_t status_sequence{0};
    AlphaRuntimePresentationState state{AlphaRuntimePresentationState::starting};
    RuntimeStatus lifecycle;
    bool configuration_loaded{false};
    bool bootstrap_complete{false};
    std::string bbs_name{"Spitfire-X BBS"};
    std::string bbs_time_zone;
    std::int64_t security_policy_revision{0};
    std::string security_policy_hash;
    std::vector<AlphaNodeStatus> nodes;
    StorageHealth storage;
    bool telnet_configured{false};
    std::string telnet_bind{"0.0.0.0"};
    std::uint16_t telnet_port{2323};
    bool telnet_bot_gate_enabled{true};
    bool telnet_active{false};
    bool ssh_configured{false};
    std::string ssh_bind{"0.0.0.0"};
    std::uint16_t ssh_port{2222};
    bool ssh_active{false};
    bool listener_restart_required{false};
    bool allow_new_users{true};
    bool hostname_lookup{true};
    bool hostname_exhausted{false};
    std::optional<AlphaSettingsRequest> pending_settings;
    std::vector<AlphaOperationalLogEntry> logs;
    std::uint8_t log_health{operational_log_healthy};
    AlphaRuntimeStatistics statistics;
    OperationalReportingSnapshot reporting;
    AdministrationReportingSnapshot administration_history;
    AlphaSchedulerStatus scheduler;
    std::string external_event_activity;
    std::string diagnostic_code;
    std::uint64_t configuration_revision{1};
};

struct AlphaStatusRequest {
    std::uint16_t screen_node{0}; // zero means metadata only
    std::uint64_t snapshot_sequence{0}; // zero starts a new immutable snapshot
    std::uint32_t offset{0};
};
std::vector<std::uint8_t> encode_status_request(const AlphaStatusRequest& request);
std::optional<AlphaStatusRequest> decode_status_request(const std::vector<std::uint8_t>& payload);

class AlphaStatusSnapshotCache final {
public:
    bool capture(AlphaRuntimeStatus status, std::uint16_t screen_node);
    std::vector<std::uint8_t> chunk(const AlphaStatusRequest& request) const;
private:
    std::uint64_t sequence_{0};
    std::uint16_t screen_node_{0};
    std::vector<std::uint8_t> bytes_;
};

// Discards any partial/mixed/stale/oversized transfer. Never publishes a partial
// node list, silently retries a changed snapshot, or expands the frame ceiling.
std::optional<AlphaRuntimeStatus> read_status_snapshot(
    std::uint16_t screen_node,
    const std::function<std::optional<std::vector<std::uint8_t>>(const AlphaStatusRequest&)>& exchange);

struct AlphaRuntimeMetricSnapshot {
    ControlPanelSqliteMetricSnapshot sqlite;
    bool uptime_available{false};
    std::uint64_t uptime_milliseconds{0};
};

enum class AlphaNodeCommandKind : std::uint8_t { enable, disable, disconnect, terminate_door, disable_observed };

struct AlphaNodeCommandRequest {
    std::uint16_t node_number{0};
    AlphaNodeCommandKind command{AlphaNodeCommandKind::enable};
    // A disconnect targets the observed assignment, never whichever caller
    // happens to occupy this node when a delayed command arrives.
    // An observed disable requires this epoch and the displayed connected
    // assignment. A zero generation means no connected caller was displayed.
    Identifier128 expected_epoch{};
    std::uint64_t expected_generation{0};
    std::uint64_t expected_door_launch{};
};

enum class AlphaLifecycleCommandKind : std::uint8_t { begin_drain, cancel_drain };
struct AlphaLifecycleCommandRequest {
    AlphaLifecycleCommandKind command{AlphaLifecycleCommandKind::begin_drain};
    // Explicit finite Sysop choice; cancellation must carry zero.
    std::uint32_t timeout_seconds{300};
};

std::vector<std::uint8_t> encode_lifecycle_command_request(const AlphaLifecycleCommandRequest& request);
std::optional<AlphaLifecycleCommandRequest> decode_lifecycle_command_request(
    const std::vector<std::uint8_t>& payload);

struct AlphaControlResponse {
    LocalControlResponseCode code{LocalControlResponseCode::internal_failure};
    std::string diagnostic_code;
    // Only the authenticated hello binds an epoch; other responses leave zero.
    Identifier128 runtime_epoch{};
};

inline constexpr std::uint32_t kAlphaLocalSessionTimeoutMilliseconds = 30000;

struct AlphaLocalSessionStartRequest {
    std::uint16_t node_number{0};
};

struct AlphaLocalSessionStartResult {
    AlphaControlResponse response;
    std::string host;
    std::uint16_t port{0};
    NodeAssignmentIdentity reservation;
    std::uint32_t timeout_milliseconds{0};
};

enum class AlphaLocalSessionCancelReason : std::uint8_t {
    operator_cancelled,
    profile_invalid,
    client_launch_failed,
};

struct AlphaLocalSessionCancelRequest {
    NodeAssignmentIdentity reservation;
    AlphaLocalSessionCancelReason reason{AlphaLocalSessionCancelReason::operator_cancelled};
};

struct AlphaBootstrapRequest {
    OperationalConfiguration configuration;
    std::string sysop_name;
    std::string sysop_password;
    std::string system_password;
};

bool valid_alpha_runtime_status(const AlphaRuntimeStatus& status) noexcept;

// Connection-owned high-water marks, separate from disposable UI caches. Only
// a successful authenticated hello may bind a new epoch. Clearing presentation
// must NOT clear these marks or reopen acceptance of delayed messages.
class AlphaStatusTracker final {
public:
    bool bind_authenticated_epoch(Identifier128 epoch) noexcept;
    bool accept(const AlphaRuntimeStatus& status) noexcept;
private:
    Identifier128 epoch_;
    std::uint64_t sequence_{0};
    std::array<std::uint64_t, kAlphaNodePoolCapacity> generations_{};
    std::array<std::uint64_t, kAlphaNodePoolCapacity> screens_{};
    std::array<std::uint64_t, kAlphaNodePoolCapacity> received_{}, sent_{};
    std::array<bool, kAlphaNodePoolCapacity> active_{};
    std::uint64_t configuration_revision_{0};
    std::uint16_t configured_{0};
    bool had_nodes_{false};
};

// UI-thread-owned, independent indicators. Polling an unchanged snapshot does
// not restart a pulse. Only a newly connected assignment starts two blinks;
// screen output, authentication/status changes and modem traffic are unrelated.
class AlphaNodeIndicators final {
public:
    void observe(const AlphaRuntimeStatus& status, std::uint64_t now);
    bool bold(std::uint16_t node, std::uint64_t now) const noexcept;
    void clear() noexcept;
private:
    struct Indicator {
        std::uint64_t generation{0}, started{0};
        bool active{false}, pulsing{false};
    };
    Identifier128 epoch_;
    std::array<Indicator, kAlphaNodePoolCapacity> nodes_{};
};

struct ModemLights { bool aa{}, cd{}, rd{}, sd{}, sc{}, mr{}; };

// One interpretation shared by the table and node strip. Unchanged snapshots,
// first observation, and assignment reuse never invent a traffic pulse.
class AlphaModemIndicators final {
public:
    void observe(const AlphaRuntimeStatus& status, std::uint64_t now) noexcept;
    ModemLights lights(const AlphaNodeStatus& node, std::uint64_t now) const noexcept;
    void clear() noexcept { nodes_ = {}; epoch_ = {}; accepting_ = false; }
private:
    struct Traffic {
        std::uint64_t generation{}, received{}, sent{}, rd_until{}, sd_until{};
        bool observed{};
    };
    Identifier128 epoch_;
    bool accepting_{};
    std::array<Traffic, kAlphaNodePoolCapacity> nodes_{};
};

std::vector<std::uint8_t> encode_local_control_frame(const LocalControlFrame& frame);
LocalControlFrameDecodeResult decode_local_control_frame(
    const std::vector<std::uint8_t>& bytes);

std::vector<std::uint8_t> encode_hello_payload(
    const std::array<std::uint8_t, kLocalControlNonceBytes>& nonce);
bool decode_hello_payload(
    const std::vector<std::uint8_t>& payload,
    std::array<std::uint8_t, kLocalControlNonceBytes>& nonce) noexcept;
std::vector<std::uint8_t> encode_runtime_status(const AlphaRuntimeStatus& status);
std::optional<AlphaRuntimeStatus> decode_runtime_status(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_runtime_metrics(
    const AlphaRuntimeMetricSnapshot& metrics);
std::vector<std::uint8_t> encode_log_cursor(OperationalLogCursor cursor);
std::optional<OperationalLogCursor> decode_log_cursor(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_log_page(const OperationalLogPage& page);
std::optional<OperationalLogPage> decode_log_page(const std::vector<std::uint8_t>& payload);
std::optional<AlphaRuntimeMetricSnapshot> decode_runtime_metrics(
    const std::vector<std::uint8_t>& payload) noexcept;
std::vector<std::uint8_t> encode_bootstrap_request(const AlphaBootstrapRequest& request);
// No bootstrap request may be constructed until Runtime has supplied an
// authoritative security policy. Preserve its provisioning diagnostic on failure.
std::optional<AlphaControlResponse> bootstrap_prerequisite_failure(const AlphaRuntimeStatus& status);
// Fixed, non-secret operator guidance; never interprets diagnostic text as input.
std::string_view alpha_recovery_guidance(std::string_view diagnostic) noexcept;
std::optional<AlphaBootstrapRequest> decode_bootstrap_request(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_control_response(const AlphaControlResponse& response);
std::optional<AlphaControlResponse> decode_control_response(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_node_command_request(const AlphaNodeCommandRequest& request);
std::optional<AlphaNodeCommandRequest> decode_node_command_request(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_local_session_start_request(
    const AlphaLocalSessionStartRequest& request);
std::optional<AlphaLocalSessionStartRequest> decode_local_session_start_request(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_local_session_start_result(
    const AlphaLocalSessionStartResult& result);
std::optional<AlphaLocalSessionStartResult> decode_local_session_start_result(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_local_session_cancel_request(
    const AlphaLocalSessionCancelRequest& request);
std::optional<AlphaLocalSessionCancelRequest> decode_local_session_cancel_request(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_settings_request(const AlphaSettingsRequest& request);
std::optional<AlphaSettingsRequest> decode_settings_request(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_botgate_feed_status(const BotGateFeedStatus& status);
std::optional<BotGateFeedStatus> decode_botgate_feed_status(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_configuration_snapshot(
    const AlphaConfigurationSnapshot& snapshot);
std::optional<AlphaConfigurationSnapshot> decode_configuration_snapshot(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_configuration_update_request(
    const AlphaConfigurationUpdateRequest& request);
std::optional<AlphaConfigurationUpdateRequest> decode_configuration_update_request(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_configuration_update_result(
    const AlphaConfigurationUpdateResult& result);
std::optional<AlphaConfigurationUpdateResult> decode_configuration_update_result(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_scheduler_settings_request(
    const AlphaSchedulerSettingsRequest& request);
std::optional<AlphaSchedulerSettingsRequest> decode_scheduler_settings_request(
    const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_scheduler_run_request(
    const AlphaSchedulerRunRequest& request);
std::optional<AlphaSchedulerRunRequest> decode_scheduler_run_request(
    const std::vector<std::uint8_t>& payload);
void erase_bootstrap_secrets(AlphaBootstrapRequest& request) noexcept;
std::vector<std::uint8_t> encode_account_query(const LocalAccountQuery& query);
std::optional<LocalAccountQuery> decode_account_query(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_account_request(const LocalAccountRequest& request);
std::optional<LocalAccountRequest> decode_account_request(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_account_result(const LocalAccountResult& result);
std::optional<LocalAccountResult> decode_account_result(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_account_snapshot(const LocalAccountSnapshot& snapshot);
std::optional<LocalAccountSnapshot> decode_account_snapshot(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> encode_throttle_clear_request(const LocalThrottleClearRequest& request);
std::optional<LocalThrottleClearRequest> decode_throttle_clear_request(const std::vector<std::uint8_t>& payload);

}  // namespace sfx::application
