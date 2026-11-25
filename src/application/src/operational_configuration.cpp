#include "sfx/application/operational_configuration.hpp"

#include "sfx/core/node.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace sfx::application {
namespace {

constexpr std::array<std::string_view, 43> kKeys{
    "format_version",
    "configuration_revision",
    "bbs_name",
    "bbs_time_zone",
    "logical_nodes",
    "ownership_mode",
    "password_mode",
    "allow_new_users",
    "hostname_lookup",
    "caller_afk_minutes",
    "maximum_connections_per_source",
    "maximum_sessions_per_account",
    "source_connection_exceptions",
    "telnet_enabled",
    "telnet_bind",
    "telnet_port",
    "telnet_bot_gate",
    "ssh_enabled",
    "ssh_bind",
    "ssh_port",
    "security_policy_revision",
    "botgate_enabled", "ssh_bot_gate", "botgate_timeout_seconds",
    "botgate_manual_blocks", "botgate_trusted_sources", "botgate_feed_exceptions", "botgate_domains",
    "runtime_log_mib", "runtime_log_archives", "callers_log_mib", "callers_log_archives",
    "attention_log_mib", "attention_log_archives",
    "login_afk_seconds",
    "menu_text_color", "menu_input_color", "menu_lightbars",
    "menu_selected_foreground", "menu_selected_background",
    "menu_footer",
    "event_sounds",
    "legacy_message_pipe_colors",
};

bool bounded_text(
    const std::string_view value,
    const std::size_t maximum,
    const bool ascii_only) noexcept {
    if (value.empty() || value.size() > maximum || value.front() == ' ' ||
        value.back() == ' ') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [ascii_only](const unsigned char value_byte) {
        if (value_byte == 0 || value_byte == '\r' || value_byte == '\n' ||
            value_byte == '=' || value_byte < 0x20U || value_byte == 0x7fU) {
            return false;
        }
        return !ascii_only || value_byte <= 0x7eU;
    });
}

bool valid_hash(const std::string_view value) noexcept {
    return value.size() == 64U &&
        std::all_of(value.begin(), value.end(), [](const unsigned char character) {
            return (character >= static_cast<unsigned char>('0') &&
                    character <= static_cast<unsigned char>('9')) ||
                (character >= static_cast<unsigned char>('a') &&
                    character <= static_cast<unsigned char>('f'));
        });
}

bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index++]);
        if (first <= 0x7fU) continue;
        std::size_t trailing = 0;
        unsigned char second_minimum = 0x80U;
        unsigned char second_maximum = 0xbfU;
        if (first >= 0xc2U && first <= 0xdfU) trailing = 1;
        else if (first >= 0xe0U && first <= 0xefU) {
            trailing = 2;
            if (first == 0xe0U) second_minimum = 0xa0U;
            if (first == 0xedU) second_maximum = 0x9fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            trailing = 3;
            if (first == 0xf0U) second_minimum = 0x90U;
            if (first == 0xf4U) second_maximum = 0x8fU;
        } else {
            return false;
        }
        if (index + trailing > value.size()) return false;
        const auto second = static_cast<unsigned char>(value[index]);
        if (second < second_minimum || second > second_maximum) return false;
        ++index;
        for (std::size_t remaining = 1; remaining < trailing; ++remaining, ++index) {
            const auto continuation = static_cast<unsigned char>(value[index]);
            if (continuation < 0x80U || continuation > 0xbfU) return false;
        }
    }
    return true;
}

bool valid_ipv4(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kTelnetBindMaximum) return false;
    std::size_t start = 0;
    for (std::size_t component = 0; component < 4U; ++component) {
        const std::size_t end = component == 3U ? value.size() : value.find('.', start);
        if (end == std::string_view::npos || end == start || end - start > 3U) return false;
        const std::string_view token = value.substr(start, end - start);
        if (token.size() > 1U && token.front() == '0') return false;
        unsigned int number = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || number > 255U) {
            return false;
        }
        start = end + 1U;
    }
    return start == value.size() + 1U;
}

template <typename Integer>
std::optional<Integer> parse_decimal(const std::string_view value) noexcept {
    if (value.empty() || (value.size() > 1U && value.front() == '0')) return std::nullopt;
    Integer result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
    return result;
}

