// Harmless launch witness only: query own elevation, then exit. No arguments,
// files, registry, network, UI, DLL loading, target tree or receipt operation.
// CRT-free; entry-time imports are KERNEL32 and ADVAPI32 only. This is not Setup.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
extern "C" void fixture_entry() {
    HANDLE t = nullptr;
    TOKEN_ELEVATION e;
    DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) ExitProcess(75);
    if (!GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n)) {
        CloseHandle(t); ExitProcess(76);
    }
    CloseHandle(t);
    ExitProcess(e.TokenIsElevated ? 73 : 74);
}
