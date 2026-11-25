// Test-only incompatible protocol peer. It never opens/provisions C:\SFX,
// starts listeners, loads keys, opens a database, or ships in the owner archive.
// The actual frozen Control Panel launches it through its normal owner pipe.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

bool read_exact(HANDLE pipe, void* data, DWORD size) {
    auto* out = static_cast<unsigned char*>(data);
    while (size) {
        DWORD count = 0;
        if (!ReadFile(pipe, out, size, &count, nullptr) || !count) return false;
        out += count; size -= count;
    }
    return true;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc != 2 || wcsncmp(argv[1], L"--owner-handle=", 15) != 0) return 2;
    const HANDLE bootstrap = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(_wcstoui64(argv[1]+15, nullptr, 10)));
    LocalFree(argv);
    DWORD owner = 0;
    std::array<unsigned char, 32> nonce{};
    bool valid = read_exact(bootstrap, &owner, sizeof(owner)) && read_exact(bootstrap, nonce.data(), 32);
    CloseHandle(bootstrap);
    if (!valid || !owner) return 3;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 4;
    DWORD length = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    std::vector<unsigned char> user(length);
    valid = GetTokenInformation(token, TokenUser, user.data(), length, &length) != FALSE;
    CloseHandle(token);
    if (!valid) return 5;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) return 6;
    std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return 7;
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    std::uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : std::wstring(L"c:\\sfx")) {
        hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ULL;
        hash ^= static_cast<unsigned char>(c >> 8); hash *= 1099511628211ULL;
    }
    std::wostringstream name;
    name << L"\\\\.\\pipe\\SFX-Control-" << std::hex << std::setw(16) << std::setfill(L'0') << hash;
    HANDLE pipe = CreateNamedPipeW(name.str().c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 1000, &security);
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) return 8;
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return 9;
    ULONG actual_owner = 0;
    if (!GetNamedPipeClientProcessId(pipe, &actual_owner) || actual_owner != owner) return 10;
    std::array<unsigned char, 52> hello{};
    if (!read_exact(pipe, hello.data(), static_cast<DWORD>(hello.size()))) return 11;
    valid = hello[0] == 'S' && hello[1] == 'F' && hello[2] == 'X' && hello[3] == '1' &&
        hello[4] == 3 && hello[5] == 0 && hello[6] == 0 && hello[7] == 0 && hello[8] == 1 &&
        hello[16] == 32 && hello[17] == 0 && hello[18] == 0 && hello[19] == 0;
    unsigned difference = 0;
    for (unsigned i = 0; i < 32; ++i) difference |= nonce[i] ^ hello[i+20];
    SecureZeroMemory(nonce.data(), nonce.size());
    if (!valid || difference) return 12;
    std::array<wchar_t, 32768> module{};
    if (!GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()))) return 13;
    const auto folder = std::filesystem::path(module.data()).parent_path();
    const bool minor = folder.filename() == L"minor";
    std::array<unsigned char, 20> response{};
    for (unsigned i = 0; i < 20; ++i) response[i] = hello[i];
    SecureZeroMemory(hello.data(), hello.size());
    response[4] = minor ? 3 : 2; response[6] = minor ? 1 : 0;
    response[8] = 2; response[16] = 0;
    DWORD count = 0;
    valid = WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &count, nullptr) &&
        count == response.size() && FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe); CloseHandle(pipe);
    std::ofstream evidence(folder / "peer-result.txt");
    evidence << "observed_control_panel_protocol=3.0\nowner_pid_verified=1\nnonce_verified_and_erased=1\n"
             << "sent_protocol=" << (minor ? "3.1" : "2.0") << "\nresponse_consumed=" << valid
             << "\ninstallation_files_opened=0\nlisteners_started=0\n";
    return valid && evidence.good() ? 0 : 14;
}
