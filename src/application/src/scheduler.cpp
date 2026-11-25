#include "sfx/application/scheduler.hpp"

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace sfx::application {
namespace {

constexpr std::string_view config_header = "SFX-SCHEDULER-CONFIG-2";
constexpr std::string_view legacy_config_header = "SFX-SCHEDULER-CONFIG-1";
constexpr std::string_view state_header = "SFX-SCHEDULER-STATE-1";
constexpr std::string_view maintenance_config_header = "SFX-SCHEDULER-CONFIG-3";
constexpr std::string_view maintenance_state_header = "SFX-SCHEDULER-STATE-2";

bool run_id_valid(const std::string_view value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](const char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F');
    });
}

bool detail_valid(const std::string_view value) {
    return value.size() <= 2048 && value.find('\0') == std::string_view::npos &&
           value.find('\r') == std::string_view::npos &&
           value.find('\n') == std::string_view::npos;
}

const char* result_name(const ScheduledJobResult value) {
    switch (value) {
        case ScheduledJobResult::never: return "never";
        case ScheduledJobResult::running: return "running";
        case ScheduledJobResult::success: return "success";
        case ScheduledJobResult::failed: return "failed";
        case ScheduledJobResult::interrupted: return "interrupted";
        case ScheduledJobResult::partial: return "partial";
        case ScheduledJobResult::skipped: return "skipped";
    }
    return "invalid";
}

std::optional<ScheduledJobResult> parse_result(const std::string_view value) {
    if (value == "never") return ScheduledJobResult::never;
    if (value == "running") return ScheduledJobResult::running;
    if (value == "success") return ScheduledJobResult::success;
    if (value == "failed") return ScheduledJobResult::failed;
    if (value == "interrupted") return ScheduledJobResult::interrupted;
    if (value == "partial") return ScheduledJobResult::partial;
    if (value == "skipped") return ScheduledJobResult::skipped;
    return std::nullopt;
}

template <typename Value>
std::optional<Value> number(const std::string_view value) {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) return std::nullopt;
    Value output{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
    return output;
}

std::string escape(const std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    for (const unsigned char byte : value) {
        if (byte >= 0x20 && byte <= 0x7e && byte != '%' && byte != '|' && byte != '=') {
            output.push_back(static_cast<char>(byte));
        } else {
            output.push_back('%');
            output.push_back(hex[byte >> 4]);
            output.push_back(hex[byte & 0x0f]);
        }
    }
    return output;
}

