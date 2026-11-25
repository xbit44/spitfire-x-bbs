#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::wstring field(const std::map<std::wstring, std::wstring>& values,
    const wchar_t* name) {
    const auto found = values.find(name);
    require(found != values.end() && !found->second.empty(), "required argument missing");
    return found->second;
}

std::string narrow_ascii(const std::wstring& value) {
    std::string result;
    for (const wchar_t c : value) {
        require(c >= 32 && c <= 126, "witness accepts printable ASCII evidence fields only");
        result.push_back(static_cast<char>(c));
    }
    return result;
}

class Winsock final {
public:
    Winsock() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock startup failed");
    }
    ~Winsock() { WSACleanup(); }
};

} // namespace

int wmain(const int argc, wchar_t** argv) {
    std::filesystem::path output;
    try {
        require(argc == 6, "expected exactly five ordered client arguments");
        const std::array<std::wstring, 5> names{{
            L"--output=", L"--release=", L"--label=", L"--host=", L"--port="}};
        std::map<std::wstring, std::wstring> values;
        for (int index = 1; index < argc; ++index) {
            const std::wstring argument = argv[index];
            bool recognized = false;
            for (const auto& name : names) {
                if (argument.rfind(name, 0) != 0) continue;
                require(values.emplace(name, argument.substr(name.size())).second,
                    "duplicate client argument");
                recognized = true;
                break;
            }
            require(recognized, "unknown client argument");
        }
        output = field(values, L"--output=");
        const std::filesystem::path release = field(values, L"--release=");
        const auto label = field(values, L"--label=");
        const auto host = field(values, L"--host=");
        require(host == L"127.0.0.1", "non-loopback host refused");
        const auto port_text = field(values, L"--port=");
        wchar_t* port_end = nullptr;
        const auto port_value = std::wcstoul(port_text.c_str(), &port_end, 10);
        require(port_end && *port_end == L'\0' && port_value > 0 && port_value <= 65535,
            "invalid port");

        std::array<wchar_t, 64> inherited_text{};
        SetLastError(ERROR_SUCCESS);
        const DWORD inherited_length = GetEnvironmentVariableW(L"SFX_F06_INHERITABLE_HANDLE",
            inherited_text.data(), static_cast<DWORD>(inherited_text.size()));
        const bool parent_sentinel_supplied = inherited_length != 0;
        if (parent_sentinel_supplied) {
            require(inherited_length < inherited_text.size(), "parent sentinel invalid");
            wchar_t* handle_end = nullptr;
            const auto handle_value = std::wcstoull(inherited_text.data(), &handle_end, 10);
            require(handle_end && *handle_end == L'\0' && handle_value != 0,
                "parent sentinel invalid");
            DWORD flags = 0;
            SetLastError(ERROR_SUCCESS);
            const BOOL inherited = GetHandleInformation(
                reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle_value)), &flags);
            require(inherited == FALSE && GetLastError() == ERROR_INVALID_HANDLE,
                "client inherited a Control Panel handle");
        } else {
            require(GetLastError() == ERROR_ENVVAR_NOT_FOUND,
                "parent sentinel query failed");
        }

        Winsock winsock;
        const SOCKET socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(socket_value != INVALID_SOCKET, "socket creation failed");
        struct CloseSocket final {
            SOCKET value;
            ~CloseSocket() { if (value != INVALID_SOCKET) closesocket(value); }
        } close_socket{socket_value};
        DWORD timeout = 5000;
        require(setsockopt(socket_value, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0 &&
                setsockopt(socket_value, SOL_SOCKET, SO_SNDTIMEO,
                    reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0,
            "socket timeout setup failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<u_short>(port_value));
        require(connect(socket_value, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0, "one-use endpoint connection failed");
        std::string transcript;
        std::array<char, 2048> block{};
        while (transcript.find("Do you want ANSI COLOR GRAPHICS? [Y/n] ") == std::string::npos) {
            const int received = recv(socket_value, block.data(), static_cast<int>(block.size()), 0);
            require(received > 0, "ordinary Telnet banner not received");
            transcript.append(block.data(), static_cast<std::size_t>(received));
            require(transcript.size() <= 65536, "Telnet witness transcript exceeded bound");
        }
        unsigned node = 0;
        for (unsigned candidate = 1; candidate <= 4; ++candidate) {
            if (transcript.find(" - Node " + std::to_string(candidate) + "\r\n") != std::string::npos) {
                require(node == 0, "multiple node identities in banner");
                node = candidate;
            }
        }
        require(node != 0, "node identity missing from banner");

        require(!std::filesystem::exists(output), "witness output already exists");
        std::ofstream evidence(output, std::ios::binary | std::ios::out);
        require(evidence.good(), "witness output create failed");
        evidence << "result=PASS\r\nlabel=" << narrow_ascii(label)
            << "\r\nhost=127.0.0.1\r\nport=" << port_value
            << "\r\nnode=" << node << "\r\nparent_handle_inherited="
            << (parent_sentinel_supplied ? "0" : "not-supplied") << "\r\nargc=" << argc;
        for (int index = 0; index < argc; ++index)
            evidence << "\r\nargv" << index << '=' << narrow_ascii(argv[index]);
        evidence << "\r\n";
        evidence.close();
        require(!evidence.fail(), "witness output flush failed");

        const ULONGLONG deadline = GetTickCount64() + 20000ULL;
        while (!std::filesystem::exists(release) && GetTickCount64() < deadline) Sleep(20);
        require(std::filesystem::exists(release), "witness release timed out");
        shutdown(socket_value, SD_BOTH);
        std::ofstream completed(output, std::ios::binary | std::ios::out | std::ios::app);
        require(completed.good(), "witness completion reopen failed");
        completed << "exit=clean\r\n";
        completed.close();
        require(!completed.fail(), "witness completion flush failed");
        return 0;
    } catch (const std::exception& error) {
        if (!output.empty()) {
            std::ofstream evidence(output, std::ios::binary | std::ios::out);
            evidence << "result=FAIL\r\nreason=" << error.what() << "\r\n";
        }
        return 1;
    }
}
