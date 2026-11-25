#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/hostname_lookup.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace sfx::application {
namespace {

class Writer final {
public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void u16(const std::uint16_t value) {
        bytes_.push_back(static_cast<std::uint8_t>(value & 0xffU));
        bytes_.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    }
    void u32(const std::uint32_t value) {
        for (unsigned int shift = 0; shift < 32U; shift += 8U)
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
    void u64(const std::uint64_t value) {
        for (unsigned int shift = 0; shift < 64U; shift += 8U)
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
    bool text(const std::string_view value, const std::size_t maximum) {
        if (value.size() > maximum || value.size() > std::numeric_limits<std::uint16_t>::max())
            return false;
        u16(static_cast<std::uint16_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        return true;
    }
    void raw(const std::uint8_t* begin, const std::size_t size) {
        bytes_.insert(bytes_.end(), begin, begin + size);
    }
    std::vector<std::uint8_t> take() { return std::move(bytes_); }
private:
    std::vector<std::uint8_t> bytes_;
};

class Reader final {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}
    bool u8(std::uint8_t& value) {
        if (offset_ + 1U > bytes_.size()) return false;
        value = bytes_[offset_++];
        return true;
    }
    bool u16(std::uint16_t& value) {
        if (offset_ + 2U > bytes_.size()) return false;
        value = static_cast<std::uint16_t>(bytes_[offset_]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes_[offset_ + 1U]) << 8U);
        offset_ += 2U;
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (offset_ + 4U > bytes_.size()) return false;
        value = 0;
        for (unsigned int index = 0; index < 4U; ++index)
            value |= static_cast<std::uint32_t>(bytes_[offset_ + index]) << (index * 8U);
        offset_ += 4U;
        return true;
    }
    bool u64(std::uint64_t& value) {
        if (offset_ + 8U > bytes_.size()) return false;
        value = 0;
        for (unsigned int index = 0; index < 8U; ++index)
            value |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << (index * 8U);
        offset_ += 8U;
        return true;
    }
    bool text(std::string& value, const std::size_t maximum) {
        std::uint16_t length = 0;
        if (!u16(length) || length > maximum || offset_ + length > bytes_.size()) return false;
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
        offset_ += length;
        return true;
    }
    bool raw(std::uint8_t* output, const std::size_t size) {
        if (offset_ + size > bytes_.size()) return false;
        std::copy_n(bytes_.data() + offset_, size, output);
        offset_ += size;
        return true;
    }
    bool complete() const noexcept { return offset_ == bytes_.size(); }
private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_{0};
};

bool nonnegative64(Reader& reader, std::int64_t& value);
bool known_type(const std::uint16_t value) noexcept {
    return value >= static_cast<std::uint16_t>(LocalControlMessageType::hello) &&
        value <= static_cast<std::uint16_t>(LocalControlMessageType::door_bulletin_change_response);
}

void secure_clear(std::string& value) noexcept {
    volatile char* cursor = value.empty() ? nullptr : value.data();
    for (std::size_t index = 0; index < value.size(); ++index) cursor[index] = 0;
    value.clear();
    value.shrink_to_fit();
}

}  // namespace

bool valid_alpha_runtime_status(const AlphaRuntimeStatus& status) noexcept {
    const auto text = [](std::string_view value, std::size_t maximum) {
        return value.size() <= maximum && std::all_of(value.begin(), value.end(),
            [](unsigned char c) { return c >= 32 && c <= 126; });
    };
    if (!status.runtime_epoch.valid() || status.status_sequence == 0 ||
        status.lifecycle.configured_nodes < 1 ||
        status.lifecycle.configured_nodes > core::kMaximumLogicalNodes ||
        status.configuration_revision == 0 ||
        status.state > AlphaRuntimePresentationState::stopped ||
        status.lifecycle.state > RuntimeState::recovery_required ||
        status.lifecycle.owner_mode > RuntimeOwnershipMode::service ||
        status.lifecycle.active_work.supervised_processes != 0 ||
        (!status.nodes.empty() && status.nodes.size() != status.lifecycle.configured_nodes) ||
        (status.nodes.empty() && (status.telnet_active || status.ssh_active ||
            status.lifecycle.active_work.caller_sessions != 0)) ||
        (status.state == AlphaRuntimePresentationState::ready &&
            (!status.configuration_loaded || !status.bootstrap_complete || status.nodes.empty())) ||
        status.security_policy_revision < 0 ||
        !text(status.bbs_name, kBbsNameMaximum) ||
        status.bbs_time_zone.size() > kBbsTimeZoneKeyMaximum ||
        std::any_of(status.bbs_time_zone.begin(), status.bbs_time_zone.end(),
            [](unsigned char c) { return c < 32 || c == 127; }) ||
        !text(status.security_policy_hash, 64) ||
        !text(status.telnet_bind, kTelnetBindMaximum) ||
        !text(status.ssh_bind, kTelnetBindMaximum) ||
        (status.telnet_active && !status.telnet_configured) ||
        (status.ssh_active && !status.ssh_configured) ||
        (status.listener_restart_required != status.pending_settings.has_value()) ||
        (status.pending_settings && !status.configuration_loaded) ||
        status.telnet_port == 0 || status.ssh_port == 0 ||
        status.storage.state > StorageRuntimeState::stopping ||
        status.storage.last_error > StorageErrorCode::system_sysop_protected ||
        status.storage.schema_version < 0 ||
        status.storage.queued_operations > std::numeric_limits<std::uint32_t>::max() ||
        !text(status.storage.engine_version, 64) || !text(status.storage.last_operation, 128) ||
        !text(status.diagnostic_code, 128) || status.logs.size() > kControlPanelMaximumLogEntries ||
        status.log_health > 31)
        return false;
    if (status.pending_settings) {
        const auto& pending = *status.pending_settings;
        if (pending.bbs_name.empty() || !text(pending.bbs_name, kBbsNameMaximum) ||
            pending.telnet_bind.empty() || !text(pending.telnet_bind, kTelnetBindMaximum) ||
            pending.ssh_bind.empty() || !text(pending.ssh_bind, kTelnetBindMaximum) ||
            !pending.telnet_port || !pending.ssh_port) return false;
    }
    std::uint16_t active = 0;
    for (std::size_t index = 0; index < status.nodes.size(); ++index) {
        const auto& node = status.nodes[index];
        if (node.screen_active && !node.screen_included &&
            std::any_of(node.screen.cells.begin(), node.screen.cells.end(), [](const auto& cell) {
                return cell.character != 32 || cell.attribute != 7;
            })) return false;
        if (node.number != index + 1 || node.state > core::NodeState::draining ||
            !text(node.caller_handle, 30) || !text(node.connection, 32) ||
            !text(node.activity, 64) || !text(node.details, 128) || !text(node.source_ip, 64) ||
            (!node.hostname.empty() && (!status.hostname_lookup || !normalize_hostname_hint(node.hostname) ||
                normalize_hostname_hint(node.hostname)->view() != node.hostname || !hostname_source_address(node.source_ip))) ||
            !node.terminal_columns || node.terminal_columns > 4096 ||
            !node.terminal_rows || node.terminal_rows > 4096) return false;
        if (node.ssh_encrypted && (!node.caller_connected || node.connection != "SSH")) return false;
        if (node.account_id<0 || (node.account_id && (!node.caller_connected || node.caller_handle.empty() ||
            (node.state!=core::NodeState::online && node.state!=core::NodeState::door && node.state!=core::NodeState::draining)))) return false;
        if(node.door_id>999 || (node.door_id==0)!=(node.door_launch_sequence==0) ||
            (node.door_id && (!node.account_id || !node.caller_connected || (node.state!=core::NodeState::door&&node.state!=core::NodeState::draining))))return false;
        if (!node.screen_active && (node.caller_connected || node.ssh_encrypted ||
            node.receive_activity || node.send_activity)) return false;
        if (node.screen_active) {
            ++active;
            if (!node.available || !node.assignment_sequence || node.source_ip.empty() ||
                (node.connection != "Telnet" && node.connection != "SSH") ||
                (node.state != core::NodeState::connecting && node.state != core::NodeState::online && node.state != core::NodeState::door &&
                    node.state != core::NodeState::draining) ||
                !(node.screen_owner == NodeAssignmentIdentity{
                    status.runtime_epoch, node.number, node.assignment_sequence})) return false;
        } else if (!node.caller_handle.empty() || !node.connection.empty() ||
            !node.source_ip.empty() || !node.hostname.empty() || node.connected_seconds ||
            node.terminal_columns != 80 || node.terminal_rows != 25 ||
            (node.state != core::NodeState::ready && node.state != core::NodeState::offline) ||
            (node.state == core::NodeState::ready && (!node.available || !node.enabled)) ||
            node.screen_owner.runtime_epoch.valid() || node.screen_owner.node ||
            node.screen_owner.generation ||
            std::any_of(node.screen.cells.begin(), node.screen.cells.end(), [](const auto& cell) {
                return cell.character != 32 || cell.attribute != 7;
            })) return false;
    }
    if (active != status.lifecycle.active_work.caller_sessions) return false;
    std::uint64_t last_log = 0;
    for (const auto& log : status.logs) {
        if (log.sequence <= last_log || !valid_operational_entry(log)) return false;
        if (log.event) for (const auto& previous : status.logs) {
            if (previous.sequence >= log.sequence) break;
            if (previous.event && previous.event->context.epoch == log.event->context.epoch &&
                previous.event->context.origin_sequence >= log.event->context.origin_sequence) return false;
        }
        last_log = log.sequence;
    }
    for (const auto* statistic : {&status.statistics.successful_logons,
            &status.statistics.caller_seconds, &status.statistics.new_users}) {
        if (statistic->today > statistic->total ||
            (!statistic->available && (statistic->today || statistic->total))) return false;
    }
    if (!valid_administration_reporting(status.administration_history) ||
        status.reporting.recent.size() > kRecentOperatingEvents ||
        (!status.reporting.available && (!status.reporting.recent.empty() ||
            std::any_of(status.reporting.counts.begin(),status.reporting.counts.end(),
                [](auto value) { return value != 0; })))) return false;
    std::uint64_t previous_event = 0, previous_origin = 0;
    for (const auto& entry : status.reporting.recent) {
        if (!valid_operational_entry(entry) || !entry.event || !operating_event(entry.event->kind) ||
            entry.event->context.epoch != status.runtime_epoch || entry.sequence <= previous_event ||
            entry.event->context.origin_sequence <= previous_origin) return false;
        previous_event = entry.sequence;
        previous_origin = entry.event->context.origin_sequence;
    }
    const auto& scheduler = status.scheduler;
    const auto run_id = [](const std::string_view value) {
        return value.size() == 32 && std::all_of(value.begin(), value.end(), [](const char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F');
        });
    };
    const auto detail = [](const std::string_view value, const std::size_t maximum = 2048U) {
        return value.size() <= maximum && value.find('\0') == std::string_view::npos &&
               value.find('\r') == std::string_view::npos &&
               value.find('\n') == std::string_view::npos;
    };
    if (scheduler.job_id != kBackupJobId || scheduler.configuration_revision == 0 ||
        scheduler.local_hour >= 24 || scheduler.local_minute >= 60 ||
        scheduler.next_run_utc < 0 || scheduler.active_started_utc < 0 ||
        scheduler.last_run_utc < 0 ||
        scheduler.last_result > ScheduledJobResult::interrupted ||
        scheduler.history.size() > kSchedulerHistoryLimit ||
        !detail(scheduler.last_detail) || !text(scheduler.diagnostic_code, 128) ||
        (scheduler.active != (scheduler.last_result == ScheduledJobResult::running)) ||
        (scheduler.active && (!run_id(scheduler.active_run_id) ||
                              scheduler.active_started_utc == 0)) ||
        (scheduler.queued && !scheduler.active) ||
        (!scheduler.active && (!scheduler.active_run_id.empty() ||
                               scheduler.active_started_utc != 0 ||
                               scheduler.active_scheduled)) ||
        (!scheduler.configuration_loaded &&
             (scheduler.active || scheduler.next_run_utc != 0 ||
              !scheduler.history.empty()))) return false;
    for (const auto& item : scheduler.history) {
        if (!run_id(item.run_id) || item.started_utc <= 0 ||
            item.finished_utc < item.started_utc ||
            (item.result != ScheduledJobResult::success &&
             item.result != ScheduledJobResult::failed &&
             item.result != ScheduledJobResult::interrupted) ||
            !detail(item.detail, kSchedulerStatusHistoryDetailMaximum)) return false;
    }
    return true;
}

bool AlphaStatusTracker::bind_authenticated_epoch(Identifier128 epoch) noexcept {
    if (!epoch.valid()) return false;
    epoch_ = epoch;
    sequence_ = 0;
    generations_ = {}; screens_ = {}; active_ = {}; received_ = {}; sent_ = {};
    configured_ = 0; had_nodes_ = false; configuration_revision_ = 0;
    return true;
}

bool AlphaStatusTracker::accept(const AlphaRuntimeStatus& status) noexcept {
    if (!valid_alpha_runtime_status(status) || !(status.runtime_epoch == epoch_) ||
        status.status_sequence <= sequence_ ||
        status.configuration_revision < configuration_revision_ ||
        (configured_ && status.lifecycle.configured_nodes != configured_ &&
            status.configuration_revision <= configuration_revision_) ||
        (had_nodes_ && status.nodes.empty())) return false;
    for (std::size_t i = 0; i < status.nodes.size(); ++i) {
        const auto& node = status.nodes[i];
        if (node.screen_active && active_[i] && node.assignment_sequence == generations_[i] &&
            (node.receive_activity < received_[i] || node.send_activity < sent_[i])) return false;
        if (node.assignment_sequence < generations_[i] ||
            (node.assignment_sequence == generations_[i] && node.screen_active &&
                ((!active_[i] && generations_[i] != 0) || node.screen.sequence < screens_[i])))
            return false;
    }
    // Publish high-water marks only after the entire bounded snapshot passes.
    sequence_ = status.status_sequence;
    configuration_revision_ = status.configuration_revision;
    // Configuration may legitimately be chosen during first-run setup.
    if (!status.nodes.empty()) { configured_ = status.lifecycle.configured_nodes; had_nodes_ = true; }
    for (std::size_t i = 0; i < status.nodes.size(); ++i) {
        generations_[i] = status.nodes[i].assignment_sequence;
        screens_[i] = status.nodes[i].screen_active ? status.nodes[i].screen.sequence : 0;
        received_[i] = status.nodes[i].receive_activity;
        sent_[i] = status.nodes[i].send_activity;
        active_[i] = status.nodes[i].screen_active;
    }
    return true;
}

void AlphaNodeIndicators::clear() noexcept { nodes_ = {}; epoch_ = {}; }
void AlphaNodeIndicators::observe(const AlphaRuntimeStatus& status, std::uint64_t now) {
    if (!(epoch_ == status.runtime_epoch)) { clear(); epoch_ = status.runtime_epoch; }
    for (const auto& node : status.nodes) {
        if (!node.number || node.number > nodes_.size()) continue;
        auto& item = nodes_[node.number - 1];
        const bool changed = !item.active || item.generation != node.assignment_sequence;
        if (!node.caller_connected) item = {};
        else {
            if (changed) { item.started = now; item.pulsing = true; }
            item.generation = node.assignment_sequence;
            item.active = true;
        }
    }
}
bool AlphaNodeIndicators::bold(std::uint16_t node, std::uint64_t now) const noexcept {
    if (!node || node > nodes_.size()) return false;
    const auto& item = nodes_[node - 1];
    return item.pulsing && now >= item.started && now - item.started < 1000 &&
        ((now - item.started) / 250) % 2 == 0;
}

void AlphaModemIndicators::observe(const AlphaRuntimeStatus& status, std::uint64_t now) noexcept {
    if (!(epoch_ == status.runtime_epoch)) { clear(); epoch_ = status.runtime_epoch; }
    accepting_ = status.telnet_active || status.ssh_active;
    for (const auto& node : status.nodes) {
        if (!node.number || node.number > nodes_.size()) continue;
        auto& item = nodes_[node.number - 1];
        if (!node.caller_connected) { item = {}; continue; }
        if (item.observed && item.generation == node.assignment_sequence) {
            if (node.receive_activity > item.received) item.rd_until = now + 750;
            if (node.send_activity > item.sent) item.sd_until = now + 750;
        } else item = {};
        item.observed = true;
        item.generation = node.assignment_sequence;
        item.received = node.receive_activity;
        item.sent = node.send_activity;
    }
}
ModemLights AlphaModemIndicators::lights(const AlphaNodeStatus& node, std::uint64_t now) const noexcept {
    ModemLights result;
    result.mr = node.available && node.enabled && node.state != core::NodeState::offline;
    result.aa = accepting_ && result.mr && !node.caller_connected && node.state == core::NodeState::ready;
    result.cd = node.caller_connected;
    result.sc = result.cd && node.ssh_encrypted && node.connection == "SSH";
    if (result.cd && node.number && node.number <= nodes_.size()) {
        const auto& item = nodes_[node.number - 1];
        if (item.observed && item.generation == node.assignment_sequence) {
            result.rd = now < item.rd_until;
            result.sd = now < item.sd_until;
        }
    }
    return result;
}

std::vector<std::uint8_t> encode_local_control_frame(const LocalControlFrame& frame) {
    if (frame.payload.size() > kLocalControlMaximumPayloadBytes ||
        frame.request_id == 0 ||
        !known_type(static_cast<std::uint16_t>(frame.type))) return {};
    Writer writer;
    writer.u32(kLocalControlFrameMagic);
    writer.u16(frame.version.major);
    writer.u16(frame.version.minor);
    writer.u16(static_cast<std::uint16_t>(frame.type));
    writer.u16(0);
    writer.u32(frame.request_id);
    writer.u32(static_cast<std::uint32_t>(frame.payload.size()));
    writer.raw(frame.payload.data(), frame.payload.size());
    return writer.take();
}

LocalControlFrameDecodeResult decode_local_control_frame(
    const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kLocalControlFrameHeaderBytes) {
        return {LocalControlFrameDecodeCode::invalid_header, std::nullopt};
    }
    Reader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t major = 0, minor = 0, raw_type = 0, flags = 0;
    std::uint32_t request_id = 0, payload_size = 0;
    if (!reader.u32(magic) || !reader.u16(major) || !reader.u16(minor) ||
        !reader.u16(raw_type) || !reader.u16(flags) || !reader.u32(request_id) ||
        !reader.u32(payload_size) || magic != kLocalControlFrameMagic || flags != 0 ||
        request_id == 0) {
        return {LocalControlFrameDecodeCode::invalid_header, std::nullopt};
    }
    if (major != kAlphaLocalControlVersion.major ||
        minor != kAlphaLocalControlVersion.minor) {
        return {LocalControlFrameDecodeCode::incompatible_version, std::nullopt};
    }
    if (!known_type(raw_type)) {
        return {LocalControlFrameDecodeCode::unknown_message, std::nullopt};
    }
    if (payload_size > kLocalControlMaximumPayloadBytes) {
        return {LocalControlFrameDecodeCode::oversized, std::nullopt};
    }
    if (bytes.size() != kLocalControlFrameHeaderBytes + payload_size) {
        return {LocalControlFrameDecodeCode::length_mismatch, std::nullopt};
    }
    LocalControlFrame frame;
    frame.version = {major, minor};
    frame.type = static_cast<LocalControlMessageType>(raw_type);
    frame.request_id = request_id;
    frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kLocalControlFrameHeaderBytes),
        bytes.end());
    return {LocalControlFrameDecodeCode::complete, std::move(frame)};
}

