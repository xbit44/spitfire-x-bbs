#include "alpha_operational_journal.hpp"
#include "sfx/application/control_panel.hpp"
#include "sfx/platform/network.hpp"
#ifdef _WIN32
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/durable_journal_files.hpp"
#include <windows.h>
#else
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/platform/linux/durable_journal_files.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#endif
#include <chrono>
#include <algorithm>
#include <limits>
#include <map>

namespace sfx::apps::runtime {
namespace {
namespace app = application;

#ifdef _WIN32
struct File {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
#endif
}
std::vector<app::AlphaOperationalLogEntry> AlphaOperationalJournal::read(
    const std::filesystem::path& path, std::uint8_t& health) noexcept {
    std::vector<app::AlphaOperationalLogEntry> result;
    try {
#ifdef _WIN32
        File file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) health |= app::operational_log_write_failed;
            return result;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        LARGE_INTEGER size{};
        if (!GetFileInformationByHandle(file.value, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            !GetFileSizeEx(file.value, &size) || size.QuadPart < 0) {
            health |= app::operational_log_write_failed; return result;
        }
        if (static_cast<std::uint64_t>(size.QuadPart) > app::kDurableJournalMaximumFileBytes) {
            health |= app::operational_log_history_invalid; return result;
        }
        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD offset = 0;
        while (offset < bytes.size()) {
            DWORD count = 0;
            if (!ReadFile(file.value, bytes.data() + offset,
                    static_cast<DWORD>(bytes.size() - offset), &count, nullptr) || !count) {
                health |= app::operational_log_write_failed; return {};
            }
            offset += count;
        }
#else
        auto directory=platform::linux_host::open_owned_directory(path.parent_path());
        struct stat info{};
        if(fstatat(directory.get(),path.filename().c_str(),&info,AT_SYMLINK_NOFOLLOW)!=0) {
            if(errno!=ENOENT)health|=app::operational_log_write_failed;
            return result;
        }
        if(info.st_size<0||static_cast<std::uint64_t>(info.st_size)>app::kDurableJournalMaximumFileBytes) {
            health|=app::operational_log_history_invalid;return result;
        }
        const auto data=platform::linux_host::read_private_file(directory.get(),path.filename().string(),
            app::kDurableJournalMaximumFileBytes);
        const std::string bytes(data.begin(),data.end());
#endif
        std::uint64_t sequence = 0;
        std::map<decltype(app::Identifier128{}.bytes), std::uint64_t> origins;
        std::size_t begin = 0;
        while (begin < bytes.size()) {
            const auto newline = bytes.find('\n', begin);
            if (newline == bytes.npos) { health |= app::operational_log_history_invalid; break; }
            std::string_view line(bytes.data() + begin, newline - begin);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            auto entry = app::decode_operational_record(line, sequence + 1);
            if (entry && entry->event) {
                const auto& context = entry->event->context;
                auto& previous = origins[context.epoch.bytes];
                if (context.origin_sequence <= previous) entry.reset();
                else previous = context.origin_sequence;
            }
            if (entry) {
                ++sequence;
                if (result.size() == app::kControlPanelMaximumLogEntries) result.erase(result.begin());
                result.push_back(std::move(*entry));
            } else health |= app::operational_log_history_invalid;
            begin = newline + 1;
        }
    } catch (...) { health |= app::operational_log_write_failed; }
    return result;
}
AlphaOperationalJournal::AlphaOperationalJournal(std::filesystem::path path,
    std::wstring sid, app::Identifier128 epoch,std::shared_ptr<app::EventSoundStream> sounds)
    : path_(std::move(path)), owner_sid_(std::move(sid)), epoch_(epoch),sounds_(sounds?std::move(sounds):std::make_shared<app::EventSoundStream>(epoch)), records_(epoch) {
    gate_started_=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    for(auto& count:gate_counts_) count.store(0);
    std::uint8_t health = 0;
    // Validate old durable history for safe append, but never import it into a
    // new Runtime's transient history or relabel its delivery/epoch identities.
    (void)read(path_, health);
    health_.store(health);
    blocked_append_ = health != 0; // preserve malformed/full history; never rewrite/reset it
    try { worker_=std::thread([this] { writer(); }); }
    catch (...) { blocked_append_=true; health_.fetch_or(app::operational_log_write_failed); }
#if defined(SFX_ALPHA_TEST_AUTOMATION) && defined(_WIN32)
    // Test builds only: bounded synthetic immutable events for actual IPC/UI
    // backlog qualification. Never compiled into the owner/shipping pair.
    wchar_t fixture[16]{};
    const auto length = GetEnvironmentVariableW(L"SFX_UI6_LOG_FIXTURE", fixture, 16);
    if (length && length < 16) {
        wchar_t* end = nullptr;
        const auto count = wcstoul(fixture, &end, 10);
        if (end != fixture && *end == 0 && count <= 12000) {
            wchar_t large[2]{};
            const bool large_records = GetEnvironmentVariableW(L"SFX_UI6_LARGE_LOG_FIXTURE", large, 2) == 1 && large[0] == '1';
            for (unsigned long i = 0; i < count; ++i) {
                app::OperationalEvent e; e.kind = app::OperationalKind::connection_assigned;
                auto& c = e.context; c.node = static_cast<std::uint16_t>(i % 4 + 1);
                c.generation = i + 1; c.attempt = i + 1;
                c.transport = i % 2 ? app::OperationalTransport::ssh : app::OperationalTransport::telnet;
                c.source_ip = "127.0.0.1"; c.diagnostic_code = "ui6_fixture_" + std::to_string(i);
                if (large_records) {
                    c.hostname = std::string(63, 'a') + "." + std::string(63, 'b') + "." +
                        std::string(63, 'c') + "." + std::string(61, 'd');
                    c.resource_id = 1; c.resource_name.assign(128, '%');
                    c.diagnostic_code.assign(96, 'd');
                }
                emit(std::move(e));
            }
        }
    }
#endif
}
std::uint64_t AlphaOperationalJournal::next_attempt() noexcept {
    auto current = attempts_.load();
    while (current != UINT64_MAX) {
        if (attempts_.compare_exchange_weak(current, current + 1)) return current + 1;
    }
    health_.fetch_or(app::operational_log_event_rejected);
    return 0; // never wrap/reuse an identity
}
void AlphaOperationalJournal::gate_count(unsigned index) noexcept {
    if(index>=gate_counts_.size()) return;
    auto& count=gate_counts_[index];auto n=count.load();
    while(n!=UINT64_MAX && !count.compare_exchange_weak(n,n+1)) {}
}
void AlphaOperationalJournal::gate_active(bool entering) noexcept {
    if(entering) gate_count(6);
    else {auto& count=gate_counts_[6];auto n=count.load();while(n && !count.compare_exchange_weak(n,n-1)) {}}
}
void AlphaOperationalJournal::gate_status(app::BotGateFeedStatus& status) const noexcept {
    status.protection_started=gate_started_;
    for(unsigned i=0;i<gate_counts_.size();++i) status.protection_counts[i]=gate_counts_[i].load();
}
void AlphaOperationalJournal::emit(app::OperationalEvent event) noexcept {
    try {
        std::lock_guard<std::mutex> serial(file_mutex_);
        if (stopping_ && event.kind!=app::OperationalKind::flood_aggregate) return;
        if (origin_ == UINT64_MAX || delivery_ == UINT64_MAX) {
            health_.fetch_or(app::operational_log_event_rejected); return;
        }
        if (event.context.epoch.valid() && event.context.epoch != epoch_) {
            health_.fetch_or(app::operational_log_event_rejected); return;
        }
        sounds_->observe(event,platform::network::monotonic_millis());
        const auto now=platform::network::monotonic_millis()/1000;
        if (now!=flood_window_) { flood_window_=now; flood_emitted_=0; }
        const auto category=app::operational_class(event.kind);
        const bool traffic=event.kind!=app::OperationalKind::flood_aggregate && (category==app::OperationalClass::connection || category==app::OperationalClass::authentication ||
            category==app::OperationalClass::registration || category==app::OperationalClass::activity ||
            category==app::OperationalClass::cleanup ||
            (event.kind>=app::OperationalKind::botgate_passed && event.kind<=app::OperationalKind::botgate_blocked));
        event.context.epoch = epoch_;
        event.context.origin_sequence = ++origin_;
        const auto utc = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto entry = app::make_operational_entry(delivery_ + 1, utc, std::move(event));
        const auto record = app::encode_operational_record(entry);
        if (record.empty()) { health_.fetch_or(app::operational_log_event_rejected); return; }
        switch(entry.event->kind) {
            case app::OperationalKind::botgate_passed: gate_count(1);break;
            case app::OperationalKind::botgate_failed: gate_count(2);break;
            case app::OperationalKind::botgate_timed_out: gate_count(3);break;
            case app::OperationalKind::botgate_blocked: gate_count(4);break;
            case app::OperationalKind::botgate_bypassed: gate_count(5);break;
            default:break;
        }
        const auto kind=static_cast<std::size_t>(entry.event->kind);
        if (traffic && ++flood_emitted_>128) {
            // Aggregation changes presentation volume, not truthful counters.
            auto counter=app::OperationalCounter::count;
            switch (entry.event->kind) {
                case app::OperationalKind::connection_assigned: counter=app::OperationalCounter::connections; break;
                case app::OperationalKind::capacity_refused: counter=app::OperationalCounter::capacity_refusals; break;
                case app::OperationalKind::source_refused: counter=app::OperationalCounter::source_refusals; break;
                case app::OperationalKind::authentication_denied: counter=app::OperationalCounter::authentication_denials; break;
                case app::OperationalKind::duplicate_denied: counter=app::OperationalCounter::duplicate_denials; break;
                default: break;
            }
            if (counter!=app::OperationalCounter::count) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto& count=reporting_.counts[static_cast<std::size_t>(counter)];
                if (count!=UINT64_MAX) ++count;
            }
            if (suppressed_[kind]!=UINT64_MAX) ++suppressed_[kind];
            const auto& ip=entry.event->context.source_ip;
            if (samples_[kind].empty()) samples_[kind]=ip;
            else if (samples_[kind]!=ip) samples_[kind]="mixed";
            changed_.notify_one(); return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!records_.append({entry.sequence, record})) {
                health_.fetch_or(app::operational_log_event_rejected); return;
            }
            ++delivery_; // successful deliveries are contiguous; rejected input consumes no cursor
            app::observe_operational_reporting(reporting_,entry);
        }
        if (blocked_append_) return;
        if (pending_.size()==512) {
            if(!(health_.fetch_or(app::operational_log_write_failed)&app::operational_log_write_failed))sound(app::SoundEvent::operational_alert);
            if (entry.event->kind!=app::OperationalKind::flood_aggregate && suppressed_[kind]!=UINT64_MAX) ++suppressed_[kind];
            return;
        }
        pending_.push_back(record+"\r\n"); changed_.notify_one();
    } catch (...) { health_.fetch_or(app::operational_log_event_rejected); }
}
AlphaOperationalJournal::~AlphaOperationalJournal() {
    { std::lock_guard<std::mutex> lock(file_mutex_); stopping_=true; }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}
