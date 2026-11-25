// Ordinary lifetime control over fixed NON-EXECUTABLE dummy media only.
// Reuse reviewed preflight helpers; no privileged launch, ACL edit, payload
// replacement, DLL creation/loading or arbitrary/installed target is possible.
#include <algorithm>
#define main unused_preflight_main
#include "media_preflight.cpp"
#undef main
namespace {
fs::path retirement_area(const std::wstring& id) {
    need(id.size() >= 3 && id.size() <= 40 && std::all_of(id.begin(), id.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'-'; }), "bounded generated case ID, no target path");
    const auto root = fs::current_path();
    need(fs::is_regular_file(root / L"test/platform/final_gates/media_retirement.cpp"), "fixed checkout only");
    return root / L"build/final-gates" / (L"media-retirement-" + id);
}
void require_ordinary() {
    HANDLE raw = nullptr; need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw) != FALSE, "own token");
    Handle token(raw); TOKEN_ELEVATION e{}; DWORD size = 0;
    need(GetTokenInformation(token.get(), TokenElevation, &e, sizeof e, &size) && !e.TokenIsElevated, "ordinary fixture only");
}
int holder(const std::wstring& id) {
    const auto root = retirement_area(id);
    auto dir = open_dir(root, GENERIC_READ, FILE_SHARE_READ); inspect(dir.get(), true);
    auto setup = file(root / L"setup.fixture", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    auto payload = file(root / L"payload.fixture", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    auto manifest = file(root / L"manifest.fixture", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    need(inventory(root) && matches(setup.get(), setup_bytes) && matches(payload.get(), payload_bytes) && matches(manifest.get(), manifest_bytes), "child pins exact known dummy inventory");
    const auto event_name = L"Local\\SFX-Final-Retirement-" + id;
    Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str()));
    need(ready && SetEvent(ready.get()), "signal exact own pins acquired");
    // No detached worker or indefinite lifetime; the parent normally terminates
    // this exact child earlier. The timeout never grants installation authority.
    Sleep(5000);
    return 44;
}
int lifetime_control() {
    const auto id = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto root = retirement_area(id); prepare(root);
    const auto event_name = L"Local\\SFX-Final-Retirement-" + id;
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    need(ready && GetLastError() != ERROR_ALREADY_EXISTS, "fresh fixture readiness event");
    Handle job(CreateJobObjectW(nullptr, nullptr)); need(job != nullptr, "fresh own child job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    need(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits) != FALSE, "one supervised child with kill-on-close");
    std::array<wchar_t, 32768> self{};
    const DWORD n = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    need(n && n < self.size(), "own executable path");
    std::wstring command = L"\"" + std::wstring(self.data()) + L"\" --hold " + id;
    STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    need(CreateProcessW(self.data(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi) != FALSE, "create exact own child suspended");
    Handle process(pi.hProcess), thread(pi.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 46); WaitForSingleObject(process.get(), 5000);
        need(false, "no unsupervised child allowed");
    }
    need(ResumeThread(thread.get()) != static_cast<DWORD>(-1), "resume own supervised holder"); thread.reset();
    HANDLE waits[]{ready.get(), process.get()};
    need(WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0, "exact child acquired and checked all pins");
    {
        auto blocked = file(root / L"setup.fixture", GENERIC_WRITE, all_sharing, OPEN_EXISTING);
        need(blocked.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION, "live verifier pin denies writer acquisition");
    }
    need(TerminateProcess(process.get(), 45) != FALSE, "terminate only own dummy-media holder");
    need(WaitForSingleObject(process.get(), 5000) == WAIT_OBJECT_0, "bounded confirmed holder exit");
    DWORD exit_code = 0; need(GetExitCodeProcess(process.get(), &exit_code) && exit_code == 45, "exact deliberate holder interruption");
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{}; bool queried = false;
    const auto deadline = GetTickCount64() + 1000;
    do {
        queried = QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof accounting, nullptr) != FALSE;
        if (!queried || !accounting.ActiveProcesses) break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    need(queried && !accounting.ActiveProcesses, "exact job retired; no remaining child");
    // Access acquisition ONLY. No write, rename, substitution or permissions
    // change occurs. The before/after dummy contents remain byte-for-byte exact.
    {
        auto writable = file(root / L"setup.fixture", GENERIC_WRITE, all_sharing, OPEN_EXISTING);
        inspect(writable.get(), false);
    }
    auto setup = file(root / L"setup.fixture", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    auto payload = file(root / L"payload.fixture", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    auto manifest = file(root / L"manifest.fixture", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    need(inventory(root) && matches(setup.get(), setup_bytes) && matches(payload.get(), payload_bytes) && matches(manifest.get(), manifest_bytes), "unchanged exact dummy inventory after access-only observation");
    std::wcout << L"ROOT=" << root.native() << L'\n';
    std::cout << "MEDIA RETIREMENT checks=" << checks << " failed=0 skipped=0; live_writer_denied=1 after_holder_exit_writer_opened=1 dummy_bytes_changed=0 children_remaining=0\n";
    std::cout << "UNRESOLVED exit=3: verifier-owned pins do not survive verifier loss. This is NOT an actual UAC/loader observation or a claim that every bootstrap design is impossible. No privileged operation attempted.\n";
    return 3;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        require_ordinary();
        if (argc == 1) return lifetime_control();
        need(argc == 3 && std::wstring(argv[1]) == L"--hold", "fixed internal holder role only");
        return holder(argv[2]);
    } catch (const std::exception& e) { std::cerr << "MEDIA RETIREMENT FAILURE checks=" << checks << ' ' << e.what() << '\n'; return 2; }
}