std::vector<std::uint8_t> encode_hello_payload(
    const std::array<std::uint8_t, kLocalControlNonceBytes>& nonce) {
    return {nonce.begin(), nonce.end()};
}

bool decode_hello_payload(
    const std::vector<std::uint8_t>& payload,
    std::array<std::uint8_t, kLocalControlNonceBytes>& nonce) noexcept {
    if (payload.size() != nonce.size()) return false;
    std::copy(payload.begin(), payload.end(), nonce.begin());
    return true;
}

std::vector<std::uint8_t> encode_runtime_status(const AlphaRuntimeStatus& status) {
    if (!valid_alpha_runtime_status(status)) return {};
    if (status.nodes.size() > core::kMaximumLogicalNodes || status.diagnostic_code.size() > 128U ||
        status.bbs_name.size() > kBbsNameMaximum ||
        status.bbs_time_zone.size() > kBbsTimeZoneKeyMaximum ||
        status.security_policy_hash.size() > 64U ||
        status.telnet_bind.size() > kTelnetBindMaximum ||
        status.ssh_bind.size() > kTelnetBindMaximum ||
        status.storage.engine_version.size() > 64U || status.storage.last_operation.size() > 128U ||
        status.logs.size() > kControlPanelMaximumLogEntries) return {};
    Writer writer;
    writer.raw(status.runtime_epoch.bytes.data(), status.runtime_epoch.bytes.size());
    writer.u64(status.status_sequence);
    writer.u8(static_cast<std::uint8_t>(status.state));
    writer.u8(static_cast<std::uint8_t>(status.lifecycle.state));
    writer.u8(static_cast<std::uint8_t>(status.lifecycle.owner_mode));
    writer.u16(status.lifecycle.configured_nodes);
    writer.u16(status.lifecycle.active_work.caller_sessions);
    writer.u16(status.lifecycle.active_work.supervised_processes);
    writer.u8(status.configuration_loaded ? 1U : 0U);
    writer.u8(status.bootstrap_complete ? 1U : 0U);
    if (!writer.text(status.bbs_name, kBbsNameMaximum)) return {};
    if (!writer.text(status.bbs_time_zone, kBbsTimeZoneKeyMaximum)) return {};
    writer.u64(static_cast<std::uint64_t>(status.security_policy_revision));
    if (!writer.text(status.security_policy_hash, 64U)) return {};
    writer.u16(static_cast<std::uint16_t>(status.nodes.size()));
    for (const auto& node : status.nodes) {
        if (node.caller_handle.size() > 64U || node.connection.size() > 32U ||
            node.activity.size() > 64U || node.details.size() > 128U ||
            node.source_ip.size() > 64U) return {};
        writer.u16(node.number);
        writer.u8(static_cast<std::uint8_t>(node.state));
        writer.u8(node.available ? 1U : 0U);
        writer.u8(node.enabled ? 1U : 0U);
        if (!writer.text(node.caller_handle, 64U) || !writer.text(node.connection, 32U) ||
            !writer.text(node.activity, 64U) || !writer.text(node.details, 128U) ||
            !writer.text(node.source_ip, 64U) || !writer.text(node.hostname, kHostnameMaximum)) return {};
        writer.u64(node.connected_seconds);
        writer.u64(node.assignment_sequence);
        writer.u16(node.door_id);writer.u64(node.door_launch_sequence);
        writer.u64(static_cast<std::uint64_t>(node.account_id));
        writer.u8(node.caller_connected ? 1U : 0U);
        writer.u8(node.ssh_encrypted ? 1U : 0U);
        writer.u64(node.receive_activity);
        writer.u64(node.send_activity);
        if (!node.terminal_columns || node.terminal_columns > 4096U ||
            !node.terminal_rows || node.terminal_rows > 4096U) return {};
        writer.u16(node.terminal_columns);
        writer.u16(node.terminal_rows);
        writer.u8(node.screen_active ? 1U : 0U);
        writer.u8(node.screen_included ? 1U : 0U);
        writer.u64(node.screen.sequence);
        if (node.screen_active) {
            writer.raw(node.screen_owner.runtime_epoch.bytes.data(), 16);
            writer.u16(node.screen_owner.node);
            writer.u64(node.screen_owner.generation);
            if (node.screen_included) for (const auto& cell : node.screen.cells) {
                writer.u8(cell.character);
                writer.u8(cell.attribute);
            }
        }
    }
    writer.u8(static_cast<std::uint8_t>(status.storage.state));
    writer.u8(static_cast<std::uint8_t>(status.storage.last_error));
    writer.u32(static_cast<std::uint32_t>(status.storage.schema_version));
    writer.u64(status.storage.committed_operations);
    writer.u32(static_cast<std::uint32_t>(status.storage.queued_operations));
    if (!writer.text(status.storage.engine_version, 64U) ||
        !writer.text(status.storage.last_operation, 128U)) return {};
    writer.u8(status.telnet_configured ? 1U : 0U);
    if (!writer.text(status.telnet_bind, kTelnetBindMaximum)) return {};
    writer.u16(status.telnet_port);
    writer.u8(status.telnet_bot_gate_enabled ? 1U : 0U);
    writer.u8(status.telnet_active ? 1U : 0U);
    writer.u8(status.ssh_configured ? 1U : 0U);
    if (!writer.text(status.ssh_bind, kTelnetBindMaximum)) return {};
    writer.u16(status.ssh_port);
    writer.u8(status.ssh_active ? 1U : 0U);
    writer.u8(status.listener_restart_required ? 1U : 0U);
    writer.u8(status.allow_new_users ? 1U : 0U);
    writer.u8(status.hostname_lookup ? 1U : 0U);
    writer.u8(status.hostname_exhausted ? 1U : 0U);
    if (status.pending_settings) {
        const auto pending = encode_settings_request(*status.pending_settings);
        if (pending.empty()) return {};
        writer.u16(static_cast<std::uint16_t>(pending.size()));
        writer.raw(pending.data(), pending.size());
    }
    writer.u16(static_cast<std::uint16_t>(status.logs.size()));
    for (const auto& entry : status.logs) {
        const auto record = encode_operational_record(entry);
        if (record.empty()) return {};
        writer.u64(entry.sequence);
        if (!writer.text(record, kOperationalRecordMaximum)) return {};
    }
    writer.u8(status.log_health);
    const auto statistic = [&writer](const AlphaStatisticValue& value) {
        writer.u8(value.available ? 1U : 0U);
        writer.u64(value.today);
        writer.u64(value.total);
    };
    statistic(status.statistics.successful_logons);
    statistic(status.statistics.caller_seconds);
    statistic(status.statistics.new_users);
    writer.u8(status.scheduler.configuration_loaded ? 1U : 0U);
    if (!writer.text(status.scheduler.job_id, 64U)) return {};
    writer.u8(status.scheduler.enabled ? 1U : 0U);
    writer.u64(status.scheduler.configuration_revision);
    writer.u8(status.scheduler.local_hour);
    writer.u8(status.scheduler.local_minute);
    writer.u64(static_cast<std::uint64_t>(status.scheduler.next_run_utc));
    writer.u8(status.scheduler.active ? 1U : 0U);
    writer.u8(status.scheduler.active_scheduled ? 1U : 0U);
    writer.u8(status.scheduler.queued ? 1U : 0U);
    if (!writer.text(status.scheduler.active_run_id, 32U)) return {};
    writer.u64(static_cast<std::uint64_t>(status.scheduler.active_started_utc));
    writer.u64(static_cast<std::uint64_t>(status.scheduler.last_run_utc));
    writer.u8(static_cast<std::uint8_t>(status.scheduler.last_result));
    if (!writer.text(status.scheduler.last_detail, 2048U)) return {};
    writer.u8(static_cast<std::uint8_t>(status.scheduler.history.size()));
    for (const auto& item : status.scheduler.history) {
        if (!writer.text(item.run_id, 32U)) return {};
        writer.u64(static_cast<std::uint64_t>(item.started_utc));
        writer.u64(static_cast<std::uint64_t>(item.finished_utc));
        writer.u8(static_cast<std::uint8_t>(item.result));
        if (!writer.text(item.detail, kSchedulerStatusHistoryDetailMaximum)) return {};
    }
    if (!writer.text(status.scheduler.diagnostic_code, 128U)) return {};
    if (!writer.text(status.diagnostic_code, 128U)) return {};
    writer.u64(status.configuration_revision);
    writer.u8(status.reporting.available ? 1 : 0);
    for (const auto count : status.reporting.counts) writer.u64(count);
    writer.u8(static_cast<std::uint8_t>(status.reporting.recent.size()));
    for (const auto& entry : status.reporting.recent) {
        writer.u64(entry.sequence);
        if (!writer.text(encode_operational_record(entry), kOperationalRecordMaximum)) return {};
    }
    writer.u8(status.administration_history.available ? 1 : 0);
    writer.u8(static_cast<std::uint8_t>(status.administration_history.recent.size()));
    for (const auto& entry : status.administration_history.recent) {
        writer.u8(entry.authentication ? 1 : 0);
        writer.u8(entry.kind);
        writer.u64(static_cast<std::uint64_t>(entry.subject_account_id));
        writer.u64(static_cast<std::uint64_t>(entry.actor_account_id));
        writer.u64(static_cast<std::uint64_t>(entry.occurred_utc_seconds));
        writer.raw(entry.event_id.bytes.data(),entry.event_id.bytes.size());
    }
    if (!status.external_event_activity.empty() && !writer.text(status.external_event_activity, 2048U)) return {};
    auto bytes = writer.take();
    return bytes.size() <= kRuntimeStatusSnapshotMaximumBytes ? bytes : std::vector<std::uint8_t>{};
}

