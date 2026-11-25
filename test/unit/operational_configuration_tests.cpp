#include "sfx/application/operational_configuration.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace app = sfx::application;
unsigned assertions = 0;
app::OperationalConfiguration valid_configuration();

std::string legacy_v12(std::string serialized) {
    serialized.replace(serialized.find("format_version=13"),17,"format_version=12");
    const auto at=serialized.find("legacy_message_pipe_colors=");
    serialized.erase(at,serialized.find('\n',at)-at+1);
    return serialized;
}
std::string legacy_v9(std::string serialized) {
    serialized=legacy_v12(std::move(serialized));
    serialized.replace(serialized.find("format_version=12"),17,"format_version=9");
    const auto sound_at=serialized.find("event_sounds=");serialized.erase(sound_at,serialized.find('\n',sound_at)-sound_at+1);
    for(const auto* key:{"menu_text_color=","menu_input_color=","menu_lightbars=",
            "menu_selected_foreground=","menu_selected_background=","menu_footer="}) {
        const auto start=serialized.find(key);
        serialized.erase(start,serialized.find('\n',start)-start+1);
    }
    return serialized;
}
std::string legacy_v7(std::string serialized) {
    serialized=legacy_v9(std::move(serialized));
    serialized.replace(serialized.find("format_version=9"),16,"format_version=7");
    for (const auto* key : {"botgate_enabled=","ssh_bot_gate=","botgate_timeout_seconds=",
            "botgate_manual_blocks=","botgate_trusted_sources=","botgate_feed_exceptions=","botgate_domains=",
            "runtime_log_mib=","runtime_log_archives=","callers_log_mib=","callers_log_archives=",
            "attention_log_mib=","attention_log_archives=","login_afk_seconds="}) {
        const auto start=serialized.find(key);
        serialized.erase(start,serialized.find('\n',start)-start+1);
    }
    return serialized;
}
std::string legacy_v5(std::string serialized) {
    serialized=legacy_v7(std::move(serialized));
    serialized.replace(serialized.find("format_version=7"),16,"format_version=5");
    for (const auto* key : {"caller_afk_minutes=", "maximum_connections_per_source=","maximum_sessions_per_account=","source_connection_exceptions="}) {
        const auto start = serialized.find(key);
        serialized.erase(start,serialized.find('\n',start)-start+1);
    }
    return serialized;
}

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++assertions;
}

std::string editor_crlf(std::string_view text) {
    std::string result;
    for (const char c : text) {
        if (c == '\n') result += '\r';
        result += c;
    }
    return result;
}

void newline_compatibility_tests() {
    const auto value = valid_configuration();
    const auto canonical = app::serialize_operational_configuration(value);
    auto mixed = canonical;
    mixed.insert(mixed.find('\n'), 1, '\r');
    for (const auto& text : {canonical, editor_crlf(canonical), mixed}) {
        const auto parsed = app::parse_operational_configuration(text);
        require(parsed.valid(), "LF/CRLF and paired mixed newlines parse");
        require(app::equivalent_operational_configuration(value, *parsed.configuration),
            "editor-only newline changes preserve every configuration field");
        require(app::serialize_operational_configuration(*parsed.configuration) == canonical,
            "publisher always returns canonical LF, including editor round trip");
        auto malformed = text;
        malformed.insert(malformed.find('='), 1, '\r');
        require(!app::parse_operational_configuration(malformed).valid(), "bare CR in key rejects");
        malformed = text;
        malformed.insert(malformed.find('=') + 1, 1, '\r');
        require(!app::parse_operational_configuration(malformed).valid(), "bare CR in value rejects");
        malformed = text;
        malformed.insert(malformed.find('\n'), "\r\r");
        require(!app::parse_operational_configuration(malformed).valid(), "CRCRLF rejects");
        malformed = text;
        malformed.insert(0, 1, '\0');
        require(!app::parse_operational_configuration(malformed).valid(), "embedded NUL rejects");
        for (const auto* extra : {"bbs_name=duplicate\r\n", "unknown=value\r\n", "\r\n", "bad\r\n"})
            require(!app::parse_operational_configuration(text + extra).valid(),
                "duplicate/unknown/blank/malformed CRLF records reject");
        require(!app::parse_operational_configuration(text.substr(0, text.size() - 1)).valid(),
            "unterminated final line rejects");
    }
    require(!app::parse_operational_configuration(std::string(
        app::kOperationalConfigurationMaximumBytes + 1, '\n')).valid(), "raw oversized input rejects");
}

