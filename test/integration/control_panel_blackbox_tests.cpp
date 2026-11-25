#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Winsock final {
public:
    Winsock() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0,
            "start Winsock for Control Panel black-box test");
    }
    ~Winsock() { WSACleanup(); }
};

class ProcessGuard final {
public:
    explicit ProcessGuard(PROCESS_INFORMATION process) : process_(process) {}
    ~ProcessGuard() {
        if (process_.hThread != nullptr) CloseHandle(process_.hThread);
        if (process_.hProcess != nullptr) {
            if (WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) {
                (void)TerminateProcess(process_.hProcess, 99);
                (void)WaitForSingleObject(process_.hProcess, 5000);
            }
            CloseHandle(process_.hProcess);
        }
    }
    HANDLE process() const noexcept { return process_.hProcess; }
    DWORD process_id() const noexcept { return process_.dwProcessId; }

private:
    PROCESS_INFORMATION process_{};
};

DWORD find_runtime_child(const DWORD control_panel_process_id) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD result = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == control_panel_process_id &&
                _wcsicmp(entry.szExeFile, L"SFXRuntime.exe") == 0) {
                result = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

DWORD listener_owner(const std::uint16_t port) {
    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
            TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return 0;
    std::vector<std::uint8_t> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET,
            TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        return 0;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const auto& row = table->table[index];
        if (ntohs(static_cast<u_short>(row.dwLocalPort)) == port) return row.dwOwningPid;
    }
    return 0;
}

SOCKET connect_when_ready(const std::uint16_t port) {
    const ULONGLONG deadline = GetTickCount64() + 20000ULL;
    while (GetTickCount64() < deadline) {
        const SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(client != INVALID_SOCKET, "create Control Panel black-box Telnet socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            DWORD timeout = 10000;
            require(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0 &&
                    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0,
                "bound black-box Telnet socket timeouts");
            return client;
        }
        closesocket(client);
        Sleep(100);
    }
    throw std::runtime_error("Control Panel-owned Runtime did not expose Telnet port 2323");
}

void send_text(const SOCKET client, const std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const int sent = send(client, text.data() + offset,
            static_cast<int>(text.size() - offset), 0);
        require(sent > 0, "send black-box Telnet input");
        offset += static_cast<std::size_t>(sent);
    }
}

