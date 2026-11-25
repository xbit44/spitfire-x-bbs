#include "sfx/platform/win32/durable_journal_files.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include "alpha_durable_journal.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>

namespace {
namespace app = sfx::application;
namespace win = sfx::platform::win32;
namespace fs = std::filesystem;
unsigned checks{};
void check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write_fixture(const fs::path& path, std::string_view text, const std::wstring& sid) {
    check(!fs::exists(path), "test fixture never overwrites an existing path");
    { std::ofstream file(path, std::ios::binary); file.write(text.data(), static_cast<std::streamsize>(text.size()));
      check(file.good(), "write isolated fixture"); }
    win::WindowsStorageDaclPolicy policy(sid);
    check(policy.apply(path, win::StorageDaclObjectKind::file).succeeded(), "protect isolated fixture content");
}
std::vector<fs::path> archives(const fs::path& root, std::string_view base) {
    std::vector<fs::path> result;
    for (const auto& item : fs::directory_iterator(root / "Logs")) {
        const auto name = item.path().filename().string();
        if (name.size() == base.size() + 57 && name.substr(0, base.size() + 1) == std::string(base) + '-') result.push_back(item.path());
    }
    std::sort(result.begin(), result.end()); return result;
}
void run(const fs::path& root) {
    const auto sid = win::current_process_user_sid_string();
    win::WindowsStorageDaclPolicy policy(sid);
    check(fs::create_directories(root / "Logs"), "create unique disposable log directory");
    check(fs::create_directory(root / "Config"), "create disposable config directory");
    check(fs::create_directory(root / "Data"), "create disposable data directory");
    check(policy.apply(root / "Logs", win::StorageDaclObjectKind::directory).succeeded(), "private Logs boundary");
    check(policy.apply(root / "Config", win::StorageDaclObjectKind::directory).succeeded(), "private Config boundary");
    check(policy.apply(root / "Data", win::StorageDaclObjectKind::directory).succeeded(), "private Data boundary");
    check(win::load_new_caller_names(root, sid).valid() && win::load_new_caller_names(root, sid).size() == 0,
        "missing JOKER is an empty usable list");
    write_fixture(root / "Config" / "JOKER.DAT", "; operator comment\r\n  Reserved Caller  \r\n", sid);
    const auto names = win::load_new_caller_names(root, sid);
    check(names.valid() && names.evaluate("reserved caller") == app::NewCallerNameDecision::unavailable &&
        names.evaluate("Reserved Caller Two") == app::NewCallerNameDecision::allowed, "native bounded immutable exact-name snapshot");
    write_fixture(root / "Logs" / "CALLERS-manual.LOG", "unrelated operator file\r\n", sid);
    {
        win::WindowsDurableJournalFiles files(root, sid);
        for (const auto kind : {app::DurableJournalKind::callers, app::DurableJournalKind::attention}) {
            const auto state = files.inspect(kind); check(state.has_value(), "open fixed protected active file");
            check(state->bytes > 0 && !state->identity.empty(), "version header and exact file identity");
            const std::string text = kind == app::DurableJournalKind::callers ? "fixture complete\r\n\r\n---\r\n" : "fixture attention\r\n";
            check(files.append(kind, *state, text), "native serialized append flushes whole record");
            check(files.read(kind, state->bytes, text.size()) == text, "exact appended bytes");
            check(!files.append(kind, *state, text), "stale file offset rejected");
            auto wrong = *files.inspect(kind); wrong.identity = "0:0";
            check(!files.append(kind, wrong, text), "stale file identity rejected");
            check(!files.read(kind, 0, 8193), "native reader is bounded");
        }
        auto reader = win::installation::open_object(root / "Logs" / "CALLERS.LOG", false,
            GENERIC_READ | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE); // deliberately denies delete sharing
        const auto before = read(root / "Logs" / "CALLERS.LOG");
        const auto shared = *files.inspect(app::DurableJournalKind::callers);
        check(files.append(app::DurableJournalKind::callers, shared, "next\r\n\r\n---\r\n"), "ordinary read sharing while appending");
        const auto preserved = read(root / "Logs" / "CALLERS.LOG");
        check(!files.archive_and_reopen(app::DurableJournalKind::callers) &&
            read(root / "Logs" / "CALLERS.LOG") == preserved, "reader-denied rotation preserves complete active file");
        reader.reset();
        check(files.archive_and_reopen(app::DurableJournalKind::callers), "rotation retry after reader releases");
        check(read(archives(root, "CALLERS").front()) == preserved, "timestamped archive exact bytes");
        for (unsigned n = 0; n < 11; ++n) {
            auto state = *files.inspect(app::DurableJournalKind::callers);
            check(files.append(app::DurableJournalKind::callers, state, "record " + std::to_string(n) + "\r\n\r\n---\r\n"), "complete rotation fixture block");
            check(files.archive_and_reopen(app::DurableJournalKind::callers), "native between-record rotation and prune");
        }
        check(archives(root, "CALLERS").size() == 10, "native newest-ten retention bound");
        const auto oldest = archives(root, "CALLERS").front();
        auto blocked_archive = win::installation::open_object(oldest, false, GENERIC_READ | READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE); // reader prevents pruning, never forced closed
        const auto oldest_bytes = read(oldest);
        const auto next_state = *files.inspect(app::DurableJournalKind::callers);
        check(files.append(app::DurableJournalKind::callers, next_state, "prune failure fixture\r\n\r\n---\r\n"), "record before blocked prune");
        check(!files.archive_and_reopen(app::DurableJournalKind::callers), "blocked oldest archive reports pruning failure");
        check(archives(root, "CALLERS").size() == 11 && read(oldest) == oldest_bytes, "failed prune preserves all existing history");
        check(!files.inspect(app::DurableJournalKind::callers) && !files.archive_and_reopen(app::DurableJournalKind::callers) &&
            archives(root, "CALLERS").size() == 11, "failed prune prevents further archive accumulation");
        blocked_archive.reset();
        check(files.inspect(app::DurableJournalKind::callers).has_value() && archives(root, "CALLERS").size() == 10,
            "reader release permits bounded prune retry and append recovery");
        check(read(root / "Logs" / "CALLERS-manual.LOG") == "unrelated operator file\r\n", "unrelated archive-like file preserved");
        auto state = *files.inspect(app::DurableJournalKind::attention);
        check(files.append(app::DurableJournalKind::attention, state, "partial"), "simulate uncertain partial write in fixture");
        const auto partial = read(root / "Logs" / "HEYSYSOP.LOG");
        check(!files.archive_and_reopen(app::DurableJournalKind::attention) &&
            read(root / "Logs" / "HEYSYSOP.LOG") == partial, "incomplete record cannot be rotated or truncated");
    }
    // Parsed snapshots remain immutable; the Runtime policy must read a new
    // snapshot at every check, without holding a long-lived leaf handle.
    win::WindowsLocalStoragePathPolicy paths(sid);
    sfx::adapters::storage::SqliteStorageSettings settings; settings.data_directory = root / "Data";
    sfx::adapters::storage::SqliteStorage store(settings, paths);
    win::WindowsSecureIdentifierGenerator ids;
    auto operational = std::make_shared<sfx::apps::runtime::AlphaOperationalJournal>(root / "Logs/SFXRuntime.log", sid, ids.generate());
    sfx::apps::runtime::AlphaDurableJournal live(store, ids, root, sid, "Pacific Standard Time", operational);
    check(store.start().succeeded(), "start native journal storage for idle publication check");
    check(live.start(), "start Runtime durable journal publication worker");
    Sleep(500);
    const auto idle_before = store.pager_counters();
    Sleep(400);
    const auto idle_after = store.pager_counters();
    check(idle_before.available && idle_after.available &&
            idle_after.cache_hits == idle_before.cache_hits &&
            idle_after.cache_misses == idle_before.cache_misses,
        "idle durable journal worker performs no repeated SQLite publication probes");
    live.wake();
    Sleep(500);
    const auto wake_settled = store.pager_counters();
    Sleep(400);
    const auto wake_idle = store.pager_counters();
    check(wake_idle.cache_hits == wake_settled.cache_hits &&
            wake_idle.cache_misses == wake_settled.cache_misses,
        "empty caller wake performs one bounded probe then returns to event-driven idle");
    live.stop();
    check(store.shutdown().succeeded(), "stop native journal storage after idle publication check");
    check(live.new_names().evaluate("reserved caller") == app::NewCallerNameDecision::unavailable,
        "Runtime uses current protected exact-name list");
    {
        std::ofstream file(root / "Config" / "JOKER.DAT", std::ios::binary | std::ios::trunc); file << "*invalid*\r\n";
    }
    check(names.evaluate("reserved caller") == app::NewCallerNameDecision::unavailable, "loaded snapshot remains unchanged");
    check(live.new_names().evaluate("reserved caller") == app::NewCallerNameDecision::allowed,
        "ordinary saved removal affects next Runtime check, not a cached startup snapshot");
    // A control byte is unambiguously invalid under the production name grammar.
    { std::ofstream file(root / "Config" / "JOKER.DAT", std::ios::binary | std::ios::trunc); file << "bad\x1bname\r\n"; }
    check(!win::load_new_caller_names(root, sid).valid(), "present-invalid native file fails closed");
    check(live.new_names().evaluate("reserved caller") == app::NewCallerNameDecision::temporarily_unavailable,
        "live malformed file never falls back to previous allowed contents");
    auto exclusive = win::installation::open_object(root / "Config" / "JOKER.DAT", false, GENERIC_READ, 0);
    check(!win::load_new_caller_names(root, sid).valid(), "present-unreadable native file fails closed");
    exclusive.reset();
    const auto replacement = root / "Config" / "JOKER-replacement.tmp";
    write_fixture(replacement, "Reserved Caller\r\n", sid);
    check(MoveFileExW(replacement.c_str(), (root / "Config/JOKER.DAT").c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE, "ordinary atomic replacement save succeeds after prior check returns");
    check(live.new_names().evaluate("reserved caller") == app::NewCallerNameDecision::unavailable,
        "corrected replacement file immediately restores current policy after invalid input");
    {
        std::ofstream file(root / "Config/JOKER.DAT", std::ios::binary | std::ios::trunc);
        file << std::string(app::NewCallerNames::maximum_bytes + 1, ' ');
    }
    check(live.new_names().evaluate("reserved caller") == app::NewCallerNameDecision::temporarily_unavailable,
        "live oversize file fails closed instead of retaining last good policy");
    { std::ofstream file(root / "Config/JOKER.DAT", std::ios::binary | std::ios::trunc); }
    check(live.new_names().evaluate("reserved caller") == app::NewCallerNameDecision::allowed,
        "saved empty correction takes effect without Runtime restart");
    check(fs::exists(root / "Logs" / "CALLERS.LOG") && fs::exists(root / "Logs" / "HEYSYSOP.LOG"),
        "active filenames preserved; fixture retained for inspection");
}
}
int main() {
    try {
        const auto base = fs::current_path();
        check(base.parent_path().filename() == "build" && !base.filename().empty(),
            "native fixture limited to one repository disposable build area");
        const auto root = base / "native-journal-fixtures" / win::installation::unique_id();
        run(root);
        std::cout << "PASS native durable journal: " << checks << " checks; 0 failed; 0 skipped\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL after " << checks << ": " << error.what() << '\n'; return 1; }
}