app::OperationalConfiguration valid_configuration() {
    app::OperationalConfiguration value;
    value.bbs_time_zone = "Pacific Standard Time";
    value.security_policy_revision = 1;
    value.security_policy_hash =
        "71cb5a767e365786bbedaa779e4dc743473fb70c2b9dd3a99e4f5ce154062a95";
    return value;
}

void round_trip_tests() {
    const auto source = valid_configuration();
    const std::string serialized = app::serialize_operational_configuration(source);
    require(!serialized.empty(), "valid operational configuration serializes");
    const auto parsed = app::parse_operational_configuration(serialized);
    require(parsed.valid(), "canonical operational configuration parses");
    require(app::equivalent_operational_configuration(source, *parsed.configuration),
        "canonical operational configuration round trips");
    require(app::serialize_operational_configuration(*parsed.configuration) == serialized,
        "canonical serialization is stable");
}

void rejection_tests() {
    const std::string canonical = app::serialize_operational_configuration(valid_configuration());
    require(!app::parse_operational_configuration(
        canonical + "unknown=value\n").valid(), "unknown key rejects");
    require(!app::parse_operational_configuration(
        canonical + "bbs_name=Duplicate\n").valid(), "duplicate key rejects");
    std::string missing = canonical;
    missing.erase(missing.find("telnet_port="), std::string("telnet_port=2323\n").size());
    require(!app::parse_operational_configuration(missing).valid(), "missing key rejects");

    auto invalid = valid_configuration();
    invalid.logical_nodes = 129;
    require(!app::validate_operational_configuration(invalid).valid(), "node count is bounded");
    invalid = valid_configuration();
    invalid.telnet.bind_address = "01.2.3.4";
    require(!app::validate_operational_configuration(invalid).valid(), "IPv4 is canonical");
    invalid = valid_configuration();
    invalid.bbs_name = " bad";
    require(!app::validate_operational_configuration(invalid).valid(), "BBS name is normalized");
    invalid = valid_configuration();
    invalid.bbs_time_zone = std::string("Pacific") + static_cast<char>(0xc0) +
        static_cast<char>(0xaf);
    require(!app::validate_operational_configuration(invalid).valid(),
        "malformed UTF-8 time-zone identifier rejects");
    invalid = valid_configuration();
    invalid.security_policy_hash[0] = 'A';
    require(!app::validate_operational_configuration(invalid).valid(), "policy hash is lowercase SHA-256");
}

