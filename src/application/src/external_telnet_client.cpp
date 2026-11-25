#include "sfx/application/external_telnet_client.hpp"

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace sfx::application {
namespace {

bool unsafe_text(const std::wstring_view value) noexcept {
    return value.empty() || value.find(L'\0') != value.npos ||
        value.find(L'\r') != value.npos || value.find(L'\n') != value.npos ||
        value.find(L'"') != value.npos ||
        std::iswspace(value.front()) || std::iswspace(value.back());
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

bool substitute(std::wstring_view input, std::wstring_view host,
    std::wstring_view port, std::wstring& output,
    unsigned& hosts, unsigned& ports) {
    output.clear();
    for (std::size_t offset = 0; offset < input.size();) {
        if (input.compare(offset, 6, L"{HOST}") == 0) {
            output.append(host); offset += 6; ++hosts; continue;
        }
        if (input.compare(offset, 6, L"{PORT}") == 0) {
            output.append(port); offset += 6; ++ports; continue;
        }
        if (input[offset] == L'{' || input[offset] == L'}') return false;
        output.push_back(input[offset++]);
    }
    return true;
}

std::wstring quote_windows_argument(const std::wstring_view argument) {
    std::wstring output;
    output.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') {
            output.append(backslashes * 2U + 1U, L'\\');
            output.push_back(c);
        } else {
            output.append(backslashes, L'\\');
            output.push_back(c);
        }
        backslashes = 0;
    }
    output.append(backslashes * 2U, L'\\');
    output.push_back(L'"');
    return output;
}

} // namespace

ExternalTelnetClientType external_telnet_client_type_for_path(
    const std::wstring_view executable) noexcept {
    try {
        const auto filename = lower(std::filesystem::path(executable).filename().wstring());
        return filename == L"syncterm.exe" ? ExternalTelnetClientType::syncterm :
            ExternalTelnetClientType::custom;
    } catch (...) {
        return ExternalTelnetClientType::custom;
    }
}

ExternalTelnetClientValidation validate_external_telnet_client_profile(
    const ExternalTelnetClientProfile& profile) noexcept {
    try {
        if (profile.type != ExternalTelnetClientType::syncterm &&
            profile.type != ExternalTelnetClientType::custom)
            return {false, "telnet_client_type_invalid"};
        if (profile.executable.size() > 32767U || unsafe_text(profile.executable))
            return {false, "telnet_client_path_invalid"};
        const std::filesystem::path path(profile.executable);
        if (!path.is_absolute() || lower(path.extension().wstring()) != L".exe")
            return {false, "telnet_client_executable_required"};
        if (profile.type == ExternalTelnetClientType::syncterm) {
            if (external_telnet_client_type_for_path(profile.executable) !=
                    ExternalTelnetClientType::syncterm || !profile.custom_arguments.empty())
                return {false, "syncterm_profile_invalid"};
            return {true, "telnet_client_profile_valid"};
        }
        if (profile.custom_arguments.empty() ||
            profile.custom_arguments.size() > kExternalTelnetClientMaximumTokens)
            return {false, "custom_argument_count_invalid"};
        unsigned hosts = 0, ports = 0;
        for (const auto& token : profile.custom_arguments) {
            if (token.size() > kExternalTelnetClientMaximumTokenCharacters || unsafe_text(token))
                return {false, "custom_argument_invalid"};
            std::wstring ignored;
            if (!substitute(token, L"127.0.0.1", L"65535", ignored, hosts, ports))
                return {false, "custom_placeholder_invalid"};
        }
        if (hosts != 1 || ports != 1)
            return {false, "custom_placeholders_require_one_host_and_port"};
        return {true, "telnet_client_profile_valid"};
    } catch (...) {
        return {false, "telnet_client_profile_invalid"};
    }
}

bool external_telnet_client_executable_is_usable(
    const ExternalTelnetClientProfile& profile) noexcept {
    try {
        return validate_external_telnet_client_profile(profile).valid &&
            std::filesystem::is_regular_file(std::filesystem::path(profile.executable));
    } catch (...) {
        return false;
    }
}

std::optional<ExternalTelnetClientLaunch> prepare_external_telnet_client_launch(
    const ExternalTelnetClientProfile& profile, const std::wstring_view host,
    const std::uint16_t port) noexcept {
    try {
        if (!validate_external_telnet_client_profile(profile).valid ||
            host != L"127.0.0.1" || port == 0) return std::nullopt;
        ExternalTelnetClientLaunch result;
        if (profile.type == ExternalTelnetClientType::syncterm) {
            result.arguments = {L"-4", L"telnet://127.0.0.1:" + std::to_wstring(port)};
        } else {
            const auto port_text = std::to_wstring(port);
            unsigned hosts = 0, ports = 0;
            for (const auto& token : profile.custom_arguments) {
                std::wstring expanded;
                if (!substitute(token, host, port_text, expanded, hosts, ports))
                    return std::nullopt;
                result.arguments.push_back(std::move(expanded));
            }
            if (hosts != 1 || ports != 1) return std::nullopt;
        }
        result.command_line = quote_windows_argument(profile.executable);
        for (const auto& token : result.arguments) {
            result.command_line.push_back(L' ');
            result.command_line += quote_windows_argument(token);
        }
        if (result.command_line.size() + 1U > kExternalTelnetClientMaximumCommandCharacters)
            return std::nullopt;
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace sfx::application