std::optional<AlphaRuntimeStatus> decode_runtime_status(
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() > kRuntimeStatusSnapshotMaximumBytes) return std::nullopt;
    Reader reader(payload);
    AlphaRuntimeStatus status;
    if (!reader.raw(status.runtime_epoch.bytes.data(), 16) ||
        !reader.u64(status.status_sequence)) return std::nullopt;
    std::uint8_t presentation = 0, lifecycle = 0, owner = 0, loaded = 0, complete = 0;
    if (!reader.u8(presentation) || !reader.u8(lifecycle) || !reader.u8(owner) ||
        !reader.u16(status.lifecycle.configured_nodes) ||
        !reader.u16(status.lifecycle.active_work.caller_sessions) ||
        !reader.u16(status.lifecycle.active_work.supervised_processes) ||
        !reader.u8(loaded) || !reader.u8(complete) ||
        !reader.text(status.bbs_name, kBbsNameMaximum) ||
        !reader.text(status.bbs_time_zone, kBbsTimeZoneKeyMaximum)) return std::nullopt;
    if (presentation > static_cast<std::uint8_t>(AlphaRuntimePresentationState::stopped) ||
        lifecycle > static_cast<std::uint8_t>(RuntimeState::recovery_required) ||
        owner > static_cast<std::uint8_t>(RuntimeOwnershipMode::service) || loaded > 1U || complete > 1U)
        return std::nullopt;
    status.state = static_cast<AlphaRuntimePresentationState>(presentation);
    status.lifecycle.state = static_cast<RuntimeState>(lifecycle);
    status.lifecycle.owner_mode = static_cast<RuntimeOwnershipMode>(owner);
    status.configuration_loaded = loaded != 0;
    status.bootstrap_complete = complete != 0;
    std::uint64_t policy_revision = 0;
    if (!reader.u64(policy_revision) || policy_revision >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        !reader.text(status.security_policy_hash, 64U)) return std::nullopt;
    status.security_policy_revision = static_cast<std::int64_t>(policy_revision);
    std::uint16_t node_count = 0;
    if (!reader.u16(node_count) || node_count > kAlphaNodePoolCapacity) return std::nullopt;
    for (std::uint16_t index = 0; index < node_count; ++index) {
        AlphaNodeStatus node;
        std::uint8_t state = 0;
        std::uint8_t available = 0, enabled = 0, screen_active = 0, screen_included = 0;
        if (!reader.u16(node.number) || !reader.u8(state) || !reader.u8(available) ||
            !reader.u8(enabled) || node.number == 0 ||
            state > static_cast<std::uint8_t>(core::NodeState::draining)) return std::nullopt;
        std::uint8_t connected = 0, encrypted = 0;
        if (available > 1U || enabled > 1U ||
            !reader.text(node.caller_handle, 64U) || !reader.text(node.connection, 32U) ||
            !reader.text(node.activity, 64U) || !reader.text(node.details, 128U) ||
            !reader.text(node.source_ip, 64U) || !reader.text(node.hostname, kHostnameMaximum) ||
            !reader.u64(node.connected_seconds) ||
            !reader.u64(node.assignment_sequence) ||
            !reader.u16(node.door_id) || !reader.u64(node.door_launch_sequence) ||
            !nonnegative64(reader,node.account_id) ||
            !reader.u8(connected) || connected > 1U ||
            !reader.u8(encrypted) || encrypted > 1U ||
            !reader.u64(node.receive_activity) || !reader.u64(node.send_activity) ||
            !reader.u16(node.terminal_columns) || !reader.u16(node.terminal_rows) ||
            !node.terminal_columns || node.terminal_columns > 4096U ||
            !node.terminal_rows || node.terminal_rows > 4096U || !reader.u8(screen_active) ||
            !reader.u8(screen_included) || screen_included > 1U ||
            !reader.u64(node.screen.sequence) || screen_active > 1U) return std::nullopt;
        if (screen_active != 0) {
            if (!reader.raw(node.screen_owner.runtime_epoch.bytes.data(), 16) ||
                !reader.u16(node.screen_owner.node) ||
                !reader.u64(node.screen_owner.generation)) return std::nullopt;
            if (screen_included) for (auto& cell : node.screen.cells) {
                if (!reader.u8(cell.character) || !reader.u8(cell.attribute)) return std::nullopt;
            }
        }
        node.state = static_cast<core::NodeState>(state);
        node.available = available != 0;
        node.enabled = enabled != 0;
        node.caller_connected = connected != 0;
        node.ssh_encrypted = encrypted != 0;
        node.screen_active = screen_active != 0;
        node.screen_included = screen_included != 0;
        status.nodes.push_back(node);
    }
    std::uint8_t storage_state = 0, storage_error = 0;
    std::uint32_t schema_version = 0, queued = 0;
    if (!reader.u8(storage_state) || !reader.u8(storage_error) || reader.u32(schema_version) == false ||
        !reader.u64(status.storage.committed_operations) || !reader.u32(queued) ||
        !reader.text(status.storage.engine_version, 64U) ||
        !reader.text(status.storage.last_operation, 128U)) return std::nullopt;
    if (storage_state > static_cast<std::uint8_t>(StorageRuntimeState::stopping) ||
        storage_error > static_cast<std::uint8_t>(StorageErrorCode::system_sysop_protected)) return std::nullopt;
    status.storage.state = static_cast<StorageRuntimeState>(storage_state);
    status.storage.last_error = static_cast<StorageErrorCode>(storage_error);
    status.storage.schema_version = static_cast<std::int32_t>(schema_version);
    status.storage.queued_operations = queued;
    std::uint8_t configured = 0, bot_gate = 0, active = 0, restart = 0;
    std::uint8_t ssh_configured = 0, ssh_active = 0, allow_new_users = 0;
    if (!reader.u8(configured) || !reader.text(status.telnet_bind, kTelnetBindMaximum) ||
        !reader.u16(status.telnet_port) || !reader.u8(bot_gate) || !reader.u8(active) ||
        !reader.u8(ssh_configured) || !reader.text(status.ssh_bind, kTelnetBindMaximum) ||
        !reader.u16(status.ssh_port) || !reader.u8(ssh_active) ||
        ssh_configured > 1U || ssh_active > 1U ||
        !reader.u8(restart) || configured > 1U || bot_gate > 1U || active > 1U || restart > 1U)
        return std::nullopt;
    if (!reader.u8(allow_new_users) || allow_new_users > 1U) return std::nullopt;
    status.allow_new_users = allow_new_users != 0;
    std::uint8_t hostname_lookup = 0, hostname_exhausted = 0;
    if (!reader.u8(hostname_lookup) || hostname_lookup > 1U ||
        !reader.u8(hostname_exhausted) || hostname_exhausted > 1U) return std::nullopt;
    status.hostname_lookup = hostname_lookup != 0;
    status.hostname_exhausted = hostname_exhausted != 0;
    if (restart) {
        std::uint16_t size = 0;
        constexpr std::size_t maximum = kBbsNameMaximum + 2 * kTelnetBindMaximum + 13;
        if (!reader.u16(size) || !size || size > maximum) return std::nullopt;
        std::vector<std::uint8_t> pending(size);
        if (!reader.raw(pending.data(), pending.size())) return std::nullopt;
        status.pending_settings = decode_settings_request(pending);
        if (!status.pending_settings) return std::nullopt;
    }
    std::uint16_t log_count = 0;
    if (!reader.u16(log_count) || log_count > kControlPanelMaximumLogEntries) return std::nullopt;
    for (std::uint16_t index = 0; index < log_count; ++index) {
        std::uint64_t sequence = 0;
        std::string record;
        if (!reader.u64(sequence) || !reader.text(record, kOperationalRecordMaximum)) return std::nullopt;
        auto entry = decode_operational_record(record, sequence);
        if (!entry) return std::nullopt;
        status.logs.push_back(std::move(*entry));
    }
    if (!reader.u8(status.log_health) || status.log_health > 31) return std::nullopt;
    const auto statistic = [&reader](AlphaStatisticValue& value) {
        std::uint8_t available = 0;
        if (!reader.u8(available) || available > 1U || !reader.u64(value.today) ||
            !reader.u64(value.total)) return false;
        value.available = available != 0;
        return true;
    };
    if (!statistic(status.statistics.successful_logons) ||
        !statistic(status.statistics.caller_seconds) ||
        !statistic(status.statistics.new_users)) return std::nullopt;
    std::uint8_t scheduler_loaded = 0, scheduler_enabled = 0,
                 scheduler_active = 0, scheduler_scheduled = 0,
                 scheduler_queued = 0,
                 scheduler_result = 0,
                 scheduler_history_count = 0;
    std::uint64_t next_run = 0, active_started = 0, last_run = 0;
    if (!reader.u8(scheduler_loaded) || scheduler_loaded > 1U ||
        !reader.text(status.scheduler.job_id, 64U) ||
        !reader.u8(scheduler_enabled) || scheduler_enabled > 1U ||
        !reader.u64(status.scheduler.configuration_revision) ||
        !reader.u8(status.scheduler.local_hour) ||
        !reader.u8(status.scheduler.local_minute) ||
        !reader.u64(next_run) ||
        !reader.u8(scheduler_active) || scheduler_active > 1U ||
        !reader.u8(scheduler_scheduled) || scheduler_scheduled > 1U ||
        !reader.u8(scheduler_queued) || scheduler_queued > 1U ||
        !reader.text(status.scheduler.active_run_id, 32U) ||
        !reader.u64(active_started) || !reader.u64(last_run) ||
        !reader.u8(scheduler_result) ||
        scheduler_result > static_cast<std::uint8_t>(ScheduledJobResult::interrupted) ||
        !reader.text(status.scheduler.last_detail, 2048U) ||
        !reader.u8(scheduler_history_count) ||
        next_run > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        active_started > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        last_run > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        scheduler_history_count > kSchedulerHistoryLimit) return std::nullopt;
    for (std::uint8_t index = 0; index < scheduler_history_count; ++index) {
        ScheduledJobHistory item;
        std::uint64_t started = 0, finished = 0;
        std::uint8_t result = 0;
        if (!reader.text(item.run_id, 32U) || !reader.u64(started) ||
            !reader.u64(finished) || !reader.u8(result) ||
            result > static_cast<std::uint8_t>(ScheduledJobResult::interrupted) ||
            !reader.text(item.detail, kSchedulerStatusHistoryDetailMaximum) ||
            started > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            finished > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return std::nullopt;
        item.started_utc = static_cast<std::int64_t>(started);
        item.finished_utc = static_cast<std::int64_t>(finished);
        item.result = static_cast<ScheduledJobResult>(result);
        status.scheduler.history.push_back(std::move(item));
    }
    if (!reader.text(status.scheduler.diagnostic_code, 128U) ||
        !reader.text(status.diagnostic_code, 128U) ||
        !reader.u64(status.configuration_revision)) return std::nullopt;
    std::uint8_t reporting_available = 0, recent_count = 0;
    if (!reader.u8(reporting_available) || reporting_available > 1) return std::nullopt;
    status.reporting.available = reporting_available != 0;
    for (auto& count : status.reporting.counts) if (!reader.u64(count)) return std::nullopt;
    if (!reader.u8(recent_count) || recent_count > kRecentOperatingEvents) return std::nullopt;
    for (unsigned index = 0; index < recent_count; ++index) {
        std::uint64_t sequence = 0;
        std::string record;
        if (!reader.u64(sequence) || !reader.text(record,kOperationalRecordMaximum)) return std::nullopt;
        auto entry = decode_operational_record(record,sequence);
        if (!entry) return std::nullopt;
        status.reporting.recent.push_back(std::move(*entry));
    }
    if (!reader.u8(reporting_available) || reporting_available > 1 ||
        !reader.u8(recent_count) || recent_count > kAdministrationHistoryLimit) return std::nullopt;
    status.administration_history.available = reporting_available != 0;
    for (unsigned index = 0; index < recent_count; ++index) {
        AdministrationAuditSummary entry;
        std::uint8_t authentication = 0;
        std::uint64_t subject = 0, actor = 0, utc = 0;
        if (!reader.u8(authentication) || authentication > 1 || !reader.u8(entry.kind) ||
            !reader.u64(subject) || subject > INT64_MAX || !reader.u64(actor) || actor > INT64_MAX ||
            !reader.u64(utc) || !reader.raw(entry.event_id.bytes.data(),entry.event_id.bytes.size())) return std::nullopt;
        entry.authentication = authentication != 0;
        entry.subject_account_id = static_cast<AccountId>(subject);
        entry.actor_account_id = static_cast<AccountId>(actor);
        entry.occurred_utc_seconds = utc <= INT64_MAX ? static_cast<std::int64_t>(utc) :
            -1 - static_cast<std::int64_t>(UINT64_MAX - utc);
        status.administration_history.recent.push_back(entry);
    }
    if (!reader.complete() && (!reader.text(status.external_event_activity, 2048U) ||
            status.external_event_activity.empty())) return std::nullopt;
    if (!reader.complete()) return std::nullopt;
    status.scheduler.configuration_loaded = scheduler_loaded != 0;
    status.scheduler.enabled = scheduler_enabled != 0;
    status.scheduler.next_run_utc = static_cast<std::int64_t>(next_run);
    status.scheduler.active = scheduler_active != 0;
    status.scheduler.active_scheduled = scheduler_scheduled != 0;
    status.scheduler.queued = scheduler_queued != 0;
    status.scheduler.active_started_utc = static_cast<std::int64_t>(active_started);
    status.scheduler.last_run_utc = static_cast<std::int64_t>(last_run);
    status.scheduler.last_result = static_cast<ScheduledJobResult>(scheduler_result);
    status.telnet_configured = configured != 0;
    status.telnet_bot_gate_enabled = bot_gate != 0;
    status.telnet_active = active != 0;
    status.ssh_configured = ssh_configured != 0;
    status.ssh_active = ssh_active != 0;
    status.listener_restart_required = restart != 0;
    if (!valid_alpha_runtime_status(status)) return std::nullopt;
    return status;
}