void ssh_configuration_tests() {
    auto config = valid_configuration();
    require(config.ssh.enabled && config.ssh.bind_address == "0.0.0.0" && config.ssh.port == 2222,
        "SSH defaults are enabled, all IPv4 interfaces, port 2222");
    const auto canonical = app::serialize_operational_configuration(config);
    std::string old = legacy_v5(canonical);
    old.replace(old.find("format_version=5"), 16, "format_version=1");
    old.erase(old.find("configuration_revision="),
        std::string("configuration_revision=1\n").size());
    old.erase(old.find("hostname_lookup="), std::string("hostname_lookup=true\n").size());
    old.erase(old.find("allow_new_users="), std::string("allow_new_users=true\n").size());
    const auto ssh_start = old.find("ssh_enabled=");
    const auto ssh_end = old.find("security_policy_revision=");
    old.erase(ssh_start, ssh_end - ssh_start);
    auto legacy_defaults = config;
    legacy_defaults.maximum_connections_per_source = 4;
    const auto upgraded = app::parse_operational_configuration(old);
    require(upgraded.valid() && app::equivalent_operational_configuration(legacy_defaults, *upgraded.configuration),
        "exact prior Alpha configuration receives SSH defaults without changing Telnet");
    const auto upgraded_crlf = app::parse_operational_configuration(editor_crlf(old));
    require(upgraded_crlf.valid() && app::equivalent_operational_configuration(legacy_defaults, *upgraded_crlf.configuration),
        "exact prior v1 CRLF configuration receives identical validated defaults");
    require(!app::parse_operational_configuration(old + "ssh_port=2222\n").valid(),
        "mixed-version SSH configuration is rejected");
    config.ssh.enabled = false;
    config.ssh.bind_address = "127.0.0.1";
    config.ssh.port = 2200;
    const auto disabled = app::parse_operational_configuration(app::serialize_operational_configuration(config));
    require(disabled.valid() && !disabled.configuration->ssh.enabled && disabled.configuration->ssh.port == 2200,
        "disabled SSH retains configured endpoint through serialization");
    config.ssh.port = 0;
    require(!app::validate_operational_configuration(config).valid(), "zero SSH port rejects");
    config = valid_configuration();
    config.ssh.bind_address = "01.0.0.1";
    require(!app::validate_operational_configuration(config).valid(), "noncanonical SSH IPv4 rejects");
    config = valid_configuration();
    config.ssh.port = config.telnet.port;
    require(!app::validate_operational_configuration(config).valid(), "conflicting enabled listeners reject");
}

void registration_configuration_tests() {
    auto config = valid_configuration();
    require(config.allow_new_users, "new installations allow registration");
    const auto canonical = app::serialize_operational_configuration(config);
    auto old = legacy_v5(canonical);
    old.replace(old.find("format_version=5"), 16, "format_version=2");
    old.erase(old.find("configuration_revision="),
        std::string("configuration_revision=1\n").size());
    old.erase(old.find("hostname_lookup="), std::string("hostname_lookup=true\n").size());
    old.erase(old.find("allow_new_users="), std::string("allow_new_users=true\n").size());
    auto legacy_defaults = config;
    legacy_defaults.maximum_connections_per_source = 4;
    const auto upgraded = app::parse_operational_configuration(old);
    require(upgraded.valid() && app::equivalent_operational_configuration(legacy_defaults, *upgraded.configuration),
        "prior SSH candidate upgrades to registration enabled without other changes");
    const auto upgraded_crlf = app::parse_operational_configuration(editor_crlf(old));
    require(upgraded_crlf.valid() && app::equivalent_operational_configuration(legacy_defaults, *upgraded_crlf.configuration),
        "exact prior v2 CRLF configuration receives identical validated defaults");
    require(!app::parse_operational_configuration(old + "allow_new_users=false\n").valid(),
        "mixed-version registration setting fails closed");
    auto missing = canonical;
    missing.erase(missing.find("allow_new_users="), std::string("allow_new_users=true\n").size());
    require(!app::parse_operational_configuration(missing).valid(), "v3 requires explicit registration setting");
    config.allow_new_users = false;
    const auto closed = app::parse_operational_configuration(app::serialize_operational_configuration(config));
    require(closed.valid() && !closed.configuration->allow_new_users &&
        !app::equivalent_operational_configuration(config, valid_configuration()),
        "closed-to-new-callers setting persists and participates in equivalence");
}

