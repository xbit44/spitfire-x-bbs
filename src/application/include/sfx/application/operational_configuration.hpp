#pragma once

#include "sfx/application/authentication_security.hpp"
#include "sfx/application/configuration.hpp"
#include "sfx/application/botgate.hpp"
#include "sfx/application/event_sounds.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sfx::application {

inline constexpr std::uint32_t kOperationalConfigurationFormatVersion = 13;
// Format 8 accommodates independent bounded security lists alongside the
// existing maximum source-connection exceptions. Wire frames remain <=64 KiB.
// Two snapshots plus framing must fit the protected 64 KiB IPC frame.
inline constexpr std::size_t kOperationalConfigurationMaximumBytes = 24576;
inline constexpr std::size_t kBbsNameMaximum = 64;
inline constexpr std::size_t kBbsTimeZoneKeyMaximum = 128;
inline constexpr std::size_t kTelnetBindMaximum = 15;

struct TelnetOperationalConfiguration {
    bool enabled{true};
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{2323};
    bool bot_gate_enabled{true};
};

struct SshOperationalConfiguration {
    bool enabled{true};
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{2222};
};

struct MenuOperationalConfiguration {
    std::uint8_t text_color{7};
    std::uint8_t input_color{11};
    bool lightbars_enabled{true};
    std::uint8_t selected_foreground{0};
    std::uint8_t selected_background{7};
    bool compact_footer{false};
    bool legacy_message_pipe_colors{false};
    bool operator==(const MenuOperationalConfiguration& other) const noexcept {
        return text_color==other.text_color && input_color==other.input_color &&
            lightbars_enabled==other.lightbars_enabled && selected_foreground==other.selected_foreground &&
            selected_background==other.selected_background && compact_footer==other.compact_footer &&
            legacy_message_pipe_colors==other.legacy_message_pipe_colors;
    }
};

struct OperationalConfiguration {
    std::uint32_t format_version{kOperationalConfigurationFormatVersion};
    std::uint64_t configuration_revision{1};
    std::string bbs_name{"Spitfire-X BBS"};
    std::string bbs_time_zone;
    std::uint16_t logical_nodes{4};
    RuntimeOwnershipMode ownership_mode{RuntimeOwnershipMode::desktop};
    PasswordMode password_mode{PasswordMode::standard};
    bool allow_new_users{true};
    bool hostname_lookup{true};
    std::uint16_t caller_afk_minutes{5};
    std::uint16_t login_afk_seconds{60};
    std::uint16_t maximum_connections_per_source{2};
    std::uint16_t maximum_sessions_per_account{1};
    std::vector<SourceConnectionLimit> source_connection_exceptions;
    TelnetOperationalConfiguration telnet;
    SshOperationalConfiguration ssh;
    BotGateConfiguration botgate;
    LoggingConfiguration logging;
    MenuOperationalConfiguration menus;
    EventSoundConfiguration event_sounds;
    std::int64_t security_policy_revision{0};
    std::string security_policy_hash;
};

struct OperationalConfigurationParseResult {
    ConfigurationValidation validation;
    std::optional<OperationalConfiguration> configuration;

    bool valid() const noexcept {
        return validation.valid() && configuration.has_value();
    }
};

ConfigurationValidation validate_operational_configuration(
    const OperationalConfiguration& configuration);
OperationalConfigurationParseResult parse_operational_configuration(
    std::string_view serialized);
std::string serialize_operational_configuration(
    const OperationalConfiguration& configuration);
bool equivalent_operational_configuration(
    const OperationalConfiguration& left,
    const OperationalConfiguration& right) noexcept;

}  // namespace sfx::application
