#include "sfx/application/control_panel.hpp"
#include "sfx/application/external_telnet_client.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <set>
#include <limits>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read_binary(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string row_text(const sfx::application::ControlPanelTerminalSnapshot& screen,
    const std::size_t row) {
    std::string result;
    for (std::size_t column = 0; column < 80; ++column)
        result.push_back(static_cast<char>(screen.cells[row * 80 + column].character));
    return result;
}

void terminal_tests() {
    {
        sfx::application::ControlPanelTerminalMirror attributes;
        attributes.consume("\x1b[1;5;31;44mA\x1b[22mB\x1b[25mC\x1b[39;49mD");
        const auto frame=attributes.snapshot();
        require(frame.cells[0].attribute==0x9c && frame.cells[1].attribute==0x94 && frame.cells[2].attribute==0x14 && frame.cells[3].attribute==0x07,"NodeView intensity/blink/default-color semantics retain static eight-bit cells");
    }
    sfx::application::ControlPanelTerminalMirror mirror;
    mirror.consume("Hello\r\n\x1b[1;36mWorld\x1b[0m");
    {
        const std::string music="\x1b[|MBT180O4MLL16GP32C#P32G\x0e\x0f\b \b";
        for(std::size_t split=0;split<=music.size();++split) {
            sfx::application::ControlPanelTerminalMirror silent;
            silent.consume("AB");silent.consume(music.substr(0,split));silent.consume(music.substr(split));silent.consume("CD");
            const auto frame=silent.snapshot();
            require(frame.cells[0].character=='A' && frame.cells[1].character=='B' && frame.cells[2].character=='C' && frame.cells[3].character=='D' && frame.cells[4].character==' ',"split background music never leaks notes or moves mirror cursor");
        }
        sfx::application::ControlPanelTerminalMirror recovered;
        recovered.consume("\x1b[|MBT180");recovered.consume("\x1b[2J\x1b[HOK");
        require(recovered.snapshot().cells[0].character=='O',"truncated music resynchronizes at next escape");
    }
    const auto screen = mirror.snapshot();
    require(screen.cells[0].character == 'H' && screen.cells[80].character == 'W',
        "terminal mirror preserves rows");
    require((screen.cells[80].attribute & 0x0fU) == 0x0bU,
        "terminal mirror preserves reviewed ANSI color");
    mirror.consume("\x1b[2J\x1b[25;80HZ");
    require(mirror.snapshot().cells.back().character == 'Z',
        "terminal mirror supports clear and bounded cursor addressing");
    mirror.clear();
    mirror.consume("\x1b[?");mirror.consume("7l\x1b[1;80HAB\x1b[?7h");
    auto no_wrap=mirror.snapshot();
    require(no_wrap.cells[79].character=='B' && no_wrap.cells[80].character==' ',
        "split private wrap controls consumed, disabled wrap stays in last cell");
    mirror.consume("CD");
    require(mirror.snapshot().cells[80].character=='D',"restored wrap advances on next printable character");
    mirror.consume("\x1b[2;1H\x1b[0;30;47mselected\x1b[0;37;40m\x1b[3;1H\x1b[2Knext");
    const auto rendition=mirror.snapshot();
    require(rendition.cells[80].attribute==0x70 && rendition.cells[160+30].attribute==0x07,
        "selected row and following full-line erase use independent backgrounds");
    mirror.consume("\x1b[?25l\x1b[>0;0c");
    const auto ignored=mirror.snapshot();
    require(std::equal(ignored.cells.begin(),ignored.cells.end(),rendition.cells.begin(),
        [](const auto& a,const auto& b){return a.character==b.character && a.attribute==b.attribute;}),
        "unknown private controls never leak text");
    mirror.consume("\x1b[?7l\x1b[1;80H\tZ");
    require(mirror.snapshot().cells[79].character=='Z',"no-wrap tab at last column completes in bounded work");
    const auto layout = sfx::application::control_panel_terminal_grid_layout(
        1000, 600, 8, 15, 1);
    require(layout && layout->cell_height == 16 && layout->right <= 1000 &&
        layout->bottom <= 600 && layout->left == 180 && layout->top == 100,
        "terminal grid uses actual glyph height plus leading and centers without clipping");
    require(!sfx::application::control_panel_terminal_grid_layout(639, 400, 8, 15, 1),
        "terminal grid rejects metrics that cannot keep all 80 columns visible");
    const auto idle = sfx::application::control_panel_idle_reference_layout(
        1292, 656, 16);
    require(idle && idle->left == 0 && idle->top == 0 &&
            idle->cell_width == 16 && idle->cell_height == 32 &&
            idle->right == 1280 && idle->bottom == 512,
        "idle reference is top-left, 80-column, and uses exact integer-scaled 8x16 cells");
    const auto narrow_idle = sfx::application::control_panel_idle_reference_layout(
        1275, 715, 16);
    require(narrow_idle && narrow_idle->cell_width == 8 && narrow_idle->right == 640,
        "idle renderer never crops an occupied cell to force a larger bitmap scale");
    const auto high_dpi_idle = sfx::application::control_panel_idle_reference_layout(
        2006, 850, 24);
    require(high_dpi_idle && high_dpi_idle->cell_width == 16 &&
            high_dpi_idle->cell_height == 32 && high_dpi_idle->right == 1280,
        "144 DPI retains the same displayed Moebius size without 3x enlargement");
    require(!sfx::application::control_panel_idle_reference_layout(639, 400, 16),
        "panes below native 80-column width never crop occupied cells");
}