void hostname_configuration_tests() {
    auto config = valid_configuration();
    require(config.hostname_lookup && config.format_version == app::kOperationalConfigurationFormatVersion &&
        config.configuration_revision == 1,
        "hostname default On with revisioned format 8");
    const auto canonical = app::serialize_operational_configuration(config);
    auto old = legacy_v5(canonical);
    old.replace(old.find("format_version=5"), 16, "format_version=3");
    old.erase(old.find("configuration_revision="),
        std::string("configuration_revision=1\n").size());
    old.erase(old.find("hostname_lookup="), std::string("hostname_lookup=true\n").size());
    auto legacy_defaults = config;
    legacy_defaults.maximum_connections_per_source = 4;
    for (const auto& text : {old, editor_crlf(old)}) {
        const auto upgraded = app::parse_operational_configuration(text);
        require(upgraded.valid() && app::equivalent_operational_configuration(legacy_defaults, *upgraded.configuration),
            "exact LF/CRLF v3 defaults to On without changing other settings");
    }
    require(!app::parse_operational_configuration(old + "hostname_lookup=false\n").valid(), "mixed v3/v4 rejects");
    auto missing = canonical; missing.erase(missing.find("hostname_lookup="), std::string("hostname_lookup=true\n").size());
    require(!app::parse_operational_configuration(missing).valid(), "v5 requires hostname field");
    for (const auto* invalid : {"yes", "1", "TRUE", "false ", "", "false=true"}) {
        auto text = canonical; text.replace(text.find("hostname_lookup=true"), 20, std::string("hostname_lookup=") + invalid);
        require(!app::parse_operational_configuration(text).valid(), "strict hostname boolean");
    }
    config.hostname_lookup = false;
    for (const auto& text : {app::serialize_operational_configuration(config), editor_crlf(app::serialize_operational_configuration(config))}) {
        const auto parsed = app::parse_operational_configuration(text);
        require(parsed.valid() && !parsed.configuration->hostname_lookup &&
            app::equivalent_operational_configuration(config, *parsed.configuration), "Off persists through LF/CRLF");
    }
    require(!app::equivalent_operational_configuration(config, valid_configuration()), "hostname participates in staged equivalence");

    auto prior_v4 = legacy_v5(canonical);
    prior_v4.replace(prior_v4.find("format_version=5"), 16, "format_version=4");
    prior_v4.erase(prior_v4.find("configuration_revision="),
        std::string("configuration_revision=1\n").size());
    const auto upgraded_v4 = app::parse_operational_configuration(prior_v4);
    require(upgraded_v4.valid() && upgraded_v4.configuration->format_version == app::kOperationalConfigurationFormatVersion &&
        upgraded_v4.configuration->configuration_revision == 1 &&
        app::equivalent_operational_configuration(legacy_defaults,
            *upgraded_v4.configuration),
        "exact prior v4 upgrades to revisioned format 8 in memory");

    config = valid_configuration();
    config.configuration_revision = 2;
    require(!app::equivalent_operational_configuration(config, valid_configuration()),
        "configuration revision participates in exact stored-state equivalence");
    config.configuration_revision = 0;
    require(!app::validate_operational_configuration(config).valid(),
        "zero configuration revision fails closed");
}
void connection_policy_tests() {
    auto config = valid_configuration();
    const auto fresh = app::parse_operational_configuration(app::serialize_operational_configuration(config));
    require(fresh.valid() && fresh.configuration->maximum_connections_per_source == 2,
        "fresh boards allow two connections per source");
    const auto old = legacy_v5(app::serialize_operational_configuration(config));
    const auto upgraded = app::parse_operational_configuration(old);
    require(upgraded.valid() && upgraded.configuration->maximum_connections_per_source == 4 &&
        upgraded.configuration->maximum_sessions_per_account == 1 && upgraded.configuration->source_connection_exceptions.empty(),
        "exact v5 retains four-source/one-account defaults with no exceptions");
    require(!app::parse_operational_configuration(old + "maximum_sessions_per_account=2\n").valid(),"mixed v5/v6 rejects");
    config.maximum_connections_per_source = 8; config.maximum_sessions_per_account = 3;
    config.source_connection_exceptions = {{"127.0.0.1",16},{"192.0.2.1",1},{"2001:db8::1",2}};
    const auto wire = app::serialize_operational_configuration(config);
    const auto parsed = app::parse_operational_configuration(wire);
    require(parsed.valid() && app::equivalent_operational_configuration(config,*parsed.configuration),
        "bounded source/account policy and separate exact-IP overrides round trip");
    for (const auto* invalid : {"", "host.invalid|2", "192.0.2.1/24|2", "192.0.2.1|0", "192.0.2.1|129",
            "192.0.2.1|02", "192.0.2.1|2,", "192.0.2.1|2,192.0.2.1|3", "192.0.2.2|2,192.0.2.1|2", "192.0.002.1|2"})
        require(!app::parse_source_connection_limits(invalid),"ambiguous/noncanonical/unbounded source override refuses");
    config.maximum_sessions_per_account = 0;
    require(!app::validate_operational_configuration(config).valid(),"zero is not unlimited account authority");
    config.maximum_sessions_per_account = 128; config.maximum_connections_per_source = 129;
    require(!app::validate_operational_configuration(config).valid(),"source policy bounded at 128");
    config.maximum_connections_per_source = 128; config.source_connection_exceptions.clear();
    for (unsigned index=0;index<32;++index) config.source_connection_exceptions.push_back({"2001:db8::"+std::to_string(index+100),128});
    const auto maximum = app::serialize_operational_configuration(config);
    require(!maximum.empty() && maximum.size() <= app::kOperationalConfigurationMaximumBytes &&
        app::parse_operational_configuration(maximum).valid(),"32 canonical IPv6 overrides remain within bounded configuration");
    std::string dense;
    for (unsigned i=0;i<32;++i) { if (!dense.empty()) dense+=';'; dense+="192.168.100."+std::to_string(i+100); }
    config.botgate.manual_blocks=dense; config.botgate.trusted_sources=dense; config.botgate.feed_exceptions=dense;
    config.botgate.domains="-";
    const auto combined=app::serialize_operational_configuration(config);
    require(!combined.empty() && app::parse_operational_configuration(combined).valid(),
        "maximum independent BotGate lists coexist with 32 existing source exceptions");
    config.source_connection_exceptions.push_back({"2001:db8::999",1});
    require(!app::validate_operational_configuration(config).valid(),"33rd source override refuses");
}
}  // namespace

