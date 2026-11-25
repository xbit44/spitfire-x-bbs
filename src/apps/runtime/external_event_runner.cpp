#include "external_event_runner.hpp"
#include "alpha_durable_journal.hpp"
#include "alpha_operational_journal.hpp"
#include "sfx/application/external_event_files.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include "sfx/platform/win32/bbs_time_zone_provider.hpp"
#include <windows.h>
#else
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace sfx::apps::runtime {
namespace {
namespace app = application;
using Event = app::ExternalEvent;
using Kind = app::OperationalKind;
#ifdef _WIN32
using Zone = platform::win32::WindowsRegistryBbsTimeZoneRuleProvider;
#else
using Zone = platform::linux_host::LinuxBbsTimeZoneRuleProvider;
#endif

#ifndef _WIN32
std::optional<std::vector<std::string>> event_arguments(std::string_view text) {
    std::vector<std::string> values;
    std::string item;
    char quote{};
    bool started{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && quote != '\'') {
            if (++i == text.size()) return {};
            item += text[i];
        } else if ((c == '"' || c == '\'') && (!quote || quote == c)) {
            quote = quote ? 0 : c;
        } else if (c == ' ' && !quote) {
            if (started) values.push_back(std::move(item));
            item.clear();
            started = false;
            continue;
        } else {
            item += c;
        }
        started = true;
    }
    if (quote) return {};
    if (started) values.push_back(std::move(item));
    return values;
}
#endif

class EventProcess final {
public:
    ~EventProcess() { cancel(); }
    bool start(const Event& event, [[maybe_unused]] bool desktop_console) {
        const auto program = std::filesystem::u8path(event.program);
        const auto directory = std::filesystem::u8path(event.directory);
        if (!program.is_absolute() || !directory.is_absolute() || !std::filesystem::is_regular_file(program) ||
            !std::filesystem::is_directory(directory)) return false;
#ifdef _WIN32
        auto image = program.native();
        if (image.find(L'"') != image.npos) return false;
        auto extension = program.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](wchar_t ch) { return ch >= L'A' && ch <= L'Z' ? ch + L'a' - L'A' : ch; });
        auto command = L"\"" + image + L"\"";
        if (!event.arguments.empty()) command += L" " + std::filesystem::u8path(event.arguments).wstring();
        if (extension == L".bat" || extension == L".cmd") {
            wchar_t system[MAX_PATH]{};
            const auto length = GetSystemDirectoryW(system, MAX_PATH);
            if (!length || length >= MAX_PATH || image.find(L'%') != image.npos) return false;
            image = std::wstring(system, length) + L"\\cmd.exe";
            command = L"\"" + image + L"\" /d /v:off /s /c \"" + command + L"\"";
        }
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION policy{};
        policy.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &policy, sizeof(policy)))
            return false;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        std::wstring title;
        if (desktop_console) {
            title = L"SFX Event - " + std::filesystem::u8path(event.name).wstring();
            startup.lpTitle = title.data();
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_SHOWMINNOACTIVE;
        }
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_SUSPENDED | (desktop_console ? CREATE_NEW_CONSOLE : CREATE_NO_WINDOW),
                nullptr, directory.c_str(), &startup, &process)) return false;
        process_ = process.hProcess;
        const bool started = AssignProcessToJobObject(job_, process_) && ResumeThread(process.hThread) != DWORD(-1);
        CloseHandle(process.hThread);
        if (!started) cancel();
        return started;
#else
        auto parsed = event_arguments(event.arguments);
        if (!parsed) return false;
        parsed->insert(parsed->begin(), event.program);
        std::vector<char*> argv;
        for (auto& arg : *parsed) argv.push_back(arg.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) return false;
        posix_spawnattr_t attr;
        const bool attributes = posix_spawnattr_init(&attr) == 0;
        const bool prepared = attributes && !posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP) &&
            !posix_spawnattr_setpgroup(&attr, 0) && !posix_spawn_file_actions_addchdir_np(&actions, directory.c_str()) &&
            !posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) &&
            !posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) &&
            !posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        pid_t pid{};
        const bool started = prepared && !posix_spawn(&pid, program.c_str(), &actions, &attr, argv.data(), environ);
        if (attributes) posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);
        if (started) pid_ = pid;
        return started;
