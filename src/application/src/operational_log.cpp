#include "sfx/application/operational_log.hpp"
#include "sfx/application/hostname_lookup.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>
#include <vector>

namespace sfx::application {
namespace {
bool printable(std::string_view value, std::size_t maximum) noexcept {
    return value.size() <= maximum && std::all_of(value.begin(), value.end(),
        [](unsigned char c) { return c >= 32 && c <= 126; });
}
template<class T> bool number(std::string_view text, T& value) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0') || text.front() == '-') return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
std::string hex(Identifier128 id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(32);
    for (auto c : id.bytes) { result += digits[c >> 4]; result += digits[c & 15]; }
    return result;
}
bool identifier(std::string_view text, Identifier128& id) noexcept {
    if (text.size() != 32) return false;
    const auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (std::size_t i = 0; i < 16; ++i) {
        const auto hi = nibble(text[i * 2]), lo = nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        id.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}
std::vector<std::string_view> split(std::string_view value) {
    std::vector<std::string_view> result;
    for (;;) {
        const auto position = value.find('|');
        result.push_back(value.substr(0, position));
        if (position == value.npos) break;
        if (result.size() >= 28) return {};
        value.remove_prefix(position + 1);
    }
    return result;
}
std::string escape(std::string_view value) {
    std::string result;
    for (char c : value) {
        if (c == '%') result += "%25";
        else if (c == '|') result += "%7C";
        else result += c;
    }
    return result;
}
bool unescape(std::string_view value, std::string& result) {
    result.clear();
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') result += value[i];
        else if (value.substr(i, 3) == "%25") { result += '%'; i += 2; }
        else if (value.substr(i, 3) == "%7C") { result += '|'; i += 2; }
        else return false;
    }
    return true;
}
bool ipv4(std::string_view value) noexcept {
    for (unsigned i = 0; i < 4; ++i) {
        const auto dot = value.find('.');
        unsigned n = 0;
        if (!number(value.substr(0, dot), n) || n > 255 || ((i == 3) != (dot == value.npos))) return false;
        if (i < 3) value.remove_prefix(dot + 1);
    }
    return true;
}
const char* transport(OperationalTransport kind) noexcept {
    return kind == OperationalTransport::ssh ? "SSH" : kind == OperationalTransport::telnet ? "Telnet" : "";
}
std::string display_name(std::string_view name) {
    std::string result;
    for (char c : name) {
        if (c == '<') result += "\\x3C";
        else if (c == '>') result += "\\x3E";
        else if (c == '\\') result += "\\x5C";
        else result += c;
    }
    return result;
}
std::string display_attempted_name(std::string_view name) {
    std::string result;
    for (char c : name) {
        if (c == '"') result += "\\x22";
        else if (c == '<') result += "\\x3C";
        else if (c == '>') result += "\\x3E";
        else if (c == '\\') result += "\\x5C";
        else if (c == '|') result += "\\x7C";
        else result += c;
    }
    return result;
}
std::string legacy_message(std::string_view message) {
    // Recognize only the exact old generated prefix. Keep the original complete
    // text in the legacy diagnostic record; never borrow current node context.
    const auto original = message;
    if (message.substr(0, 5) == "Node ") {
        message.remove_prefix(5);
        const auto assignment = message.find(" assignment ");
        unsigned node = 0;
        if (assignment != message.npos && number(message.substr(0, assignment), node) && node >= 1 && node <= 128) {
            message.remove_prefix(assignment + 12);
            const auto epoch_at = message.find(" epoch ");
            std::uint64_t generation = 0;
            if (epoch_at != message.npos && number(message.substr(0, epoch_at), generation) && generation) {
                message.remove_prefix(epoch_at + 7);
                Identifier128 epoch;
                if (message.size() > 32 && identifier(message.substr(0, 32), epoch) && epoch.valid()) {
                    message.remove_prefix(32);
                    for (const auto prefix : {std::string_view(" SSH: "), std::string_view(" Telnet: ")})
                        if (message.substr(0, prefix.size()) == prefix && message.size() > prefix.size())
                            return "[legacy] Node " + std::to_string(node) + std::string(prefix) +
                                std::string(message.substr(prefix.size()));
                }
            }
        }
    }
    return "[legacy] " + std::string(original);
}
const char* outcome(OperationalOutcome code) noexcept {
    switch (code) {
        case OperationalOutcome::none: return "";
        case OperationalOutcome::normal: return "normal logoff";
        case OperationalOutcome::cancelled: return "cancelled";
        case OperationalOutcome::timed_out: return "timed out";
        case OperationalOutcome::disconnected: return "caller disconnected";
        case OperationalOutcome::shutdown: return "runtime shutdown";
        case OperationalOutcome::denied: return "authentication denied";
        case OperationalOutcome::unavailable: return "temporarily unavailable";
        case OperationalOutcome::storage_failure: return "storage failure";
        case OperationalOutcome::terminal_failure: return "terminal failure";
        case OperationalOutcome::internal_failure: return "runtime failure";
        case OperationalOutcome::registration_closed: return "registration unavailable";
    }
    return "invalid outcome";
}
const char* component(OperationalClass value) noexcept {
    switch (value) {
        case OperationalClass::runtime: return "runtime";
        case OperationalClass::connection: return "connection";
        case OperationalClass::authentication: return "authentication";
        case OperationalClass::registration: return "registration";
        case OperationalClass::activity: return "activity";
        case OperationalClass::cleanup: return "cleanup";
        case OperationalClass::configuration: return "configuration";
        case OperationalClass::statistics: return "statistics";
        case OperationalClass::botgate: return "botgate";
    }
    return "runtime";
}
}

std::string_view caller_stage_label(CallerStage value) noexcept {
    switch (value) {
        case CallerStage::transport: return "Transport setup";
        case CallerStage::graphics: return "ANSI selection";
        case CallerStage::identity: return "Caller login";
        case CallerStage::authentication: return "Authentication";
        case CallerStage::registration: return "Registration";
        case CallerStage::system_password: return "System Password";
        case CallerStage::caller_information: return "Caller information";
        case CallerStage::main_menu: return "Main Menu";
        case CallerStage::cleanup: return "Disconnecting";
    }
    return "Invalid stage";
}
std::string_view operational_log_health_text(std::uint8_t health) noexcept {
    if (health & operational_log_write_failed)
        return "WARNING: SFXRuntime.log could not be written/protected. Live entries continue; file history may be incomplete.";
    if (health & operational_log_full)
        return "WARNING: SFXRuntime.log rotation is unavailable. Existing history is preserved; check log access and storage.";
    if (health & operational_log_history_invalid)
        return "WARNING: SFXRuntime.log contains invalid or incomplete history. Existing data was preserved.";
    if (health & operational_log_event_rejected)
        return "WARNING: An operational event could not be recorded. Caller service is unaffected.";
    if (health & operational_durable_journal_failed)
        return "WARNING: CALLERS.LOG or HEYSYSOP.LOG recording is degraded. Existing files are preserved; caller service and SQLite accounting continue. Check log access and available disk space.";
    return {};
}
OperationalClass operational_class(OperationalKind k) noexcept {
    switch (k) {
        case OperationalKind::listener_ready: case OperationalKind::listener_stopping:
        case OperationalKind::listener_stopped: case OperationalKind::runtime_stopping:
        case OperationalKind::runtime_started: case OperationalKind::runtime_stopped:
        case OperationalKind::runtime_failed: case OperationalKind::listener_failed:
        case OperationalKind::hostname_exhausted: case OperationalKind::durable_journal_degraded:
        case OperationalKind::drain_started: case OperationalKind::drain_cancelled:
        case OperationalKind::backup_started: case OperationalKind::backup_completed: case OperationalKind::backup_failed:
        case OperationalKind::message_maintenance:case OperationalKind::message_maintenance_problem:
        case OperationalKind::file_maintenance:case OperationalKind::file_maintenance_problem:
        case OperationalKind::external_event_started: case OperationalKind::external_event_finished:
        case OperationalKind::external_event_problem:
            return OperationalClass::runtime;
        case OperationalKind::authentication_denied: case OperationalKind::duplicate_denied:
        case OperationalKind::authenticated: case OperationalKind::session_persisted: return OperationalClass::authentication;
        case OperationalKind::registration_started: case OperationalKind::registration_committed:
        case OperationalKind::registration_abandoned: return OperationalClass::registration;
        case OperationalKind::stage_changed: case OperationalKind::message_area_unavailable: return OperationalClass::activity;
        case OperationalKind::assignment_released: case OperationalKind::assignment_quarantined: return OperationalClass::cleanup;
        case OperationalKind::node_enabled: case OperationalKind::node_disabled:
        case OperationalKind::settings_pending: case OperationalKind::account_administration:
        case OperationalKind::throttle_cleared: case OperationalKind::configuration_saved: return OperationalClass::configuration;
        case OperationalKind::statistics_unavailable: return OperationalClass::statistics;
        case OperationalKind::botgate_passed: case OperationalKind::botgate_failed:
        case OperationalKind::botgate_timed_out: case OperationalKind::botgate_bypassed:
        case OperationalKind::botgate_blocked: case OperationalKind::feed_update_started:
        case OperationalKind::feed_update_completed: case OperationalKind::feed_update_failed:
            return OperationalClass::botgate;
        default: return OperationalClass::connection;
    }
}
std::string_view operational_kind_label(OperationalKind kind) noexcept {
    static constexpr std::array<std::string_view,62> names{{
        "listener_ready","listener_stopping","listener_stopped","connection_assigned",
        "capacity_refused","source_refused","runtime_refused","socket_failed","ssh_setup_ended",
        "stage_changed","authentication_denied","duplicate_denied","registration_started",
        "registration_committed","registration_abandoned","authenticated","message_area_unavailable",
        "connection_ended","assignment_released","assignment_quarantined","node_enabled","node_disabled",
        "settings_pending","runtime_stopping","statistics_unavailable","session_failed","runtime_failed",
        "listener_failed","session_persisted","hostname_exhausted","durable_journal_degraded",
        "local_session_requested","local_session_reserved","local_session_cancelled","local_session_launch_failed",
        "local_session_timed_out","account_administration","throttle_cleared","drain_started","drain_cancelled",
        "backup_started","backup_completed","backup_failed","configuration_saved","botgate_passed","botgate_failed",
        "botgate_timed_out","botgate_bypassed","botgate_blocked","feed_update_started","feed_update_completed",
        "feed_update_failed","flood_aggregate","runtime_started","runtime_stopped","message_maintenance","message_maintenance_problem","file_maintenance","file_maintenance_problem",
        "external_event_started","external_event_finished","external_event_problem"}};
    const auto index=static_cast<std::size_t>(kind);
    return index<names.size() ? names[index] : "unknown_event";
}
unsigned operational_filter_group(const AlphaOperationalLogEntry& entry) noexcept {
    if (!entry.event) return 4;
    auto kind=entry.event->kind;
    if (kind==OperationalKind::flood_aggregate) {
        const std::string_view label=entry.event->context.resource_name;
        for (unsigned i=0;i<=static_cast<unsigned>(OperationalKind::external_event_problem);++i) {
            if (i==static_cast<unsigned>(OperationalKind::flood_aggregate)) continue;
            const auto candidate=static_cast<OperationalKind>(i);
            const auto name=operational_kind_label(candidate);
            if (label==name || (label.size()>name.size() && label.substr(0,name.size())==name && label.substr(name.size())=="_mixed_sources")) {
                kind=candidate; break;
            }
        }
    }
    const auto category=operational_class(kind);
    return category==OperationalClass::botgate ? 3 :
        category==OperationalClass::connection || category==OperationalClass::cleanup ? 1 :
        category==OperationalClass::authentication || category==OperationalClass::registration || category==OperationalClass::activity ? 2 : 4;
}
bool operational_sysop_attention(OperationalKind k) noexcept {
    return k == OperationalKind::duplicate_denied || k == OperationalKind::assignment_quarantined ||
        k == OperationalKind::session_failed || k == OperationalKind::statistics_unavailable ||
        k == OperationalKind::socket_failed || k == OperationalKind::runtime_failed || k == OperationalKind::listener_failed ||
        k == OperationalKind::hostname_exhausted || k == OperationalKind::durable_journal_degraded ||
        k == OperationalKind::local_session_launch_failed || k == OperationalKind::backup_failed ||
        k == OperationalKind::feed_update_failed || k == OperationalKind::flood_aggregate ||
        k == OperationalKind::message_maintenance_problem || k == OperationalKind::file_maintenance_problem ||
        k == OperationalKind::external_event_problem;
}
std::string_view operational_level(OperationalKind k) noexcept {
    if (k == OperationalKind::assignment_quarantined || k == OperationalKind::session_failed ||
        k == OperationalKind::runtime_failed || k == OperationalKind::listener_failed) return "ERROR";
    return operational_sysop_attention(k) ? "WARNING" : "INFO";
}
std::string operational_message(const OperationalEvent& e) {
    const auto& c = e.context;
    std::string line;
    if (c.node) line = "Node " + std::to_string(c.node) + " ";
    if (c.transport != OperationalTransport::none) line += std::string(transport(c.transport)) + " ";
    if (c.generation) line += "<" + (c.caller.empty() ? std::string("unknown") : display_name(c.caller)) + "> ";
    if (!c.hostname.empty()) line += c.hostname + " ";
    if (!c.source_ip.empty()) line += "[" + c.source_ip + "] ";
    if (!line.empty()) { line.pop_back(); line += ": "; }
    switch (e.kind) {
        case OperationalKind::listener_ready: line += "listener ready on port " + std::to_string(c.local_port); break;
        case OperationalKind::listener_stopping: line += "listener shutdown requested"; break;
        case OperationalKind::listener_stopped: line += "listener stopped"; break;
        case OperationalKind::connection_assigned: line += "connection accepted"; break;
        case OperationalKind::capacity_refused: line += "connection refused - no nodes available"; break;
        case OperationalKind::source_refused: line += "connection refused - source connection limit"; break;
        case OperationalKind::runtime_refused: line += "connection refused - Runtime unavailable"; break;
        case OperationalKind::socket_failed: line += "connection rejected - socket setup failed"; break;
        case OperationalKind::ssh_setup_ended: line += "SSH setup ended"; break;
        case OperationalKind::stage_changed:
            if(c.resource_id && c.diagnostic_code.rfind("door_",0)==0)line+="Door "+c.resource_name+": "+c.diagnostic_code;
            else line += caller_stage_label(c.stage);
            break;
        case OperationalKind::authentication_denied: line += "login denied"; break;
        case OperationalKind::duplicate_denied: line += "duplicate login refused after password verification; original caller remains online"; break;
        case OperationalKind::registration_started: line += "registration started"; break;
        case OperationalKind::registration_committed: line += "account created; login not yet completed"; break;
        case OperationalKind::registration_abandoned: line += "registration ended; no account created"; break;
        case OperationalKind::authenticated: line += "logged on"; break;
        case OperationalKind::message_area_unavailable: line += "Message area selected - not implemented in this Alpha"; break;
        case OperationalKind::connection_ended:
            line += c.account_id ? "session ended" : "connection ended before authentication";
            if (!c.attempted_name.empty())
                line += " - attempted name \"" + display_attempted_name(c.attempted_name) +
                    "\" rejected by JOKER.DAT";
            break;
        case OperationalKind::assignment_released: line += "cleanup complete; assignment released"; break;
        case OperationalKind::assignment_quarantined: line += "cleanup failed; assignment quarantined"; break;
        case OperationalKind::node_enabled: line += "enabled"; break;
        case OperationalKind::node_disabled: line += "disabled; shared listeners unchanged"; break;
        case OperationalKind::settings_pending: line += "settings saved; pending BBS restart"; break;
        case OperationalKind::runtime_stopping: line += "Runtime shutdown requested"; break;
        case OperationalKind::statistics_unavailable: line += "Runtime statistics unavailable"; break;
        case OperationalKind::session_failed: line += "caller session failed"; break;
        case OperationalKind::runtime_failed: line += "Runtime prerequisites unavailable"; break;
        case OperationalKind::listener_failed: line += "caller listener failed to start"; break;
        case OperationalKind::session_persisted: line += "session recorded; login not yet completed"; break;
        case OperationalKind::hostname_exhausted: line += kHostnameExhaustedWarning; break;
        case OperationalKind::durable_journal_degraded: line += operational_log_health_text(operational_durable_journal_failed); break;
        case OperationalKind::local_session_requested: line += "local logon requested"; break;
        case OperationalKind::local_session_reserved: line += "one-use loopback listener reserved"; break;
        case OperationalKind::local_session_cancelled: line += "local logon cancelled; node released"; break;
        case OperationalKind::local_session_launch_failed: line += "Telnet client launch failed; node released"; break;
        case OperationalKind::local_session_timed_out: line += "local logon reservation expired; node released"; break;
        case OperationalKind::account_administration: line += "local account administration committed"; break;
        case OperationalKind::throttle_cleared: line += "selected local throttle cleared and audited"; break;
        case OperationalKind::drain_started: line += "bounded graceful drain started"; break;
        case OperationalKind::drain_cancelled: line += "graceful drain cancelled; admission reopened"; break;
        case OperationalKind::backup_started: line += "backup job started"; break;
        case OperationalKind::backup_completed: line += "backup job completed and verified"; break;
        case OperationalKind::backup_failed: line += "backup job did not complete; review scheduler history"; break;
        case OperationalKind::configuration_saved: line += "configuration saved; check active/saved activation state"; break;
        case OperationalKind::runtime_started: line += "Runtime started"; break;
        case OperationalKind::runtime_stopped: line += "Runtime shutdown completed; callers and storage closed"; break;
        case OperationalKind::message_maintenance:case OperationalKind::message_maintenance_problem:
            line+="Message maintenance: "+c.resource_name;break;
        case OperationalKind::file_maintenance:case OperationalKind::file_maintenance_problem:
            line+="File maintenance: "+c.resource_name;break;
        case OperationalKind::external_event_started: case OperationalKind::external_event_finished:
        case OperationalKind::external_event_problem:
            line+="External event: "+c.resource_name;break;
        case OperationalKind::botgate_passed: line += "BotGate passed; account authentication remains required"; break;
        case OperationalKind::botgate_failed: line += "BotGate rejected invalid challenge input"; break;
        case OperationalKind::botgate_timed_out: line += "BotGate challenge timed out"; break;
        case OperationalKind::botgate_bypassed: line += "BotGate challenge bypassed by trusted source rule"; break;
        case OperationalKind::botgate_blocked: line += "BotGate security refusal: " + c.diagnostic_code; break;
        case OperationalKind::feed_update_started: line += "BotGate Aggregated Blocklist from AbuseIPDB update started"; break;
        case OperationalKind::feed_update_completed: line += "BotGate Aggregated Blocklist from AbuseIPDB update complete"; break;
        case OperationalKind::feed_update_failed: line += "BotGate feed update failed; retaining last good feed: " + c.diagnostic_code; break;
        case OperationalKind::flood_aggregate:
            line += "aggregated " + std::to_string(c.resource_id) + " repetitive events: " + c.resource_name; break;
    }
    if (e.outcome != OperationalOutcome::none && c.attempted_name.empty())
        line += std::string(" - ") + outcome(e.outcome);
    return line;
}
AlphaOperationalLogEntry make_operational_entry(std::uint64_t sequence, std::int64_t utc, OperationalEvent event) {
    AlphaOperationalLogEntry result{sequence, utc, std::string(operational_level(event.kind)),
        component(operational_class(event.kind)), operational_message(event), std::move(event)};
    return result;
}
bool valid_operational_entry(const AlphaOperationalLogEntry& e) noexcept {
    try {
        if (!e.sequence || e.utc_seconds < 0 || !printable(e.level, 16) || e.level.empty() ||
            !printable(e.component, 32) || e.component.empty() || e.message.empty()) return false;
        if (!e.event) return printable(e.message, 160) && e.level.find('|') == e.level.npos &&
            e.component.find('|') == e.component.npos && e.message.find('|') == e.message.npos;
        const auto& event = *e.event; const auto& c = event.context;
        if (event.kind > OperationalKind::external_event_problem || event.outcome > OperationalOutcome::registration_closed ||
            !c.epoch.valid() || !c.origin_sequence || c.node > 128 || c.transport > OperationalTransport::ssh ||
            c.account_id < 0 || c.committed_account_id < 0 || c.verified_account_id < 0 ||
            c.stage > CallerStage::cleanup || !c.columns || c.columns > 4096 || !c.rows || c.rows > 4096 ||
            !printable(c.caller, 30) || !printable(c.attempted_name, 30) ||
            !printable(c.resource_name, event.kind==OperationalKind::file_maintenance_problem&&(c.diagnostic_code=="clamav_detection_deleted"||c.diagnostic_code=="clamav_detection_retained")?600:128) ||
            c.diagnostic_code.size() > 96 || std::any_of(c.diagnostic_code.begin(), c.diagnostic_code.end(),
                [](unsigned char b) { return !((b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '_'); }) ||
            (!c.source_ip.empty() && !ipv4(c.source_ip)) ||
            (!c.hostname.empty() && (c.source_ip.empty() || !normalize_hostname_hint(c.hostname) ||
                normalize_hostname_hint(c.hostname)->view() != c.hostname)) ||
            (c.generation && (!c.node || !c.attempt || c.transport == OperationalTransport::none || c.source_ip.empty())) ||
            (c.account_id != 0 && (!c.generation || c.caller.empty() || !c.durable_session_id.valid() || !c.correlation_id.valid())) ||
            (c.account_id == 0 && !c.caller.empty()) ||
            (!c.attempted_name.empty() && (event.kind != OperationalKind::connection_ended ||
                c.account_id != 0 || event.outcome != OperationalOutcome::registration_closed)) ||
            (c.durable_session_id.valid() && !c.account_id && !c.verified_account_id) ||
            ((c.committed_account_id || c.verified_account_id) && !c.generation) ||
            ((c.resource_id == 0) != c.resource_name.empty())) return false;
        if (!c.caller.empty()) { const auto name = normalize_account_name(c.caller); if (!name || *name != c.caller) return false; }
        if (!c.attempted_name.empty()) { const auto name = normalize_account_name(c.attempted_name); if (!name || *name != c.attempted_name) return false; }
        const bool control = event.kind == OperationalKind::node_enabled || event.kind == OperationalKind::node_disabled;
        const bool listener = event.kind == OperationalKind::listener_ready || event.kind == OperationalKind::listener_stopping ||
            event.kind == OperationalKind::listener_stopped;
        const bool refusal = event.kind == OperationalKind::capacity_refused || event.kind == OperationalKind::source_refused ||
            event.kind == OperationalKind::runtime_refused || event.kind == OperationalKind::socket_failed ||
            event.kind == OperationalKind::botgate_blocked;
        const bool global = event.kind == OperationalKind::settings_pending || event.kind == OperationalKind::runtime_stopping ||
            event.kind == OperationalKind::statistics_unavailable || event.kind == OperationalKind::runtime_failed ||
            event.kind == OperationalKind::listener_failed || event.kind == OperationalKind::hostname_exhausted ||
            event.kind == OperationalKind::durable_journal_degraded ||
            (event.kind >= OperationalKind::account_administration && event.kind <= OperationalKind::configuration_saved) ||
            event.kind >= OperationalKind::feed_update_started;
        if ((!control && !listener && !refusal && !global && !c.generation) ||
            (control && (!c.node || c.generation || c.attempt || c.transport != OperationalTransport::none)) ||
            ((listener || refusal || global) && (c.node || c.generation || c.account_id || c.committed_account_id ||
                c.verified_account_id || c.durable_session_id.valid() || !c.caller.empty())) ||
            (refusal && (!c.attempt || c.transport == OperationalTransport::none)) ||
            (listener && c.transport == OperationalTransport::none) ||
            (event.kind == OperationalKind::authenticated && !c.account_id) ||
            (event.kind == OperationalKind::registration_committed && !c.committed_account_id) ||
            (event.kind == OperationalKind::registration_abandoned && c.committed_account_id) ||
            (event.kind == OperationalKind::duplicate_denied && !c.verified_account_id) ||
            (event.kind == OperationalKind::session_persisted && (!c.durable_session_id.valid() || !c.verified_account_id))) return false;
        return printable(e.message, kOperationalPresentationMaximum) && e.level == operational_level(event.kind) &&
            e.component == component(operational_class(event.kind)) && e.message == operational_message(event);
    } catch (...) { return false; }
}
std::string encode_operational_record(const AlphaOperationalLogEntry& e) {
    if (!valid_operational_entry(e)) return {};
    if (!e.event) return std::to_string(e.utc_seconds) + "|" + e.level + "|" + e.component + "|" + e.message;
    const auto& t = *e.event; const auto& c = t.context;
    const auto n = [](auto v) { return std::to_string(static_cast<std::uint64_t>(v)); };
    std::string s = std::string(t.kind>OperationalKind::file_maintenance_problem?"SFXLOG9|":t.kind>OperationalKind::message_maintenance_problem?"SFXLOG8|":t.kind>OperationalKind::runtime_stopped?"SFXLOG7|":"SFXLOG6|") + n(e.utc_seconds) + "|" + hex(c.epoch) + "|" + n(c.origin_sequence) +
        "|" + n(t.kind) + "|" + n(t.outcome) + "|" + n(c.node) + "|" + n(c.generation) + "|" + n(c.attempt) +
        "|" + n(c.transport) + "|" + c.source_ip + "|" + n(c.peer_port) + "|" + n(c.local_port) +
        "|" + n(c.account_id) + "|" + n(c.committed_account_id) + "|" + hex(c.durable_session_id) +
        "|" + hex(c.correlation_id) + "|" + escape(c.caller) + "|" + n(c.stage) + "|" + n(c.duration_seconds) +
        "|" + n(c.columns) + "|" + n(c.rows) + "|" + n(c.resource_id) + "|" + escape(c.resource_name) +
        "|" + n(c.verified_account_id) + "|" + c.diagnostic_code + "|" + c.hostname +
        "|" + escape(c.attempted_name);
    return s.size() <= kOperationalRecordMaximum ? s : std::string{};
}
std::optional<AlphaOperationalLogEntry> decode_operational_record(std::string_view s, std::uint64_t sequence) {
    if (s.empty() || !printable(s, kOperationalRecordMaximum)) return std::nullopt;
    const auto f = split(s);
    if (f.size() == 4) {
        AlphaOperationalLogEntry e{sequence, 0, std::string(f[1]), std::string(f[2]), std::string(f[3]), {}};
        if (!number(f[0], e.utc_seconds) || !valid_operational_entry(e)) return std::nullopt;
        return e;
    }
    const bool version2 = f.size() == 26 && f[0] == "SFXLOG2" && s.size() <= 1024;
    const bool version3 = f.size() == 27 && f[0] == "SFXLOG3";
    const bool version4 = f.size() == 28 && f[0] == "SFXLOG4";
    const bool version5 = f.size() == 28 && f[0] == "SFXLOG5";
    const bool version6 = f.size() == 28 && f[0] == "SFXLOG6";
    const bool version7 = f.size() == 28 && f[0] == "SFXLOG7";
    const bool version8 = f.size() == 28 && f[0] == "SFXLOG8";
    const bool version9 = f.size() == 28 && f[0] == "SFXLOG9";
    if (!version2 && !version3 && !version4 && !version5 && !version6 && !version7 && !version8 && !version9) return std::nullopt;
    OperationalEvent t; auto& c = t.context;
    std::int64_t utc = 0; unsigned kind = 0, result = 0, proto = 0, stage = 0;
    if (!number(f[1], utc) || !identifier(f[2], c.epoch) || !number(f[3], c.origin_sequence) ||
        !number(f[4], kind) || kind > static_cast<unsigned>(version2 ? OperationalKind::session_persisted :
            version9 ? OperationalKind::external_event_problem :version8 ? OperationalKind::file_maintenance_problem :version7 ? OperationalKind::message_maintenance_problem :version6 ? OperationalKind::runtime_stopped :
            version5 ? OperationalKind::configuration_saved : OperationalKind::local_session_timed_out) ||
        !number(f[5], result) || result > static_cast<unsigned>(OperationalOutcome::registration_closed) ||
        !number(f[6], c.node) || !number(f[7], c.generation) || !number(f[8], c.attempt) ||
        !number(f[9], proto) || proto > 2 || !number(f[11], c.peer_port) || !number(f[12], c.local_port) ||
        !number(f[13], c.account_id) || !number(f[14], c.committed_account_id) ||
        !identifier(f[15], c.durable_session_id) || !identifier(f[16], c.correlation_id) ||
        !unescape(f[17], c.caller) || !number(f[18], stage) || stage > static_cast<unsigned>(CallerStage::cleanup) ||
        !number(f[19], c.duration_seconds) || !number(f[20], c.columns) || !number(f[21], c.rows) ||
        !number(f[22], c.resource_id) || !unescape(f[23], c.resource_name) ||
        !number(f[24], c.verified_account_id)) return std::nullopt;
    c.diagnostic_code = f[25];
    if (!version2) c.hostname = f[26];
    if ((version4 || version5 || version6 || version7 || version8 || version9) && !unescape(f[27], c.attempted_name)) return std::nullopt;
    t.kind = static_cast<OperationalKind>(kind); t.outcome = static_cast<OperationalOutcome>(result);
    c.transport = static_cast<OperationalTransport>(proto); c.stage = static_cast<CallerStage>(stage); c.source_ip = f[10];
    auto e = make_operational_entry(sequence, utc, std::move(t));
    if (!valid_operational_entry(e)) return std::nullopt;
    return e;
}
std::string render_operational_line(const AlphaOperationalLogEntry& e, const std::optional<std::string>& local) {
    if (!valid_operational_entry(e)) return {};
    return (local ? *local : "BBS time unavailable") + " [" + e.level + "] " +
        (e.event ? e.message : legacy_message(e.message));
}
std::string_view operational_counter_label(const OperationalCounter counter) noexcept {
    static constexpr std::array<std::string_view,static_cast<std::size_t>(OperationalCounter::count)> names{
        "Connections assigned","Capacity refusals","Source-limit refusals","Authentication denials",
        "Account-session refusals","Local account changes","Local throttle clears","Drains started",
        "Drains cancelled","Backups completed","Backups not completed","Configuration saves"};
    const auto index = static_cast<std::size_t>(counter);
    return index < names.size() ? names[index] : std::string_view{};
}
bool operating_event(const OperationalKind kind) noexcept {
    return operational_class(kind) == OperationalClass::runtime || operational_class(kind) == OperationalClass::configuration ||
        kind == OperationalKind::authenticated || kind == OperationalKind::connection_ended ||
        kind == OperationalKind::assignment_quarantined;
}
void observe_operational_reporting(OperationalReportingSnapshot& snapshot,const AlphaOperationalLogEntry& entry) {
    if (!entry.event || !valid_operational_entry(entry)) return;
    auto counter = OperationalCounter::count;
    switch (entry.event->kind) {
        case OperationalKind::connection_assigned: counter=OperationalCounter::connections; break;
        case OperationalKind::capacity_refused: counter=OperationalCounter::capacity_refusals; break;
        case OperationalKind::source_refused: counter=OperationalCounter::source_refusals; break;
        case OperationalKind::authentication_denied: counter=OperationalCounter::authentication_denials; break;
        case OperationalKind::duplicate_denied: counter=OperationalCounter::duplicate_denials; break;
        case OperationalKind::account_administration: counter=OperationalCounter::account_changes; break;
        case OperationalKind::throttle_cleared: counter=OperationalCounter::throttle_clears; break;
        case OperationalKind::drain_started: counter=OperationalCounter::drains_started; break;
        case OperationalKind::drain_cancelled: counter=OperationalCounter::drains_cancelled; break;
        case OperationalKind::backup_completed: counter=OperationalCounter::backups_completed; break;
        case OperationalKind::backup_failed: counter=OperationalCounter::backups_failed; break;
        case OperationalKind::configuration_saved: counter=OperationalCounter::configuration_saves; break;
        default: break;
    }
    snapshot.available = true;
    if (counter != OperationalCounter::count) {
        auto& count = snapshot.counts[static_cast<std::size_t>(counter)];
        if (count != UINT64_MAX) ++count;
    }
    if (operating_event(entry.event->kind)) {
        if (snapshot.recent.size() == kRecentOperatingEvents) snapshot.recent.erase(snapshot.recent.begin());
        snapshot.recent.push_back(entry);
    }
}
} // namespace sfx::application