std::vector<std::uint8_t> encode_status_request(const AlphaStatusRequest& request) {
    if (request.screen_node > kAlphaNodePoolCapacity ||
        request.offset >= kRuntimeStatusSnapshotMaximumBytes ||
        request.offset % kRuntimeStatusChunkBytes != 0 ||
        ((request.snapshot_sequence == 0) != (request.offset == 0))) return {};
    Writer writer;
    writer.u16(request.screen_node); writer.u64(request.snapshot_sequence); writer.u32(request.offset);
    return writer.take();
}

std::optional<AlphaStatusRequest> decode_status_request(const std::vector<std::uint8_t>& payload) {
    Reader reader(payload); AlphaStatusRequest request;
    if (!reader.u16(request.screen_node) || !reader.u64(request.snapshot_sequence) ||
        !reader.u32(request.offset) || !reader.complete() || encode_status_request(request).empty())
        return std::nullopt;
    return request;
}

bool AlphaStatusSnapshotCache::capture(AlphaRuntimeStatus status, const std::uint16_t selected) {
    bytes_.clear(); sequence_ = 0; screen_node_ = 0;
    if (selected > status.lifecycle.configured_nodes || !valid_alpha_runtime_status(status)) return false;
    for (auto& node : status.nodes) {
        node.screen_included = node.screen_active && node.number == selected;
        if (!node.screen_included) node.screen.cells.fill({});
    }
    auto encoded = encode_runtime_status(status);
    if (encoded.empty()) return false;
    sequence_ = status.status_sequence; screen_node_ = selected;
    bytes_ = std::move(encoded);
    return true;
}

std::vector<std::uint8_t> AlphaStatusSnapshotCache::chunk(const AlphaStatusRequest& request) const {
    if (encode_status_request(request).empty() || bytes_.empty() || request.screen_node != screen_node_ ||
        (request.snapshot_sequence != 0 && request.snapshot_sequence != sequence_) ||
        request.offset >= bytes_.size()) return {};
    Writer writer;
    writer.u64(sequence_); writer.u32(request.offset); writer.u32(static_cast<std::uint32_t>(bytes_.size()));
    const auto count = std::min(kRuntimeStatusChunkBytes, bytes_.size() - request.offset);
    writer.raw(bytes_.data() + request.offset, count);
    return writer.take();
}

std::optional<AlphaRuntimeStatus> read_status_snapshot(const std::uint16_t selected,
    const std::function<std::optional<std::vector<std::uint8_t>>(const AlphaStatusRequest&)>& exchange) {
    if (selected > kAlphaNodePoolCapacity || !exchange) return std::nullopt;
    std::vector<std::uint8_t> bytes;
    std::uint64_t sequence = 0;
    std::uint32_t expected_total = 0;
    constexpr auto maximum_chunks = (kRuntimeStatusSnapshotMaximumBytes + kRuntimeStatusChunkBytes - 1) /
        kRuntimeStatusChunkBytes;
    for (std::size_t index = 0; index < maximum_chunks; ++index) {
        const auto payload = exchange({selected, sequence, static_cast<std::uint32_t>(bytes.size())});
        if (!payload || payload->size() <= kRuntimeStatusChunkHeaderBytes ||
            payload->size() > kLocalControlMaximumPayloadBytes) return std::nullopt;
        Reader reader(*payload);
        std::uint64_t actual_sequence = 0;
        std::uint32_t offset = 0, total = 0;
        if (!reader.u64(actual_sequence) || !reader.u32(offset) || !reader.u32(total) ||
            !actual_sequence || !total || total > kRuntimeStatusSnapshotMaximumBytes || offset != bytes.size() ||
            offset >= total || (sequence && actual_sequence != sequence) || (expected_total && total != expected_total))
            return std::nullopt;
        const auto count = std::min(kRuntimeStatusChunkBytes, static_cast<std::size_t>(total - offset));
        if (payload->size() != kRuntimeStatusChunkHeaderBytes + count) return std::nullopt;
        if (!sequence) { sequence = actual_sequence; expected_total = total; bytes.reserve(total); }
        bytes.insert(bytes.end(), payload->begin() + kRuntimeStatusChunkHeaderBytes, payload->end());
        if (bytes.size() == total) {
            auto status = decode_runtime_status(bytes);
            if (!status || status->status_sequence != sequence || selected > status->lifecycle.configured_nodes)
                return std::nullopt;
            for (const auto& node : status->nodes)
                if (node.screen_included != (node.screen_active && node.number == selected)) return std::nullopt;
            return status;
        }
    }
    return std::nullopt;
}

std::string bounded_scheduler_status_history_detail(const std::string_view value) {
    if (value.size() <= kSchedulerStatusHistoryDetailMaximum)
        return std::string(value);
    constexpr std::size_t suffix_size = 3;
    std::size_t length = kSchedulerStatusHistoryDetailMaximum - suffix_size;
    while (length &&
        (static_cast<unsigned char>(value[length]) & 0xC0U) == 0x80U) --length;
    return std::string(value.substr(0, length)) + "...";
}

std::vector<std::uint8_t> encode_runtime_metrics(
    const AlphaRuntimeMetricSnapshot& metrics) {
    if (!metrics.uptime_available && metrics.uptime_milliseconds) return {};
    Writer writer;
    writer.u8(metrics.sqlite.available ? 1U : 0U);
    writer.u8(metrics.sqlite.interval_available ? 1U : 0U);
    writer.u64(metrics.sqlite.page_reads_per_second);
    writer.u64(metrics.sqlite.page_writes_per_second);
    writer.u8(metrics.sqlite.cache_hit_available ? 1U : 0U);
    writer.u16(metrics.sqlite.cache_hit_basis_points);
    writer.u8(metrics.uptime_available ? 1 : 0);
    writer.u64(metrics.uptime_milliseconds);
    return writer.take();
}

std::optional<AlphaRuntimeMetricSnapshot> decode_runtime_metrics(
    const std::vector<std::uint8_t>& payload) noexcept {
    Reader reader(payload);
    AlphaRuntimeMetricSnapshot metrics;
    std::uint8_t available = 0, interval = 0, cache_available = 0, uptime_available = 0;
    if (!reader.u8(available) || !reader.u8(interval) ||
        !reader.u64(metrics.sqlite.page_reads_per_second) ||
        !reader.u64(metrics.sqlite.page_writes_per_second) ||
        !reader.u8(cache_available) ||
        !reader.u16(metrics.sqlite.cache_hit_basis_points) ||
        !reader.u8(uptime_available) || !reader.u64(metrics.uptime_milliseconds) ||
        uptime_available > 1 || (!uptime_available && metrics.uptime_milliseconds) || !reader.complete() ||
        available > 1U || interval > 1U || cache_available > 1U ||
        metrics.sqlite.cache_hit_basis_points > 10000U ||
        (interval != 0 && available == 0) ||
        (cache_available != 0 && interval == 0)) return std::nullopt;
    metrics.sqlite.available = available != 0;
    metrics.sqlite.interval_available = interval != 0;
    metrics.sqlite.cache_hit_available = cache_available != 0;
    metrics.uptime_available = uptime_available != 0;
    return metrics;
}

