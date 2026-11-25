#include "sfx/application/caller_journal.hpp"
#include "sfx/application/file.hpp"
#include <algorithm>
#include "sfx/application/phase3a_storage.hpp"
#include "sfx/application/hostname_lookup.hpp"
#include <charconv>
#include <iomanip>
#include <sstream>

namespace sfx::application {
namespace {
bool printable(const std::string_view value, const std::size_t maximum) noexcept {
    if (value.size() > maximum) return false;
    for (const unsigned char c : value) if (c < 32 || c > 126) return false;
    return true;
}
bool name_valid(const std::string& value) {
    const auto normalized = normalize_account_name(value);
    return normalized && *normalized == value;
}
std::string field(const std::string_view value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char c : value) {
        if (c == '\\' || c == '|' || c == '<' || c == '>') {
            result += "\\x"; result += digits[c >> 4]; result += digits[c & 15];
        } else result += static_cast<char>(c);
    }
    return result;
}
const char* activity_label(const CallerActivityKind kind) noexcept {
    switch (kind) {
    case CallerActivityKind::successful_logon: return "Successful logon";
    case CallerActivityKind::registration_committed: return "New caller registration committed";
    case CallerActivityKind::main_menu: return "MAIN MENU entered";
    case CallerActivityKind::message_area: return "MESSAGE AREA entered";
    case CallerActivityKind::file_area: return "FILE AREA entered";
    case CallerActivityKind::door_entered: return "DOOR entered";
    case CallerActivityKind::door_returned: return "DOOR returned";
    case CallerActivityKind::chat_entered: return "CHAT entered";
    case CallerActivityKind::chat_left: return "CHAT left";
    }
    return nullptr;
}
struct Local { std::string date, time; };
std::optional<Local> local(const JournalLocalTime& format, const std::int64_t utc) {
    const auto value = format ? format(utc) : std::nullopt;
    if (!value || value->size() != 19 || (*value)[4] != '-' || (*value)[7] != '-' ||
        (*value)[10] != ' ' || (*value)[13] != ':' || (*value)[16] != ':') return {};
    for (std::size_t i = 0; i < value->size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
        if ((*value)[i] < '0' || (*value)[i] > '9') return {};
    }
    return Local{value->substr(5, 2) + '-' + value->substr(8, 2) + '-' + value->substr(2, 2),
                 value->substr(11, 8)};
}
std::string session_token(const Identifier128& id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    for (const auto byte : id.bytes) { output += digits[byte >> 4]; output += digits[byte & 15]; }
    return output;
}
bool number(std::string_view value, std::int64_t& result) {
    if (value.empty()) return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
        std::to_string(result) == value;
}
}
bool valid_caller_activity(const CallerActivity& activity) noexcept {
    return activity_label(activity.kind) && valid_utc_seconds(activity.utc_seconds) &&
        printable(activity.configured_label, 128) &&
        (activity.kind > CallerActivityKind::main_menu || activity.configured_label.empty());
}
std::optional<std::string> encode_caller_activities(const std::vector<CallerActivity>& activities) {
    if (activities.size() > kCallerJournalActivityLimit) return {};
    std::string result;
    for (const auto& activity : activities) {
        if (!valid_caller_activity(activity)) return {};
        result += std::to_string(static_cast<unsigned>(activity.kind)) + '|' +
            std::to_string(activity.utc_seconds) + '|' + activity.configured_label + '\n';
    }
    if (result.size() > 6144) return {};
    return result;
}
std::optional<std::vector<CallerActivity>> decode_caller_activities(std::string_view encoded) {
    if (encoded.size() > 6144) return {};
    std::vector<CallerActivity> result;
    while (!encoded.empty()) {
        const auto end = encoded.find('\n'), first = encoded.find('|');
        if (end == std::string_view::npos || first >= end) return {};
        const auto second = encoded.find('|', first + 1);
        if (second >= end) return {};
        std::int64_t kind{}, utc{};
        if (!number(encoded.substr(0, first), kind) || kind < 0 || kind > 8 ||
            !number(encoded.substr(first + 1, second - first - 1), utc)) return {};
        CallerActivity activity{static_cast<CallerActivityKind>(kind), utc,
            std::string(encoded.substr(second + 1, end - second - 1))};
        if (!valid_caller_activity(activity) || result.size() == kCallerJournalActivityLimit) return {};
        result.push_back(std::move(activity)); encoded.remove_prefix(end + 1);
    }
    return result;
}
std::optional<std::string> render_caller_journal(const CallerJournalRecord& record, const JournalLocalTime& format) {
    const auto& s = record.session;
    const auto start = local(format, s.started_utc_seconds);
    const auto end = s.completed_utc_seconds ? local(format, *s.completed_utc_seconds) : std::nullopt;
    if (record.caller_number <= 0 || !s.session_id.valid() || !s.outcome || !end || !start ||
        !s.completed_utc_seconds || *s.completed_utc_seconds < s.started_utc_seconds ||
        s.persisted_seconds < 0 || s.node_id < 1 || s.node_id > 128 || !name_valid(record.display_name) ||
        (record.transport != "Telnet" && record.transport != "SSH" && record.transport != "unknown") ||
        (!record.source_ip.empty() && (!canonicalize_source_ip(record.source_ip) ||
         canonicalize_source_ip(record.source_ip)->text != record.source_ip)) ||
        (!record.hostname_hint.empty() && (record.source_ip.empty() || !normalize_hostname_hint(record.hostname_hint) ||
         normalize_hostname_hint(record.hostname_hint)->view() != record.hostname_hint)) ||
        record.ansi < -1 || record.ansi > 1 || !encode_caller_activities(record.activities)) return {};
    const char* outcome = nullptr;
    switch (*s.outcome) {
    case SessionOutcomeCode::normal: outcome = "logged off normally"; break;
    case SessionOutcomeCode::disconnected: outcome = "disconnected (network connection ended)"; break;
    case SessionOutcomeCode::timed_out: outcome = "disconnected (timeout)"; break;
    case SessionOutcomeCode::shutdown: outcome = "disconnected (Sysop/runtime shutdown)"; break;
    case SessionOutcomeCode::runtime_error: outcome = "disconnected (internal Runtime failure)"; break;
    }
    if (!outcome) return {};
    std::ostringstream out;
    out << "CONNECT " << record.transport << '/' << (record.hostname_hint.empty() ? "<unknown>" : record.hostname_hint)
        << '/' << (record.source_ip.empty() ? "<unknown>" : record.source_ip) << "\r\nCALLER #" << record.caller_number
        << "\r\n" << field(record.display_name) << " called at " << start->time << " on " << start->date << " using Node "
        << s.node_id << " via " << record.transport << ".\r\n";
    if (!record.hostname_hint.empty()) out << "Hostname is an untrusted display hint.\r\n";
    out << (record.ansi == 1 ? "ANSI Color Graphics Selected.\r\n" : record.ansi == 0 ?
        "ASCII Text Selected.\r\n" : "Graphics selection unavailable.\r\n");
    for (const auto& activity : record.activities) {
        const auto time = local(format, activity.utc_seconds);
        if (!time || (activity.kind != CallerActivityKind::registration_committed &&
                activity.utc_seconds < s.started_utc_seconds) || activity.utc_seconds > *s.completed_utc_seconds) return {};
        out << activity_label(activity.kind);
        if (!activity.configured_label.empty()) out << ": " << field(activity.configured_label);
        out << " at " << time->date << ' ' << time->time << ".\r\n";
    }
    if (record.recovered) out << "Recovered after interrupted Runtime; end and duration use last authoritative persisted evidence.\r\n";
    out << field(record.display_name) << ' ' << outcome << " at " << end->time << " on " << end->date << ".\r\nDuration: "
        << std::setfill('0') << std::setw(2) << s.persisted_seconds / 3600 << ':'
        << std::setw(2) << (s.persisted_seconds / 60) % 60 << ':' << std::setw(2) << s.persisted_seconds % 60
        << "\r\nSESSION " << session_token(s.session_id) << "\r\n\r\n---\r\n";
    auto result = out.str();
    return result.size() <= kCallerJournalRecordBytes ? std::optional<std::string>(std::move(result)) : std::nullopt;
}
std::optional<std::string> render_attention_journal(const AttentionRecord& record, const JournalLocalTime& format) {
    const auto time = local(format, record.utc_seconds);
    if (!time || record.node > 128 || record.existing_node > 128 ||
        (!record.transport.empty() && record.transport != "Telnet" && record.transport != "SSH") ||
        (!record.source_ip.empty() && (!canonicalize_source_ip(record.source_ip) ||
         canonicalize_source_ip(record.source_ip)->text != record.source_ip)) ||
        (record.identity == AttentionIdentity::none ? !record.account_name.empty() : !name_valid(record.account_name))) return {};
    const char* kind = nullptr; const char* explanation = nullptr;
    switch (record.kind) {
    case AttentionKind::duplicate_login:
        if (record.identity != AttentionIdentity::verified || !record.node) return {};
        kind = "DUPLICATE_LOGIN"; explanation = "Duplicate disconnected; original session retained."; break;
    case AttentionKind::password_attempts_exhausted:
        if (record.identity != AttentionIdentity::claimed || !record.node) return {};
        kind = "PASSWORD_ATTEMPTS_EXHAUSTED"; explanation = "Attempt limit reached; connection disconnected."; break;
    case AttentionKind::new_name_denied:
        if (record.identity != AttentionIdentity::claimed || !record.node) return {};
        kind = "NEW_NAME_DENIED"; explanation = "New account name unavailable; no account created."; break;
    case AttentionKind::name_list_invalid:
        if (record.identity != AttentionIdentity::none) return {};
        kind = "NAME_LIST_INVALID"; explanation = "JOKER.DAT unavailable or invalid; new-name checks fail closed until corrected. Saved corrections apply at the next check without restarting. Existing-account login remains available."; break;
    case AttentionKind::botgate_security:
        if (record.identity!=AttentionIdentity::none) return {};
        kind="BOTGATE_SECURITY"; explanation="Security refusal; review BotGate rules and operational log. Repetitions are aggregated."; break;
    case AttentionKind::botgate_feed_failure:
        if (record.identity!=AttentionIdentity::none) return {};
        kind="BOTGATE_FEED_FAILURE"; explanation="Downloaded feed could not refresh. Last good feed retained if available; challenge and local rules remain active."; break;
    case AttentionKind::operational_logging_failure:
        if (record.identity!=AttentionIdentity::none) return {};
        kind="OPERATIONAL_LOGGING_FAILURE"; explanation="Runtime file recording is degraded. Review storage permissions, disk space and retained active log; caller handling continues."; break;
    case AttentionKind::clamav_failure:
        if(record.identity!=AttentionIdentity::none)return {};
        kind="CLAMAV_FAILURE";explanation=record.scanner_problem?"Upload scan unavailable or incomplete; no clean result claimed. Upload held privately.":record.definitions_usable?"Definition update failed; existing validated definitions remain usable.":"Definition checking unavailable; definitions unavailable/unverified. If scanning cannot complete, new uploads are held privately.";break;
    case AttentionKind::clamav_recovery:
        if(record.identity!=AttentionIdentity::none||!record.definitions_usable)return {};
        kind="CLAMAV_RECOVERY";explanation=record.scanner_problem?"Scanning recovered; a complete scan succeeded.":"Definition checking recovered; definitions verified.";break;
    case AttentionKind::clamav_finding:
        if(record.identity!=AttentionIdentity::verified||!record.node||!valid_file_name(record.filename)||record.signatures.empty()||record.signatures.size()>32)return {};
        for(const auto& signature:record.signatures)if(signature.empty()||!printable(signature,128))return {};
        kind="CLAMAV_FINDING";explanation=record.deletion_confirmed?(record.deletion_journal_confirmed?"Malware detected; upload rejected; staged file deletion confirmed.":"Malware detected; staged file deletion confirmed; journal reconciliation required."):"Malware detected; upload rejected; deletion NOT confirmed; private staging requires Sysop review.";break;
    case AttentionKind::saved_connection_unavailable:
        if(record.identity!=AttentionIdentity::none||record.resource.empty()||!printable(record.resource,160))return {};
        kind="SAVED_CONNECTION_UNAVAILABLE";explanation="Saved connection could not be verified. Original value preserved; check BBS Health Check and the connection settings. Restore matching Config files or correct the affected credential. Unrelated Desktop operation remains available.";break;
    case AttentionKind::work_preserved:
        if(record.identity!=AttentionIdentity::none||record.resource.empty()||!printable(record.resource,160))return {};
        kind="WORK_PRESERVED";explanation="Work completion or cleanup was not confirmed. Files preserved for Sysop review; do not assume success or replay the operation blindly. Check Events and the named job's files.";break;
    case AttentionKind::door_bulletin_unavailable:
        if(record.identity!=AttentionIdentity::none||record.resource.empty()||!printable(record.resource,160))return {};
        kind="DOOR_BULLETIN_UNAVAILABLE";explanation="The Door Bulletin source could not be validated. Callers see an unavailable notice. Check Doors > Door Bulletins and Validate/Refresh; source files were not changed.";break;
    case AttentionKind::configured_event:
        if (record.identity!=AttentionIdentity::none || record.resource.empty() || !printable(record.resource,160)) return {};
        kind="EXTERNAL_EVENT"; explanation="Review the event in Events or Live Logs."; break;
    default: return {}; // Reserved producers require their own reviewed contracts.
    }
    std::ostringstream out;
    if (!record.occurrences || record.diagnostic.size()>96 ||
        !std::all_of(record.diagnostic.begin(),record.diagnostic.end(),[](char c) {
            return (c>='a'&&c<='z') || (c>='0'&&c<='9') || c=='_'; })) return {};
    out << time->date << ' ' << time->time << " | " << kind;
    if (record.node) out << " | Node " << record.node;
    if (record.existing_node) out << " | Existing node " << record.existing_node;
    if (!record.transport.empty()) out << " | " << record.transport;
    if (!record.source_ip.empty()) out << " | IP " << record.source_ip;
    if (!record.diagnostic.empty()) out << " | Reason " << record.diagnostic;
    if (record.occurrences>1) out << " | Occurrences " << record.occurrences;
    if (record.identity != AttentionIdentity::none) out << (record.identity == AttentionIdentity::verified ?
        " | Verified caller: " : " | Claimed account (unverified): ") << field(record.account_name);
    if(record.kind==AttentionKind::clamav_finding){out<<" | File "<<field(record.filename)<<" | Signature "<<field(record.signatures.front());if(record.signatures.size()>1)out<<" | Additional signatures "<<record.signatures.size()-1;}
    if(record.kind==AttentionKind::saved_connection_unavailable||record.kind==AttentionKind::work_preserved||record.kind==AttentionKind::door_bulletin_unavailable||record.kind==AttentionKind::configured_event)out<<" | Resource "<<field(record.resource);
    out << " | " << explanation << "\r\n";
    auto result = out.str();
    return result.size() <= kAttentionJournalRecordBytes ? std::optional<std::string>(std::move(result)) : std::nullopt;
}
std::optional<AttentionRecord> ClamAvAttentionTracker::observe(bool scanning,bool healthy,bool usable,std::string diagnostic,std::int64_t utc){
    auto& previous=problems_[scanning?1:0];
    if(healthy){if(previous.empty())return {};previous.clear();AttentionRecord r;r.kind=AttentionKind::clamav_recovery;r.utc_seconds=utc;r.definitions_usable=true;r.scanner_problem=scanning;r.diagnostic="clamav_recovered";return r;}
    if(diagnostic.empty()||diagnostic.size()>96||!std::all_of(diagnostic.begin(),diagnostic.end(),[](char c){return(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';}))return {};
    const auto key=diagnostic+(usable?"1":"0");if(previous==key)return {};previous=key;
    AttentionRecord r;r.kind=AttentionKind::clamav_failure;r.utc_seconds=utc;r.diagnostic=std::move(diagnostic);r.definitions_usable=usable;r.scanner_problem=scanning;return r;
}
} // namespace sfx::application