int main() {
    try {
        {
        auto sounds=valid_configuration();
        for(const auto& p:sounds.event_sounds.paths)require(p.empty(),"all ten new sound assignments are silent");
        auto v11=legacy_v12(app::serialize_operational_configuration(sounds));
        v11.replace(v11.find("format_version=12"),17,"format_version=11");
        const auto sounds_at=v11.find("event_sounds=");v11.erase(sounds_at,v11.find('\n',sounds_at)-sounds_at+1);
        const auto migrated=app::parse_operational_configuration(v11);
        require(migrated.valid() && app::equivalent_operational_configuration(sounds,*migrated.configuration),"exact v11 receives silent mappings with other values preserved");
        require(!app::parse_operational_configuration(v11+"event_sounds=-,-,-,-,-,-,-,-,-,-\n").valid(),"mixed v11/sound format refuses");
        for(std::size_t i=0;i<app::kSoundEventCount;++i)sounds.event_sounds.paths[i]="C:\\Sounds\\event="+std::to_string(i)+",.wav";
        sounds.event_sounds.paths[9]="C:\\Sounds\\caf\xc3\xa9.wav";
        const auto round=app::parse_operational_configuration(app::serialize_operational_configuration(sounds));
        require(round.valid() && round.configuration->event_sounds==sounds.event_sounds,"all ten distinct paths delimiters and UTF8 roundtrip");
        for(const std::string& p:std::vector<std::string>{"\\\\server\\x.wav","C:\\..\\x.wav","x.wav","C:\\x.wav:stream","C:\\x.exe","C:\\x.wav\\","C:\\\\x.wav","C:\\x.\\x.wav",std::string("C:\\x\0.wav",9)})require(!app::valid_event_sound_path(p),"unsafe path grammar refuses");
        auto packed=app::serialize_operational_configuration(sounds);const auto at=packed.find("event_sounds=");packed[at+13]='z';
        require(!app::parse_operational_configuration(packed).valid(),"invalid path hex refuses");
        }
        auto menus=valid_configuration();
        const auto v12=legacy_v12(app::serialize_operational_configuration(menus));
        const auto migrated_pipe_mode=app::parse_operational_configuration(v12);
        require(migrated_pipe_mode.valid() && !migrated_pipe_mode.configuration->menus.legacy_message_pipe_colors,
            "old board configuration defaults to Synchronet-style terminal pipes");
        require(!app::parse_operational_configuration(v12+"legacy_message_pipe_colors=true\n").valid(),"mixed v12 pipe mode refuses");
        menus.menus.legacy_message_pipe_colors=true;
        const auto legacy_pipe_mode=app::parse_operational_configuration(app::serialize_operational_configuration(menus));
        require(legacy_pipe_mode.valid() && app::equivalent_operational_configuration(menus,*legacy_pipe_mode.configuration),"legacy pipe mode saved configuration roundtrips");
        menus.menus.legacy_message_pipe_colors=false;
        auto v10=legacy_v12(app::serialize_operational_configuration(menus));
        v10.replace(v10.find("format_version=12"),17,"format_version=10");
        const auto sound_at=v10.find("event_sounds=");v10.erase(sound_at,v10.find('\n',sound_at)-sound_at+1);
        const auto footer_at=v10.find("menu_footer=");v10.erase(footer_at,v10.find('\n',footer_at)-footer_at+1);
        const auto old_footer=app::parse_operational_configuration(v10);
        require(old_footer.valid() && !old_footer.configuration->menus.compact_footer,"v10 and fresh installs default SF Original");
        require(!app::parse_operational_configuration(v10+"menu_footer=sfx_new\n").valid(),"mixed old/footer version refuses");
        menus.menus.compact_footer=true;
        const auto new_footer=app::parse_operational_configuration(app::serialize_operational_configuration(menus));
        require(new_footer.valid() && new_footer.configuration->menus.compact_footer,"SFX New footer roundtrips");
        menus.menus.compact_footer=false;
        const auto v9=legacy_v9(app::serialize_operational_configuration(menus));
        const auto prior_menu_config=app::parse_operational_configuration(v9);
        require(prior_menu_config.valid() && prior_menu_config.configuration->menus==menus.menus &&
            app::equivalent_operational_configuration(menus,*prior_menu_config.configuration),"exact v9 receives approved menu defaults");
        require(!app::parse_operational_configuration(v9+"menu_text_color=7\n").valid(),"mixed v9/menu format rejects");
        for(unsigned color=1;color<16;++color) {
            menus.menus.text_color=static_cast<std::uint8_t>(color);
            menus.menus.input_color=static_cast<std::uint8_t>(16-color);
            menus.menus.lightbars_enabled=(color%2)==0;
            menus.menus.selected_foreground=15;menus.menus.selected_background=1;
            const auto parsed=app::parse_operational_configuration(app::serialize_operational_configuration(menus));
            require(parsed.valid() && app::equivalent_operational_configuration(menus,*parsed.configuration),"named ANSI roles and light-bar settings roundtrip");
        }
        for(unsigned color:{0U,16U,255U}) {
            menus=valid_configuration();menus.menus.text_color=static_cast<std::uint8_t>(color);
            require(!app::validate_operational_configuration(menus).valid(),"black/invalid default text refuses");
            menus=valid_configuration();menus.menus.input_color=static_cast<std::uint8_t>(color);
            require(!app::validate_operational_configuration(menus).valid(),"black/invalid input refuses");
        }
        menus=valid_configuration();menus.menus.selected_foreground=7;
        require(!app::validate_operational_configuration(menus).valid(),"indistinguishable selection refuses");
        menus=valid_configuration();menus.menus.selected_background=8;
        require(!app::validate_operational_configuration(menus).valid(),"unsupported bright background refuses");
        auto afk = valid_configuration();
        const auto v8=app::serialize_operational_configuration(afk);
        const auto v7=legacy_v7(v8);
        const auto migrated7=app::parse_operational_configuration(v7);
        require(migrated7.valid() && migrated7.configuration->format_version==app::kOperationalConfigurationFormatVersion &&
            migrated7.configuration->botgate.enabled && !migrated7.configuration->botgate.ssh_enabled &&
            migrated7.configuration->telnet.bot_gate_enabled &&
            migrated7.configuration->botgate.timeout_seconds==20 &&
            migrated7.configuration->logging.runtime.size_mib==10 &&
            migrated7.configuration->logging.attention.archives==10,"v7 without a stored SSH choice supplies registration challenge Off");
        require(!afk.botgate.ssh_enabled,"fresh SSH registration challenge defaults Off");
        for(const bool old_choice:{false,true}) {
            auto prior=valid_configuration(); prior.botgate.ssh_enabled=old_choice;
            const auto migrated=app::parse_operational_configuration(app::serialize_operational_configuration(prior));
            require(migrated.valid() && migrated.configuration->botgate.ssh_enabled==old_choice,
                "explicit existing SSH Off/On survives compatible upgrade exactly");
            require(app::botgate_filters_enabled(migrated.configuration->botgate,false,true),
                "upgraded SSH Off/On both intentionally filter when global BotGate is On");
        }
        require(!app::parse_operational_configuration(v7+"botgate_enabled=true\n").valid(),"mixed v7/v8 rejected");
        auto changed=afk;
        changed.botgate.manual_blocks="192.0.2.0/24";
        changed.botgate.enabled=false; changed.logging.callers={2,3};
        const auto migrated8=app::parse_operational_configuration(app::serialize_operational_configuration(changed));
        require(migrated8.valid() && app::equivalent_operational_configuration(changed,*migrated8.configuration),
            "BotGate and per-log policy roundtrip");
        auto v6 = v7;
        v6.replace(v6.find("format_version=7"), 16, "format_version=6");
        const auto afk_line = v6.find("caller_afk_minutes=");
        v6.erase(afk_line, v6.find('\n', afk_line) - afk_line + 1);
        const auto migrated = app::parse_operational_configuration(v6);
        require(migrated.valid() && migrated.configuration->caller_afk_minutes == 5 &&
            migrated.configuration->format_version == app::kOperationalConfigurationFormatVersion, "exact v6 defaults to five minute AFK in current memory");
        auto old_off=v7;
        old_off.replace(old_off.find("telnet_bot_gate=true"),20,"telnet_bot_gate=false");
        const auto migrated_off=app::parse_operational_configuration(old_off);
        require(migrated_off.valid() && migrated_off.configuration->telnet.bot_gate_enabled,
            "pre-BotGate Off becomes first-introduction On");
        auto old8=legacy_v9(app::serialize_operational_configuration(afk));
        old8.replace(old8.find("format_version=9"),16,"format_version=8");
        const auto login_line=old8.find("login_afk_seconds=");
        old8.erase(login_line,old8.find('\n',login_line)-login_line+1);
        old8.replace(old8.find("telnet_bot_gate=true"),20,"telnet_bot_gate=false");
        const auto kept_off=app::parse_operational_configuration(old8);
        require(kept_off.valid() && !kept_off.configuration->telnet.bot_gate_enabled &&
            kept_off.configuration->login_afk_seconds==60,"existing BotGate choice preserved; login defaults 60s");
        for (const unsigned seconds : {0U,29U,601U,65535U}) {
            auto invalid=afk; invalid.login_afk_seconds=static_cast<std::uint16_t>(seconds);
            require(!app::validate_operational_configuration(invalid).valid(),"login idle bounds enforced");
        }
        require(!app::parse_operational_configuration(v6 + "caller_afk_minutes=5\n").valid(),
            "mixed v6 and v7 fails closed");
        for (const unsigned minutes : {0U, 1U, 1441U, 65535U}) {
            afk.caller_afk_minutes = static_cast<std::uint16_t>(minutes);
            require(!app::validate_operational_configuration(afk).valid(), "AFK out of range refuses");
        }
        for (const unsigned minutes : {2U, 5U, 1440U}) {
            afk.caller_afk_minutes = static_cast<std::uint16_t>(minutes);
            const auto parsed = app::parse_operational_configuration(app::serialize_operational_configuration(afk));
            require(parsed.valid() && parsed.configuration->caller_afk_minutes == minutes,
                "AFK valid bounds round trip");
        }
        round_trip_tests();
        newline_compatibility_tests();
        rejection_tests();
        ssh_configuration_tests();
        registration_configuration_tests();
        hostname_configuration_tests();
        connection_policy_tests();
        std::cout << "SFX Alpha operational configuration tests PASS: " << assertions << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Alpha operational configuration tests FAIL: " << error.what() << '\n';
        return 1;
    }
}