std::vector<std::uint8_t> encode_log_cursor(OperationalLogCursor cursor) {
    if (!cursor.epoch.valid()) return {};
    Writer writer; writer.raw(cursor.epoch.bytes.data(), 16); writer.u64(cursor.after);
    return writer.take();
}
std::optional<OperationalLogCursor> decode_log_cursor(const std::vector<std::uint8_t>& payload) {
    Reader reader(payload); OperationalLogCursor cursor;
    if (!reader.raw(cursor.epoch.bytes.data(), 16) || !reader.u64(cursor.after) ||
        !reader.complete() || !cursor.epoch.valid()) return std::nullopt;
    return cursor;
}
std::vector<std::uint8_t> encode_log_page(const OperationalLogPage& page) {
    if (!valid_operational_page(page)) return {};
    Writer writer;
    writer.raw(page.cursor.epoch.bytes.data(), 16); writer.u64(page.cursor.after);
    writer.u64(page.oldest); writer.u64(page.newest); writer.u64(page.next);
    writer.u8(page.gap ? 1 : 0); writer.u16(static_cast<std::uint16_t>(page.entries.size()));
    for (const auto& item : page.entries) {
        writer.u64(item.sequence);
        if (!writer.text(item.record, kOperationalRecordMaximum)) return {};
    }
    return writer.take();
}
std::optional<OperationalLogPage> decode_log_page(const std::vector<std::uint8_t>& payload) {
    if (payload.size() > kOperationalPageBytes) return std::nullopt;
    Reader reader(payload); OperationalLogPage page; std::uint8_t gap = 0; std::uint16_t count = 0;
    if (!reader.raw(page.cursor.epoch.bytes.data(), 16) || !reader.u64(page.cursor.after) ||
        !reader.u64(page.oldest) || !reader.u64(page.newest) || !reader.u64(page.next) ||
        !reader.u8(gap) || gap > 1 || !reader.u16(count) || count > kOperationalPageEntries) return std::nullopt;
    page.gap = gap != 0;
    for (std::uint16_t i = 0; i < count; ++i) {
        OperationalEncodedEntry entry;
        if (!reader.u64(entry.sequence) || !reader.text(entry.record, kOperationalRecordMaximum)) return std::nullopt;
        page.entries.push_back(std::move(entry));
    }
    if (!reader.complete() || !valid_operational_page(page)) return std::nullopt;
    return page;
}

std::string_view alpha_recovery_guidance(const std::string_view diagnostic) noexcept {
    if (diagnostic == "configuration_missing")
        return "The active Config\\SFX.conf is missing from an existing installation. "
            "Close SFX and restore this installation's known-good configuration to that path, "
            "preserving its protected ownership and permissions. Then restart SFX. "
            "Do not run first-run setup or replace the database or SSH host key.";
    if (diagnostic.substr(0, 14) == "configuration_" || diagnostic == "alpha_node_count_requires_1_to_128")
        return "The active Config\\SFX.conf is invalid, unavailable, or not securely protected. "
            "Close SFX and correct or restore this installation's known-good configuration, "
            "preserving its protected ownership and permissions. Then restart SFX. "
            "Do not replace the database or SSH host key.";
    return "Close SFX and preserve the existing installation files. "
        "Resolve the reported Runtime problem before restarting. "
        "Do not run first-run setup, delete installation state, or replace credentials "
        "to bypass recovery. Interrupted setup requires owner-assisted recovery.";
}

std::optional<AlphaControlResponse> bootstrap_prerequisite_failure(const AlphaRuntimeStatus& status) {
    const bool setup_state = status.state == AlphaRuntimePresentationState::setup_required;
    const bool valid_policy = status.security_policy_revision > 0 && status.security_policy_hash.size() == 64U &&
        std::all_of(status.security_policy_hash.begin(), status.security_policy_hash.end(),
            [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    if (setup_state && valid_policy) return std::nullopt;
    return AlphaControlResponse{LocalControlResponseCode::recovery_required,
        status.diagnostic_code.empty() ? "runtime_setup_prerequisites_unavailable" : status.diagnostic_code};
}

std::vector<std::uint8_t> encode_bootstrap_request(const AlphaBootstrapRequest& request) {
    const std::string configuration = serialize_operational_configuration(request.configuration);
    if (configuration.empty()) return {};
    Writer writer;
    if (!writer.text(configuration, kOperationalConfigurationMaximumBytes) ||
        !writer.text(request.sysop_name, kAccountNameMaximum) ||
        !writer.text(request.sysop_password, kPasswordMaximum) ||
        !writer.text(request.system_password, kPasswordMaximum)) return {};
    auto bytes = writer.take();
    return bytes.size() <= kLocalControlMaximumPayloadBytes ? bytes : std::vector<std::uint8_t>{};
}

std::optional<AlphaBootstrapRequest> decode_bootstrap_request(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    std::string configuration;
    AlphaBootstrapRequest request;
    if (!reader.text(configuration, kOperationalConfigurationMaximumBytes) ||
        !reader.text(request.sysop_name, kAccountNameMaximum) ||
        !reader.text(request.sysop_password, kPasswordMaximum) ||
        !reader.text(request.system_password, kPasswordMaximum) || !reader.complete()) {
        erase_bootstrap_secrets(request);
        return std::nullopt;
    }
    auto parsed = parse_operational_configuration(configuration);
    if (!parsed.valid()) {
        erase_bootstrap_secrets(request);
        return std::nullopt;
    }
    request.configuration = std::move(*parsed.configuration);
    return request;
}

std::vector<std::uint8_t> encode_control_response(const AlphaControlResponse& response) {
    if (response.diagnostic_code.size() > 128U) return {};
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(response.code));
    writer.raw(response.runtime_epoch.bytes.data(), 16);
    if (!writer.text(response.diagnostic_code, 128U)) return {};
    return writer.take();
}

std::optional<AlphaControlResponse> decode_control_response(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    AlphaControlResponse response;
    std::uint8_t code = 0;
    if (!reader.u8(code) || code > static_cast<std::uint8_t>(LocalControlResponseCode::internal_failure) ||
        !reader.raw(response.runtime_epoch.bytes.data(), 16) ||
        !reader.text(response.diagnostic_code, 128U) || !reader.complete()) return std::nullopt;
    response.code = static_cast<LocalControlResponseCode>(code);
    return response;
}

std::vector<std::uint8_t> encode_node_command_request(
    const AlphaNodeCommandRequest& request) {
    if (request.node_number == 0 || request.node_number > kAlphaNodePoolCapacity ||
        request.command > AlphaNodeCommandKind::disable_observed ||
        (request.command == AlphaNodeCommandKind::terminate_door ? !request.expected_door_launch : request.expected_door_launch!=0) ||
        (request.command == AlphaNodeCommandKind::disconnect || request.command == AlphaNodeCommandKind::terminate_door
            ? (!request.expected_epoch.valid() || request.expected_generation == 0)
            : request.command == AlphaNodeCommandKind::disable_observed
                ? !request.expected_epoch.valid()
                : (request.expected_epoch.valid() || request.expected_generation != 0))) return {};
    Writer writer;
    writer.u16(request.node_number);
    writer.u8(static_cast<std::uint8_t>(request.command));
    writer.raw(request.expected_epoch.bytes.data(), 16);
    writer.u64(request.expected_generation);
    writer.u64(request.expected_door_launch);
    return writer.take();
}

std::vector<std::uint8_t> encode_lifecycle_command_request(
    const AlphaLifecycleCommandRequest& request) {
    if (request.command > AlphaLifecycleCommandKind::cancel_drain ||
        (request.command == AlphaLifecycleCommandKind::begin_drain
            ? (request.timeout_seconds == 0 || request.timeout_seconds > 3600)
            : request.timeout_seconds != 0)) return {};
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(request.command));
    writer.u32(request.timeout_seconds);
    return writer.take();
}

std::vector<std::uint8_t> encode_throttle_clear_request(const LocalThrottleClearRequest& request) {
    if (!valid_local_throttle_clear(request)) return {};
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(request.scope));
    if (!writer.text(request.subject,45)) return {};
    return writer.take();
}

std::optional<LocalThrottleClearRequest> decode_throttle_clear_request(const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    LocalThrottleClearRequest request;
    std::uint8_t scope{};
    if (!reader.u8(scope) || !reader.text(request.subject,45) || !reader.complete()) return {};
    request.scope = static_cast<RateLimitScope>(scope);
    return valid_local_throttle_clear(request) ? std::optional<LocalThrottleClearRequest>(request) : std::nullopt;
}

std::optional<AlphaLifecycleCommandRequest> decode_lifecycle_command_request(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    AlphaLifecycleCommandRequest request;
    std::uint8_t command = 0;
    if (!reader.u8(command) || !reader.u32(request.timeout_seconds) || !reader.complete())
        return std::nullopt;
    request.command = static_cast<AlphaLifecycleCommandKind>(command);
    if (encode_lifecycle_command_request(request).empty()) return std::nullopt;
    return request;
}

std::optional<AlphaNodeCommandRequest> decode_node_command_request(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    AlphaNodeCommandRequest request;
    std::uint8_t command = 0;
    if (!reader.u16(request.node_number) || !reader.u8(command) ||
        !reader.raw(request.expected_epoch.bytes.data(), 16) ||
        !reader.u64(request.expected_generation) || !reader.u64(request.expected_door_launch) || !reader.complete() ||
        request.node_number == 0 || request.node_number > kAlphaNodePoolCapacity ||
        command > static_cast<std::uint8_t>(AlphaNodeCommandKind::disable_observed))
        return std::nullopt;
    request.command = static_cast<AlphaNodeCommandKind>(command);
    if (encode_node_command_request(request).empty()) return std::nullopt;
    return request;
}

std::vector<std::uint8_t> encode_local_session_start_request(
    const AlphaLocalSessionStartRequest& request) {
    if (request.node_number == 0 || request.node_number > kAlphaNodePoolCapacity) return {};
    Writer writer;
    writer.u16(request.node_number);
    return writer.take();
}

std::optional<AlphaLocalSessionStartRequest> decode_local_session_start_request(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    AlphaLocalSessionStartRequest request;
    if (!reader.u16(request.node_number) || !reader.complete() || request.node_number == 0 ||
        request.node_number > kAlphaNodePoolCapacity) return std::nullopt;
    return request;
}

std::vector<std::uint8_t> encode_local_session_start_result(
    const AlphaLocalSessionStartResult& result) {
    const bool success = result.response.code == LocalControlResponseCode::ok;
    if (result.response.runtime_epoch.valid() || result.response.diagnostic_code.size() > 128U ||
        (success && (result.host != "127.0.0.1" || result.port == 0 ||
            !result.reservation.valid() || result.reservation.node > kAlphaNodePoolCapacity ||
            result.timeout_milliseconds != kAlphaLocalSessionTimeoutMilliseconds)) ||
        (!success && (!result.host.empty() || result.port != 0 || result.reservation.valid() ||
            result.timeout_milliseconds != 0))) return {};
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(result.response.code));
    if (!writer.text(result.response.diagnostic_code, 128U) || !writer.text(result.host, 64U)) return {};
    writer.u16(result.port);
    writer.raw(result.reservation.runtime_epoch.bytes.data(), result.reservation.runtime_epoch.bytes.size());
    writer.u16(result.reservation.node);
    writer.u64(result.reservation.generation);
    writer.u32(result.timeout_milliseconds);
    return writer.take();
}

std::optional<AlphaLocalSessionStartResult> decode_local_session_start_result(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    AlphaLocalSessionStartResult result;
    std::uint8_t code = 0;
    if (!reader.u8(code) || code > static_cast<std::uint8_t>(LocalControlResponseCode::internal_failure) ||
        !reader.text(result.response.diagnostic_code, 128U) || !reader.text(result.host, 64U) ||
        !reader.u16(result.port) ||
        !reader.raw(result.reservation.runtime_epoch.bytes.data(), result.reservation.runtime_epoch.bytes.size()) ||
        !reader.u16(result.reservation.node) || !reader.u64(result.reservation.generation) ||
        !reader.u32(result.timeout_milliseconds) || !reader.complete()) return std::nullopt;
    result.response.code = static_cast<LocalControlResponseCode>(code);
    const bool success = result.response.code == LocalControlResponseCode::ok;
    if ((success && (result.host != "127.0.0.1" || result.port == 0 ||
            !result.reservation.valid() || result.reservation.node > kAlphaNodePoolCapacity ||
            result.timeout_milliseconds != kAlphaLocalSessionTimeoutMilliseconds)) ||
        (!success && (!result.host.empty() || result.port != 0 || result.reservation.valid() ||
            result.timeout_milliseconds != 0))) return std::nullopt;
    return result;
}