#endif
    }
    std::optional<std::int64_t> poll() {
#ifdef _WIN32
        const auto result = WaitForSingleObject(process_, 0);
        if (result == WAIT_TIMEOUT) return {};
        DWORD code{};
        if (result != WAIT_OBJECT_0 || !GetExitCodeProcess(process_, &code)) return -1;
        CloseHandle(process_);
        process_ = nullptr;
        return code;
#else
        int status{};
        const auto result = waitpid(pid_, &status, WNOHANG);
        if (!result || (result < 0 && errno == EINTR)) return {};
        pid_ = 0;
        if (result < 0) return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
    }
private:
    void cancel() noexcept {
#ifdef _WIN32
        if (job_) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        if (process_) {
            TerminateProcess(process_, ERROR_CANCELLED);
            WaitForSingleObject(process_, INFINITE);
            CloseHandle(process_);
            process_ = nullptr;
        }
#else
        if (pid_ > 0) {
            kill(-pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
            pid_ = 0;
        }
#endif
    }
#ifdef _WIN32
    HANDLE process_{}, job_{};
#else
    pid_t pid_{};
#endif
};

std::string progress_document(const std::map<std::uint64_t, std::int64_t>& consumed) {
    std::string result = "SFX-EXTERNAL-EVENT-PROGRESS-1\n";
    for (const auto& [id, utc] : consumed) result += std::to_string(id) + " " + std::to_string(utc) + "\n";
    return result;
}
} // namespace

struct ExternalEventRunner::Impl {
    struct Schedule {
        Event event;
        std::optional<std::int64_t> next;
    };
    struct Work {
        Event event;
        std::uint64_t since{};
        bool cutoff_requested{}, warned{};
        std::unique_ptr<EventProcess> process;
    };
    std::filesystem::path root;
    std::wstring owner;
    std::string zone_name;
    const app::IClock& clock;
    ExternalEventCallerGate gate;
    std::shared_ptr<AlphaOperationalJournal> journal;
    std::shared_ptr<AlphaDurableJournal> durable;
    const bool desktop_console;
    std::map<std::uint64_t, std::int64_t> consumed;
    std::map<std::uint64_t, Schedule> schedules;
    std::map<std::uint64_t, Work> work;
    std::uint64_t revision{};
    bool reported_configuration_error{};
    std::atomic<bool> stopping{false}, active{false};
    std::mutex wait_mutex;
    mutable std::mutex activity_mutex;
    std::string activity_text;
    std::condition_variable wake;
    std::thread worker;

    Impl(std::filesystem::path installation, std::wstring installation_owner, std::string zone,
        const app::IClock& event_clock, ExternalEventCallerGate caller_gate,
        std::shared_ptr<AlphaOperationalJournal> operational, std::shared_ptr<AlphaDurableJournal> attention,
        bool desktop)
        : root(std::move(installation)), owner(std::move(installation_owner)), zone_name(std::move(zone)),
          clock(event_clock), gate(std::move(caller_gate)), journal(std::move(operational)), durable(std::move(attention)),
          desktop_console(desktop) {}

    void report_problem(std::string name, std::string message, std::string diagnostic) {
        Event event;
        event.name = std::move(name);
        log(event, Kind::external_event_problem, std::move(message), std::move(diagnostic));
    }