std::string receive_until(const SOCKET client, const std::string_view expected) {
    std::string output;
    output.reserve(4096);
    std::array<char, 2048> block{};
    while (output.find(expected) == std::string::npos) {
        const int received = recv(client, block.data(), static_cast<int>(block.size()), 0);
        require(received > 0, "receive black-box Telnet output");
        output.append(block.data(), static_cast<std::size_t>(received));
        require(output.size() <= 1024U * 1024U, "bound black-box Telnet transcript");
    }
    return output;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        Winsock winsock;
        require(argc == 2, "Control Panel black-box test requires its executable path");
        const std::filesystem::path control_panel = std::filesystem::absolute(argv[1]);
        require(std::filesystem::is_regular_file(control_panel),
            "Control Panel black-box executable is missing");
        std::wstring command = L"\"" + control_panel.native() +
            L"\" --fixture-bootstrap --auto-close --completion-delay-ms=15000";
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION launched{};
        require(CreateProcessW(control_panel.c_str(), mutable_command.data(), nullptr, nullptr,
                    FALSE, 0, nullptr, control_panel.parent_path().c_str(), &startup, &launched) != FALSE,
            "launch only the Control Panel executable");
        CloseHandle(launched.hThread);
        launched.hThread = nullptr;
        ProcessGuard process(launched);

        const SOCKET caller = connect_when_ready(2323);
        DWORD runtime_process_id = 0;
        const ULONGLONG process_deadline = GetTickCount64() + 5000ULL;
        while (runtime_process_id == 0 && GetTickCount64() < process_deadline) {
            runtime_process_id = find_runtime_child(process.process_id());
            if (runtime_process_id == 0) Sleep(50);
        }
        require(runtime_process_id != 0,
            "SFXRuntime.exe is the direct child of the launched Control Panel");
        require(listener_owner(2323) == runtime_process_id,
            "the Control Panel-owned Runtime process owns the real TCP listener");

        const auto question = receive_until(caller,
            "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
        require(question.find("Your Sysop is Test Sysop\r\n") != std::string::npos &&
                question.find("SPITFIRE-X Bulletin Board System - Version 0.0.1 Alpha\r\n") != std::string::npos &&
                question.find("Copyright (C) 2026 Spitfire-X BBS Project\r\n\r\nDo you want ANSI COLOR GRAPHICS? [Y/n] ") !=
                    std::string::npos && question.find('\x1b') == std::string::npos,
            "Control Panel-to-Runtime chain preserves owner-accepted F01 plain identity before graphics with one blank row");
        send_text(caller, "N\r");
        const auto banner = receive_until(caller, "Enter your first name: ");
        require(banner.find("Your Sysop") == std::string::npos && banner.find('\x1b') == std::string::npos,
            "ASCII selection continues to name entry without repeated identity or ANSI styling");
        send_text(caller, "Test\r");
        (void)receive_until(caller, "Enter your last name: ");
        send_text(caller, "Sysop\r");
        (void)receive_until(caller, "Enter your password: ");
        send_text(caller, "Acct-fixture-81a9\r");
        const auto system_prompt = receive_until(caller, "su password: ");
        require(system_prompt.find("Acct-fixture-81a9") == std::string::npos,
            "Control Panel black-box transcript never reveals the account password");
        send_text(caller, "SY-fixture-72f1\r");
        const auto welcome = receive_until(caller, "Press <ENTER> to continue.");
        const bool first_login = welcome.find(
            "Test Sysop, welcome to Spitfire-X GUI QA.\r\n") != std::string::npos;
        const bool returning_login = welcome.find(
            "Test Sysop, welcome back.\r\n") != std::string::npos &&
            welcome.find("You last called on ") != std::string::npos;
        require((first_login || returning_login) &&
                (welcome.find(" call today.\r\n") != std::string::npos ||
                 welcome.find(" calls today.\r\n") != std::string::npos),
            "real login presents a first or returning welcome with the real daily call count");
        require(welcome.find("You have called ") != std::string::npos &&
                welcome.find("You have spent ") != std::string::npos &&
                welcome.find("Your security level is 999.\r\n") != std::string::npos &&
                welcome.find("Caller:") == std::string::npos &&
                welcome.find("Authenticated session seconds:") == std::string::npos &&
                welcome.find("SY-fixture-72f1") == std::string::npos,
            "real SQLite/Argon2id Sysop authentication succeeds without revealing SY secret");
        send_text(caller, "\r");
        const auto menu = receive_until(caller, "Selection: ");
        require(menu.find("[M]essage area") != std::string::npos &&
                menu.find("[G]oodbye") != std::string::npos,
            "Control Panel-owned Runtime reaches the accepted bounded main menu");
        send_text(caller, "G\r");
        (void)receive_until(caller, "Goodbye from Spitfire-X BBS.");
        shutdown(caller, SD_BOTH);
        closesocket(caller);

        require(WaitForSingleObject(process.process(), 30000) == WAIT_OBJECT_0,
            "Control Panel automation performs orderly owned-Runtime shutdown");
        DWORD exit_code = 1;
        require(GetExitCodeProcess(process.process(), &exit_code) != FALSE && exit_code == 0,
            "Control Panel black-box process exits successfully");
        const HANDLE runtime = OpenProcess(SYNCHRONIZE, FALSE, runtime_process_id);
        if (runtime != nullptr) {
            require(WaitForSingleObject(runtime, 5000) == WAIT_OBJECT_0,
                "owned Runtime exits when the Control Panel closes");
            CloseHandle(runtime);
        }
        std::cout << "SFX Control Panel -> Runtime -> Telnet black-box tests PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Control Panel -> Runtime -> Telnet black-box tests FAIL: "
                  << error.what() << '\n';
        return 1;
    }
}
