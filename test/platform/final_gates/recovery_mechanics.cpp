// ORDINARY test-only crash/relaunch mechanics. No UAC, HKLM, ACL change or
// installation authority. Only fixed, newly-created checkout build fixtures.
// Dummy Bin bytes are constants, never programs and never loaded/executed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs = std::filesystem;
namespace {
unsigned checks = 0, children_started = 0, children_reaped = 0;
void need(bool b, const char* label) {
    ++checks;
    if (!b) throw std::runtime_error(std::string(label) + " win32=" + std::to_string(GetLastError()));
}
struct Close { void operator()(void* p) const { if (p && p != INVALID_HANDLE_VALUE) CloseHandle(p); } };
using Handle = std::unique_ptr<void, Close>;
constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD no_reparse = FILE_FLAG_OPEN_REPARSE_POINT;
constexpr unsigned max_cases = 256;
constexpr int prior_state = 21, new_state = 22, source_state = 23, refusal_state = 24;
using Inventory = std::map<std::string, std::string>;
const std::array<std::string, 4> flows{"fresh", "update", "rollback", "import"};
const std::array<std::string, 4> modes{"terminate", "cancel", "timeout", "io-failure"};

void ordinary() {
    HANDLE raw = nullptr;
    need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "query own token");
    Handle h(raw); TOKEN_ELEVATION e{}; DWORD n = 0;
    need(GetTokenInformation(h.get(), TokenElevation, &e, sizeof e, &n) && !e.TokenIsElevated,
        "ordinary execution required; no privileged mode exists");
}
void inspect(HANDLE h, bool dir) {
    BY_HANDLE_FILE_INFORMATION i{};
    need(h != INVALID_HANDLE_VALUE && h && GetFileInformationByHandle(h, &i) &&
        !(i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == dir &&
        (dir || i.nNumberOfLinks == 1), "plain no-reparse directory or single-link fixture file");
}
void ancestry(const fs::path& p) {
    std::vector<fs::path> chain;
    for (auto q = p;; q = q.parent_path()) { chain.push_back(q); if (q == q.parent_path()) break; }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        Handle h(CreateFileW(it->c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING,
            no_reparse | FILE_FLAG_BACKUP_SEMANTICS, nullptr)); inspect(h.get(), true);
    }
    // Controlled checkout assumption only. This is NOT elevated ancestor proof.
}
void directory(const fs::path& p) {
    ancestry(p.parent_path());
    need(CreateDirectoryW(p.c_str(), nullptr) != FALSE, "new fixture directory; no adoption");
}
void parents(const fs::path& base, const fs::path& relative) {
    auto p = base;
    for (const auto& part : relative.parent_path()) {
        need(part != L".." && part != L"." && !part.empty(), "fixed relative component");
        p /= part;
        if (!fs::exists(p)) directory(p);
        ancestry(p);
    }
}
std::string read(const fs::path& p, DWORD permitted_sharing = FILE_SHARE_READ, DWORD maximum = 1024 * 1024) {
    ancestry(p.parent_path());
    Handle h(CreateFileW(p.c_str(), GENERIC_READ, permitted_sharing, nullptr, OPEN_EXISTING, no_reparse, nullptr));
    inspect(h.get(), false); LARGE_INTEGER size{};
    need(GetFileSizeEx(h.get(), &size) && size.QuadPart >= 0 && size.QuadPart <= maximum, "bounded fixture file");
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0'); DWORD got = 0;
    need(ReadFile(h.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) && got == bytes.size(), "complete bounded file read");
    return bytes;
}
void write_new(const fs::path& p, const std::string& bytes) {
    ancestry(p.parent_path());
    need(bytes.size() <= 1024 * 1024, "bounded fixture output");
    Handle h(CreateFileW(p.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr, CREATE_NEW, no_reparse, nullptr));
    inspect(h.get(), false); DWORD done = 0;
    need(WriteFile(h.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr) &&
        done == bytes.size() && FlushFileBuffers(h.get()), "new file written and flushed through creating handle");
}
void replace_from(const fs::path& from, const fs::path& to) {
    ancestry(from.parent_path()); ancestry(to.parent_path());
    (void)read(from); if (fs::exists(to)) (void)read(to);
    const BOOL moved = MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    const DWORD error = GetLastError();
    if (!moved) std::cerr << "PUBLICATION REFUSED from=" << from.u8string() << " to=" << to.u8string() << " error=" << error << '\n';
    SetLastError(error); need(moved != FALSE, "ordinary same-volume fixture publication");
}
std::string hash(const std::string& bytes) {
    HCRYPTPROV provider = 0; HCRYPTHASH digest = 0;
    need(CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE, "ephemeral SHA256 provider");
    if (!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &digest)) { CryptReleaseContext(provider, 0); need(false, "SHA256 create"); }
    const auto ok = CryptHashData(digest, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0);
    std::array<BYTE, 32> value{}; DWORD n = static_cast<DWORD>(value.size());
    const auto got = CryptGetHashParam(digest, HP_HASHVAL, value.data(), &n, 0);
    CryptDestroyHash(digest); CryptReleaseContext(provider, 0);
    need(ok && got && n == value.size(), "SHA256 complete");
    std::string result; constexpr char digits[] = "0123456789ABCDEF";
    for (BYTE b : value) { result += digits[b >> 4]; result += digits[b & 15]; }
    return result;
}
Inventory inventory(const fs::path& root) {
    Inventory out; ancestry(root);
    // Explicit traversal checks directories before descent (no recursive iterator
    // following unchecked source boundaries). Fixed ordinary test data only.
    std::vector<fs::path> pending{root};
    while (!pending.empty()) {
        const auto p = pending.back(); pending.pop_back(); ancestry(p);
        for (const auto& e : fs::directory_iterator(p)) {
            const DWORD a = GetFileAttributesW(e.path().c_str());
            need(a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_REPARSE_POINT), "inventory rejects reparse entries");
            const auto key = e.path().lexically_relative(root).generic_string();
            need(out.size() < 128 && key.size() < 200, "inventory entry and relative-name bounds");
            if (a & FILE_ATTRIBUTE_DIRECTORY) { out.emplace(key + "/", "directory"); pending.push_back(e.path()); }
            else { const auto bytes = read(e.path()); out.emplace(key, std::to_string(bytes.size()) + ":" + hash(bytes)); }
        }
    }
    return out;
}
std::string rendered(const Inventory& inv) {
    std::string s;
    for (const auto& e : inv) s += e.first + "\t" + e.second + "\n";
    return s;
}
struct Database {
    sqlite3* db = nullptr;
    explicit Database(const fs::path& p) {
        ancestry(p.parent_path());
        need(sqlite3_open_v2(p.u8string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK, "private dummy SQLite open");
    }
    ~Database() { if (db) sqlite3_close(db); }
    void sql(const char* s) { need(sqlite3_exec(db, s, nullptr, nullptr, nullptr) == SQLITE_OK, "fixed fixture SQLite operation"); }
    int scalar(const char* s) {
        sqlite3_stmt* stmt = nullptr; need(sqlite3_prepare_v2(db, s, -1, &stmt, nullptr) == SQLITE_OK, "fixed query prepare");
        const int status = sqlite3_step(stmt); const int value = status == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
        sqlite3_finalize(stmt); need(status == SQLITE_ROW, "fixed query returns row"); return value;
    }
};
const Inventory dummy_data{
    {"Config/SFX.conf", "SYNTHETIC ONLY\r\nformat=4\r\nroot=.\r\nFiles=Files\r\nDoors=Doors\r\n"},
    {"Config/control-panel.fixture", "grayscale=0\nnode-scale=125\n"},
    {"Config/ssh-host-key.fixture", "NONSECRET NONKEY ED25519-SHAPED FIXTURE - NO SSH AUTHORITY\n"},
    {"Displays/MAIN.ASC", "SYNTHETIC MENU ART\r\n"},
    {"Displays/MAIN.ANS", "\x1b[0mSYNTHETIC ANSI ART\r\n"},
    {"Displays/Sysop-unknown.notes", "Unknown owner material must remain exact.\n"},
    {"Files/example.txt", "Dummy downloadable file.\n"},
    {"Files/catalog.fixture", "relative=Files/example.txt\n"},
    {"Doors/Demo/door.exe.fixture", "NOT AN EXECUTABLE; NEVER EXECUTE COPIED DOOR CONTENT\n"},
    {"Doors/Demo/door.conf", "support=Doors/Demo/support.dat\n"},
    {"Doors/Demo/support.dat", "DUMMY DOOR SUPPORT\n"},
    {"Logs/SFXRuntime.log", "SYNTHETIC NONSECRET HISTORICAL EVENT\n"},
    {"Backups/owner-backup.fixture", "SYNTHETIC BACKUP RETAINED\n"},
    {"Temp/disposable.tmp", "Preserve this fixture even though disposable.\n"},
    {"Work/disposable.tmp", "Do not infer authority to delete unknown material.\n"},
    {"unknown-root.txt", "UNKNOWN ROOT MATERIAL - retained in preserved source\n"},
    {"Bin/old-program.exe.fixture", "UNTRUSTED SOURCE BIN - never import to new Bin or execute\n"}
};
Inventory import_data(const fs::path& source) {
    Inventory result;
    for (const auto& e : dummy_data) {
        if (e.first.rfind("Bin/", 0) == 0 || e.first == "unknown-root.txt") continue;
        result.emplace(e.first, read(source / e.first));
    }
    for (const char* n : {"Data/SFX.DB", "Data/SFX.DB-wal", "Data/SFX.DB-shm"}) result.emplace(n, read(source / n));
    return result;
}
void create_source(const fs::path& root) {
    directory(root / "source");
    for (const auto& e : dummy_data) { parents(root / "source", e.first); write_new(root / "source" / e.first, e.second); }
    directory(root / "seed"); parents(root / "source", "Data/SFX.DB");
    {
        Database db(root / "seed/SFX.DB");
        db.sql("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; CREATE TABLE fixture(id INTEGER PRIMARY KEY, value TEXT NOT NULL); INSERT INTO fixture VALUES(1,'nonsecret'); BEGIN; INSERT INTO fixture VALUES(2,'rolled back'); ROLLBACK;");
        need(db.scalar("SELECT count(*) FROM fixture") == 1, "seed transaction rolled back");
        // Quiescent synthetic writer is kept open while all three supported
        // sidecars are copied. No live SFX DB and no concurrent writer exists.
        for (const char* n : {"SFX.DB", "SFX.DB-wal", "SFX.DB-shm"}) write_new(root / "source/Data" / n, read(root / "seed" / n, sharing));
    }
    write_new(root / "source-inventory.txt", rendered(inventory(root / "source")));
}
std::string program(unsigned generation, unsigned role) {
    return "NONEXECUTABLE COMPILED FIXTURE PROGRAM generation=" + std::to_string(generation) + " role=" + std::to_string(role) + "\n";
}
std::string receipt(unsigned generation) {
    return "PUBLIC TEST MARKER; NOT HKLM, NOT PROVENANCE, NO EXECUTION AUTHORITY\ngeneration=" + std::to_string(generation) + "\n";
}
void seed_candidate(const fs::path& p, unsigned flow, const Inventory& data) {
    directory(p); directory(p / "target"); directory(p / "target/Bin"); directory(p / "stage");
    write_new(p / "flow", flows.at(flow));
    if (flow == 1 || flow == 2) {
        const unsigned gen = flow == 1 ? 1 : 2;
        write_new(p / "target/Bin/cp.fixture", program(gen, 0));
        write_new(p / "target/Bin/rt.fixture", program(gen, 1));
        for (const auto& e : data) { parents(p / "target", e.first); write_new(p / "target" / e.first, e.second); }
        write_new(p / "target/committed.fixture", receipt(gen));
    }
}
bool exact_file(const fs::path& p, const std::string& bytes) { return fs::exists(p) && read(p) == bytes; }
bool exact_pair(const fs::path& target, unsigned gen) {
    unsigned n = 0;
    for (const auto& e : fs::directory_iterator(target / "Bin")) {
        const auto name = e.path().filename().string();
        if (name != "cp.fixture" && name != "rt.fixture") return false;
        ++n;
    }
    return n == 2 && exact_file(target / "Bin/cp.fixture", program(gen, 0)) && exact_file(target / "Bin/rt.fixture", program(gen, 1));
}
int classify(const fs::path& p, unsigned flow, const Inventory& data) {
    const auto target = p / "target";
    bool complete = true;
    for (const auto& e : data) if (!exact_file(target / e.first, e.second)) complete = false;
    const unsigned next = flow == 2 ? 1 : 2;
    if (complete && exact_pair(target, next) && exact_file(target / "committed.fixture", receipt(next))) return new_state;
    const unsigned prior = flow == 2 ? 2 : 1;
    if ((flow == 1 || flow == 2) && complete && exact_pair(target, prior) && exact_file(target / "committed.fixture", receipt(prior))) return prior_state;
    return flow == 3 ? source_state : refusal_state;
}
std::wstring event_name(const std::string& nonce, unsigned c, const wchar_t* suffix) {
    return L"Local\\SFX-Final-Ordinary-" + std::wstring(nonce.begin(), nonce.end()) + L"-" + std::to_wstring(c) + suffix;
}
struct Operation { fs::path path; std::string bytes; bool publish = false; };
std::vector<Operation> operations(unsigned flow, const Inventory& data) {
    const unsigned next = flow == 2 ? 1 : 2;
    std::vector<Operation> ops{{"intent.fixture", "PUBLIC INCOMPLETE TEST INTENT\n", false},
        {"stage/cp.fixture", program(next, 0), false}, {"stage/rt.fixture", program(next, 1), false},
        {"Bin/cp.fixture", "", true}, {"Bin/rt.fixture", "", true}};
    if (flow == 0 || flow == 3)
        for (const auto& e : data) ops.push_back({fs::path("target") / e.first, e.second, false});
    ops.push_back({"stage/committed.fixture", receipt(next), false});
    ops.push_back({"committed.fixture", "", true});
    return ops;
}
int writer(const fs::path& p, unsigned flow, const Inventory& data, unsigned cut, unsigned mode,
    const std::string& nonce, unsigned c) {
    const auto ops = operations(flow, data);
    need(cut < ops.size() && mode < modes.size(), "bounded child operation and mode");
    need(!fs::exists(p / "intent.fixture"), "retry refuses existing interrupted state without cleanup");
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name(nonce, c, L"-ready").c_str()));
    Handle cancel(OpenEventW(SYNCHRONIZE, FALSE, event_name(nonce, c, L"-cancel").c_str()));
    need(ready && cancel, "child opens only its case-specific readiness/cancel events");
    for (unsigned i = 0; i < ops.size(); ++i) {
        const auto& op = ops[i];
        if (op.publish) replace_from(p / "stage" / op.path.filename(), p / "target" / op.path);
        else { parents(p, op.path); write_new(p / op.path, op.bytes); }
        if (i == cut) {
            need(SetEvent(ready.get()) != FALSE, "report durable point to supervising parent");
            if (mode == 3) return 79; // Safe explicit I/O failure seam BEFORE the next operation.
            const DWORD wait = WaitForSingleObject(cancel.get(), 5000);
            if (wait == WAIT_OBJECT_0) return 78; // Graceful cancellation, no invented rollback.
            return 90; // Self-bounded even if supervisor is lost (job is additional protection).
        }
    }
    return 91;
}
std::string hex_nonce() {
    std::array<char, 33> s{};
    std::snprintf(s.data(), s.size(), "%016llX%016llX", static_cast<unsigned long long>(GetTickCount64()), static_cast<unsigned long long>(GetCurrentProcessId()));
    return s.data();
}
unsigned small_number(const std::string& s, unsigned limit) {
    need(!s.empty() && s.size() < 4 && (s.size() == 1 || s.front() != '0') &&
        std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }), "canonical small numeric child field");
    const unsigned n = static_cast<unsigned>(std::stoul(s)); need(n < limit, "bounded child field"); return n;
}
fs::path checkout() {
    const auto p = fs::current_path();
    need(fs::is_regular_file(p / "test/platform/final_gates/recovery_mechanics.cpp"), "fixed checkout working directory required");
    ancestry(p / "build/final-gates"); return p;
}
fs::path area(const fs::path& repo, const std::string& nonce) {
    need(nonce.size() == 32 && std::all_of(nonce.begin(), nonce.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }), "fixed 32-hex fixture nonce only; no target paths");
    return repo / "build/final-gates" / ("recovery-" + nonce);
}
struct Child {
    Handle job, process, thread;
    Child(const fs::path& self, const std::wstring& args) {
        job.reset(CreateJobObjectW(nullptr, nullptr)); need(job != nullptr, "own bounded job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        need(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits) != FALSE, "one-child job; kill-on-supervisor-loss");
        auto command = L"\"" + self.wstring() + L"\" " + args;
        STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
        need(CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi) != FALSE, "launch exact own ordinary child suspended");
        process.reset(pi.hProcess); thread.reset(pi.hThread); ++children_started;
        if (!AssignProcessToJobObject(job.get(), process.get())) {
            TerminateProcess(process.get(), 92); WaitForSingleObject(process.get(), 5000); ++children_reaped;
            need(false, "job assignment required; never run an unsupervised child");
        }
        need(ResumeThread(thread.get()) != static_cast<DWORD>(-1), "resume only after containment");
        thread.reset();
    }
    ~Child() {
        if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT) {
            TerminateJobObject(job.get(), 93); WaitForSingleObject(process.get(), 5000);
        }
    }
    int finish() {
        need(WaitForSingleObject(process.get(), 5000) == WAIT_OBJECT_0, "bounded confirmed child exit");
        DWORD code = 0; need(GetExitCodeProcess(process.get(), &code) && code != STILL_ACTIVE, "collect own child exit");
        // Process signaling and job accounting are distinct observations. Wait
        // within a fixed budget for the latter too; never infer job retirement
        // from the process signal or an arbitrary sleep alone.
        const auto deadline = GetTickCount64() + 1000;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        bool queried = false;
        do {
            queried = QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof accounting, nullptr) != FALSE;
            if (!queried || accounting.ActiveProcesses == 0) break;
            Sleep(1);
        } while (GetTickCount64() < deadline);
        need(queried && accounting.ActiveProcesses == 0, "bounded exact job retirement after process exit");
        ++children_reaped; return static_cast<int>(code);
    }
};
void compatibility(const fs::path& p) {
    const auto target = p / "target";
    for (const char* d : {"Config", "Data", "Displays", "Backups", "Logs", "Doors", "Temp", "Work"}) {
        write_new(target / d / "ordinary-save.tmp", "old");
        write_new(target / d / "ordinary-save.next", "new");
        replace_from(target / d / "ordinary-save.next", target / d / "ordinary-save.tmp");
        need(read(target / d / "ordinary-save.tmp") == "new", "ordinary atomic content replacement");
    }
    {
        Database db(target / "Data/SFX.DB");
        need(db.scalar("SELECT count(*) FROM fixture") == 1, "imported WAL reopens with exact synthetic row");
        db.sql("PRAGMA journal_mode=WAL; BEGIN; INSERT INTO fixture VALUES(2,'rollback'); ROLLBACK;");
        need(db.scalar("SELECT count(*) FROM fixture") == 1, "ordinary rollback preserved");
        db.sql("INSERT INTO fixture VALUES(3,'commit');");
        need(fs::exists(target / "Data/SFX.DB-wal") && fs::exists(target / "Data/SFX.DB-shm"), "real WAL and SHM supported");
        Database backup(target / "Backups/ordinary.db");
        auto* b = sqlite3_backup_init(backup.db, "main", db.db, "main"); need(b != nullptr, "online backup init");
        const int stepped = sqlite3_backup_step(b, -1); const int ended = sqlite3_backup_finish(b);
        need(stepped == SQLITE_DONE && ended == SQLITE_OK, "backup complete");
        need(backup.scalar("SELECT count(*) FROM fixture") == 2, "backup exact committed rows");
    }
    Database reopened(target / "Data/SFX.DB");
    need(reopened.scalar("SELECT count(*) FROM fixture") == 2, "ordinary close/reopen retains commit");
}
int child_main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--lifecycle") return 27;
    need(argc == 8, "fixed internal child arity");
    const std::string role = argv[1], nonce = argv[2];
    need(role == "--write" || role == "--recover" || role == "--retry", "closed child roles");
    const unsigned c = small_number(argv[3], max_cases), flow = small_number(argv[4], 4),
        cut = small_number(argv[5], 64), mode = small_number(argv[6], 4);
    need(std::string(argv[7]) == "NONAUTHORITATIVE-FIXTURE", "explicit non-installation child boundary");
    const auto root = area(checkout(), nonce), p = root / ("case-" + std::to_string(c));
    need(read(root / "fixture-only") == nonce && read(p / "flow") == flows[flow], "exact pre-existing own fixture marker and case");
    const auto source_before = read(root / "source-inventory.txt");
    need(rendered(inventory(root / "source")) == source_before, "child verifies source still preserved");
    const auto data = import_data(root / "source");
    if (role == "--write") return writer(p, flow, data, cut, mode, nonce, c);
    if (role == "--retry") return fs::exists(p / "intent.fixture") ? 25 : 26;
    // Newly launched recovery process only classifies; no automatic repair or deletion.
    return classify(p, flow, data);
}
int parent_main() {
    const auto repo = checkout(); const auto nonce = hex_nonce(); const auto root = area(repo, nonce);
    directory(root); write_new(root / "fixture-only", nonce); create_source(root);
    const auto source_before = rendered(inventory(root / "source"));
    const auto data = import_data(root / "source");
    std::array<wchar_t, 32768> name{}; const DWORD n = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    need(n && n < name.size(), "bound self executable"); const fs::path self(name.data());
    const auto self_hash = hash(read(self, FILE_SHARE_READ, 4 * 1024 * 1024));
    std::cout << "ORDINARY RECOVERY MECHANICS; NOT INSTALLATION/PROVENANCE AUTHORITY\nROOT=" << root.u8string() << '\n';
    unsigned c = 0, successes = 0; DWORD cold_handles = 0, handles_before = 0;
    need(GetProcessHandleCount(GetCurrentProcess(), &cold_handles) != FALSE, "cold own handle count");
    // Explicit no-file/no-target process-lifecycle controls expose lazy Windows
    // launch initialization separately. Every later count must equal this
    // measured stable baseline, not an arbitrary allowance for leaks.
    std::cout << "HANDLE COLD " << cold_handles << '\n';
    for (unsigned i = 0; i < 3; ++i) {
        { Child warm(self, L"--lifecycle"); need(warm.finish() == 27, "no-file ordinary lifecycle control"); }
        DWORD measured = 0;
        need(GetProcessHandleCount(GetCurrentProcess(), &measured) != FALSE, "lifecycle control own handle count");
        std::cout << "LIFECYCLE CONTROL " << i << " handles=" << measured << '\n';
        if (i) need(measured == handles_before, "repeated lifecycle controls have no handle growth");
        handles_before = measured;
    }
    std::cout << "HANDLE STABLE BASELINE " << handles_before << '\n';
    for (unsigned flow = 0; flow < flows.size(); ++flow) {
        const auto ops = operations(flow, data);
        for (unsigned cut = 0; cut < ops.size(); ++cut) for (unsigned mode = 0; mode < modes.size(); ++mode) {
            need(c < max_cases, "total case bound"); const auto p = root / ("case-" + std::to_string(c));
            seed_candidate(p, flow, data);
            Handle ready(CreateEventW(nullptr, TRUE, FALSE, event_name(nonce, c, L"-ready").c_str()));
            need(ready && GetLastError() != ERROR_ALREADY_EXISTS, "fresh own readiness event");
            Handle cancel(CreateEventW(nullptr, TRUE, FALSE, event_name(nonce, c, L"-cancel").c_str()));
            need(cancel && GetLastError() != ERROR_ALREADY_EXISTS, "fresh own cancellation event");
            const auto tail = std::wstring(nonce.begin(), nonce.end()) + L" " + std::to_wstring(c) + L" " +
                std::to_wstring(flow) + L" " + std::to_wstring(cut) + L" " + std::to_wstring(mode) + L" NONAUTHORITATIVE-FIXTURE";
            {
                Child child(self, L"--write " + tail);
                HANDLE waits[]{ready.get(), child.process.get()};
                need(WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0, "child reaches actual flushed durable point");
                if (mode == 0) need(TerminateProcess(child.process.get(), 80) != FALSE, "terminate only own signaled disposable child");
                else if (mode == 1) need(SetEvent(cancel.get()) != FALSE, "request own child cancellation");
                else if (mode == 2) {
                    need(WaitForSingleObject(child.process.get(), 40) == WAIT_TIMEOUT, "actual bounded timeout while child is waiting");
                    need(TerminateProcess(child.process.get(), 81) != FALSE, "retire timed-out own child");
                }
                const std::array<int, 4> wanted{80, 78, 81, 79}; need(child.finish() == wanted[mode], "exact injected exit cause");
            }
            ready.reset(); cancel.reset();
            const auto before_recovery = rendered(inventory(p));
            const bool committed = cut + 1 == ops.size();
            const int wanted = committed ? new_state : ((flow == 1 || flow == 2) && cut < 3 ? prior_state : (flow == 3 ? source_state : refusal_state));
            { Child recover(self, L"--recover " + tail); need(recover.finish() == wanted, "independent process reports exact honest state"); }
            need(rendered(inventory(p)) == before_recovery, "recovery does not mutate or delete interrupted evidence");
            { Child retry(self, L"--retry " + tail); need(retry.finish() == 25, "independent retry refuses ambiguous existing state"); }
            need(rendered(inventory(p)) == before_recovery, "refused retry preserves everything");
            write_new(root / ("case-" + std::to_string(c) + "-before-controls.txt"), before_recovery);
            if (wanted == prior_state || wanted == new_state) { compatibility(p); ++successes; }
            need(rendered(inventory(root / "source")) == source_before, "source and unknown material unchanged after each path");
            write_new(root / ("case-" + std::to_string(c) + "-final.txt"), rendered(inventory(p)));
            DWORD current_handles = 0;
            need(GetProcessHandleCount(GetCurrentProcess(), &current_handles) != FALSE, "per-case own handle count");
            need(current_handles == handles_before, "every matrix case returns exact stable handle baseline");
            std::cout << "CASE PASS " << c << " flow=" << flows[flow] << " cut=" << cut << '/' << ops.size() - 1 << " mode=" << modes[mode]
                << " state=" << wanted << " publication_sqlite=" << (wanted == prior_state || wanted == new_state) << " handles=" << current_handles << '\n' << std::flush;
            ++c;
        }
    }
    DWORD handles_after = 0; need(GetProcessHandleCount(GetCurrentProcess(), &handles_after) != FALSE, "final own handle count");
    std::cout << "FINAL COUNTS handles=" << handles_before << '/' << handles_after << " children=" << children_started << '/' << children_reaped << '\n' << std::flush;
    need(handles_after <= handles_before, "no own handle growth");
    need(children_started == children_reaped, "every exact child reaped; no detached worker");
    need(hash(read(self, FILE_SHARE_READ, 4 * 1024 * 1024)) == self_hash, "own image unchanged");
    write_new(root / "source-final-inventory.txt", rendered(inventory(root / "source")));
    std::cout << "RECOVERY MECHANICS PASS checks=" << checks << " failed=0 skipped=0 cases=" << c << " children=" << children_started
        << " reaped=" << children_reaped << " publication_sqlite_paths=" << successes << " handles=" << handles_before << '/' << handles_after << '\n';
    std::cout << "LIMITATION: controlled ordinary fixture, public non-authoritative markers, no elevated/HKLM/media/namespace proof. No physical power-loss/cache claim. All dummy evidence retained.\n";
    return 0;
}
}
int main(int argc, char** argv) {
    try { ordinary(); return argc == 1 ? parent_main() : child_main(argc, argv); }
    catch (const std::exception& e) { std::cerr << "RECOVERY MECHANICS FAILURE checks=" << checks << ' ' << e.what() << '\n'; return 2; }
}