std::vector<std::uint8_t> encode_local_session_cancel_request(
    const AlphaLocalSessionCancelRequest& request) {
    if (!request.reservation.valid() || request.reservation.node > kAlphaNodePoolCapacity ||
        request.reason > AlphaLocalSessionCancelReason::client_launch_failed) return {};
    Writer writer;
    writer.raw(request.reservation.runtime_epoch.bytes.data(), request.reservation.runtime_epoch.bytes.size());
    writer.u16(request.reservation.node);
    writer.u64(request.reservation.generation);
    writer.u8(static_cast<std::uint8_t>(request.reason));
    return writer.take();
}

std::optional<AlphaLocalSessionCancelRequest> decode_local_session_cancel_request(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    AlphaLocalSessionCancelRequest request;
    std::uint8_t reason = 0;
    if (!reader.raw(request.reservation.runtime_epoch.bytes.data(), request.reservation.runtime_epoch.bytes.size()) ||
        !reader.u16(request.reservation.node) || !reader.u64(request.reservation.generation) ||
        !reader.u8(reason) || !reader.complete() || !request.reservation.valid() ||
        request.reservation.node > kAlphaNodePoolCapacity ||
        reason > static_cast<std::uint8_t>(AlphaLocalSessionCancelReason::client_launch_failed))
        return std::nullopt;
    request.reason = static_cast<AlphaLocalSessionCancelReason>(reason);
    return request;
}

std::vector<std::uint8_t> encode_settings_request(const AlphaSettingsRequest& request) {
    if (request.bbs_name.empty() || request.bbs_name.size() > kBbsNameMaximum ||
        request.telnet_bind.empty() || request.telnet_bind.size() > kTelnetBindMaximum ||
        request.telnet_port == 0 || request.ssh_bind.empty() ||
        request.ssh_bind.size() > kTelnetBindMaximum || request.ssh_port == 0) return {};
    Writer writer;
    if (!writer.text(request.bbs_name, kBbsNameMaximum) ||
        !writer.text(request.telnet_bind, kTelnetBindMaximum)) return {};
    writer.u16(request.telnet_port);
    writer.u8(request.ssh_enabled ? 1U : 0U);
    if (!writer.text(request.ssh_bind, kTelnetBindMaximum)) return {};
    writer.u16(request.ssh_port);
    writer.u8(request.allow_new_users ? 1U : 0U);
    writer.u8(request.hostname_lookup ? 1U : 0U);
    return writer.take();
}

std::optional<AlphaSettingsRequest> decode_settings_request(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    AlphaSettingsRequest request;
    std::uint8_t ssh_enabled = 0, allow_new_users = 0, hostname_lookup = 0;
    if (!reader.text(request.bbs_name, kBbsNameMaximum) ||
        !reader.text(request.telnet_bind, kTelnetBindMaximum) ||
        !reader.u16(request.telnet_port) || !reader.u8(ssh_enabled) || ssh_enabled > 1U ||
        !reader.text(request.ssh_bind, kTelnetBindMaximum) || !reader.u16(request.ssh_port) ||
        !reader.u8(allow_new_users) || allow_new_users > 1U ||
        !reader.u8(hostname_lookup) || hostname_lookup > 1U ||
        request.ssh_bind.empty() || request.ssh_port == 0 || !reader.complete() ||
        request.bbs_name.empty() || request.telnet_bind.empty() || request.telnet_port == 0)
        return std::nullopt;
    request.ssh_enabled = ssh_enabled != 0;
    request.allow_new_users = allow_new_users != 0;
    request.hostname_lookup = hostname_lookup != 0;
    return request;
}