void node_view_scale_and_placement_tests() {
    using namespace sfx::application;
    for (const auto percent : {100U, 125U, 150U, 200U}) {
        const auto scale = node_view_scale(percent);
        require(scale.has_value(), "approved fixed scale recognized");
        const auto expected_width = static_cast<int>(percent * 640U / 100U);
        const auto expected_height = static_cast<int>(percent * 400U / 100U);
        for (const auto dpi : {96, 120, 144, 192}) {
            (void)dpi; // Surrounding DPI is deliberately NOT a layout input.
            const auto full = node_view_pixel_size(2200, 1800, *scale);
            require(full && full->width == expected_width && full->height == expected_height,
                "fixed device-pixel sizes are exact, never DPI multiplied or enlarged");
        }
        for (int width = 8; width <= 1800; width += 13) {
            for (int height = 5; height <= 1100; height += 17) {
                const auto size = node_view_pixel_size(width, height, *scale);
                require(size && size->width <= width && size->height <= height &&
                    size->width <= expected_width && size->height <= expected_height &&
                    size->width * 5 == size->height * 8 && size->width > 0,
                    "entire 80x25 grid shrinks proportionately without changing preference");
            }
        }
        const auto restored = node_view_pixel_size(2200, 1800, *scale);
        require(restored->width == expected_width && restored->height == expected_height,
            "enlarging viewport restores saved fixed choice");
    }
    const auto automatic = node_view_pixel_size(1360, 900, NodeViewScale::auto_fit);
    require(automatic && automatic->width == 1360 && automatic->height == 850,
        "Auto Fit uses available viewport at 8:5 without a fixed ceiling");
    for (auto bad : {1U, 99U, 101U, 126U, 201U, 65536U, 0xffffffffU})
        require(!node_view_scale(bad), "invalid stored DWORD is not a scale");
    require(!node_view_pixel_size(7,400,NodeViewScale::percent100) &&
        !node_view_pixel_size(640,4,NodeViewScale::auto_fit) &&
        !node_view_pixel_size(-1,400,NodeViewScale::auto_fit) &&
        !node_view_pixel_size(640,400,static_cast<NodeViewScale>(101)), "invalid viewport/scale fails safely");
    ScreenSaverPlacement first, same, neighbor;
    first.seed(42); same.seed(42); neighbor.seed(84);
    ScreenSaverPosition previous{-1,-1}; std::set<std::pair<int,int>> visited;
    bool distinct = false;
    for (std::uint64_t step = 0; step < 4096; ++step) {
        const auto p = first.place(4,2,step), replica = same.place(4,2,step);
        const auto repaint = first.place(4,2,step), other = neighbor.place(4,2,step);
        require(p.x >= 0 && p.x <= 4 && p.y >= 0 && p.y <= 2, "placement stays inside exact pixel bounds");
        require(p.x != previous.x || p.y != previous.y, "no immediate position repeat");
        require(p.x == replica.x && p.y == replica.y && p.x == repaint.x && p.y == repaint.y,
            "seeded test is exact and repaint never advances movement");
        distinct |= p.x != other.x || p.y != other.y;
        visited.emplace(p.x,p.y); previous=p;
    }
    require(visited.size() == 15 && distinct, "all positions including corners are reachable; nodes have independent state");
    const auto one = first.place(0,0,4097);
    require(one.x == 0 && one.y == 0, "one-position viewport terminates with no retry loop");
    for (std::uint64_t step = 0; step < 1000; ++step) {
        const auto p = first.place(static_cast<int>(step%37), static_cast<int>(step%19),step);
        require(p.x >= 0 && p.x <= static_cast<int>(step%37) &&
            p.y >= 0 && p.y <= static_cast<int>(step%19), "resize recomputes contained placement");
    }
    const auto huge = first.place(std::numeric_limits<int>::max(),std::numeric_limits<int>::max(),5000);
    require(huge.x >= 0 && huge.y >= 0, "bounded arithmetic survives extreme viewport inputs");
    std::cout << "PASS UI5: four exact fixed scales x four DPI contracts; 80x25 fit/recovery; 4096 deterministic nonrepeating moves, all 15 positions, 1000 resizes, independent nodes\n";
}