bool AlphaOperationalJournal::configure(const app::LoggingConfiguration& value) noexcept {
    if (!app::valid_logging_configuration(value)) return false;
    try { std::lock_guard<std::mutex> lock(file_mutex_); limits_=value; changed_.notify_one(); return true; }
    catch (...) { return false; }
}
bool AlphaOperationalJournal::flush() noexcept {
    try {
        std::unique_lock<std::mutex> lock(file_mutex_);
        flush_attention_=true;
        changed_.notify_one();
        return flushed_.wait_for(lock,std::chrono::seconds(10),[this] {
            return pending_.empty() && !writing_ && !flush_attention_ &&
                std::all_of(suppressed_.begin(),suppressed_.end(),[](auto n) { return n==0; });
        });
    } catch (...) { return false; }
}
#if defined(__linux__)
bool AlphaOperationalJournal::capture_backup_file(const std::function<bool()>& capture) noexcept {
    try {
        if(!capture||!flush())return false;
        return capture_backup_files(capture);
    }catch(...){return false;}
}
bool AlphaOperationalJournal::capture_backup_files(const std::function<bool()>& capture) {
    if(!capture)return false;
    std::lock_guard<std::mutex> lock(file_io_mutex_);
    return capture();
}
#endif
void AlphaOperationalJournal::writer() noexcept {
    try {
        std::array<std::uint64_t,4> attention_last{},attention_counts{};
        std::array<app::AttentionRecord,4> attention_records{};
#ifdef _WIN32
        platform::win32::WindowsDurableJournalFiles files(path_.parent_path().parent_path(),owner_sid_);
#else
        platform::linux_host::LinuxDurableJournalFiles files(path_.parent_path().parent_path());
#endif
        std::uint64_t last_summary=platform::network::monotonic_millis()/1000;
        for (;;) {
            std::string line;
            app::LoggingConfiguration limits;
            bool ending=false;
            bool force_attention=false;
            std::function<void(app::AttentionRecord)> attention;
            std::array<std::uint64_t,kinds> suppressed{};
            std::array<std::string,kinds> samples;
            {
                std::unique_lock<std::mutex> lock(file_mutex_);
                changed_.wait_for(lock,std::chrono::seconds(1),[this] { return stopping_ || flush_attention_ || !pending_.empty(); });
                ending=stopping_; limits=limits_;
                attention=attention_;
                if (!pending_.empty()) { line=std::move(pending_.front()); pending_.pop_front(); }
                writing_=!line.empty();
                force_attention=flush_attention_ && pending_.empty();
                if (platform::network::monotonic_millis()/1000!=last_summary || ending) {
                    last_summary=platform::network::monotonic_millis()/1000;
                    suppressed=suppressed_; samples=samples_; suppressed_={}; samples_={};
                    writing_=writing_ || std::any_of(suppressed.begin(),suppressed.end(),[](auto n) { return n!=0; });
                }
            }
            if (!line.empty()) {
                if (attention) {
                    auto decoded=app::decode_operational_record(std::string_view(line).substr(0,line.size()-2),1);
                    if (decoded && decoded->event) {
                        const auto& event=*decoded->event;
                        const bool gate=event.kind==app::OperationalKind::botgate_blocked || event.kind==app::OperationalKind::botgate_failed ||
                            event.kind==app::OperationalKind::botgate_timed_out;
                        const bool feed=event.kind==app::OperationalKind::feed_update_failed;
                        if (gate || feed) {
                            const unsigned index=feed ? 1 : 0;
                            if (attention_counts[index]!=UINT64_MAX) ++attention_counts[index];
                            auto& notice=attention_records[index];
                            notice.kind=feed ? app::AttentionKind::botgate_feed_failure : app::AttentionKind::botgate_security;
                            notice.utc_seconds=decoded->utc_seconds;
                            if (attention_counts[index]==1) { notice.source_ip=event.context.source_ip; notice.node=event.context.node; }
                            else if (notice.source_ip!=event.context.source_ip || notice.node!=event.context.node) { notice.source_ip.clear(); notice.node=0; }
                            notice.diagnostic=event.context.diagnostic_code;
                        }
                    }
                }
                bool appended{};
                {
#if defined(__linux__)
                // No queue locks, callbacks or error reporting inside this
                // fence. Capture takes durable publication -> file I/O -> DB.
                std::lock_guard<std::mutex> io(file_io_mutex_);
#endif
                bool okay=files.configure(limits);
                auto file=okay ? files.inspect(app::DurableJournalKind::runtime) : std::nullopt;
                const auto maximum=static_cast<std::uint64_t>(limits.runtime.size_mib)*1024*1024;
                if (file && (file->bytes>maximum || line.size()>maximum-file->bytes)) {
                    okay=files.archive_and_reopen(app::DurableJournalKind::runtime);
                    file=okay ? files.inspect(app::DurableJournalKind::runtime) : std::nullopt;
                }
                appended=file&&files.append(app::DurableJournalKind::runtime,*file,line);
                }
                if (!appended) {
                    if(!(health_.fetch_or(app::operational_log_write_failed)&app::operational_log_write_failed))sound(app::SoundEvent::operational_alert);
                    attention_counts[2]=1;
                    attention_records[2].kind=app::AttentionKind::operational_logging_failure;
                    attention_records[2].utc_seconds=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    // Uncertain partial write cannot be followed by another
                    // record. Preserve all bytes for deliberate recovery.
                    std::lock_guard<std::mutex> lock(file_mutex_);
                    blocked_append_=true; pending_.clear();
                }
            }
            for (std::size_t i=0;i<kinds;++i) if (suppressed[i]) {
                app::OperationalContext context;
                context.resource_id=suppressed[i];
                context.resource_name=std::string(app::operational_kind_label(static_cast<app::OperationalKind>(i)))+
                    (samples[i]=="mixed" ? "_mixed_sources" : "");
                if (samples[i]!="mixed") context.source_ip=samples[i];
                emit({app::OperationalKind::flood_aggregate,{},std::move(context)});
            }
            for (std::size_t i=0;i<attention_counts.size();++i) if (attention && attention_counts[i] &&
                    (ending || force_attention || !attention_last[i] || platform::network::monotonic_millis()-attention_last[i]>=60000)) {
                attention_records[i].occurrences=attention_counts[i];
                attention(attention_records[i]); attention_counts[i]=0; attention_last[i]=platform::network::monotonic_millis();
            }
            {
                std::lock_guard<std::mutex> lock(file_mutex_);
                writing_=false;
                if (force_attention) flush_attention_=false;
                if (pending_.empty()) { flushed_.notify_all(); if (ending) break; }
            }
        }
    } catch (...) {
        if(!(health_.fetch_or(app::operational_log_write_failed)&app::operational_log_write_failed))sound(app::SoundEvent::operational_alert);
        std::lock_guard<std::mutex> lock(file_mutex_);
        blocked_append_=true; pending_.clear(); writing_=false; flushed_.notify_all();
    }
}
void AlphaOperationalJournal::attention_sink(std::function<void(app::AttentionRecord)> sink) {
    std::lock_guard<std::mutex> delivery(clamav_delivery_mutex_);
    {std::lock_guard<std::mutex> lock(file_mutex_);attention_=sink;}
    if(sink)while(!clamav_pending_attention_.empty()){auto record=std::move(clamav_pending_attention_.front());clamav_pending_attention_.pop_front();sink(std::move(record));}
}
void AlphaOperationalJournal::dispatch_clamav_attention(app::AttentionRecord record){
    std::lock_guard<std::mutex> delivery(clamav_delivery_mutex_);std::function<void(app::AttentionRecord)> sink;
    {std::lock_guard<std::mutex> lock(file_mutex_);sink=attention_;}
    if(sink)sink(std::move(record));else if(clamav_pending_attention_.size()<32)clamav_pending_attention_.push_back(std::move(record));else mark_event_failed();
}
void AlphaOperationalJournal::clamav_health(bool scanning,bool healthy,bool usable,std::string diagnostic)noexcept{
    try{
        std::lock_guard<std::mutex> health_lock(clamav_health_mutex_);
        const auto notice=clamav_health_.observe(scanning,healthy,usable,std::move(diagnostic),std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        if(!notice)return;
        dispatch_clamav_attention(*notice);
        app::OperationalContext c;c.resource_id=1;c.diagnostic_code=notice->diagnostic;
        c.resource_name=healthy?(scanning?"ClamAV scanning recovered":"ClamAV definition checking recovered"):(scanning?"ClamAV scan incomplete/unavailable; upload held privately":usable?"ClamAV update failed; validated definitions retained":"ClamAV definitions unavailable/unverified; uploads require a complete scan");
        emit({healthy?app::OperationalKind::file_maintenance:app::OperationalKind::file_maintenance_problem,healthy?app::OperationalOutcome::normal:app::OperationalOutcome::unavailable,std::move(c)});
    }catch(...){mark_event_failed();}
}
void AlphaOperationalJournal::problem(app::AttentionKind kind,std::string resource,std::string diagnostic)noexcept{
    try{
        if(kind!=app::AttentionKind::saved_connection_unavailable&&kind!=app::AttentionKind::work_preserved&&kind!=app::AttentionKind::door_bulletin_unavailable){mark_event_failed();return;}
        app::AttentionRecord notice;notice.kind=kind;notice.resource=std::move(resource);notice.diagnostic=std::move(diagnostic);
        notice.utc_seconds=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if(!app::render_attention_journal(notice,[](std::int64_t){return std::optional<std::string>("2000-01-01 00:00:00");})){mark_event_failed();return;}
        {
            std::lock_guard<std::mutex> guard(problem_mutex_);const auto key=std::to_string(static_cast<unsigned>(kind))+"/"+notice.resource+"/"+notice.diagnostic;
            auto found=problem_windows_.find(key);const auto now=platform::network::monotonic_millis();
            if(found!=problem_windows_.end()&&now-found->second.emitted<60000){++found->second.suppressed;return;}
            if(found==problem_windows_.end()){
                if(problem_windows_.size()>=64){const auto oldest=std::min_element(problem_windows_.begin(),problem_windows_.end(),[](const auto& a,const auto& b){return a.second.emitted<b.second.emitted;});problem_windows_.erase(oldest);}
                found=problem_windows_.emplace(key,ProblemWindow{}).first;
            }
            notice.occurrences=found->second.suppressed+1;found->second={now,0};
        }
        dispatch_clamav_attention(notice);
        app::OperationalContext context;context.resource_id=1;context.diagnostic_code=notice.diagnostic;
        context.resource_name=(kind==app::AttentionKind::work_preserved?"Unconfirmed work preserved: ":kind==app::AttentionKind::door_bulletin_unavailable?"Door Bulletin source unavailable: ":"Saved connection unavailable: ")+notice.resource+(kind==app::AttentionKind::door_bulletin_unavailable?". Review HEYSYSOP.LOG and Doors > Door Bulletins.":". Review HEYSYSOP.LOG and BBS Health Check.");
        emit({app::OperationalKind::file_maintenance_problem,app::OperationalOutcome::unavailable,std::move(context)});
    }catch(...){mark_event_failed();}
}
void AlphaOperationalJournal::clamav_finding(app::AttentionRecord record)noexcept{
    try{
        if(record.kind!=app::AttentionKind::clamav_finding||!app::render_attention_journal(record,[](std::int64_t){return std::optional<std::string>("2000-01-01 00:00:00");})){mark_event_failed();return;}
        sound(app::SoundEvent::security_alert);
        dispatch_clamav_attention(record);
        // Keep the durable notice's exact disposition, with the useful fields
        // bounded for the Events display. Full notice evidence stays durable.
        const auto rendered=*app::render_attention_journal(record,[](std::int64_t){return std::optional<std::string>("2000-01-01 00:00:00");});
        const auto disposition=rendered.rfind(" | Malware detected");
        bool clipped{};const auto field=[&](std::string_view value,std::size_t limit){std::string out;for(const unsigned char c:value){std::string part;if(c>=32&&c<=126&&c!='|'&&c!='\\'&&c!='<'&&c!='>')part+=static_cast<char>(c);else{part="\\x";part+="0123456789ABCDEF"[c>>4];part+="0123456789ABCDEF"[c&15];}if(out.size()+part.size()>limit-3){out+="...";clipped=true;break;}out+=part;}return out;};
        app::OperationalContext c;c.resource_id=1;c.resource_name="ClamAV: Node "+std::to_string(record.node)+" | Caller "+field(record.account_name,60)+" | File "+field(record.filename,140)+" | Signature "+field(record.signatures.front(),140);if(record.signatures.size()>1)c.resource_name+=" | Additional signatures "+std::to_string(record.signatures.size()-1);c.resource_name+=rendered.substr(disposition);while(!c.resource_name.empty()&&(c.resource_name.back()=='\r'||c.resource_name.back()=='\n'))c.resource_name.pop_back();if(clipped)c.resource_name+=" [details in HEYSYSOP]";c.diagnostic_code=record.deletion_confirmed?"clamav_detection_deleted":"clamav_detection_retained";
        emit({app::OperationalKind::file_maintenance_problem,app::OperationalOutcome::none,std::move(c)});
    }catch(...){mark_event_failed();}
}
std::vector<app::AlphaOperationalLogEntry> AlphaOperationalJournal::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.tail(app::kControlPanelMaximumLogEntries);
}
void AlphaOperationalJournal::sound(app::SoundEvent event) noexcept {sounds_->publish(event,platform::network::monotonic_millis());}
app::SoundPollSnapshot AlphaOperationalJournal::sound_poll(const app::SoundPollRequest& r,const app::EventSoundConfiguration& c){return sounds_->poll(r,c,platform::network::monotonic_millis());}
app::OperationalReportingSnapshot AlphaOperationalJournal::reporting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reporting_;
}
std::optional<app::OperationalLogPage> AlphaOperationalJournal::page(app::OperationalLogCursor cursor) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.page(cursor);
}
} // namespace sfx::apps::runtime