    void log(const Event& event, Kind kind, std::string message, std::string diagnostic = {}) {
        app::OperationalContext context;
        context.resource_id = event.id;
        context.resource_name = event.name + ": " + std::move(message);
        context.diagnostic_code = diagnostic;
        journal->emit({kind, app::OperationalOutcome::none, std::move(context)});
        if (kind == Kind::external_event_problem) {
            app::AttentionRecord record;
            record.kind = app::AttentionKind::configured_event;
            record.utc_seconds = clock.unix_seconds();
            record.resource = event.name;
            record.diagnostic = std::move(diagnostic);
            durable->attention(std::move(record));
        }
    }
    bool load_progress() {
        const auto text = app::read_external_event_file(root, owner, app::ExternalEventFile::schedule_progress);
        if (!text) return false;
        if (text->empty()) return true;
        std::istringstream in(*text);
        std::string marker;
        if (!(in >> marker) || marker != "SFX-EXTERNAL-EVENT-PROGRESS-1") return false;
        std::uint64_t id{};
        std::int64_t utc{};
        while (in >> id >> utc) {
            if (!id || !app::valid_utc_seconds(utc) || utc <= 0 || !consumed.emplace(id, utc).second) return false;
        }
        return progress_document(consumed) == *text;
    }
    bool claim(const Event& event, std::int64_t utc) {
        if (!utc) return true;
        consumed[event.id] = utc;
        if (app::publish_external_event_file(root, owner, app::ExternalEventFile::schedule_progress, progress_document(consumed)))
            return true;
        log(event, Kind::external_event_problem, "skipped because schedule progress could not be recorded", "progress_not_saved");
        return false;
    }
    void enqueue(const Event& event, bool cutoff) {
        if (work.count(event.id)) {
            log(event, Kind::external_event_problem, "skipped because its previous run is active", "previous_run_active");
        } else {
            work.emplace(event.id, Work{event, clock.monotonic_millis(), !cutoff, false, {}});
        }
    }
    void refresh(const app::BbsLocalTimeAdapters& zone, std::int64_t now, bool initialize) {
        const auto bytes = app::read_external_event_file(root, owner, app::ExternalEventFile::catalog);
        const auto catalog = !bytes ? std::nullopt : bytes->empty() ?
            std::optional<app::ExternalEventCatalog>{app::ExternalEventCatalog{}} : app::parse_external_events(*bytes);
        if (!catalog) {
            schedules.clear();
            revision = 0;
            if (!reported_configuration_error) {
                report_problem("Configuration", "could not read the protected event list", "event_list_unavailable");
                reported_configuration_error = true;
            }
            return;
        }
        reported_configuration_error = false;
        if (catalog->revision == revision) return;
        std::map<std::uint64_t, Schedule> replacement;
        for (const auto& event : catalog->events) {
            if (!event.enabled) continue;
            const auto old = schedules.find(event.id);
            auto previous = consumed.find(event.id);
            const auto last = previous == consumed.end() ? 0 : previous->second;
            if (old != schedules.end() && app::same_event_schedule(old->second.event, event) &&
                old->second.event.effective_utc == event.effective_utc) {
                replacement.emplace(event.id, Schedule{event, old->second.next});
            } else {
                replacement.emplace(event.id, Schedule{event, app::external_event_slot(event, zone, std::max(now, last))});
            }
            if (!initialize) continue;
            const auto missed = event.run_missed ? app::external_event_slot(event, zone, now, true) : std::nullopt;
            if (missed && *missed > last) {
                if (claim(event, *missed)) enqueue(event, false);
            } else if (event.at_start) {
                enqueue(event, false);
            }
        }
        schedules = std::move(replacement);
        revision = catalog->revision;
    }
    void publish_gate() {
        std::int64_t cutoff{};
        for (const auto& [id, schedule] : schedules) {
            if (schedule.event.exclusive && schedule.event.end_calls && schedule.next &&
                (!cutoff || *schedule.next < cutoff)) cutoff = *schedule.next;
        }
        const auto held = std::any_of(work.begin(), work.end(),
            [](const auto& entry) { return entry.second.event.exclusive; });
        gate.update(held, cutoff);
        active.store(!work.empty());
        const auto now = clock.monotonic_millis();
        std::string activity;
        for (const auto& [id, run] : work) {
            const auto seconds = (now - run.since) / 1000;
            const auto line = run.event.name + (run.process ? "  Running  " : "  Waiting for callers and Doors  ") +
                std::to_string(seconds / 60) + " min " + std::to_string(seconds % 60) + " sec\r\n";
            if (activity.size() + line.size() > 2000) { activity += "More events are active.\r\n"; break; }
            activity += line;
        }
        std::lock_guard<std::mutex> lock(activity_mutex);
        activity_text = std::move(activity);
    }
    void tick(const app::BbsLocalTimeAdapters& zone, std::int64_t now) {
        for (auto& [id, schedule] : schedules) {
            if (!schedule.next || *schedule.next > now) continue;
            const auto due = app::external_event_slot(schedule.event, zone, now, true).value_or(*schedule.next);
            schedule.next = app::external_event_slot(schedule.event, zone, now);
            if (claim(schedule.event, due)) enqueue(schedule.event, schedule.event.end_calls);
        }
        publish_gate();
        const auto mono = clock.monotonic_millis();
        for (auto next = work.begin(); next != work.end();) {
            auto& run = next->second;
            const auto& event = run.event;
            bool done{};
            const auto elapsed = (mono - run.since) / 60000;
            if (run.process) {
                if (const auto code = run.process->poll()) {
                    log(event, *code ? Kind::external_event_problem : Kind::external_event_finished,
                        "finished with exit code " + std::to_string(*code), *code ? "command_failed" : "");
                    done = true;
                } else if (!run.warned && event.warn_minutes && elapsed >= event.warn_minutes) {
                    log(event, Kind::external_event_problem, "still running after " +
                        std::to_string(event.warn_minutes) + " minutes", "long_run");
                    run.warned = true;
                }
            } else {
                if (!run.cutoff_requested) {
                    gate.disconnect();
                    run.cutoff_requested = true;
                }
                if (!event.exclusive || gate.empty()) {
                    auto process = std::make_unique<EventProcess>();
                    bool started{};
                    try { started = process->start(event, desktop_console); } catch (...) {}
                    if (started) {
                        run.process = std::move(process);
                        run.since = mono;
                        log(event, Kind::external_event_started, "started");
                    } else {
                        log(event, Kind::external_event_problem, "could not start the configured command", "command_start_failed");
                        done = true;
                    }
                } else if (elapsed >= event.wait_minutes) {
                    log(event, Kind::external_event_problem, "skipped because callers or Doors remained active", "caller_wait_expired");
                    done = true;
                }
            }
            if (done) next = work.erase(next); else ++next;
        }
        publish_gate();
    }
    void run(bool board_start) noexcept {
        try {
            auto rules = Zone::create(zone_name);
            if (!rules || !load_progress()) {
                report_problem("Scheduler", "schedule state is unavailable", "schedule_state_unavailable");
            } else {
                app::BbsLocalTimeAdapters zone(rules->configuration(), *rules, clock);
                refresh(zone, clock.unix_seconds(), board_start);
                auto refreshed = clock.monotonic_millis();
                while (!stopping.load()) {
                    const auto now = clock.unix_seconds();
                    if (clock.monotonic_millis() - refreshed >= 1000) {
                        refresh(zone, now, false);
                        refreshed = clock.monotonic_millis();
                    }
                    tick(zone, now);
                    std::unique_lock<std::mutex> lock(wait_mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(200), [&] { return stopping.load(); });
                }
            }
        } catch (...) {
            try { report_problem("Scheduler", "scheduler stopped", "scheduler_failed"); }
            catch (...) {}
        }
        for (const auto& [id, run] : work) {
            try { log(run.event, Kind::external_event_problem, "interrupted by Runtime shutdown", "runtime_stopped"); }
            catch (...) {}
        }
        work.clear();
        gate.update(false, 0);
        active.store(false);
        std::lock_guard<std::mutex> lock(activity_mutex);
        activity_text.clear();
    }
};

ExternalEventRunner::ExternalEventRunner(std::filesystem::path root, std::wstring owner, std::string zone,
    const app::IClock& clock, ExternalEventCallerGate gate, std::shared_ptr<AlphaOperationalJournal> journal,
    std::shared_ptr<AlphaDurableJournal> durable, bool desktop_console)
    : impl_(std::make_unique<Impl>(std::move(root), std::move(owner), std::move(zone), clock, std::move(gate),
        std::move(journal), std::move(durable), desktop_console)) {}
ExternalEventRunner::~ExternalEventRunner() { stop(); }
bool ExternalEventRunner::start(bool board_start) {
    try {
        impl_->worker = std::thread([this, board_start] { impl_->run(board_start); });
        return true;
    } catch (...) {
        impl_->gate.update(false, 0);
        return false;
    }
}
void ExternalEventRunner::stop() noexcept {
    impl_->stopping.store(true);
    impl_->wake.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}
bool ExternalEventRunner::busy() const noexcept { return impl_->active.load(); }
std::string ExternalEventRunner::activity() const {
    std::lock_guard<std::mutex> lock(impl_->activity_mutex);
    return impl_->activity_text;
}
} // namespace sfx::apps::runtime