[[maybe_unused]] void banner_tests() {
    const auto lines = sfx::application::control_panel_node_banner(1, "RDY", "9.8.7");
    for (const auto& line : lines) require(line.size() == 80, "banner is exactly 80 columns");
    require(lines[0].substr(0, 16) == ">>>>>>>><<<<<<<<" &&
        lines[0].substr(64) == ">>>>>>>><<<<<<<<", "banner uses exact ornament");
    require(lines[1].find("Mike Woltz") != std::string::npos,
        "banner preserves confirmed memorial spelling");
    require(lines[2].find("NODE 1 RDY") != std::string::npos,
        "banner contains dynamic node state");
    require(lines[3].find("Spitfire-X BBS v9.8.7") != std::string::npos,
        "banner consumes the product version supplied by the host");
}

void preference_and_history_tests() {
    sfx::application::ControlPanelUiPreferences preferences;
    require(!preferences.system_monitor_grayscale, "new users default to Color");
    preferences.left = -50;
    preferences.top = 31;
    preferences.width = 1440;
    preferences.height = 880;
    preferences.system_monitor_visible = true;
    preferences.system_monitor_grayscale = true;
    preferences.node_column_widths = {{91, 128, 144, 111, 137, 222}};
    const auto parsed = sfx::application::parse_control_panel_ui_preferences(
        sfx::application::serialize_control_panel_ui_preferences(preferences));
    require(parsed && parsed->left == -50 && parsed->top == 31 && parsed->width == 1440 &&
        parsed->height == 880 && parsed->system_monitor_visible &&
        parsed->node_column_widths == preferences.node_column_widths && parsed->system_monitor_grayscale,
        "version 3 UI preferences preserve all settings and grayscale");
    preferences.system_monitor_grayscale = false;
    const auto color_serialized = sfx::application::serialize_control_panel_ui_preferences(preferences);
    const auto color = sfx::application::parse_control_panel_ui_preferences(color_serialized);
    require(color && !color->system_monitor_grayscale, "version 3 Color round trip");
    std::string version_two = color_serialized;
    const std::string width_line = "system_monitor_width=251\n";
    version_two.erase(version_two.find(width_line), width_line.size());
    auto version_four = version_two;
    version_four.replace(0, 23, "SFX_CONTROL_PANEL_UI=4\n");
    const auto migrated_four = sfx::application::parse_control_panel_ui_preferences(version_four);
    require(migrated_four && migrated_four->system_monitor_width == 251 &&
        migrated_four->node_column_widths == preferences.node_column_widths,
        "V4 migrates width without losing modem or layout preferences");
    for (const auto width : {251,320,400,520}) {
        preferences.system_monitor_width = width;
        const auto resized = sfx::application::parse_control_panel_ui_preferences(
            sfx::application::serialize_control_panel_ui_preferences(preferences));
        require(resized && resized->system_monitor_width == width, "operator monitor width persists");
    }
    for (const auto width : {0,250,521,INT32_MAX}) {
        preferences.system_monitor_width = width;
        require(!sfx::application::parse_control_panel_ui_preferences(
            sfx::application::serialize_control_panel_ui_preferences(preferences)), "unbounded width rejected");
    }
    preferences.system_monitor_width = 251;
    const std::string modem_line = "modem_lights=0\n";
    version_two.erase(version_two.find(modem_line), modem_line.size());
    auto version_three = version_two;
    version_three.replace(0, 23, "SFX_CONTROL_PANEL_UI=3\n");
    const auto migrated_three = sfx::application::parse_control_panel_ui_preferences(version_three);
    require(migrated_three && !migrated_three->modem_lights && migrated_three->left == -50 &&
        migrated_three->node_column_widths == preferences.node_column_widths,
        "V3 migrates to lights off without losing layout");
    for (bool enabled : {true, false}) {
        preferences.modem_lights = enabled;
        const auto saved = sfx::application::parse_control_panel_ui_preferences(
            sfx::application::serialize_control_panel_ui_preferences(preferences));
        require(saved && saved->modem_lights == enabled, "modem on and off persist");
    }
    version_two.replace(0, 23, "SFX_CONTROL_PANEL_UI=2\n");
    const std::string appearance = "system_monitor_grayscale=0\n";
    version_two.erase(version_two.find(appearance), appearance.size());
    const auto migrated = sfx::application::parse_control_panel_ui_preferences(version_two);
    require(migrated && !migrated->system_monitor_grayscale && migrated->left == -50 &&
        migrated->top == 31 && migrated->width == 1440 && migrated->height == 880 &&
        migrated->system_monitor_visible &&
        migrated->node_column_widths == preferences.node_column_widths,
        "version 2 migrates to Color without losing existing settings");
    for (const auto* invalid : {"2", "-1", "true", "", "01", "1 ", " 0"}) {
        auto malformed = color_serialized;
        malformed.replace(malformed.find(appearance), appearance.size(),
            "system_monitor_grayscale=" + std::string(invalid) + "\n");
        require(!sfx::application::parse_control_panel_ui_preferences(malformed),
            "version 3 rejects invalid appearance values");
    }
    for (const auto* extra : {"system_monitor_grayscale=1\n", "left=1\n", "unknown=0\n", "\n"})
        require(!sfx::application::parse_control_panel_ui_preferences(color_serialized + extra),
            "version 3 rejects duplicate/unknown fields and trailing data");
    auto missing = color_serialized;
    missing.erase(missing.find(appearance), appearance.size());
    require(!sfx::application::parse_control_panel_ui_preferences(missing),
        "version 3 requires an explicit appearance value");
    const auto legacy = sfx::application::parse_control_panel_ui_preferences(
        "SFX_CONTROL_PANEL_UI=1\nleft=0\ntop=0\nwidth=1080\nheight=700\nsystem_monitor=0\n");
    require(legacy && legacy->node_column_widths[1] == 120 && !legacy->system_monitor_grayscale,
        "version 1 UI preferences retain defaults and migrate to Color");
    require(!sfx::application::parse_control_panel_ui_preferences(
        "SFX_CONTROL_PANEL_UI=1\nleft=0\ntop=0\nwidth=1\nheight=1\nsystem_monitor=2\n"),
        "malformed UI preferences fail closed");

    sfx::application::BoundedMetricHistory history(3);
    history.push(1); history.push(2); history.push(3); history.push(4);
    require(history.values().size() == 3 && history.values().front() == 2 &&
        history.values().back() == 4, "metric history remains bounded");
    using sfx::application::sqlite_activity_graph_maximum;
    using sfx::application::disk_activity_graph_maximum;
    require(disk_activity_graph_maximum({}) == 65536 &&
        disk_activity_graph_maximum({0,0,0}) == 65536 &&
        disk_activity_graph_maximum({1024,16384,65536}) == 65536,
        "idle is flat and low byte rates use a stable linear 64 KiB/s range");
    require(disk_activity_graph_maximum({65537,262144}) == 262144 &&
        disk_activity_graph_maximum({262145,1048576}) == 1048576 &&
        disk_activity_graph_maximum({1048577,4194304}) == 4194304,
        "explicit byte-rate ranges include every visible peak without clipping");
    require(disk_activity_graph_maximum({4194304,1024,1024}) == 4194304 &&
        disk_activity_graph_maximum({1024,1024,1024}) == 65536,
        "throughput zoom contracts after a large peak leaves the visible history");
    require(disk_activity_graph_maximum({std::numeric_limits<double>::quiet_NaN(),0}) == 65536 &&
        disk_activity_graph_maximum({std::numeric_limits<double>::max()}) == std::numeric_limits<double>::max(),
        "non-finite graph inputs cannot corrupt scaling and finite maxima cannot overflow");
    require(sqlite_activity_graph_maximum({}) == 1000 &&
        sqlite_activity_graph_maximum({0,0,0}) == 1000,
        "SQLite idle graph retains a stable nonzero reference");
    require(sqlite_activity_graph_maximum({0,87,87,87}) == 1000 &&
        sqlite_activity_graph_maximum({87,87,0,0}) == 1000,
        "one-caller activity has the same height before and after logout");
    require(sqlite_activity_graph_maximum({749,750}) == 1000 &&
        sqlite_activity_graph_maximum({751,1500}) == 2000 &&
        sqlite_activity_graph_maximum({0,100000}) > 100000,
        "SQLite bursts expand the scale with headroom instead of clipping");
    require(sfx::application::control_panel_monitor_should_poll(0, true) &&
        !sfx::application::control_panel_monitor_should_poll(0, false) &&
        !sfx::application::control_panel_monitor_should_poll(1, true) &&
        !sfx::application::control_panel_monitor_should_poll(4, true),
        "monitor telemetry polling stops whenever its Nodes-only panel is hidden");
}

