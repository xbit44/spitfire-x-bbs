// Ordinary, non-executable fixture only. No elevated modes, external target,
// ACL edit, reparse creation, DLL loading, copied program or installed SFX use.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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
unsigned checks = 0;
void need(bool b, const char* label) {
    ++checks;
    if (!b) throw std::runtime_error(std::string(label) + " error=" + std::to_string(GetLastError()));
}
struct Close { void operator()(void* h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
using Handle = std::unique_ptr<void, Close>;
constexpr DWORD all_sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORD directory_flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
constexpr const char* setup_bytes = "SFX FINAL GATES: NON-EXECUTABLE SETUP STAND-IN\r\n";
constexpr const char* payload_bytes = "SFX FINAL GATES: NON-EXECUTABLE PROGRAM STAND-IN\r\n";
constexpr const char* manifest_bytes = "SFX FINAL GATES FIXED INVENTORY 1\r\nsetup.fixture\r\npayload.fixture\r\n";
BY_HANDLE_FILE_INFORMATION inspect(HANDLE h, bool directory) {
    BY_HANDLE_FILE_INFORMATION i{};
    need(h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &i) &&
        !(i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory &&
        (directory || i.nNumberOfLinks == 1), "non-reparse bound directory or single-link regular file");
    return i;
}
bool same(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}
Handle open_dir(const fs::path& p, DWORD access, DWORD sharing) {
    return Handle(CreateFileW(p.c_str(), access, sharing, nullptr, OPEN_EXISTING, directory_flags, nullptr));
}
void directory(const fs::path& p) { need(CreateDirectoryW(p.c_str(), nullptr) != FALSE, "fresh directory only; collision refuses"); }
Handle file(const fs::path& p, DWORD access, DWORD sharing, DWORD creation) {
    return Handle(CreateFileW(p.c_str(), access, sharing, nullptr, creation, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}
void write(HANDLE h, const char* text) {
    inspect(h, false);
    const auto n = static_cast<DWORD>(std::strlen(text));
    DWORD done = 0;
    need(WriteFile(h, text, n, &done, nullptr) && done == n && FlushFileBuffers(h), "write/flush fixed harmless bytes via creating handle");
}
void create(const fs::path& p, const char* text) { auto h = file(p, GENERIC_READ | GENERIC_WRITE, 0, CREATE_NEW); write(h.get(), text); }
bool matches(HANDLE h, const char* text) {
    auto i = inspect(h, false);
    if (i.nFileSizeHigh || i.nFileSizeLow != std::strlen(text)) return false;
    need(SetFilePointer(h, 0, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER, "rewind pinned fixture");
    std::array<char, 256> b{}; DWORD got = 0;
    need(ReadFile(h, b.data(), static_cast<DWORD>(b.size()), &got, nullptr) != FALSE, "read through pinned handle");
    return got == std::strlen(text) && !std::memcmp(b.data(), text, got);
}
bool inventory(const fs::path& root) {
    unsigned found = 0;
    for (const auto& item : fs::directory_iterator(root)) {
        const auto name = item.path().filename().wstring();
        if (name != L"setup.fixture" && name != L"payload.fixture" && name != L"manifest.fixture") return false;
        ++found;
    }
    return found == 3;
}
void prepare(const fs::path& root) {
    directory(root);
    create(root / L"setup.fixture", setup_bytes);
    create(root / L"payload.fixture", payload_bytes);
    create(root / L"manifest.fixture", manifest_bytes);
}
void known_file_controls(const fs::path& root) {
    prepare(root);
    auto directory_pin = open_dir(root, GENERIC_READ, FILE_SHARE_READ);
    auto before = inspect(directory_pin.get(), true);
    auto setup = file(root / L"setup.fixture", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    auto payload = file(root / L"payload.fixture", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    auto manifest = file(root / L"manifest.fixture", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    need(inventory(root) && matches(setup.get(), setup_bytes) && matches(payload.get(), payload_bytes) && matches(manifest.get(), manifest_bytes), "exact complete known inventory through retained handles");
    auto conflicting_writer = file(root / L"setup.fixture", GENERIC_WRITE, all_sharing, OPEN_EXISTING);
    need(conflicting_writer.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "pinned known image denies data-writer acquisition");
    auto conflicting_delete = file(root / L"setup.fixture", DELETE, all_sharing, OPEN_EXISTING);
    need(conflicting_delete.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "pinned known image denies delete acquisition");
    auto rename_access = open_dir(root, DELETE, all_sharing);
    need(rename_access.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "pinned directory denies rename/delete acquisition");
    need(same(before, inspect(directory_pin.get(), true)) && matches(setup.get(), setup_bytes), "all access-only controls preserve exact object and bytes");
    std::cout << "GROUP PASS: complete known-file inventory and retained read handles deny writer/delete/rename opens\n";
}
void earlier_writer(const fs::path& root) {
    prepare(root);
    auto writer = file(root / L"setup.fixture", GENERIC_WRITE, all_sharing, OPEN_EXISTING);
    inspect(writer.get(), false);
    auto verifier = file(root / L"setup.fixture", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    need(verifier.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "pre-existing data writer blocks verifier before any privileged action");
    writer.reset();
    auto retry = file(root / L"setup.fixture", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    need(matches(retry.get(), setup_bytes), "retry after own writer closes sees unchanged fixed bytes");
    std::cout << "GROUP PASS: pre-existing writer refuses verification; exact retry succeeds\n";
}
bool closed_inventory_control(const fs::path& root) {
    prepare(root);
    auto pin = open_dir(root, GENERIC_READ, FILE_SHARE_READ);
    const auto before = inspect(pin.get(), true);
    need(inventory(root), "complete inventory before harmless extra-member control");
    // Never a DLL, executable, symlink, junction or loadable input. This merely
    // asks whether a directory sharing handle seals its child inventory.
    auto extra = file(root / L"unexpected.fixture", GENERIC_READ | GENERIC_WRITE, 0, CREATE_NEW);
    const auto error = GetLastError();
    const bool created = extra.get() != INVALID_HANDLE_VALUE;
    if (created) write(extra.get(), "HARMLESS EXTRA INVENTORY MEMBER - NEVER EXECUTE\r\n");
    extra.reset();
    need(same(before, inspect(pin.get(), true)), "same directory object throughout inventory observation");
    if (created) need(!inventory(root), "strict revalidation detects new member and refuses");
    else need(error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED, "new-member refusal has explicit access/sharing cause");
    std::cout << "OBSERVATION directory_pin_seals_new_child=" << !created << " creation_error=" << (created ? 0 : error) << '\n';
    return !created;
}
int run() {
    HANDLE raw = nullptr;
    need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "query own token");
    Handle t(raw); TOKEN_ELEVATION e{}; DWORD n = 0;
    need(GetTokenInformation(t.get(), TokenElevation, &e, sizeof(e), &n) && !e.TokenIsElevated, "normal unelevated execution only");
    auto cwd = fs::current_path();
    need(fs::is_regular_file(cwd / L"test/platform/final_gates/media_preflight.cpp"), "run only from this checkout");
    auto parent = cwd / L"build/final-gates";
    std::vector<fs::path> chain;
    for (auto p = parent;; p = p.parent_path()) { chain.push_back(p); if (p == p.parent_path()) break; }
    std::vector<Handle> ancestry;
    for (auto p = chain.rbegin(); p != chain.rend(); ++p) {
        auto h = open_dir(*p, FILE_READ_ATTRIBUTES, all_sharing);
        inspect(h.get(), true); ancestry.push_back(std::move(h));
    }
    // Read-only ancestry inspection confines an ordinary fixture. It does NOT
    // certify privileged ancestry or protection against concurrent tampering.
    auto root = parent / (L"media-preflight-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    directory(root); std::wcout << L"Disposable root: " << root.native() << L'\n';
    known_file_controls(root / L"known-files");
    earlier_writer(root / L"earlier-writer");
    const bool sealed = closed_inventory_control(root / L"closed-inventory");
    std::cout << "PREFLIGHT checks=" << checks << " failed=0 skipped=0; unexpected child sealed=" << sealed << '\n';
    std::cout << "No privilege/permissions/registry/network/installed SFX action. All dummy bytes retained.\n";
    if (!sealed) {
        std::cout << "UNRESOLVED: directory sharing pin is not a closed media inventory. No elevated launch attempted.\n";
        return 3;
    }
    std::cout << "LIMITED preflight only: elevated launch and retirement remain unproved.\n";
    return 0;
}
}
int main(int argc, char**) {
    try { need(argc == 1, "no target or action arguments accepted"); return run(); }
    catch (const std::exception& e) { std::cerr << "PREFLIGHT FAILURE checks=" << checks << ' ' << e.what() << '\n'; return 2; }
}