std::optional<bool> parse_bool(const std::string_view value) noexcept {
    if (value == "true") return true;
    if (value == "false") return false;
    return std::nullopt;
}

void add_error(ConfigurationValidation& result, std::string field, std::string code) {
    result.errors.push_back({std::move(field), std::move(code)});
}

}  // namespace

bool valid_event_sound_path(const std::string_view path) noexcept {
    if(path.empty())return true;
#if defined(__linux__)
    if(path.front()=='/'){
        if(path.size()>kSoundPathMaximumBytes||path.size()<6||!valid_utf8(path))return false;
        for(const auto c:path)if(static_cast<unsigned char>(c)<32||c==127||c=='\\')return false;
        for(std::size_t at=1;at<path.size();){
            const auto end=path.find('/',at);const auto part=path.substr(at,end==path.npos?path.size()-at:end-at);
            if(part.empty()||part=="."||part==".."||part.size()>240)return false;
            if(end==path.npos)break;
            at=end+1;if(at==path.size())return false;
        }
        const auto suffix=path.substr(path.size()-4);
        return suffix[0]=='.'&&(suffix[1]=='w'||suffix[1]=='W')&&(suffix[2]=='a'||suffix[2]=='A')&&(suffix[3]=='v'||suffix[3]=='V');
    }
#endif
    if(path.size()>kSoundPathMaximumBytes || !valid_utf8(path) || path.size()<7 ||
        !((path[0]>='A'&&path[0]<='Z')||(path[0]>='a'&&path[0]<='z')) ||
        path[1]!=':' || path[2]!='\\' || path.find(':',2)!=std::string_view::npos)return false;
    for(const auto c:path)if(static_cast<unsigned char>(c)<32 || c==127 || c=='/' || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|')return false;
    for(std::size_t at=3;at<path.size();) {
        const auto end=path.find('\\',at);const auto part=path.substr(at,end==path.npos?path.size()-at:end-at);
        if(part.empty() || part=="." || part==".." || part.back()=='.' || part.back()==' ')return false;
        if(end==path.npos)break;
        at=end+1;if(at==path.size())return false;
    }
    const auto lower=[](char c){return c>='A'&&c<='Z'?char(c+32):c;};
    const auto suffix=path.substr(path.size()-4);
    return suffix[0]=='.' && lower(suffix[1])=='w' && lower(suffix[2])=='a' && lower(suffix[3])=='v';
}

ConfigurationValidation validate_operational_configuration(
    const OperationalConfiguration& configuration) {
    ConfigurationValidation result;
    for(const auto& path:configuration.event_sounds.paths)
        if(!valid_event_sound_path(path))add_error(result,"event_sounds","invalid_local_wav_path");
    const auto& menu=configuration.menus;
    if (!menu.text_color || menu.text_color>15 || !menu.input_color || menu.input_color>15 ||
        menu.selected_foreground>15 || menu.selected_background>7 ||
        (menu.selected_foreground&7U)==menu.selected_background)
        add_error(result,"menus","invalid_or_indistinguishable_colors");
    if (!valid_botgate_configuration(configuration.botgate))
        add_error(result,"botgate","invalid_policy");
    if (!valid_logging_configuration(configuration.logging))
        add_error(result,"logging","outside_1_100");
    if (configuration.format_version != kOperationalConfigurationFormatVersion) {
        add_error(result, "format_version", "unsupported_version");
    }
    if (configuration.configuration_revision == 0) {
        add_error(result, "configuration_revision", "must_be_positive");
    }
    if (!bounded_text(configuration.bbs_name, kBbsNameMaximum, true)) {
        add_error(result, "bbs_name", "invalid_printable_ascii");
    }
    if (!bounded_text(configuration.bbs_time_zone, kBbsTimeZoneKeyMaximum, false) ||
        !valid_utf8(configuration.bbs_time_zone)) {
        add_error(result, "bbs_time_zone", "invalid_registry_key");
    }
    if (configuration.logical_nodes == 0 ||
        configuration.logical_nodes > core::kMaximumLogicalNodes) {
        add_error(result, "logical_nodes", "outside_1_128");
    }
    if (configuration.ownership_mode != RuntimeOwnershipMode::desktop) {
        add_error(result, "ownership_mode", "desktop_required");
    }
    if (configuration.caller_afk_minutes < 2 || configuration.caller_afk_minutes > 1440)
        add_error(result, "caller_afk_minutes", "outside_2_1440");
    if (configuration.login_afk_seconds < 30 || configuration.login_afk_seconds > 600)
        add_error(result, "login_afk_seconds", "outside_30_600");
    if (configuration.password_mode != PasswordMode::standard &&
        configuration.password_mode != PasswordMode::strong) {
        add_error(result, "password_mode", "unsupported_value");
    }
    if (!valid_connection_limit_policy({configuration.maximum_connections_per_source,
            configuration.maximum_sessions_per_account,configuration.logical_nodes}) ||
        !valid_source_connection_limits(configuration.source_connection_exceptions))
        add_error(result,"connection_policy","invalid_limits_or_exceptions");
    if (!valid_ipv4(configuration.telnet.bind_address)) {
        add_error(result, "telnet_bind", "invalid_ipv4");
    }
    if (configuration.telnet.port == 0) {
        add_error(result, "telnet_port", "outside_1_65535");
    }
    if (!valid_ipv4(configuration.ssh.bind_address)) {
        add_error(result, "ssh_bind", "invalid_ipv4");
    }
    if (configuration.ssh.port == 0) {
        add_error(result, "ssh_port", "outside_1_65535");
    }
    if (configuration.telnet.enabled && configuration.ssh.enabled &&
        configuration.telnet.port == configuration.ssh.port &&
        (configuration.telnet.bind_address == configuration.ssh.bind_address ||
            configuration.telnet.bind_address == "0.0.0.0" ||
            configuration.ssh.bind_address == "0.0.0.0")) {
        add_error(result, "ssh_port", "listener_endpoint_conflict");
    }
    if (configuration.security_policy_revision <= 0) {
        add_error(result, "security_policy_revision", "must_be_positive");
    }
    if (!valid_hash(configuration.security_policy_hash)) {
        add_error(result, "security_policy_hash", "invalid_sha256");
    }
    return result;
}

OperationalConfigurationParseResult parse_operational_configuration(
    const std::string_view serialized) {
    OperationalConfigurationParseResult result;
    if (serialized.empty() || serialized.size() > kOperationalConfigurationMaximumBytes ||
        serialized.find('\0') != std::string_view::npos ||
        serialized.back() != '\n') {
        add_error(result.validation, "document", "invalid_encoding_or_size");
        return result;
    }

    std::map<std::string, std::string> fields;
    std::size_t offset = 0;
    while (offset < serialized.size()) {
        const std::size_t newline = serialized.find('\n', offset);
        if (newline == std::string_view::npos || newline == offset) {
            add_error(result.validation, "document", "invalid_line");
            return result;
        }
        // Accept editor-native CRLF without changing the canonical LF publisher.
        // Strip only a CR immediately paired with this LF; every other CR is
        // malformed. Bounds apply to the original bytes, before normalization.
        std::string_view line = serialized.substr(offset, newline - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.find('\r') != std::string_view::npos) {
            add_error(result.validation, "document", "invalid_line");
            return result;
        }
        const std::size_t delimiter = line.find('=');
        if (delimiter == std::string_view::npos || delimiter == 0 || delimiter + 1U >= line.size() ||
            line.find('=', delimiter + 1U) != std::string_view::npos) {
            add_error(result.validation, "document", "invalid_line");
            return result;
        }
        const std::string key(line.substr(0, delimiter));
        const std::string value(line.substr(delimiter + 1U));
        if (std::find(kKeys.begin(), kKeys.end(), key) == kKeys.end() &&
            key != "security_policy_hash") {
            add_error(result.validation, key, "unknown_field");
            return result;
        }
        if (!fields.emplace(key, value).second) {
            add_error(result.validation, key, "duplicate_field");
            return result;
        }
        offset = newline + 1U;
    }

    const auto version = parse_decimal<std::uint32_t>(fields["format_version"]);
    // Exact older documents receive approved defaults in memory. Mixed or
    // incomplete versions fail closed; the next save writes canonical v13.
    const bool previous_version = version && *version == 1U;
    const bool prior_registration = version && (*version == 1U || *version == 2U);
    const bool prior_hostname = version && *version >= 1U && *version <= 3U;
    const bool prior_revision = version && *version >= 1U && *version <= 4U;
    const bool prior_limits = version && *version >= 1U && *version <= 5U;
    const bool prior_afk = version && *version >= 1U && *version <= 6U;
    const bool prior_botgate = version && *version >= 1U && *version <= 7U;
    const bool prior_login_afk = version && *version >= 1U && *version <= 8U;
    const bool prior_menus = version && *version >= 1U && *version <= 9U;
    const bool prior_footer = version && *version >= 1U && *version <= 10U;
    const bool prior_sounds = version && *version >= 1U && *version <= 11U;
    const bool prior_pipe_mode = version && *version >= 1U && *version <= 12U;
    const std::size_t expected_fields = previous_version ? 12U :
        prior_registration ? 15U : prior_hostname ? 16U :
        prior_revision ? 17U : prior_limits ? 18U : prior_afk ? 21U : prior_botgate ? 22U :
        prior_login_afk ? 35U : prior_menus ? 36U : prior_footer ? 41U : prior_sounds ? 42U :
        prior_pipe_mode ? 43U : kKeys.size() + 1U;
    if(prior_pipe_mode && fields.count("legacy_message_pipe_colors")) {
        add_error(result.validation,"document","mixed_version");return result;
    }
    if(prior_sounds && fields.count("event_sounds")) {
        add_error(result.validation,"document","mixed_version");return result;
    }
    if(prior_footer && fields.count("menu_footer")) {
        add_error(result.validation,"document","mixed_version"); return result;
    }
    if (prior_menus) {
        for (std::size_t index=35;index<kKeys.size();++index) {
            if (fields.count(std::string(kKeys[index]))) {
                add_error(result.validation,"document","mixed_version"); return result;
            }
        }
    }
    if (prior_botgate) {
        for (std::size_t index=21;index<kKeys.size();++index) {
            if (fields.count(std::string(kKeys[index]))) {
                add_error(result.validation,"document","mixed_version"); return result;
            }
        }
    }
    if (fields.size() != expected_fields ||
        (prior_login_afk && fields.count("login_afk_seconds")) ||
        (prior_afk && fields.count("caller_afk_minutes")) ||
        (prior_limits && (fields.count("maximum_connections_per_source") ||
            fields.count("maximum_sessions_per_account") || fields.count("source_connection_exceptions"))) ||
        (prior_registration && fields.count("allow_new_users")) ||
        (prior_hostname && fields.count("hostname_lookup")) ||
        (previous_version && (fields.count("ssh_enabled") || fields.count("ssh_bind") ||
            fields.count("ssh_port")))) {
        add_error(result.validation, "document", "missing_field");
        return result;
    }

    OperationalConfiguration value;
    const auto nodes = parse_decimal<std::uint16_t>(fields["logical_nodes"]);
    const auto port = parse_decimal<std::uint16_t>(fields["telnet_port"]);
    const auto revision = parse_decimal<std::int64_t>(fields["security_policy_revision"]);
    const auto telnet_enabled = parse_bool(fields["telnet_enabled"]);
    const auto bot_gate = parse_bool(fields["telnet_bot_gate"]);
    if (!version || !nodes || !port || !revision || !telnet_enabled || !bot_gate) {
        add_error(result.validation, "document", "invalid_scalar");
        return result;
    }
    value.format_version = prior_pipe_mode ? kOperationalConfigurationFormatVersion : *version;
    if(!prior_sounds) {
        const auto& packed=fields["event_sounds"];std::size_t start{};
        const auto digit=[](char c)->int {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
        for(std::size_t index=0;index<kSoundEventCount;++index) {
            const auto end=packed.find(',',start);
            if((index+1==kSoundEventCount)!=(end==packed.npos)) {
                add_error(result.validation,"event_sounds","invalid_count");return result;
            }
            const auto token=std::string_view(packed).substr(start,end==packed.npos?packed.size()-start:end-start);
            if(token!="-") {
                if(token.empty() || token.size()%2 || token.size()>2*kSoundPathMaximumBytes) {
                    add_error(result.validation,"event_sounds","invalid_encoding");return result;
                }
                auto& path=value.event_sounds.paths[index];
                for(std::size_t byte=0;byte<token.size();byte+=2) {
                    const auto high=digit(token[byte]),low=digit(token[byte+1]);
                    if(high<0 || low<0){add_error(result.validation,"event_sounds","invalid_encoding");return result;}
                    path.push_back(static_cast<char>((high<<4)|low));
                }
            }
            start=end==packed.npos?packed.size():end+1;
        }
    }
    if (!prior_menus) {
        const auto text=parse_decimal<std::uint8_t>(fields["menu_text_color"]);
        const auto input=parse_decimal<std::uint8_t>(fields["menu_input_color"]);
        const auto bars=parse_bool(fields["menu_lightbars"]);
        const auto foreground=parse_decimal<std::uint8_t>(fields["menu_selected_foreground"]);
        const auto background=parse_decimal<std::uint8_t>(fields["menu_selected_background"]);
        if (!text || !input || !bars || !foreground || !background) {
            add_error(result.validation,"menus","invalid_scalar"); return result;
        }
        value.menus={*text,*input,*bars,*foreground,*background};
    }
    if(!prior_footer) {
        if(fields["menu_footer"]!="sf_original" && fields["menu_footer"]!="sfx_new") {
            add_error(result.validation,"menu_footer","invalid_choice"); return result;
        }
        value.menus.compact_footer=fields["menu_footer"]=="sfx_new";
    }
    if(!prior_pipe_mode){
        const auto legacy=parse_bool(fields["legacy_message_pipe_colors"]);
        if(!legacy){add_error(result.validation,"legacy_message_pipe_colors","invalid_scalar");return result;}
        value.menus.legacy_message_pipe_colors=*legacy;
    }
    if (!prior_login_afk) {
        const auto seconds=parse_decimal<std::uint16_t>(fields["login_afk_seconds"]);
        if (!seconds) { add_error(result.validation,"login_afk_seconds","invalid_scalar"); return result; }
        value.login_afk_seconds=*seconds;
    }
    if (!prior_botgate) {
        const auto enabled=parse_bool(fields["botgate_enabled"]);
        const auto ssh_gate=parse_bool(fields["ssh_bot_gate"]);
        const auto timeout=parse_decimal<std::uint16_t>(fields["botgate_timeout_seconds"]);
        if (!enabled || !ssh_gate || !timeout) {
            add_error(result.validation,"botgate","invalid_scalar"); return result;
        }
        value.botgate.enabled=*enabled; value.botgate.ssh_enabled=*ssh_gate;
        value.botgate.timeout_seconds=*timeout;
        value.botgate.manual_blocks=fields["botgate_manual_blocks"];
        value.botgate.trusted_sources=fields["botgate_trusted_sources"];
        value.botgate.feed_exceptions=fields["botgate_feed_exceptions"];
        value.botgate.domains=fields["botgate_domains"];
        for (auto entry : {std::pair<const char*,LogRetention*>{"runtime",&value.logging.runtime},
                {"callers",&value.logging.callers},{"attention",&value.logging.attention}}) {
            auto size=parse_decimal<std::uint16_t>(fields[std::string(entry.first)+"_log_mib"]);
            auto count=parse_decimal<std::uint16_t>(fields[std::string(entry.first)+"_log_archives"]);
            if (!size || !count) { add_error(result.validation,"logging","invalid_scalar"); return result; }
            *entry.second={*size,*count};
        }
    }
    if (!prior_afk) {
        const auto afk = parse_decimal<std::uint16_t>(fields["caller_afk_minutes"]);
        if (!afk) {
            add_error(result.validation, "caller_afk_minutes", "invalid_scalar"); return result;
        }
        value.caller_afk_minutes = *afk;
    }
    if (prior_limits) {
        // Older boards implicitly used four connections per source.
        value.maximum_connections_per_source = 4;
    } else {
        const auto source = parse_decimal<std::uint16_t>(fields["maximum_connections_per_source"]);
        const auto account = parse_decimal<std::uint16_t>(fields["maximum_sessions_per_account"]);
        const auto exceptions = parse_source_connection_limits(fields["source_connection_exceptions"]);
        if (!source || !account || !exceptions) {
            add_error(result.validation,"connection_policy","invalid_scalar"); return result;
        }
        value.maximum_connections_per_source = *source;
        value.maximum_sessions_per_account = *account;
        value.source_connection_exceptions = *exceptions;
    }
    if (!prior_revision) {
        const auto configuration_revision =
            parse_decimal<std::uint64_t>(fields["configuration_revision"]);
        if (!configuration_revision || *configuration_revision == 0) {
            add_error(result.validation, "configuration_revision", "invalid_scalar");
            return result;
        }
        value.configuration_revision = *configuration_revision;
    }
    if (!prior_hostname) {
        const auto lookup = parse_bool(fields["hostname_lookup"]);
        if (!lookup) {
            add_error(result.validation, "hostname_lookup", "invalid_scalar");
            return result;
        }
        value.hostname_lookup = *lookup;
    }
    if (!prior_registration) {
        const auto allow = parse_bool(fields["allow_new_users"]);
        if (!allow) {
            add_error(result.validation, "allow_new_users", "invalid_scalar");
            return result;
        }
        value.allow_new_users = *allow;
    }
    value.bbs_name = fields["bbs_name"];
    value.bbs_time_zone = fields["bbs_time_zone"];
    value.logical_nodes = *nodes;
    value.telnet.enabled = *telnet_enabled;
    value.telnet.bind_address = fields["telnet_bind"];
    value.telnet.port = *port;
    value.telnet.bot_gate_enabled = prior_botgate ? true : *bot_gate;
    if (!previous_version) {
        const auto ssh_enabled = parse_bool(fields["ssh_enabled"]);
        const auto ssh_port = parse_decimal<std::uint16_t>(fields["ssh_port"]);
        if (!ssh_enabled || !ssh_port) {
            add_error(result.validation, "document", "invalid_scalar");
            return result;
        }
        value.ssh.enabled = *ssh_enabled;
        value.ssh.bind_address = fields["ssh_bind"];
        value.ssh.port = *ssh_port;
    }
    value.security_policy_revision = *revision;
    value.security_policy_hash = fields["security_policy_hash"];
    if (fields["ownership_mode"] != "desktop") {
        add_error(result.validation, "ownership_mode", "unsupported_value");
    }
    if (fields["password_mode"] == "standard") value.password_mode = PasswordMode::standard;
    else if (fields["password_mode"] == "strong") value.password_mode = PasswordMode::strong;
    else add_error(result.validation, "password_mode", "unsupported_value");

    const ConfigurationValidation checked = validate_operational_configuration(value);
    result.validation.errors.insert(result.validation.errors.end(),
        checked.errors.begin(), checked.errors.end());
    if (result.validation.valid()) result.configuration = std::move(value);
    return result;
}

std::string serialize_operational_configuration(
    const OperationalConfiguration& configuration) {
    if (!validate_operational_configuration(configuration).valid()) return {};
    std::string output;
    output.reserve(512U);
    output += "format_version=" + std::to_string(configuration.format_version) + "\n";
    output += "configuration_revision=" +
        std::to_string(configuration.configuration_revision) + "\n";
    output += "bbs_name=" + configuration.bbs_name + "\n";
    output += "bbs_time_zone=" + configuration.bbs_time_zone + "\n";
    output += "logical_nodes=" + std::to_string(configuration.logical_nodes) + "\n";
    output += "ownership_mode=desktop\n";
    output += configuration.password_mode == PasswordMode::strong
        ? "password_mode=strong\n" : "password_mode=standard\n";
    output += configuration.allow_new_users ? "allow_new_users=true\n" : "allow_new_users=false\n";
    output += configuration.hostname_lookup ? "hostname_lookup=true\n" : "hostname_lookup=false\n";
    output += "caller_afk_minutes=" + std::to_string(configuration.caller_afk_minutes) + "\n";
    output += "login_afk_seconds=" + std::to_string(configuration.login_afk_seconds) + "\n";
    output += "menu_text_color="+std::to_string(configuration.menus.text_color)+"\n";
    output += "menu_input_color="+std::to_string(configuration.menus.input_color)+"\n";
    output += configuration.menus.lightbars_enabled ? "menu_lightbars=true\n" : "menu_lightbars=false\n";
    output += "menu_selected_foreground="+std::to_string(configuration.menus.selected_foreground)+"\n";
    output += "menu_selected_background="+std::to_string(configuration.menus.selected_background)+"\n";
    output += configuration.menus.compact_footer ? "menu_footer=sfx_new\n" : "menu_footer=sf_original\n";
    output += configuration.menus.legacy_message_pipe_colors ? "legacy_message_pipe_colors=true\n" : "legacy_message_pipe_colors=false\n";
    output += "event_sounds=";
    for(std::size_t i=0;i<kSoundEventCount;++i) {
        if(i)output+=',';
        const auto& path=configuration.event_sounds.paths[i];
        if(path.empty())output+='-';
        else for(const unsigned char c:path) {output+="0123456789abcdef"[c>>4U];output+="0123456789abcdef"[c&15U];}
    }
    output+='\n';
    output += "maximum_connections_per_source=" + std::to_string(configuration.maximum_connections_per_source) + "\n";
    output += "maximum_sessions_per_account=" + std::to_string(configuration.maximum_sessions_per_account) + "\n";
    output += "source_connection_exceptions=" + serialize_source_connection_limits(configuration.source_connection_exceptions) + "\n";
    output += configuration.telnet.enabled ? "telnet_enabled=true\n" : "telnet_enabled=false\n";
    output += "telnet_bind=" + configuration.telnet.bind_address + "\n";
    output += "telnet_port=" + std::to_string(configuration.telnet.port) + "\n";
    output += configuration.telnet.bot_gate_enabled
        ? "telnet_bot_gate=true\n" : "telnet_bot_gate=false\n";
    output += configuration.ssh.enabled ? "ssh_enabled=true\n" : "ssh_enabled=false\n";
    output += "ssh_bind=" + configuration.ssh.bind_address + "\n";
    output += "ssh_port=" + std::to_string(configuration.ssh.port) + "\n";
    output += configuration.botgate.enabled ? "botgate_enabled=true\n" : "botgate_enabled=false\n";
    output += configuration.botgate.ssh_enabled ? "ssh_bot_gate=true\n" : "ssh_bot_gate=false\n";
    output += "botgate_timeout_seconds="+std::to_string(configuration.botgate.timeout_seconds)+"\n";
    output += "botgate_manual_blocks="+configuration.botgate.manual_blocks+"\n";
    output += "botgate_trusted_sources="+configuration.botgate.trusted_sources+"\n";
    output += "botgate_feed_exceptions="+configuration.botgate.feed_exceptions+"\n";
    output += "botgate_domains="+configuration.botgate.domains+"\n";
    for (auto entry : {std::pair<const char*,LogRetention>{"runtime",configuration.logging.runtime},
            {"callers",configuration.logging.callers},{"attention",configuration.logging.attention}}) {
        output += std::string(entry.first)+"_log_mib="+std::to_string(entry.second.size_mib)+"\n";
        output += std::string(entry.first)+"_log_archives="+std::to_string(entry.second.archives)+"\n";
    }
    output += "security_policy_revision=" +
        std::to_string(configuration.security_policy_revision) + "\n";
    output += "security_policy_hash=" + configuration.security_policy_hash + "\n";
    return output.size() <= kOperationalConfigurationMaximumBytes ? output : std::string{};
}

bool equivalent_operational_configuration(
    const OperationalConfiguration& left,
    const OperationalConfiguration& right) noexcept {
    return left.format_version == right.format_version &&
        left.event_sounds == right.event_sounds &&
        left.menus == right.menus &&
        equivalent_botgate_configuration(left.botgate,right.botgate) && left.logging==right.logging &&
        left.configuration_revision == right.configuration_revision &&
        left.bbs_name == right.bbs_name && left.bbs_time_zone == right.bbs_time_zone &&
        left.logical_nodes == right.logical_nodes && left.ownership_mode == right.ownership_mode &&
        left.password_mode == right.password_mode &&
        left.allow_new_users == right.allow_new_users &&
        left.hostname_lookup == right.hostname_lookup &&
        left.caller_afk_minutes == right.caller_afk_minutes &&
        left.login_afk_seconds == right.login_afk_seconds &&
        left.maximum_connections_per_source == right.maximum_connections_per_source &&
        left.maximum_sessions_per_account == right.maximum_sessions_per_account &&
        left.source_connection_exceptions == right.source_connection_exceptions &&
        left.telnet.enabled == right.telnet.enabled &&
        left.telnet.bind_address == right.telnet.bind_address &&
        left.telnet.port == right.telnet.port &&
        left.telnet.bot_gate_enabled == right.telnet.bot_gate_enabled &&
        left.ssh.enabled == right.ssh.enabled &&
        left.ssh.bind_address == right.ssh.bind_address &&
        left.ssh.port == right.ssh.port &&
        left.security_policy_revision == right.security_policy_revision &&
        left.security_policy_hash == right.security_policy_hash;
}

}  // namespace sfx::application