bool inside_live_range(const std::size_t row, const std::size_t column) {
    for (const auto& range : sfx::application::kControlPanelNodeViewLiveRanges)
        if (row == range.row && column >= range.column &&
            column < range.column + range.width) return true;
    return false;
}

void require_live_fields(const sfx::application::ControlPanelTerminalSnapshot& owner,
    const sfx::application::ControlPanelNodeViewFields& fields) {
    using namespace sfx::application;
    const std::array<std::string_view, 11> values{{
        fields.abbreviated_state, fields.work_path, fields.display_path,
        fields.message_path, fields.protocol_path, fields.configuration_result,
        fields.database_result, fields.node_readiness_result, fields.listener_result,
        fields.health_summary_result, fields.ready_result}};
    constexpr std::array<std::uint8_t, 11> foregrounds{{
        0x0f, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0f, 0x07}};
    const auto actual = compose_control_panel_node_view(owner, fields);
    for (std::size_t index = 0; index < actual.cells.size(); ++index) {
        require((actual.cells[index].attribute & 0xf0U) ==
                (owner.cells[index].attribute & 0xf0U),
            "all live and static background nibbles remain authored");
        if (!inside_live_range(index / 80U, index % 80U))
            require(actual.cells[index].character == owner.cells[index].character &&
                    actual.cells[index].attribute == owner.cells[index].attribute,
                "static characters and attributes remain byte-exact");
    }
    for (std::size_t field = 0; field < values.size(); ++field) {
        const auto& range = kControlPanelNodeViewLiveRanges[field];
        for (std::size_t offset = 0; offset < range.width; ++offset) {
            const auto index = range.row * 80U + range.column + offset;
            const bool written = offset < values[field].size();
            const auto expected_character = written
                ? static_cast<std::uint8_t>(values[field][offset]) : std::uint8_t{0x20};
            const auto expected_attribute = written
                ? static_cast<std::uint8_t>((owner.cells[index].attribute & 0xf0U) |
                    foregrounds[field]) : owner.cells[index].attribute;
            if (actual.cells[index].character != expected_character ||
                actual.cells[index].attribute != expected_attribute)
                throw std::runtime_error("live semantic color/content failure at row " +
                    std::to_string(range.row) + " column " +
                    std::to_string(range.column + offset) + " expected attribute " +
                    std::to_string(expected_attribute) + " actual " +
                    std::to_string(actual.cells[index].attribute));
        }
    }
}

