// Fixed, ordinary-user defensive controls. No installed paths or target options.
// No elevation, privilege adjustment, reparse/alias creation, process manipulation,
// recursive traversal, cleanup, or product code. Only new owned fixture objects.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <sqlite3.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
unsigned checks = 0, groups = 0;
void check(bool result, const char* label) {
    ++checks;
    if (!result) throw std::runtime_error(std::string(label) + " Win32=" + std::to_string(GetLastError()));
}
struct Close { void operator()(void* h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
struct Free { void operator()(void* p) const { if (p) LocalFree(p); } };
using Handle = std::unique_ptr<void, Close>;
using Local = std::unique_ptr<void, Free>;
constexpr DWORD shared = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD directory_flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS;
void group(const char* name) { ++groups; std::cout << "CONTROL " << groups << ": " << name << "\n" << std::flush; }

std::wstring user() {
    HANDLE raw = nullptr;
    check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "own token query");
    Handle token(raw); TOKEN_ELEVATION elevation{}; DWORD bytes = 0;
    check(GetTokenInformation(raw, TokenElevation, &elevation, sizeof(elevation), &bytes) && !elevation.TokenIsElevated,
        "fixture must be unelevated; this is NOT UAC evidence");
    GetTokenInformation(raw, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> data(bytes);
    check(GetTokenInformation(raw, TokenUser, data.data(), bytes, &bytes) != FALSE, "own token SID");
    wchar_t* text = nullptr;
    check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &text) != FALSE, "SID conversion");
    Local holder(text); return text;
}
Local read_only_descriptor(const std::wstring& sid) {
    const auto text = L"O:" + sid + L"D:P(A;;RC;;;OW)(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;" + sid + L")";
    void* raw = nullptr;
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &raw, nullptr) != FALSE,
        "noninheriting structural fixture descriptor");
    return Local(raw);
}
bool exact_security(HANDLE h, void* expected) {
    PSID owner = nullptr, wanted = nullptr; PACL acl = nullptr, want_acl = nullptr; void* raw = nullptr;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &raw) != ERROR_SUCCESS) return false;
    Local holder(raw); BOOL present = FALSE, defaulted = FALSE; SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    if (!GetSecurityDescriptorOwner(expected, &wanted, &defaulted) || !owner || !EqualSid(owner, wanted) ||
        !GetSecurityDescriptorDacl(expected, &present, &want_acl, &defaulted) || !present || !acl || !want_acl ||
        !GetSecurityDescriptorControl(raw, &control, &revision) || !(control & SE_DACL_PROTECTED) ||
        acl->AceCount != want_acl->AceCount) return false;
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void* a = nullptr; void* b = nullptr;
        if (!GetAce(acl, i, &a) || !GetAce(want_acl, i, &b)) return false;
        const auto* header = static_cast<ACE_HEADER*>(a);
        if (header->AceSize != static_cast<ACE_HEADER*>(b)->AceSize || std::memcmp(a, b, header->AceSize)) return false;
    }
    return true;
}
DWORD set_same_handle_dacl(HANDLE h, void* sd) {
    PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    check(GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted) && present && acl, "non-null DACL only");
    // There are no inheritable ACEs. No recursion or mutable-content ACL repair.
    return SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
}
void directory(const fs::path& p, void* sd = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    check(CreateDirectoryW(p.c_str(), sd ? &sa : nullptr) != FALSE, "new disposable directory only");
}
Handle open_dir(const fs::path& p, DWORD access = GENERIC_READ | READ_CONTROL, DWORD sharing = shared) {
    return Handle(CreateFileW(p.c_str(), access, sharing, nullptr, OPEN_EXISTING, directory_flags, nullptr));
}
BY_HANDLE_FILE_INFORMATION info(HANDLE h, bool dir) {
    BY_HANDLE_FILE_INFORMATION value{};
    check(h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &value) &&
        !(value.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == dir && (dir || value.nNumberOfLinks == 1),
        "inspect retained non-reparse directory or single-link regular file");
    return value;
}
bool same(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}
Handle create(const fs::path& p, const std::string& text, void* sd = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    Handle h(CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL | WRITE_DAC, FILE_SHARE_READ,
        sd ? &sa : nullptr, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    check(h.get() != INVALID_HANDLE_VALUE, "CREATE_NEW returns owned file handle"); info(h.get(), false);
    DWORD done = 0;
    check(WriteFile(h.get(), text.data(), static_cast<DWORD>(text.size()), &done, nullptr) && done == text.size() && FlushFileBuffers(h.get()),
        "write and flush fixed dummy bytes through creating handle");
    return h;
}
void bytes(HANDLE h, const std::string& wanted) {
    info(h, false); LARGE_INTEGER zero{}; DWORD count = 0; char data[128]{};
    check(SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) && ReadFile(h, data, sizeof(data), &count, nullptr) &&
        count == wanted.size() && !std::memcmp(data, wanted.data(), wanted.size()), "exact dummy bytes through retained handle");
}
void bytes(const fs::path& p, const std::string& wanted) {
    Handle h(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    check(h.get() != INVALID_HANDLE_VALUE, "open known fixture file for verification"); bytes(h.get(), wanted);
}
void denied_dir(const fs::path& p, DWORD access) {
    auto h = open_dir(p, access); const auto error = GetLastError();
    check(h.get() == INVALID_HANDLE_VALUE && error == ERROR_ACCESS_DENIED, "new ordinary structural access denied by ACL");
}

void controls(const fs::path& root, void* sd) {
    group("fresh protected directory has exact descriptor from creation");
    const auto fresh = root / L"fresh-readonly"; directory(fresh, sd);
    auto fresh_handle = open_dir(fresh); info(fresh_handle.get(), true);
    check(exact_security(fresh_handle.get(), sd), "exact owner, protected flag, ACEs from creation");
    for (DWORD access : {DWORD(WRITE_DAC), DWORD(WRITE_OWNER), DWORD(FILE_ADD_FILE), DWORD(FILE_ADD_SUBDIRECTORY)})
        denied_dir(fresh, access);
    // The engineering parent is intentionally NOT an installation security anchor.
    // Request access only: never delete or rename this protected control object.
    auto parent_delete = open_dir(fresh, DELETE);
    check(parent_delete.get() != INVALID_HANDLE_VALUE, "writable fixture parent still grants child DELETE despite child ACL");
    std::cout << "PARENT GATE: exact child ACL does not establish permanent protection under a writable parent; no deletion attempted.\n";
    parent_delete.reset();
    check(!CreateDirectoryW(fresh.c_str(), nullptr) && GetLastError() == ERROR_ALREADY_EXISTS, "absent-root collision refuses without adoption");
    check(exact_security(fresh_handle.get(), sd), "collision preserves exact security");

    group("retained new-file handle writes trusted fixture bytes with restrictive initial DACL");
    const auto new_file = root / L"new-protected-dummy.bin";
    auto h = create(new_file, "fixed fixture payload", sd);
    const auto identity = info(h.get(), false); bytes(h.get(), "fixed fixture payload");
    check(exact_security(h.get(), sd), "initial restrictive file descriptor is exact");
    const auto created_acl_result = set_same_handle_dacl(h.get(), sd);
    std::cout << "NEW-FILE metadata control: SetSecurityInfo=" << created_acl_result
        << " (ordinary token; denial is not elevated-operation evidence).\n";
    check((created_acl_result == ERROR_SUCCESS || created_acl_result == ERROR_ACCESS_DENIED) && exact_security(h.get(), sd),
        "same-object descriptor remains exact after ordinary metadata attempt");
    check(same(identity, info(h.get(), false)), "retained object identity unchanged");
    h.reset(); bytes(new_file, "fixed fixture payload");
    Handle rejected(CreateFileW(new_file.c_str(), GENERIC_WRITE, shared, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    check(rejected.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED, "new ordinary file writer denied");

    group("pre-existing data writer blocks maintenance entry before any content operation");
    const auto conflict = root / L"writer-conflict"; directory(conflict);
    auto writer = open_dir(conflict, FILE_ADD_FILE, shared);
    check(writer.get() != INVALID_HANDLE_VALUE, "known own writer control");
    auto blocked = open_dir(conflict, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
    check(blocked.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "maintenance refuses pre-existing writer");
    writer.reset(); auto retry = open_dir(conflict, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
    info(retry.get(), true);

    group("ACL tightening and restrictive sharing do not retire a pre-existing security handle");
    const auto authority = root / L"security-handle-control"; directory(authority);
    auto earlier = open_dir(authority, READ_CONTROL | WRITE_DAC, shared);
    check(earlier.get() != INVALID_HANDLE_VALUE, "own security handle established before maintenance");
    auto maintenance = open_dir(authority, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
    const auto original = info(maintenance.get(), true);
    check(set_same_handle_dacl(earlier.get(), sd) == ERROR_SUCCESS, "tighten only owned empty fixture boundary");
    check(exact_security(maintenance.get(), sd), "maintenance sees exact restrictive ACL");
    denied_dir(authority, WRITE_DAC);
    // Harmless identical reapplication, NOT a redirection, broadened grant, or payload.
    const auto retained = set_same_handle_dacl(earlier.get(), sd);
    check(retained == ERROR_SUCCESS, "earlier WRITE_DAC remains usable despite exact ACL and maintenance pin");
    check(exact_security(maintenance.get(), sd) && same(original, info(maintenance.get(), true)), "identical policy and object preserved");
    std::cout << "SAFETY GATE: security handle survives tightening; snapshot plus share pin cannot certify unsafe in-place repair.\n";
    earlier.reset(); maintenance.reset(); denied_dir(authority, WRITE_DAC);
}

void sql(sqlite3* db, const char* statement) { check(sqlite3_exec(db, statement, nullptr, nullptr, nullptr) == SQLITE_OK, "fixed fixture SQLite statement"); }
struct Database {
    sqlite3* value = nullptr;
    explicit Database(const fs::path& p) {
        check(sqlite3_open16(p.c_str(), &value) == SQLITE_OK, "open only new or known dummy database");
    }
    ~Database() { if (value) sqlite3_close(value); }
    Database(const Database&) = delete;
    void close() { check(sqlite3_close(value) == SQLITE_OK, "close fixture database"); value = nullptr; }
};
int scalar(sqlite3* db, const char* text) {
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(db, text, -1, &statement, nullptr) == SQLITE_OK, "prepare fixed scalar");
    check(sqlite3_step(statement) == SQLITE_ROW, "scalar row"); const int value = sqlite3_column_int(statement, 0);
    check(sqlite3_finalize(statement) == SQLITE_OK, "finalize scalar"); return value;
}

void ordinary_work(const fs::path& root) {
    group("metadata-only maintenance ends completely before ordinary data publication");
    const auto install = root / L"ordinary-tree"; directory(install);
    constexpr std::array<const wchar_t*, 9> leaves{L"Bin", L"Config", L"Data", L"Displays", L"Backups", L"Logs", L"Files", L"Doors", L"Work"};
    for (auto leaf : leaves) directory(install / leaf);
    {
        std::vector<Handle> pins;
        pins.push_back(open_dir(install, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ));
        for (auto leaf : leaves) pins.push_back(open_dir(install / leaf, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ));
        for (const auto& pin : pins) info(pin.get(), true);
        const auto before = info(pins[2].get(), true);
        check(!MoveFileExW((install / L"Config").c_str(), (install / L"Config-moved").c_str(), 0) &&
            GetLastError() == ERROR_SHARING_VIOLATION, "ordinary boundary rename denied during metadata-only maintenance");
        check(same(before, info(pins[2].get(), true)), "same Config object retained");
    }
    // End of maintenance, not temporary unlocking inside privileged publication.
    // All subsequent actions are ordinary-user data operations; no elevated phase exists in this fixture.
    for (auto leaf : leaves) {
        if (!wcscmp(leaf, L"Bin")) continue;
        create(install / leaf / L"owned.txt", "old").reset();
        create(install / leaf / L"owned.pending", "new").reset();
        check(MoveFileExW((install / leaf / L"owned.pending").c_str(), (install / leaf / L"owned.txt").c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE, "ordinary atomic replacement after maintenance");
        bytes(install / leaf / L"owned.txt", "new");
    }
    group("SQLite WAL/SHM, rollback, independent reader, backup and reopen are unelevated");
    const auto database = install / L"Data/fixture.db";
    Database db(database); sql(db.value, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; CREATE TABLE proof(value INTEGER); INSERT INTO proof VALUES(7);");
    Database reader(database); sql(reader.value, "BEGIN;");
    check(scalar(reader.value, "SELECT value FROM proof;") == 7, "reader original value");
    sql(db.value, "BEGIN IMMEDIATE; UPDATE proof SET value=9; ROLLBACK;");
    check(scalar(db.value, "SELECT value FROM proof;") == 7, "rollback retains original value");
    sql(db.value, "BEGIN IMMEDIATE; UPDATE proof SET value=11; COMMIT;");
    check(scalar(reader.value, "SELECT value FROM proof;") == 7, "reader snapshot preserved");
    for (const auto* suffix : {L"-wal", L"-shm"}) {
        const auto p = fs::path(database.native() + suffix);
        Handle sidecar(CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, shared, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        info(sidecar.get(), false);
    }
    sql(reader.value, "COMMIT;"); reader.close();
    Database backup(install / L"Backups/fixture-copy.db");
    auto* copy = sqlite3_backup_init(backup.value, "main", db.value, "main");
    check(copy != nullptr, "ordinary SQLite backup start");
    check(sqlite3_backup_step(copy, -1) == SQLITE_DONE, "bounded tiny fixture backup");
    check(sqlite3_backup_finish(copy) == SQLITE_OK, "backup complete");
    check(scalar(backup.value, "SELECT value FROM proof;") == 11, "backup exact committed value");
    backup.close(); sql(db.value, "PRAGMA wal_checkpoint(TRUNCATE);"); db.close();
    Database reopened(database); check(scalar(reopened.value, "SELECT value FROM proof;") == 11, "restart persisted value"); reopened.close();

    group("copy-first data relocation carries no fixture machine-bound path");
    const auto destination = root / L"copy-first-destination"; directory(destination);
    check(CopyFileW((install / L"Backups/fixture-copy.db").c_str(), (destination / L"copied.db").c_str(), TRUE) != FALSE,
        "copy known closed backup to newly created destination only");
    Database migrated(destination / L"copied.db");
    check(scalar(migrated.value, "SELECT value FROM proof;") == 11, "copied data opens in different fixture location"); migrated.close();
    bytes(install / L"Config/owned.txt", "new");
}

int run() {
    const auto sid = user(); auto descriptor = read_only_descriptor(sid);
    const auto cwd = fs::current_path();
    check(fs::is_regular_file(cwd / L"test/platform/setup_repair_defensive/main.cpp"), "repository working directory required");
    const auto parent = cwd / L"build/consolidation-security-probe";
    std::vector<fs::path> chain;
    for (auto p = parent;; p = p.parent_path()) { chain.push_back(p); if (p == p.parent_path()) break; }
    std::vector<Handle> ancestors;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        ancestors.push_back(open_dir(*it, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE));
        info(ancestors.back().get(), true);
    }
    const auto root = parent / (L"setup-repair-defensive-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    directory(root); std::wcout << L"Disposable fixture: " << root.native() << L"\n";
    std::cout << "Unelevated engineering controls only. Ancestor inspection is NOT canonical installation qualification.\n";
    controls(root, descriptor.get()); ordinary_work(root);
    std::cout << "COMPLETE: " << groups << " control groups, " << checks << " checks, 0 failed checks.\n"
        << "UNAVAILABLE/NOT RUN: genuine Win7/Win10, native UAC, elevated Setup/Repair, hostile reparse/alias races, process interruption.\n"
        << "STOP: elevated invariant not fully proven; production remains blocked. Fixtures retained; no installation or machine ACL changes.\n";
    return 3; // Explicit partial-proof gate, never a whole-checkpoint PASS.
}
}
int main(int argc, char**) {
    try { check(argc == 1, "no target arguments accepted"); return run(); }
    catch (const std::exception& e) { std::cerr << "INCOMPLETE at check " << checks << ": " << e.what() << '\n'; return 2; }
}
