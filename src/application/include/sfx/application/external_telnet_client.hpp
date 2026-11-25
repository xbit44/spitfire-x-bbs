#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {

inline constexpr std::size_t kExternalTelnetClientMaximumTokens = 8;
inline constexpr std::size_t kExternalTelnetClientMaximumTokenCharacters = 512;
inline constexpr std::size_t kExternalTelnetClientMaximumCommandCharacters = 4096;

enum class ExternalTelnetClientType : std::uint8_t { syncterm = 1, custom = 2 };

struct ExternalTelnetClientProfile {
    ExternalTelnetClientType type{ExternalTelnetClientType::custom};
    std::wstring executable;
    std::vector<std::wstring> custom_arguments;
};

struct ExternalTelnetClientValidation {
    bool valid{false};
    std::string diagnostic_code;
};

struct ExternalTelnetClientLaunch {
    std::vector<std::wstring> arguments;
    std::wstring command_line;
};

ExternalTelnetClientType external_telnet_client_type_for_path(
    std::wstring_view executable) noexcept;
ExternalTelnetClientValidation validate_external_telnet_client_profile(
    const ExternalTelnetClientProfile& profile) noexcept;
bool external_telnet_client_executable_is_usable(
    const ExternalTelnetClientProfile& profile) noexcept;
std::optional<ExternalTelnetClientLaunch> prepare_external_telnet_client_launch(
    const ExternalTelnetClientProfile& profile, std::wstring_view host,
    std::uint16_t port) noexcept;

} // namespace sfx::application