void owner_ansi_tests(const char* node_path, const char* saver_path,
    const char* font_path) {
    using namespace sfx::application;
    const std::string node_bytes = read_binary(node_path);
    const std::string saver_bytes = read_binary(saver_path);
    const std::string font_bytes = read_binary(font_path);
    require(node_bytes.size() == 1047 && saver_bytes.size() == 1363,
        "owner ANSI bytes are the expected exact sizes");
    require(font_bytes.size() == 4096 &&
            static_cast<unsigned char>(font_bytes[0x41U * 16U + 2U]) == 0x10U &&
            static_cast<unsigned char>(font_bytes[0xdaU * 16U + 7U]) == 0x1fU,
        "private candidate embeds the exact Moebius IBM VGA 8x16 glyph table");
    const auto node_template = parse_control_panel_ansi_asset(node_bytes);
    const auto saver_template = parse_control_panel_ansi_asset(saver_bytes);
    require(node_template && saver_template, "owner ANSI assets load with exact 80x25 SAUCE");
    constexpr std::array<std::uint8_t, 5> used_attributes{{0x07,0x08,0x09,0x0c,0x0f}};
    constexpr std::array<std::array<std::uint8_t, 3>, 5> expected_rgb{{
        {{170,170,170}}, {{85,85,85}}, {{85,85,255}}, {{255,85,85}}, {{255,255,255}}
    }};
    for (std::size_t index = 0; index < used_attributes.size(); ++index) {
        const auto& ega = kControlPanelMoebiusEgaPalette[used_attributes[index]];
        for (std::size_t channel = 0; channel < 3; ++channel)
            require(control_panel_vga_channel(ega[channel]) == expected_rgb[index][channel],
                "every used owner attribute has the exact measured Moebius VGA RGB value");
    }
    for (const auto* grid : {&*node_template, &*saver_template})
        for (const auto& cell : grid->cells) {
            require((cell.attribute >> 4U) == 0U,
                "all authored backgrounds remain exact palette index zero, RGB black");
            bool known = false;
            for (const auto attribute : used_attributes) known |= cell.attribute == attribute;
            require(known, "all used attributes are included in the exact palette test");
        }
    require(row_text(*node_template, 1).substr(1, 14) == ">>>>>>><<<<<<<",
        "node template retains exact cell placement");
    const auto saver_top = row_text(*saver_template, 2).substr(21, 36);
    require(static_cast<unsigned char>(saver_top.front()) == 0xdaU &&
        static_cast<unsigned char>(saver_top.back()) == 0xbfU &&
        saver_top.find("[ SPITFIRE-X BBS ]") == 3,
        "screen saver retains exact CP437 top border");

    ControlPanelNodeViewFields fields;
    fields.abbreviated_state = "OFF";
    fields.work_path = "not available - not configured";
    fields.display_path = "c:\\synthetic-board\\displays";
    fields.message_path = "not available - not configured";
    fields.protocol_path = "c:\\sfx-long-installation-directory\\work\\sexyz";
    fields.configuration_result = "not ready";
    fields.database_result = "not ready";
    fields.node_readiness_result = "disabled";
    fields.listener_result = "Listening on tcp/2323";
    fields.health_summary_result = "FAULT";
    fields.ready_result = "Node disabled";
    require_live_fields(*node_template, fields);
    fields.health_summary_result = "NOT RDY";
    require_live_fields(*node_template, fields);
    const auto composed = compose_control_panel_node_view(*node_template, fields);
    require(row_text(composed, 3).substr(67, 8) == "NODE OFF",
        "only the three-character state changes; authored NODE and ornaments are static");
    for (const auto* state : {"RDY", "OFF", "N/A", "CON", "ONL", "BSY"}) {
        fields.abbreviated_state = state;
        const auto variant = compose_control_panel_node_view(*node_template, fields);
        require_live_fields(*node_template, fields);
        require(row_text(variant,3).substr(67,5) == "NODE " &&
                row_text(variant,3).substr(72,3) == state,
            "every live state preserves literal NODE and exact three-cell state width");
    }
    std::size_t live_character_differences = 0;
    for (std::size_t row = 0; row < 25; ++row) {
        for (std::size_t column = 0; column < 80; ++column) {
            const auto index = row * 80 + column;
            if (inside_live_range(row, column)) {
                if (composed.cells[index].character != node_template->cells[index].character)
                    ++live_character_differences;
            } else {
                require(composed.cells[index].character == node_template->cells[index].character &&
                        composed.cells[index].attribute == node_template->cells[index].attribute,
                    "every static Node View cell is byte-exact to the owner ANSI grid");
            }
        }
    }
    require(live_character_differences != 0,
        "the enumerated live ranges exercise runtime substitution");
    // Deliberately colored backgrounds and contradictory foregrounds prove that
    // field roles replace foreground only, including written spaces and clipping.
    auto colored_template = *node_template;
    for (std::size_t index = 0; index < colored_template.cells.size(); ++index)
        colored_template.cells[index].attribute = static_cast<std::uint8_t>(
            ((index % 16U) << 4U) | 0x02U);
    require_live_fields(colored_template, fields);
    fields.work_path = "x ";
    fields.display_path.clear();
    fields.health_summary_result = "NOT READY OVERFLOW";
    fields.ready_result = std::string(90, 'z');
    require_live_fields(colored_template, fields);
    require(row_text(composed, 2).find("Milk Woltz") != std::string::npos &&
            row_text(composed, 3).find("Copyright [put copyright info here]") != std::string::npos &&
            row_text(composed, 4).find("https://github.url.goes.here") != std::string::npos,
        "all owner-authored header wording remains literal");
    for (const std::size_t row : {6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 15U})
        require(composed.cells[row * 80].character == 0xfaU,
            "every owner-authored CP437 middle-dot marker remains literal");
    for (std::size_t column = 11; column <= 27; ++column)
        require(node_template->cells[6U * 80U + column].attribute == 0x08U,
            "dotted leaders retain ANSI dark-gray attributes");
    require(row_text(composed, 7).substr(29).find("c:\\synthetic-board") == 0 &&
            row_text(composed, 9).substr(29).find("c:\\sfx-long-installation-directory") == 0,
        "displayed live paths remain lowercase");
    require(composed.cells[14U * 80U].character == 'N' &&
            composed.cells[14U * 80U].attribute == 0x0fU &&
            composed.cells[14U * 80U + 1U].attribute == 0x0fU,
        "overall live field uses bright-white health attributes");
    require(node_template->cells[14U * 80U].character == 'O' &&
            node_template->cells[14U * 80U].attribute == 0x0fU,
        "golden standalone OK is bright white");

    const auto first = compose_control_panel_screen_saver(*saver_template, 0);
    const auto second = compose_control_panel_screen_saver(*saver_template, 1);
    const auto third = compose_control_panel_screen_saver(*saver_template, 2);
    require(first.top <= 20 && first.left <= 44 && second.top <= 20 && second.left <= 44 &&
        first.top != second.top && first.left != second.left,
        "screen saver positions are valid and do not immediately repeat");
    require(first.color_index == 0 && second.color_index == 1 && third.color_index == 2,
        "screen saver cycles only the three owner-authored X colors");
    for (const auto* frame : {&first, &second, &third}) {
        const std::size_t source_top = 2U + frame->color_index * 6U;
        for (std::size_t row = 0; row < 25; ++row) {
            for (std::size_t column = 0; column < 80; ++column) {
                const bool in_box = row >= frame->top && row < frame->top + 5U &&
                    column >= frame->left && column < frame->left + 36U;
                const auto& actual = frame->screen.cells[row * 80U + column];
                if (in_box) {
                    const auto& expected = saver_template->cells[(source_top + row - frame->top) *
                        80U + 21U + column - frame->left];
                    require(actual.character == expected.character &&
                            actual.attribute == expected.attribute,
                        "every screen-saver box cell matches its owner-authored state");
                } else {
                    require(actual.character == 0x20U && actual.attribute == 0x07U,
                        "screen-saver cells outside the one complete box remain blank");
                }
            }
        }
        require(frame->screen.cells[frame->top * 80U + frame->left].character == 0xdaU &&
                frame->screen.cells[(frame->top + 4U) * 80U + frame->left + 35U].character == 0xd9U,
            "screen saver retains the complete CP437 outline");
    }
    require(first.screen.cells[first.top * 80U + first.left + 14U].attribute == 0x0cU &&
            second.screen.cells[second.top * 80U + second.left + 14U].attribute == 0x0fU &&
            third.screen.cells[third.top * 80U + third.left + 14U].attribute == 0x09U,
        "screen saver cycles the exact red, white, and blue title-X attributes");
    const auto constrained = compose_control_panel_screen_saver(*saver_template, 99, 50, 12);
    require(constrained.left + 36U <= 50U && constrained.top + 5U <= 12U,
        "large integer-scale saver movement keeps the complete owner box visible");
}