std::vector<std::uint8_t> encode_configuration_snapshot(
    const AlphaConfigurationSnapshot& snapshot) {
    const auto active = serialize_operational_configuration(snapshot.active);
    const auto saved = serialize_operational_configuration(snapshot.saved);
    if (active.empty() || saved.empty() ||
        snapshot.activation > AlphaConfigurationActivation::refused ||
        snapshot.detail.empty() || snapshot.detail.size() > 256U) return {};
    Writer writer;
    if (!writer.text(active, kOperationalConfigurationMaximumBytes) ||
        !writer.text(saved, kOperationalConfigurationMaximumBytes)) return {};
    writer.u8(static_cast<std::uint8_t>(snapshot.activation));
    return writer.text(snapshot.detail, 256U) ? writer.take() : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> encode_botgate_feed_status(const BotGateFeedStatus& status) {
    if (status.last_attempt<0 || status.last_success<0 || status.entries>kBotGateFeedMaximumEntries ||
        status.bytes_received>kBotGateFeedMaximumBytes || status.result.empty() || status.result.size()>96 ||
        (status.available != (status.entries>0 && status.last_success>0)) ||
        std::any_of(status.result.begin(),status.result.end(),[](char c) {
            return !((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_');
        })) return {};
    Writer writer; writer.u8(status.updating ? 1 : 0); writer.u8(status.available ? 1 : 0);
    writer.u64(static_cast<std::uint64_t>(status.last_attempt)); writer.u64(static_cast<std::uint64_t>(status.last_success));
    writer.u64(status.bytes_received); writer.u64(status.entries);
    if (!writer.text(status.result,96)) return {};
    writer.u8(status.enabled); writer.u8(status.telnet); writer.u8(status.ssh); writer.u8(status.rules_ready);
    writer.u64(status.protection_started);
    for(auto n:status.protection_counts) writer.u64(n);
    for(auto n:status.rule_counts) writer.u64(n);
    const auto safe=[](const std::string& s) {return std::all_of(s.begin(),s.end(),[](unsigned char c){return c>=32 && c<=126;});};
    if(!safe(status.rule_status) || !safe(status.geo_summary) || !writer.text(status.rule_status,128) || !writer.text(status.geo_summary,4096)) return {};
    return writer.take();
}
std::optional<BotGateFeedStatus> decode_botgate_feed_status(const std::vector<std::uint8_t>& payload) {
    Reader reader(payload); BotGateFeedStatus status;
    std::uint8_t updating{},available{}; std::uint64_t attempted{},succeeded{};
    if (!reader.u8(updating) || updating>1 || !reader.u8(available) || available>1 ||
        !reader.u64(attempted) || attempted>INT64_MAX || !reader.u64(succeeded) || succeeded>INT64_MAX ||
        !reader.u64(status.bytes_received) || !reader.u64(status.entries) || !reader.text(status.result,96)) return {};
    std::uint8_t enabled{},telnet{},ssh{},ready{};
    if(!reader.u8(enabled) || enabled>1 || !reader.u8(telnet) || telnet>1 || !reader.u8(ssh) || ssh>1 || !reader.u8(ready) || ready>1 || !reader.u64(status.protection_started)) return {};
    status.enabled=enabled!=0;status.telnet=telnet!=0;status.ssh=ssh!=0;status.rules_ready=ready!=0;
    for(auto& n:status.protection_counts) if(!reader.u64(n)) return {};
    for(auto& n:status.rule_counts) if(!reader.u64(n)) return {};
    if(!reader.text(status.rule_status,128) || !reader.text(status.geo_summary,4096) || !reader.complete()) return {};
    status.updating=updating!=0; status.available=available!=0;
    status.last_attempt=static_cast<std::int64_t>(attempted); status.last_success=static_cast<std::int64_t>(succeeded);
    return encode_botgate_feed_status(status).empty() ? std::nullopt : std::optional<BotGateFeedStatus>(status);
}

std::optional<AlphaConfigurationSnapshot> decode_configuration_snapshot(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    std::string active_text, saved_text;
    std::uint8_t activation = 0;
    AlphaConfigurationSnapshot snapshot;
    if (!reader.text(active_text, kOperationalConfigurationMaximumBytes) ||
        !reader.text(saved_text, kOperationalConfigurationMaximumBytes) ||
        !reader.u8(activation) ||
        activation > static_cast<std::uint8_t>(AlphaConfigurationActivation::refused) ||
        !reader.text(snapshot.detail, 256U) || snapshot.detail.empty() ||
        !reader.complete()) return std::nullopt;
    const auto active = parse_operational_configuration(active_text);
    const auto saved = parse_operational_configuration(saved_text);
    if (!active.valid() || !saved.valid()) return std::nullopt;
    snapshot.active = *active.configuration;
    snapshot.saved = *saved.configuration;
    snapshot.activation = static_cast<AlphaConfigurationActivation>(activation);
    return snapshot;
}

std::vector<std::uint8_t> encode_configuration_update_request(
    const AlphaConfigurationUpdateRequest& request) {
    const auto configuration = serialize_operational_configuration(request.configuration);
    if (request.expected_revision == 0 || configuration.empty()) return {};
    Writer writer;
    writer.u64(request.expected_revision);
    return writer.text(configuration, kOperationalConfigurationMaximumBytes)
        ? writer.take() : std::vector<std::uint8_t>{};
}

std::optional<AlphaConfigurationUpdateRequest> decode_configuration_update_request(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    AlphaConfigurationUpdateRequest request;
    std::string configuration;
    if (!reader.u64(request.expected_revision) || request.expected_revision == 0 ||
        !reader.text(configuration, kOperationalConfigurationMaximumBytes) ||
        !reader.complete()) return std::nullopt;
    const auto parsed = parse_operational_configuration(configuration);
    if (!parsed.valid()) return std::nullopt;
    request.configuration = *parsed.configuration;
    return request;
}

std::vector<std::uint8_t> encode_configuration_update_result(
    const AlphaConfigurationUpdateResult& result) {
    if (result.code > LocalControlResponseCode::internal_failure ||
        result.activation > AlphaConfigurationActivation::refused ||
        result.diagnostic_code.size() > 128U || result.detail.empty() ||
        result.detail.size() > 256U || result.active_revision == 0 ||
        result.saved_revision == 0) return {};
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(result.code));
    writer.u8(static_cast<std::uint8_t>(result.activation));
    writer.u64(result.active_revision);
    writer.u64(result.saved_revision);
    if (!writer.text(result.diagnostic_code, 128U) ||
        !writer.text(result.detail, 256U)) return {};
    return writer.take();
}

std::optional<AlphaConfigurationUpdateResult> decode_configuration_update_result(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    AlphaConfigurationUpdateResult result;
    std::uint8_t code = 0, activation = 0;
    if (!reader.u8(code) || code > static_cast<std::uint8_t>(LocalControlResponseCode::internal_failure) ||
        !reader.u8(activation) ||
        activation > static_cast<std::uint8_t>(AlphaConfigurationActivation::refused) ||
        !reader.u64(result.active_revision) || !reader.u64(result.saved_revision) ||
        !reader.text(result.diagnostic_code, 128U) ||
        !reader.text(result.detail, 256U) || result.detail.empty() ||
        result.active_revision == 0 || result.saved_revision == 0 ||
        !reader.complete()) return std::nullopt;
    result.code = static_cast<LocalControlResponseCode>(code);
    result.activation = static_cast<AlphaConfigurationActivation>(activation);
    return result;
}

std::vector<std::uint8_t> encode_scheduler_settings_request(
    const AlphaSchedulerSettingsRequest& request) {
    if (request.job_id != kBackupJobId || request.expected_revision == 0 ||
        request.local_hour >= 24 || request.local_minute >= 60 ||
        (request.backup_destination && !valid_backup_destination_text(*request.backup_destination))) return {};
    Writer writer;
    if (!writer.text(request.job_id, 64U)) return {};
    writer.u64(request.expected_revision);
    writer.u8(request.enabled ? 1U : 0U);
    writer.u8(request.local_hour);
    writer.u8(request.local_minute);
    writer.u8(request.backup_destination ? 1U : 0U);
    if (request.backup_destination && !writer.text(*request.backup_destination, 2048U)) return {};
    return writer.take();
}

std::optional<AlphaSchedulerSettingsRequest> decode_scheduler_settings_request(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    AlphaSchedulerSettingsRequest request;
    std::uint8_t enabled = 0;
    std::uint8_t has_destination = 0;
    if (!reader.text(request.job_id, 64U) ||
        !reader.u64(request.expected_revision) || !reader.u8(enabled) || enabled > 1U ||
        !reader.u8(request.local_hour) || !reader.u8(request.local_minute) ||
        !reader.u8(has_destination) || has_destination > 1U || request.job_id != kBackupJobId ||
        request.expected_revision == 0 || request.local_hour >= 24 ||
        request.local_minute >= 60) return std::nullopt;
    if (has_destination) {
        std::string destination;
        if (!reader.text(destination, 2048U) || !valid_backup_destination_text(destination)) return std::nullopt;
        request.backup_destination = std::move(destination);
    }
    if (!reader.complete()) return std::nullopt;
    request.enabled = enabled != 0;
    return request;
}

std::vector<std::uint8_t> encode_scheduler_run_request(
    const AlphaSchedulerRunRequest& request) {
    if (request.job_id != kBackupJobId) return {};
    Writer writer;
    return writer.text(request.job_id, 64U) ? writer.take() : std::vector<std::uint8_t>{};
}

std::optional<AlphaSchedulerRunRequest> decode_scheduler_run_request(
    const std::vector<std::uint8_t>& payload) {
    Reader reader(payload);
    AlphaSchedulerRunRequest request;
    if (!reader.text(request.job_id, 64U) || !reader.complete() ||
        request.job_id != kBackupJobId) return std::nullopt;
    return request;
}

void erase_bootstrap_secrets(AlphaBootstrapRequest& request) noexcept {
    secure_clear(request.sysop_password);
    secure_clear(request.system_password);
}

namespace {
bool nonnegative64(Reader& reader, std::int64_t& value) {
    std::uint64_t encoded{};
    if (!reader.u64(encoded) || encoded > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
    value = static_cast<std::int64_t>(encoded); return true;
}
bool account_query_valid(const LocalAccountQuery& query) {
    return query.account_id >= 0 && query.offset <= 1000000 && query.name_prefix.size() <= 30 &&
        (!query.account_id || (!query.offset && query.name_prefix.empty())) &&
        std::all_of(query.name_prefix.begin(),query.name_prefix.end(),[](unsigned char c){ return c >= 32 && c <= 126; });
}
void write_profile(Writer& writer, const AccountProfile& profile) {
    // Empty optional text has no valid persistent representation.
    writer.text(profile.real_name.value_or(""), kRealNameMaximum);
    writer.text(profile.location.value_or(""), kLocationMaximum);
    writer.text(profile.phone.value_or(""), kPhoneMaximum);
    writer.text(profile.email.value_or(""), kEmailMaximum);
    writer.text(profile.birth_date.value_or(""), 10);
    writer.u8(static_cast<std::uint8_t>(profile.sex.value_or(0)));
}
bool read_profile(Reader& reader, AccountProfile& profile) {
    const auto optional = [&](std::optional<std::string>& output, std::size_t maximum) {
        std::string text; if (!reader.text(text,maximum)) return false;
        if (!text.empty()) output = std::move(text);
        return true;
    };
    std::uint8_t sex{};
    if (!optional(profile.real_name,kRealNameMaximum) || !optional(profile.location,kLocationMaximum) ||
        !optional(profile.phone,kPhoneMaximum) || !optional(profile.email,kEmailMaximum) ||
        !optional(profile.birth_date,10) || !reader.u8(sex)) return false;
    if (sex) profile.sex = static_cast<char>(sex);
    return valid_profile(profile);
}
bool account_request_valid(const LocalAccountRequest& request) {
    return request.action <= LocalAccountAction::save_user && request.account_id >= 0 &&
        request.expected_revision > 0 && request.expected_policy_revision >= 0 && request.expected_reset_revision >= 0 &&
        request.security_level <= 999 && request.name.size() <= 30 && valid_profile(request.profile) &&
        (request.name.empty() || normalize_account_name(request.name) == request.name) &&
        request.password.size() <= kPasswordMaximum && request.confirmation.size() <= kPasswordMaximum;
}
}

std::vector<std::uint8_t> encode_classic_information_query(std::int64_t after) {
    if(after<0 || after>99999)return {};
    Writer w;w.u64(after);return w.take();
}
std::optional<std::int64_t> decode_classic_information_query(const std::vector<std::uint8_t>& payload) {
    Reader r(payload);std::int64_t value{};
    if(!nonnegative64(r,value) || value>99999 || !r.complete())return {};
    return value;
}
std::vector<std::uint8_t> encode_classic_information_change(const ClassicInformationChange& value) {
    if(!valid_classic_information_change(value))return {};
    Writer w;w.u8(static_cast<std::uint8_t>(value.action));w.u64(value.id);w.u64(value.expected_revision);
    w.text(value.values.name,kOtherBoardNameMaximum);w.text(value.values.telnet,kOtherBoardAddressMaximum);w.text(value.values.ssh,kOtherBoardAddressMaximum);w.text(value.date,10);
    return w.take();
}
std::optional<ClassicInformationChange> decode_classic_information_change(const std::vector<std::uint8_t>& payload) {
    Reader r(payload);ClassicInformationChange value;std::uint8_t action{};
    if(!r.u8(action) || !nonnegative64(r,value.id) || !nonnegative64(r,value.expected_revision) ||
        !r.text(value.values.name,kOtherBoardNameMaximum) || !r.text(value.values.telnet,kOtherBoardAddressMaximum) ||
        !r.text(value.values.ssh,kOtherBoardAddressMaximum) || !r.text(value.date,10) || !r.complete())return {};
    value.action=static_cast<ClassicInformationAction>(action);
    if(!valid_classic_information_change(value))return {};
    return value;
}
std::vector<std::uint8_t> encode_classic_information_snapshot(const LocalClassicInformationSnapshot& value) {
    if(value.code>LocalAccountResultCode::system_sysop_protected)return {};
    Writer w;w.u8(static_cast<std::uint8_t>(value.code));
    if(value.code!=LocalAccountResultCode::ok)return w.take();
    if(value.boards.entries.size()>kOtherBoardPageSize || value.opening.revision<1 ||
        !valid_utc_seconds(value.opening.initialization_utc) || (value.opening.configured_date && !valid_iso_civil_date(*value.opening.configured_date)))return {};
    w.u64(value.opening.revision);w.u64(value.opening.initialization_utc);w.text(value.opening.configured_date.value_or(""),10);
    w.u8(value.boards.has_more);w.u8(static_cast<std::uint8_t>(value.boards.entries.size()));
    std::int64_t previous=0;
    for(const auto& row:value.boards.entries) {
        if(row.id<=previous || row.id>99999 || row.revision<1 || !valid_other_board_values(row.values))return {};
        previous=row.id;w.u64(row.id);w.u64(row.revision);w.text(row.values.name,kOtherBoardNameMaximum);w.text(row.values.telnet,kOtherBoardAddressMaximum);w.text(row.values.ssh,kOtherBoardAddressMaximum);
    }
    if(value.boards.has_more && value.boards.entries.empty())return {};
    return w.take();
}
std::optional<LocalClassicInformationSnapshot> decode_classic_information_snapshot(const std::vector<std::uint8_t>& payload) {
    Reader r(payload);LocalClassicInformationSnapshot value;std::uint8_t code{},more{},count{};std::string date;
    if(!r.u8(code) || code>static_cast<std::uint8_t>(LocalAccountResultCode::system_sysop_protected))return {};
    value.code=static_cast<LocalAccountResultCode>(code);
    if(value.code!=LocalAccountResultCode::ok)return r.complete()?std::optional<LocalClassicInformationSnapshot>(value):std::nullopt;
    if(!nonnegative64(r,value.opening.revision) || !nonnegative64(r,value.opening.initialization_utc) || !r.text(date,10) || !r.u8(more) || more>1 || !r.u8(count) || count>kOtherBoardPageSize)return {};
    if(!date.empty())value.opening.configured_date=date;
    value.boards.has_more=more!=0;
    for(unsigned n=0;n<count;++n) {
        OtherBoardRecord row;
        if(!nonnegative64(r,row.id) || !nonnegative64(r,row.revision) || !r.text(row.values.name,kOtherBoardNameMaximum) || !r.text(row.values.telnet,kOtherBoardAddressMaximum) || !r.text(row.values.ssh,kOtherBoardAddressMaximum))return {};
        value.boards.entries.push_back(std::move(row));
    }
    if(!r.complete() || encode_classic_information_snapshot(value).empty())return {};
    return value;
}
std::vector<std::uint8_t> encode_account_query(const LocalAccountQuery& query) {
    if (!account_query_valid(query)) return {};
    Writer writer; writer.u64(static_cast<std::uint64_t>(query.account_id));
    writer.u32(query.offset); writer.text(query.name_prefix,30); return writer.take();
}
std::optional<LocalAccountQuery> decode_account_query(const std::vector<std::uint8_t>& payload) {
    Reader reader(payload); LocalAccountQuery result;
    if (!nonnegative64(reader,result.account_id) || !reader.u32(result.offset) ||
        !reader.text(result.name_prefix,30) || !reader.complete() || !account_query_valid(result)) return {};
    return result;
}
std::vector<std::uint8_t> encode_account_request(const LocalAccountRequest& request) {
    if (!account_request_valid(request)) return {};
    Writer writer;
    writer.u8(static_cast<std::uint8_t>(request.action));
    writer.u64(static_cast<std::uint64_t>(request.account_id));
    writer.u64(static_cast<std::uint64_t>(request.expected_revision));
    writer.u64(static_cast<std::uint64_t>(request.expected_policy_revision));
    writer.u64(static_cast<std::uint64_t>(request.expected_reset_revision));
    writer.u16(request.security_level); writer.u8(request.purge_allowed ? 1 : 0);
    writer.text(request.name,30); write_profile(writer,request.profile);
    writer.text(request.password,kPasswordMaximum); writer.text(request.confirmation,kPasswordMaximum);
    return writer.take();
}

std::vector<std::uint8_t> encode_security_snapshot(const LocalSecuritySnapshot& value) {
    if(value.code>LocalAccountResultCode::revocation_failed) return {};
    Writer w; w.u8(static_cast<std::uint8_t>(value.code));
    if(value.code!=LocalAccountResultCode::ok) return w.take();
    if(value.policy.revision<1 || !valid_sha256_text(value.policy.policy_hash) ||
        value.levels.size()>32 || !value.policy.new_user_level || value.policy.new_user_level>999 ||
        !value.policy.sysop_threshold || value.policy.sysop_threshold>999) return {};
    w.u64(value.policy.revision); w.text(value.policy.policy_hash,64);
    w.u16(value.policy.new_user_level); w.u16(value.policy.sysop_threshold);
    w.u8(value.has_more); w.u8(static_cast<std::uint8_t>(value.levels.size()));
    std::uint16_t previous=0;
    for(const auto& level:value.levels) {
        if(level.level<=previous || level.level>999 || level.state>SecurityLevelState::retired ||
            level.label.empty() || level.label.size()>kSecurityLabelMaximum ||
            level.description.value_or("").size()>kSecurityDescriptionMaximum) return {};
        previous=level.level; w.u16(level.level); w.u8(static_cast<std::uint8_t>(level.state));
        w.text(level.label,kSecurityLabelMaximum); w.text(level.description.value_or(""),kSecurityDescriptionMaximum);
        if((level.minutes_per_call && !*level.minutes_per_call) || (level.minutes_per_day && !*level.minutes_per_day) ||
            (level.level==999 && (level.minutes_per_call || level.minutes_per_day))) return {};
        w.u32(level.minutes_per_call.value_or(0));w.u32(level.minutes_per_day.value_or(0));
        if((level.private_chat_minutes&&!*level.private_chat_minutes)||(level.private_chat_minutes_per_day&&!*level.private_chat_minutes_per_day))return {};
        w.u32(level.private_chat_minutes.value_or(0));w.u32(level.private_chat_minutes_per_day.value_or(0));
    }
    return w.take();
}
std::optional<LocalSecuritySnapshot> decode_security_snapshot(const std::vector<std::uint8_t>& payload) {
    Reader r(payload); LocalSecuritySnapshot value; std::uint8_t code{},more{},count{};
    if(!r.u8(code) || code>static_cast<std::uint8_t>(LocalAccountResultCode::revocation_failed)) return {};
    value.code=static_cast<LocalAccountResultCode>(code);
    if(value.code!=LocalAccountResultCode::ok) return r.complete() ? std::optional<LocalSecuritySnapshot>(value) : std::nullopt;
    if(!nonnegative64(r,value.policy.revision) || !r.text(value.policy.policy_hash,64) ||
        !r.u16(value.policy.new_user_level) || !r.u16(value.policy.sysop_threshold) ||
        !r.u8(more) || more>1 || !r.u8(count) || count>32) return {};
    value.has_more=more!=0; value.policy.published_revision=value.policy.revision;
    value.policy.published_policy_hash=value.policy.policy_hash;
    for(unsigned i=0;i<count;++i) {
        SecurityLevelRecord level; std::uint8_t state{}; std::string description;
        if(!r.u16(level.level) || !r.u8(state) || !r.text(level.label,kSecurityLabelMaximum) ||
            !r.text(description,kSecurityDescriptionMaximum)) return {};
        level.state=static_cast<SecurityLevelState>(state);
        if(!description.empty()) level.description=std::move(description);
        std::uint32_t per_call{},per_day{};
        if(!r.u32(per_call) || !r.u32(per_day)) return {};
        if(per_call) level.minutes_per_call=per_call;
        if(per_day) level.minutes_per_day=per_day;
        std::uint32_t chat{},chat_day{};if(!r.u32(chat)||!r.u32(chat_day))return {};
        if(chat)level.private_chat_minutes=chat;
        if(chat_day)level.private_chat_minutes_per_day=chat_day;
        value.levels.push_back(std::move(level));
    }
    if(!r.complete() || encode_security_snapshot(value).empty()) return {};
    return value;
}
std::vector<std::uint8_t> encode_security_change(const LocalSecurityChange& value) {
    if(value.expected_revision<1 || value.action>LocalSecurityAction::set_defaults || value.level>999 ||
        value.replacement>999 || value.new_user_level<1 || value.new_user_level>999 ||
        value.sysop_threshold<1 || value.sysop_threshold>999 || value.label.size()>kSecurityLabelMaximum ||
        value.description.value_or("").size()>kSecurityDescriptionMaximum || value.affected_accounts_hash.size()>64) return {};
    Writer w; w.u64(value.expected_revision); w.u8(static_cast<std::uint8_t>(value.action));
    w.u16(value.level); w.u16(value.replacement); w.u16(value.new_user_level); w.u16(value.sysop_threshold);
    w.text(value.label,kSecurityLabelMaximum); w.text(value.description.value_or(""),kSecurityDescriptionMaximum);
    w.text(value.affected_accounts_hash,64);
    if((value.minutes_per_call && !*value.minutes_per_call) || (value.minutes_per_day && !*value.minutes_per_day) ||
        (value.level==999 && (value.minutes_per_call || value.minutes_per_day))) return {};
    w.u32(value.minutes_per_call.value_or(0));w.u32(value.minutes_per_day.value_or(0));
    if((value.private_chat_minutes&&!*value.private_chat_minutes)||(value.private_chat_minutes_per_day&&!*value.private_chat_minutes_per_day))return {};
    w.u32(value.private_chat_minutes.value_or(0));w.u32(value.private_chat_minutes_per_day.value_or(0));
    return w.take();
}
std::optional<LocalSecurityChange> decode_security_change(const std::vector<std::uint8_t>& payload) {
    Reader r(payload); LocalSecurityChange value; std::uint8_t action{}; std::string description;
    if(!nonnegative64(r,value.expected_revision) || !r.u8(action) || !r.u16(value.level) ||
        !r.u16(value.replacement) || !r.u16(value.new_user_level) || !r.u16(value.sysop_threshold) ||
        !r.text(value.label,kSecurityLabelMaximum) || !r.text(description,kSecurityDescriptionMaximum) || !r.text(value.affected_accounts_hash,64)) return {};
    std::uint32_t per_call{},per_day{},chat{},chat_day{};if(!r.u32(per_call) || !r.u32(per_day)||!r.u32(chat)||!r.u32(chat_day)|| !r.complete()) return {};
    if(per_call) value.minutes_per_call=per_call;
    if(per_day) value.minutes_per_day=per_day;
    if(chat)value.private_chat_minutes=chat;
    if(chat_day)value.private_chat_minutes_per_day=chat_day;
    value.action=static_cast<LocalSecurityAction>(action); if(!description.empty()) value.description=std::move(description);
    if(encode_security_change(value).empty()) return {};
    return value;
}
std::optional<LocalAccountRequest> decode_account_request(const std::vector<std::uint8_t>& payload) {
    Reader reader(payload); LocalAccountRequest request;
    struct Erase { LocalAccountRequest& request; ~Erase(){ erase_local_account_secrets(request); } } erase{request};
    std::uint8_t action{}, purge{};
    if (!reader.u8(action) || action > static_cast<std::uint8_t>(LocalAccountAction::save_user) ||
        !nonnegative64(reader,request.account_id) || !nonnegative64(reader,request.expected_revision) ||
        !nonnegative64(reader,request.expected_policy_revision) || !nonnegative64(reader,request.expected_reset_revision) ||
        !reader.u16(request.security_level) || !reader.u8(purge) || purge > 1 || !reader.text(request.name,30) ||
        !read_profile(reader,request.profile) || !reader.text(request.password,kPasswordMaximum) ||
        !reader.text(request.confirmation,kPasswordMaximum) || !reader.complete()) return {};
    request.action = static_cast<LocalAccountAction>(action); request.purge_allowed = purge != 0;
    if (!account_request_valid(request)) return {};
    // Copy once into the owned request; the decoder's transient strings are
    // erased on success as well as every malformed/truncated exit.
    return std::optional<LocalAccountRequest>(static_cast<const LocalAccountRequest&>(request));
}
std::vector<std::uint8_t> encode_account_result(const LocalAccountResult& result) {
    if (result.code > LocalAccountResultCode::system_sysop_protected || result.revision < 0 ||
        ((result.code == LocalAccountResultCode::last_sysop_protected || result.code == LocalAccountResultCode::system_sysop_protected) && result.committed) ||
        result.password_policy > PasswordPolicyCode::blocklist_unavailable ||
        (result.sessions_revoked && !result.committed)) return {};
    Writer writer; writer.u8(static_cast<std::uint8_t>(result.code));
    writer.u8(result.committed ? 1 : 0); writer.u8(result.sessions_revoked ? 1 : 0);
    writer.u64(static_cast<std::uint64_t>(result.revision)); writer.u8(static_cast<std::uint8_t>(result.password_policy));
    return writer.take();
}
std::optional<LocalAccountResult> decode_account_result(const std::vector<std::uint8_t>& payload) {
    Reader reader(payload); LocalAccountResult result; std::uint8_t code{},committed{},revoked{},policy{};
    if (!reader.u8(code) || !reader.u8(committed) || committed > 1 || !reader.u8(revoked) || revoked > 1 ||
        !nonnegative64(reader,result.revision) || !reader.u8(policy) || !reader.complete()) return {};
    result.code = static_cast<LocalAccountResultCode>(code); result.committed = committed != 0;
    result.sessions_revoked = revoked != 0; result.password_policy = static_cast<PasswordPolicyCode>(policy);
    if (encode_account_result(result).empty()) return {};
    return result;
}

std::vector<std::uint8_t> encode_account_snapshot(const LocalAccountSnapshot& snapshot) {
    if (snapshot.code > LocalAccountResultCode::revocation_failed) return {};
    Writer writer; writer.u8(static_cast<std::uint8_t>(snapshot.code));
    if (snapshot.code != LocalAccountResultCode::ok) return writer.take();
    if (snapshot.accounts.size() > kLocalAccountPageSize || snapshot.system_revision < 1 ||
        snapshot.policy.revision < 1 || !valid_sha256_text(snapshot.policy.policy_hash) ||
        !snapshot.policy.new_user_level || snapshot.policy.new_user_level > 999 ||
        !snapshot.policy.sysop_threshold || snapshot.policy.sysop_threshold > 999) return {};
    writer.u64(static_cast<std::uint64_t>(snapshot.system_revision));
    writer.u64(static_cast<std::uint64_t>(snapshot.policy.revision));
    writer.u16(snapshot.policy.new_user_level); writer.u16(snapshot.policy.sysop_threshold);
    writer.text(snapshot.policy.policy_hash,64);
    writer.u8(snapshot.online ? 1 : 0);
    writer.u8(snapshot.has_more ? 1 : 0); writer.u8(static_cast<std::uint8_t>(snapshot.accounts.size()));
    for (const auto& account : snapshot.accounts) {
        if (account.account_id <= 0 || account.revision < 1 || account.state > AccountState::tombstoned ||
            (account.security_level && (!*account.security_level || *account.security_level > 999)) ||
            (account.display_name && normalize_account_name(*account.display_name) != account.display_name) ||
            !valid_profile(account.profile)) return {};
        writer.u64(static_cast<std::uint64_t>(account.account_id)); writer.u64(static_cast<std::uint64_t>(account.revision));
        writer.u16(account.security_level.value_or(0)); writer.u8(static_cast<std::uint8_t>(account.state));
        writer.u8(account.purge_allowed ? (*account.purge_allowed ? 2 : 1) : 0);
        writer.text(account.display_name.value_or(""),30); write_profile(writer,account.profile);
    }
    writer.u8(snapshot.temporary ? 1 : 0);
    if (snapshot.temporary) {
        const auto& reset = *snapshot.temporary;
        if (snapshot.accounts.size() != 1 || reset.account_id != snapshot.accounts.front().account_id ||
            reset.revision < 1 || reset.state > TemporaryPasswordState::expired || reset.issued_utc_seconds < 0 ||
            reset.expires_utc_seconds < reset.issued_utc_seconds) return {};
        writer.u64(static_cast<std::uint64_t>(reset.account_id)); writer.u64(static_cast<std::uint64_t>(reset.revision));
        writer.u8(static_cast<std::uint8_t>(reset.state));
        writer.u64(static_cast<std::uint64_t>(reset.issued_utc_seconds)); writer.u64(static_cast<std::uint64_t>(reset.expires_utc_seconds));
    }
    auto bytes = writer.take(); return bytes.size() <= kLocalControlMaximumPayloadBytes ? bytes : std::vector<std::uint8_t>{};
}
std::optional<LocalAccountSnapshot> decode_account_snapshot(const std::vector<std::uint8_t>& payload) {
    Reader reader(payload); LocalAccountSnapshot result; std::uint8_t code{},more{},count{},temporary{},online{};
    if (!reader.u8(code) || code > static_cast<std::uint8_t>(LocalAccountResultCode::revocation_failed)) return {};
    result.code = static_cast<LocalAccountResultCode>(code);
    if (result.code != LocalAccountResultCode::ok) return reader.complete() ? std::optional<LocalAccountSnapshot>(result) : std::nullopt;
    if (!nonnegative64(reader,result.system_revision) || !nonnegative64(reader,result.policy.revision) ||
        !reader.u16(result.policy.new_user_level) || !reader.u16(result.policy.sysop_threshold) ||
        !reader.text(result.policy.policy_hash,64) || !reader.u8(online) || online>1 || !reader.u8(more) || more > 1 ||
        !reader.u8(count) || count > kLocalAccountPageSize) return {};
    result.has_more = more != 0;
    result.online = online != 0;
    for (std::size_t index=0;index<count;++index) {
        AccountRecord account; std::uint16_t level{}; std::uint8_t state{},purge{}; std::string name;
        if (!nonnegative64(reader,account.account_id) || !nonnegative64(reader,account.revision) ||
            !reader.u16(level) || !reader.u8(state) || !reader.u8(purge) || purge > 2 ||
            !reader.text(name,30) || !read_profile(reader,account.profile)) return {};
        if (level) account.security_level = level;
        account.state = static_cast<AccountState>(state);
        if (purge) account.purge_allowed = purge == 2;
        if (!name.empty()) account.display_name = std::move(name);
        result.accounts.push_back(std::move(account));
    }
    if (!reader.u8(temporary) || temporary > 1) return {};
    if (temporary) {
        TemporaryPasswordRecord reset; std::uint8_t state{};
        if (!nonnegative64(reader,reset.account_id) || !nonnegative64(reader,reset.revision) || !reader.u8(state) ||
            !nonnegative64(reader,reset.issued_utc_seconds) || !nonnegative64(reader,reset.expires_utc_seconds)) return {};
        reset.state = static_cast<TemporaryPasswordState>(state); result.temporary = reset;
    }
    if (!reader.complete() || encode_account_snapshot(result).empty()) return {};
    return result;
}

#include "message_configuration_protocol.inc"
#include "file_configuration_protocol.inc"
#include "door_configuration_protocol.inc"
#include "door_bulletin_protocol.inc"
#include "file_management_protocol.inc"
#include "file_management_read_protocol.inc"
#include "qwk_configuration_protocol.inc"
#include "message_maintenance_protocol.inc"
#include "clamav_notice_protocol.inc"
#include "event_sounds_protocol.inc"
#include "ftn_runtime_transport.inc"
#include "ftn_held_transport.inc"
#include "ftn_file_forward_plan_codec.inc"
}  // namespace sfx::application