std::optional<std::string> unescape(const std::string_view value) {
    const auto nibble = [](const char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    std::string output;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') { output.push_back(value[index]); continue; }
        if (index + 2 >= value.size()) return std::nullopt;
        const int high = nibble(value[index + 1]);
        const int low = nibble(value[index + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        output.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return output;
}

std::optional<std::map<std::string, std::string>> fields(
    const std::string_view text, const std::string_view header) {
    if (text.empty() || text.size() > kSchedulerDocumentMaximumBytes ||
        text.back() != '\n' || text.find('\0') != std::string_view::npos) return std::nullopt;
    const auto first = text.find('\n');
    if (first == std::string_view::npos || text.substr(0, first) != header) return std::nullopt;
    std::map<std::string, std::string> output;
    std::size_t offset = first + 1;
    while (offset < text.size()) {
        const auto end = text.find('\n', offset);
        if (end == std::string_view::npos || end == offset) return std::nullopt;
        auto line = text.substr(offset, end - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto separator = line.find('=');
        if (separator == std::string_view::npos || separator == 0 ||
            !output.emplace(std::string(line.substr(0, separator)),
                            std::string(line.substr(separator + 1))).second) return std::nullopt;
        offset = end + 1;
    }
    return output;
}

bool has_fields(const std::map<std::string, std::string>& values,
                const std::initializer_list<const char*> names) {
    return std::all_of(names.begin(), names.end(), [&](const char* name) {
        return values.find(name) != values.end();
    });
}

std::vector<std::string_view> split(const std::string_view value, const char delimiter) {
    std::vector<std::string_view> output;
    std::size_t offset = 0;
    for (;;) {
        const auto end = value.find(delimiter, offset);
        output.push_back(value.substr(offset, end == std::string_view::npos
                                                 ? value.size() - offset
                                                 : end - offset));
        if (end == std::string_view::npos) return output;
        offset = end + 1;
    }
}

} // namespace

bool message_maintenance_job(const std::string_view job) noexcept {
    return job==kMessageRetentionJobId || job==kMessageIntegrityJobId;
}
SchedulerConfiguration default_message_scheduler_configuration(const std::string_view job){
    if(!message_maintenance_job(job))throw std::invalid_argument("unknown message job");
    SchedulerConfiguration value;value.format_version=3;value.job_id=std::string(job);
    value.enabled=true;value.local_hour=0;value.local_minute=job==kMessageIntegrityJobId?30:0;
    if(job==kMessageIntegrityJobId)value.local_weekday=std::uint8_t{0};
    return value;
}
SchedulerState default_message_scheduler_state(const std::string_view job){
    if(!message_maintenance_job(job))throw std::invalid_argument("unknown message job");
    SchedulerState value;value.format_version=2;value.job_id=std::string(job);return value;
}

bool valid_backup_destination_text(const std::string_view value) noexcept {
    if (value.empty()) return true;
#if defined(__linux__)
    // Linux also preserves readable Windows-form saved settings for explicit
    // correction after a folder move. Native destination custody must reject
    // those foreign paths; syntax acceptance never selects a fallback.
    if(value.front()=='/') {
        if(value.size()<2||value.size()>2048||value.back()=='/')return false;
        std::size_t begin=1;
        for(std::size_t i=1;i<=value.size();++i) {
            if(i<value.size()&&value[i]!='/') {
                const auto ch=static_cast<unsigned char>(value[i]);
                if(ch<32||ch==127||ch=='\\')return false;
                continue;
            }
            const auto part=value.substr(begin,i-begin);
            if(part.empty()||part=="."||part=="..")return false;
            begin=i+1;
        }
        return true;
    }
#endif
    if (value.size() < 4 || value.size() > 2048 ||
        !((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z')) ||
        value[1] != ':' || value[2] != '\\' || value.back() == '\\') return false;
    std::size_t begin = 3;
    for (std::size_t index = 3; index <= value.size(); ++index) {
        if (index != value.size() && value[index] != '\\') {
            const auto ch = static_cast<unsigned char>(value[index]);
            if (ch < 32 || ch == 127 || ch == ':' || ch == '/' || ch == '*' ||
                ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') return false;
            continue;
        }
        const auto part = value.substr(begin, index - begin);
        if (part.empty() || part.back() == '.' || part.back() == ' ' || part.find('~') != part.npos) return false;
        // These ASCII aliases cannot denote a backup folder. Unicode itself
        // is validated by the Windows UTF-8/path boundary, not case-folded here.
        const auto stem = part.substr(0, part.find('.'));
        const auto equal = [&](const std::string_view expected) {
            if (stem.size() != expected.size()) return false;
            for (std::size_t i = 0; i < stem.size(); ++i) {
                auto ch = stem[i];
                if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
                if (ch != expected[i]) return false;
            }
            return true;
        };
        if (equal("BIN") || equal("CON") || equal("PRN") || equal("AUX") || equal("NUL")) return false;
        if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9') {
            const auto prefix = stem.substr(0, 3);
            if ((prefix[0] == 'C' || prefix[0] == 'c') && (prefix[1] == 'O' || prefix[1] == 'o') &&
                (prefix[2] == 'M' || prefix[2] == 'm')) return false;
            if ((prefix[0] == 'L' || prefix[0] == 'l') && (prefix[1] == 'P' || prefix[1] == 'p') &&
                (prefix[2] == 'T' || prefix[2] == 't')) return false;
        }
        begin = index + 1;
    }
    return true;
}

bool valid_scheduler_configuration(const SchedulerConfiguration& value) noexcept {
    if(value.format_version==3)return message_maintenance_job(value.job_id) && value.revision!=0 &&
        value.local_hour<24 && value.local_minute<60 && value.backup_destination.empty() && value.retained_backups==10 &&
        (value.job_id==kMessageRetentionJobId?!value.local_weekday:value.local_weekday && *value.local_weekday<7);
    return (value.format_version == 1 || value.format_version == 2) && value.revision != 0 &&
           value.job_id == kBackupJobId && value.local_hour < 24 &&
           value.local_minute < 60 && value.retained_backups >= 1 && value.retained_backups <= 365 &&
           valid_backup_destination_text(value.backup_destination) &&
           !value.local_weekday && (value.format_version != 1 || (value.backup_destination.empty() && value.retained_backups == 10));
}

bool valid_scheduler_state(const SchedulerState& value) noexcept {
    const bool maintenance=value.format_version==2 && message_maintenance_job(value.job_id);
    if ((!maintenance && (value.format_version != 1 || value.job_id != kBackupJobId)) ||
        value.last_consumed_slot_utc<0 || (!maintenance && (value.last_consumed_slot_utc || value.requires_fresh_check)) ||
        (maintenance && value.next_run_utc!=0 && (value.next_run_utc<value.last_consumed_slot_utc ||
            (!value.active && value.next_run_utc==value.last_consumed_slot_utc))) ||
        (value.requires_fresh_check && value.job_id!=kMessageIntegrityJobId) ||
        (value.active && value.job_id==kMessageIntegrityJobId && !value.requires_fresh_check) ||
        value.configuration_revision == 0 || value.next_run_utc < 0 ||
        value.last_run_utc < 0 || value.last_result > (maintenance?ScheduledJobResult::skipped:ScheduledJobResult::interrupted) ||
        !detail_valid(value.last_detail) || value.history.size() > kSchedulerHistoryLimit ||
        (value.active != (value.last_result == ScheduledJobResult::running)) ||
        (value.active && (!run_id_valid(value.active_run_id) ||
                          value.active_started_utc <= 0)) ||
        (!value.active && (!value.active_run_id.empty() ||
                           value.active_started_utc != 0 || value.active_scheduled))) return false;
    return std::all_of(value.history.begin(), value.history.end(), [maintenance](const auto& item) {
        return run_id_valid(item.run_id) && item.started_utc > 0 &&
               item.finished_utc >= item.started_utc &&
               (item.result == ScheduledJobResult::success ||
                item.result == ScheduledJobResult::failed ||
                item.result == ScheduledJobResult::interrupted ||
                (maintenance && (item.result==ScheduledJobResult::partial || item.result==ScheduledJobResult::skipped))) &&
               detail_valid(item.detail);
    });
}

std::string serialize_scheduler_configuration(const SchedulerConfiguration& value) {
    if (!valid_scheduler_configuration(value)) return {};
    std::ostringstream output;
    const bool maintenance=value.format_version==3;
    output << (maintenance?maintenance_config_header:value.format_version == 1 ? legacy_config_header : config_header) << '\n'
           << "job_id=" << value.job_id << '\n'
           << "job_type=" << (maintenance?(value.job_id==kMessageRetentionJobId?"message-retention":"message-integrity"):"backup") << '\n'
           << "revision=" << value.revision << '\n'
           << "enabled=" << (value.enabled ? "true" : "false") << '\n'
           << "schedule=" << (value.local_weekday?"weekly-local":"daily-local") << '\n'
           << "local_hour=" << static_cast<unsigned>(value.local_hour) << '\n'
           << "local_minute=" << static_cast<unsigned>(value.local_minute) << '\n'
           << "missed_run=skip\nretry=none\n";
    if (value.format_version == 2) {
        output << "backup_destination=" << escape(value.backup_destination) << '\n'
               << "retained_backups=" << value.retained_backups << '\n';
    }
    if(maintenance)output << "local_weekday=" << (value.local_weekday?std::to_string(*value.local_weekday):"daily") << '\n';
    return output.str();
}

std::optional<SchedulerConfiguration> parse_scheduler_configuration(
    const std::string_view text) {
    const bool legacy = text.substr(0, text.find('\n')) == legacy_config_header;
    const bool maintenance=text.substr(0,text.find('\n'))==maintenance_config_header;
    const auto parsed = fields(text, maintenance?maintenance_config_header:legacy ? legacy_config_header : config_header);
    if (!parsed || parsed->size() != (maintenance?10U:legacy ? 9U : 11U) ||
        !has_fields(*parsed, {"job_id", "job_type", "revision", "enabled",
                             "schedule", "local_hour", "local_minute",
                             "missed_run", "retry"}) ||
        (!maintenance && (parsed->at("job_id") != kBackupJobId || parsed->at("job_type") != "backup" || parsed->at("schedule") != "daily-local")) ||
        parsed->at("missed_run") != "skip" || parsed->at("retry") != "none") return std::nullopt;
    const auto revision = number<std::uint64_t>(parsed->at("revision"));
    const auto hour = number<unsigned>(parsed->at("local_hour"));
    const auto minute = number<unsigned>(parsed->at("local_minute"));
    if (!revision || !hour || !minute || *hour > 255 || *minute > 255 ||
        (parsed->at("enabled") != "true" && parsed->at("enabled") != "false")) return std::nullopt;
    SchedulerConfiguration value;
    value.format_version = maintenance?3:legacy ? 1 : 2;
    value.job_id=parsed->at("job_id");
    value.revision = *revision;
    value.enabled = parsed->at("enabled") == "true";
    value.local_hour = static_cast<std::uint8_t>(*hour);
    value.local_minute = static_cast<std::uint8_t>(*minute);
    if(maintenance){
        if(!message_maintenance_job(value.job_id) || !has_fields(*parsed,{"local_weekday"}) ||
            parsed->at("job_type")!=(value.job_id==kMessageRetentionJobId?"message-retention":"message-integrity") ||
            parsed->at("schedule")!=(value.job_id==kMessageRetentionJobId?"daily-local":"weekly-local"))return {};
        if(parsed->at("local_weekday")!="daily"){
            const auto day=number<unsigned>(parsed->at("local_weekday"));if(!day || *day>6)return {};value.local_weekday=static_cast<std::uint8_t>(*day);
        }
    }else if (!legacy) {
        if (!has_fields(*parsed, {"backup_destination", "retained_backups"})) return std::nullopt;
        const auto destination = unescape(parsed->at("backup_destination"));
        const auto retention = number<std::uint32_t>(parsed->at("retained_backups"));
        if (!destination || !retention) return std::nullopt;
        value.backup_destination = *destination;
        value.retained_backups = *retention;
    }
    return valid_scheduler_configuration(value) ? std::optional(value) : std::nullopt;
}

std::string serialize_scheduler_state(const SchedulerState& value) {
    if (!valid_scheduler_state(value)) return {};
    std::ostringstream output;
    output << (value.format_version==2?maintenance_state_header:state_header) << '\n'
           << "job_id=" << value.job_id << '\n'
           << "configuration_revision=" << value.configuration_revision << '\n'
           << "next_run_utc=" << value.next_run_utc << '\n'
           << "active=" << (value.active ? "true" : "false") << '\n'
           << "active_scheduled=" << (value.active_scheduled ? "true" : "false") << '\n'
           << "active_run_id=" << value.active_run_id << '\n'
           << "active_started_utc=" << value.active_started_utc << '\n'
           << "last_run_utc=" << value.last_run_utc << '\n'
           << "last_result=" << result_name(value.last_result) << '\n'
           << "last_detail=" << escape(value.last_detail) << '\n'
           << "history_count=" << value.history.size() << '\n';
    if(value.format_version==2)output << "last_consumed_slot_utc=" << value.last_consumed_slot_utc << '\n'
        << "requires_fresh_check=" << (value.requires_fresh_check?"true":"false") << '\n';
    for (std::size_t index = 0; index < value.history.size(); ++index) {
        const auto& item = value.history[index];
        output << "history_" << index << '=' << item.run_id << '|'
               << item.started_utc << '|' << item.finished_utc << '|'
               << result_name(item.result) << '|' << escape(item.detail) << '\n';
    }
    const auto result = output.str();
    return result.size() <= kSchedulerDocumentMaximumBytes ? result : std::string{};
}

std::optional<SchedulerState> parse_scheduler_state(const std::string_view text) {
    const bool maintenance=text.substr(0,text.find('\n'))==maintenance_state_header;
    const auto parsed = fields(text, maintenance?maintenance_state_header:state_header);
    if (!parsed || parsed->size() < 11 ||
        !has_fields(*parsed, {"job_id", "configuration_revision", "next_run_utc",
                             "active", "active_scheduled", "active_run_id",
                             "active_started_utc", "last_run_utc", "last_result",
                             "last_detail", "history_count"}) ||
        (maintenance?!message_maintenance_job(parsed->at("job_id")):parsed->at("job_id") != kBackupJobId)) return std::nullopt;
    const auto revision = number<std::uint64_t>(parsed->at("configuration_revision"));
    const auto next = number<std::int64_t>(parsed->at("next_run_utc"));
    const auto active_started = number<std::int64_t>(parsed->at("active_started_utc"));
    const auto last_run = number<std::int64_t>(parsed->at("last_run_utc"));
    const auto last_result = parse_result(parsed->at("last_result"));
    const auto count = number<std::size_t>(parsed->at("history_count"));
    const auto detail = unescape(parsed->at("last_detail"));
    if (!revision || !next || !active_started || !last_run || !last_result || !count ||
        !detail || *count > kSchedulerHistoryLimit || parsed->size() != (maintenance?13:11) + *count ||
        (parsed->at("active") != "true" && parsed->at("active") != "false") ||
        (parsed->at("active_scheduled") != "true" &&
         parsed->at("active_scheduled") != "false")) return std::nullopt;
    SchedulerState value;
    if(maintenance){
        if(!has_fields(*parsed,{"last_consumed_slot_utc","requires_fresh_check"}))return {};
        const auto slot=number<std::int64_t>(parsed->at("last_consumed_slot_utc"));const auto guard=parsed->at("requires_fresh_check");
        if(!slot || (guard!="true" && guard!="false"))return {};
        value.format_version=2;value.job_id=parsed->at("job_id");value.last_consumed_slot_utc=*slot;value.requires_fresh_check=guard=="true";
    }
    value.configuration_revision = *revision;
    value.next_run_utc = *next;
    value.active = parsed->at("active") == "true";
    value.active_scheduled = parsed->at("active_scheduled") == "true";
    value.active_run_id = parsed->at("active_run_id");
    value.active_started_utc = *active_started;
    value.last_run_utc = *last_run;
    value.last_result = *last_result;
    value.last_detail = *detail;
    for (std::size_t index = 0; index < *count; ++index) {
        const auto found = parsed->find("history_" + std::to_string(index));
        if (found == parsed->end()) return std::nullopt;
        const auto parts = split(found->second, '|');
        if (parts.size() != 5) return std::nullopt;
        const auto started = number<std::int64_t>(parts[1]);
        const auto finished = number<std::int64_t>(parts[2]);
        const auto result = parse_result(parts[3]);
        const auto history_detail = unescape(parts[4]);
        if (!started || !finished || !result || !history_detail) return std::nullopt;
        value.history.push_back({std::string(parts[0]), *started, *finished,
                                 *result, *history_detail});
    }
    return valid_scheduler_state(value) ? std::optional(value) : std::nullopt;
}

RuntimeScheduler::RuntimeScheduler(SchedulerConfiguration configuration,
                                   SchedulerState state,
                                   NextDailyRun next_daily_run,NextMaintenanceRun next_maintenance_run)
    : configuration_(std::move(configuration)), state_(std::move(state)),
      next_daily_run_(std::move(next_daily_run)),next_maintenance_run_(std::move(next_maintenance_run)) {
    if (!valid_scheduler_configuration(configuration_) ||
        !valid_scheduler_state(state_) || state_.job_id!=configuration_.job_id || !next_daily_run_ ||
        (message_maintenance_job(configuration_.job_id) && !next_maintenance_run_)) {
        throw std::invalid_argument("invalid scheduler construction");
    }
}

bool RuntimeScheduler::schedule_next(const std::int64_t after_utc) {
    if (!configuration_.enabled) { state_.next_run_utc = 0; return true; }
    const auto next = message_maintenance_job(configuration_.job_id)?
        next_maintenance_run_(std::max(after_utc,state_.last_consumed_slot_utc),configuration_.local_hour,configuration_.local_minute,configuration_.local_weekday,state_.last_consumed_slot_utc):
        next_daily_run_(after_utc, configuration_.local_hour,configuration_.local_minute);
    if (!next || *next <= after_utc) return false;
    state_.next_run_utc = *next;
    return true;
}

void RuntimeScheduler::append_history(ScheduledJobHistory record) {
    state_.history.push_back(std::move(record));
    if (state_.history.size() > kSchedulerHistoryLimit) {
        state_.history.erase(state_.history.begin(),
                             state_.history.begin() +
                                 (state_.history.size() - kSchedulerHistoryLimit));
    }
}

bool RuntimeScheduler::recover_on_start(const std::int64_t now_utc) {
    if (now_utc <= 0) return false;
    const bool interrupted=state_.active;
    if (state_.active) {
        const auto recorded_finish = std::max(now_utc, state_.active_started_utc);
        append_history({state_.active_run_id, state_.active_started_utc, recorded_finish,
                        ScheduledJobResult::interrupted,
                        "Runtime stopped before completion; no automatic retry"});
        state_.last_run_utc = recorded_finish;
        state_.last_result = ScheduledJobResult::interrupted;
        state_.last_detail = "Runtime stopped before completion; no automatic retry";
        state_.active = false;
        state_.active_scheduled = false;
        state_.active_run_id.clear();
        state_.active_started_utc = 0;
    }
    if (state_.configuration_revision != configuration_.revision ||
        (interrupted && state_.format_version==2) ||
        (configuration_.enabled && state_.next_run_utc <= now_utc) ||
        (!configuration_.enabled && state_.next_run_utc != 0)) {
        state_.configuration_revision = configuration_.revision;
        return schedule_next(now_utc);
    }
    return true;
}

bool RuntimeScheduler::recompute_next(const std::int64_t now_utc) {
    if (now_utc <= 0) return false;
    state_.configuration_revision = configuration_.revision;
    return schedule_next(now_utc);
}

bool RuntimeScheduler::reconfigure(SchedulerConfiguration configuration,
                                   const std::int64_t now_utc) {
    if (!valid_scheduler_configuration(configuration) || now_utc <= 0 ||
        configuration.job_id!=configuration_.job_id || configuration.revision <= configuration_.revision) return false;
    configuration_ = std::move(configuration);
    state_.configuration_revision = configuration_.revision;
    return schedule_next(now_utc);
}

std::optional<ScheduledRun> RuntimeScheduler::begin(
    const std::int64_t now_utc, std::string run_id, const bool scheduled,
    const bool consumes_due_run) {
    if (now_utc <= 0 || state_.active || !run_id_valid(run_id)) return std::nullopt;
    ScheduledRun run{configuration_.job_id, std::move(run_id),
                     configuration_.revision, now_utc, scheduled,
                     consumes_due_run, configuration_.backup_destination,
                     configuration_.retained_backups};
    state_.active = true;
    state_.active_scheduled = scheduled;
    state_.active_run_id = run.run_id;
    state_.active_started_utc = now_utc;
    if(state_.format_version==2 && consumes_due_run)state_.last_consumed_slot_utc=state_.next_run_utc;
    if(configuration_.job_id==kMessageIntegrityJobId)state_.requires_fresh_check=true;
    state_.last_run_utc = now_utc;
    state_.last_result = ScheduledJobResult::running;
    state_.last_detail = message_maintenance_job(configuration_.job_id)?(scheduled?"Scheduled message maintenance running":"Manual message maintenance running"):scheduled ? "Scheduled backup running" : "Manual backup running";
    return run;
}

std::optional<ScheduledRun> RuntimeScheduler::begin_due(
    const std::int64_t now_utc, std::string run_id) {
    if (!configuration_.enabled || state_.next_run_utc == 0 ||
        now_utc < state_.next_run_utc) return std::nullopt;
    return begin(now_utc, std::move(run_id), true, true);
}

std::optional<ScheduledRun> RuntimeScheduler::begin_manual(
    const std::int64_t now_utc, std::string run_id) {
    const bool consumes_due = configuration_.enabled && state_.next_run_utc != 0 &&
                              now_utc >= state_.next_run_utc;
    return begin(now_utc, std::move(run_id), false, consumes_due);
}

bool RuntimeScheduler::complete(const ScheduledRun& run,
                                const std::int64_t finished_utc,
                                const ScheduledJobResult result,
                                std::string detail) {
    if (!state_.active || run.job_id!=configuration_.job_id || state_.active_run_id != run.run_id ||
        state_.active_started_utc != run.started_utc || finished_utc <= 0 ||
        (result != ScheduledJobResult::success &&
         result != ScheduledJobResult::failed && !(state_.format_version==2 && (result==ScheduledJobResult::partial || result==ScheduledJobResult::skipped || result==ScheduledJobResult::interrupted))) || !detail_valid(detail)) return false;
    const auto recorded_finish = std::max(finished_utc, run.started_utc);
    append_history({run.run_id, run.started_utc, recorded_finish, result, detail});
    state_.active = false;
    state_.active_scheduled = false;
    state_.active_run_id.clear();
    state_.active_started_utc = 0;
    state_.last_run_utc = recorded_finish;
    state_.last_result = result;
    state_.last_detail = std::move(detail);
    return !run.consumes_due_run || schedule_next(finished_utc);
}

void RuntimeScheduler::replace_state(SchedulerState state) {
    if (!valid_scheduler_state(state) || state.job_id!=configuration_.job_id) throw std::invalid_argument("invalid scheduler state");
    state_ = std::move(state);
}

} // namespace sfx::application