void sqlite_rate_tests() {
    using namespace sfx::application;
    ControlPanelSqliteRateTracker tracker;
    auto sample = tracker.sample({true, 1, 100, 20, 5}, 1000);
    require(sample.available && !sample.interval_available,
        "first SQLite counter sample is a baseline");
    sample = tracker.sample({true, 1, 115, 25, 7}, 2000);
    require(sample.interval_available && sample.page_reads_per_second == 20 &&
        sample.page_writes_per_second == 2 && sample.cache_hit_available &&
        sample.cache_hit_basis_points == 7500,
        "SQLite interval rates use nonnegative pager-counter deltas");
    sample = tracker.sample({true, 2, 3, 1, 0}, 3000);
    require(!sample.interval_available, "connection replacement creates a new baseline");
    sample = tracker.sample({true, 2, 1, 0, 0}, 4000);
    require(!sample.interval_available, "counter regression creates a new baseline");
    sample = tracker.sample({false, 0, 0, 0, 0}, 5000);
    require(!sample.available, "unavailable SQLite telemetry fails closed");
}

void external_telnet_client_tests() {
    using namespace sfx::application;
    const auto root = std::filesystem::absolute("sfx-f06-client-profile-fixture");
    const auto syncterm_path = root / "path with spaces" / "SyncTERM.exe";
    std::filesystem::create_directories(syncterm_path.parent_path());
    { std::ofstream executable(syncterm_path, std::ios::binary); executable << "harmless argv witness"; }
    ExternalTelnetClientProfile syncterm{ExternalTelnetClientType::syncterm,
        syncterm_path.wstring(), {}};
    require(validate_external_telnet_client_profile(syncterm).valid &&
        external_telnet_client_executable_is_usable(syncterm),
        "recognized SyncTERM absolute regular exe profile is usable");
    const auto sync_launch = prepare_external_telnet_client_launch(
        syncterm, L"127.0.0.1", 49152);
    require(sync_launch && sync_launch->arguments == std::vector<std::wstring>{
            L"-4", L"telnet://127.0.0.1:49152"} &&
        sync_launch->command_line.find(L"\"-4\"") != std::wstring::npos &&
        sync_launch->command_line.find(L"\"telnet://127.0.0.1:49152\"") != std::wstring::npos,
        "SyncTERM expands only the verified IPv4 and Telnet URI tokens");
    require(!prepare_external_telnet_client_launch(syncterm, L"192.0.2.1", 23) &&
        !prepare_external_telnet_client_launch(syncterm, L"127.0.0.1", 0),
        "client launch refuses remote hosts and zero ports");

    const auto custom_path = root / "Custom Client.exe";
    { std::ofstream executable(custom_path, std::ios::binary); executable << "harmless argv witness"; }
    ExternalTelnetClientProfile custom{ExternalTelnetClientType::custom,
        custom_path.wstring(), {L"--protocol=telnet", L"--host", L"{HOST}",
            L"--port={PORT}", L"value with spaces"}};
    const auto custom_launch = prepare_external_telnet_client_launch(
        custom, L"127.0.0.1", 60000);
    require(custom_launch && custom_launch->arguments.size() == 5 &&
        custom_launch->arguments[2] == L"127.0.0.1" &&
        custom_launch->arguments[3] == L"--port=60000" &&
        custom_launch->arguments[4] == L"value with spaces" &&
        custom_launch->command_line.find(L"\"value with spaces\"") != std::wstring::npos,
        "Custom ordered tokens expand exact controlled values and quote spaces as one argv value");
    for (const auto& invalid : std::vector<std::vector<std::wstring>>{
            {}, {L"{HOST}"}, {L"{PORT}"}, {L"{HOST}", L"{HOST}", L"{PORT}"},
            {L"{HOST}", L"{PORT}", L"{USER}"}, {L" {HOST}", L"{PORT}"},
            {L"{HOST}", L"{PORT}\n"}, {L"{HOST}", L"\"{PORT}\""},
            {L"{HOST}", L"{PORT}", L""}}) {
        auto rejected = custom; rejected.custom_arguments = invalid;
        require(!validate_external_telnet_client_profile(rejected).valid,
            "malformed Custom token or placeholder set fails closed");
    }
    auto too_many = custom;
    too_many.custom_arguments = {L"{HOST}", L"{PORT}", L"1", L"2", L"3", L"4", L"5", L"6", L"7"};
    require(!validate_external_telnet_client_profile(too_many).valid,
        "Custom profile rejects more than eight tokens");
    auto too_long = custom;
    too_long.custom_arguments = {L"{HOST}" + std::wstring(506, L'a'),
        L"{PORT}" + std::wstring(506, L'b'), std::wstring(512, L'c'),
        std::wstring(512, L'd'), std::wstring(512, L'e'), std::wstring(512, L'f'),
        std::wstring(512, L'g'), std::wstring(512, L'h')};
    require(validate_external_telnet_client_profile(too_long).valid &&
        !prepare_external_telnet_client_launch(too_long, L"127.0.0.1", 65535),
        "individually bounded tokens still fail when rendered command exceeds 4096 characters");
    auto wrong_extension = custom; wrong_extension.executable = (root / "client.com").wstring();
    require(!validate_external_telnet_client_profile(wrong_extension).valid,
        "non-exe path is rejected before launch");
    std::filesystem::remove(custom_path);
    require(!external_telnet_client_executable_is_usable(custom),
        "moved or removed executable invalidates the saved profile on next use");
    auto directory = custom; directory.executable = root.wstring();
    require(!external_telnet_client_executable_is_usable(directory),
        "directory cannot be used as a Telnet executable");
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    require(!cleanup, "remove bounded client-profile fixture");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        if(argc==4 && std::string_view(argv[1])=="--mirror-file"){
            sfx::application::ControlPanelTerminalMirror mirror;
            mirror.consume(read_binary(argv[2]));
            std::ofstream output(argv[3],std::ios::binary|std::ios::app);
            require(output.tellp()==0,"new mirror cell artifact required");
            for(const auto& cell:mirror.snapshot().cells){output.put(static_cast<char>(cell.character));output.put(static_cast<char>(cell.attribute));}
            require(static_cast<bool>(output),"mirror cell artifact written");return 0;
        }
        require(argc == 4, "owner ANSI and Moebius font paths are required");
        terminal_tests();
        node_view_scale_and_placement_tests();
        preference_and_history_tests();
        owner_ansi_tests(argv[1], argv[2], argv[3]);
        sqlite_rate_tests();
        external_telnet_client_tests();
        std::cout << "SFX Control Panel portable model tests PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Control Panel portable model tests FAIL: " << error.what() << '\n';
        return 1;
    }
}
