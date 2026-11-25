#include "sfx/application/control_panel.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace sfx::application {

std::string compact_uptime(std::uint64_t milliseconds) {
    const auto minutes = milliseconds / 60000;
    const auto hours = (minutes / 60) % 24;
    const auto remainder = minutes % 60;
    return std::to_string(minutes / 1440) + "d " + (hours < 10 ? "0" : "") +
        std::to_string(hours) + ":" + (remainder < 10 ? "0" : "") + std::to_string(remainder);
}
double uptime_reference_fraction(std::uint64_t bbs, std::uint64_t os) noexcept {
    if (!os) return 0;
    return static_cast<double>(std::min(bbs, os)) / static_cast<double>(os);
}
namespace {

std::string centered(const std::string_view value, const std::size_t width) {
    std::string clipped(value.substr(0, width));
    const std::size_t remaining = width - clipped.size();
    return std::string(remaining / 2U, ' ') + clipped +
        std::string(remaining - remaining / 2U, ' ');
}

std::optional<std::int32_t> integer_field(const std::string_view value) noexcept {
    std::int32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
    return result;
}

std::uint32_t little_u32(const char* value) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(value[0])) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(value[1])) << 8U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(value[2])) << 16U) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(value[3])) << 24U);
}

std::uint16_t little_u16(const char* value) noexcept {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(value[0]) |
        (static_cast<std::uint16_t>(static_cast<unsigned char>(value[1])) << 8U));
}

void replace_characters(ControlPanelTerminalSnapshot& screen, const std::size_t row,
    const std::size_t column, const std::size_t width, const std::string_view value,
    const std::uint8_t foreground, const bool center = false) noexcept {
    if (row >= kControlPanelTerminalRows || column >= kControlPanelTerminalColumns) return;
    const std::size_t bounded_width = std::min(width, kControlPanelTerminalColumns - column);
    for (std::size_t index = 0; index < bounded_width; ++index)
        screen.cells[row * kControlPanelTerminalColumns + column + index].character = 0x20;
    const std::size_t count = std::min(bounded_width, value.size());
    const std::size_t start = center ? (bounded_width - count) / 2U : 0U;
    for (std::size_t index = 0; index < count; ++index) {
        auto& cell = screen.cells[row * kControlPanelTerminalColumns + column + start + index];
        cell.character = static_cast<std::uint8_t>(value[index]);
        cell.attribute = static_cast<std::uint8_t>(
            (cell.attribute & 0xf0U) | (foreground & 0x0fU));
    }
}

std::string preference_field(const std::string_view serialized,
    const std::string_view name) {
    const std::string needle = std::string(name) + "=";
    const auto position = serialized.find(needle);
    if (position == std::string_view::npos ||
        (position != 0 && serialized[position - 1U] != '\n')) return {};
    const auto begin = position + needle.size();
    const auto end = serialized.find('\n', begin);
    return std::string(serialized.substr(begin,
        end == std::string_view::npos ? serialized.size() - begin : end - begin));
}

}  // namespace

std::optional<ControlPanelTerminalGridLayout> control_panel_terminal_grid_layout(
    const std::int32_t pane_width, const std::int32_t pane_height,
    const std::int32_t measured_glyph_width, const std::int32_t text_metric_height,
    const std::int32_t external_leading) noexcept {
    if (pane_width <= 0 || pane_height <= 0 || measured_glyph_width <= 0 ||
        text_metric_height <= 0 || external_leading < 0 ||
        text_metric_height > std::numeric_limits<std::int32_t>::max() - external_leading)
        return std::nullopt;
    const std::int32_t cell_height = text_metric_height + external_leading;
    if (measured_glyph_width > pane_width / static_cast<std::int32_t>(
            kControlPanelTerminalColumns) ||
        cell_height > pane_height / static_cast<std::int32_t>(kControlPanelTerminalRows))
        return std::nullopt;
    ControlPanelTerminalGridLayout result;
    result.cell_width = measured_glyph_width;
    result.cell_height = cell_height;
    const std::int32_t grid_width = measured_glyph_width *
        static_cast<std::int32_t>(kControlPanelTerminalColumns);
    const std::int32_t grid_height = cell_height *
        static_cast<std::int32_t>(kControlPanelTerminalRows);
    result.left = (pane_width - grid_width) / 2;
    result.top = (pane_height - grid_height) / 2;
    result.right = result.left + grid_width;
    result.bottom = result.top + grid_height;
    return result;
}

std::optional<ControlPanelTerminalGridLayout> control_panel_idle_reference_layout(
    const std::int32_t pane_width, const std::int32_t pane_height,
    const std::int32_t maximum_cell_width) noexcept {
    // Moebius's IBM VGA renderer is an 8x16 bitmap grid. Choose only an integer
    // device-pixel scale that fits every occupied cell; the Win32 renderer draws
    // those final pixels directly and never stretches a smaller GDI text bitmap.
    constexpr std::int32_t occupied_rows = 16;
    if (pane_width <= 0 || pane_height <= 0 || maximum_cell_width <= 0)
        return std::nullopt;
    constexpr std::int32_t native_width = 8;
    constexpr std::int32_t native_height = 16;
    constexpr std::int32_t columns = static_cast<std::int32_t>(
        kControlPanelTerminalColumns);
    const std::int32_t preferred_scale = std::clamp(maximum_cell_width / native_width, 1, 2);
    const std::int32_t scale = std::min({preferred_scale,
        pane_width / (columns * native_width),
        pane_height / (occupied_rows * native_height)});
    if (scale < 1) return std::nullopt;
    ControlPanelTerminalGridLayout result;
    result.cell_width = native_width * scale;
    result.cell_height = native_height * scale;
    result.right = result.cell_width * columns;
    result.bottom = result.cell_height * occupied_rows;
    return result;
}

std::optional<NodeViewScale> node_view_scale(const std::uint32_t value) noexcept {
    switch (value) {
        case 0: case 100: case 125: case 150: case 200:
            return static_cast<NodeViewScale>(value);
        default: return std::nullopt;
    }
}

std::optional<NodeViewPixelSize> node_view_pixel_size(const std::int32_t width,
    const std::int32_t height, const NodeViewScale scale) noexcept {
    if (width < 8 || height < 5 || !node_view_scale(static_cast<std::uint16_t>(scale)))
        return std::nullopt;
    // Exact 8:5 aspect, top-left anchored, complete 80x25 canvas. Flooring
    // in 8x5 pixel increments never clips or exceeds the selected fixed size.
    auto units = std::min(width / 8, height / 5);
    if (scale != NodeViewScale::auto_fit)
        units = std::min(units, static_cast<int>(scale) * 80 / 100);
    return NodeViewPixelSize{units * 8, units * 5};
}

void ScreenSaverPlacement::seed(const std::uint64_t value) noexcept {
    random_ = value; positioned_ = false;
}

ScreenSaverPosition ScreenSaverPlacement::place(const std::int32_t max_x,
    const std::int32_t max_y, const std::uint64_t movement) noexcept {
    const auto x = std::max(0, max_x), y = std::max(0, max_y);
    if (positioned_ && maximum_x_ == x && maximum_y_ == y && movement_ == movement)
        return position_;
    // SplitMix64: local visual PRNG, fixed work and no external dependency.
    auto value = (random_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    const auto columns = static_cast<std::uint64_t>(x) + 1U;
    const auto count = columns * (static_cast<std::uint64_t>(y) + 1U);
    const bool exclude_previous = positioned_ && position_.x <= x && position_.y <= y && count > 1;
    auto selected = value % (count - (exclude_previous ? 1U : 0U));
    if (exclude_previous) {
        const auto previous = static_cast<std::uint64_t>(position_.y) * columns + position_.x;
        if (selected >= previous) ++selected; // no retry loop or immediate repeat
    }
    position_ = {static_cast<std::int32_t>(selected % columns),
        static_cast<std::int32_t>(selected / columns)};
    maximum_x_ = x; maximum_y_ = y; movement_ = movement; positioned_ = true;
    return position_;
}

ControlPanelTerminalMirror::ControlPanelTerminalMirror() noexcept { clear(); }

void ControlPanelTerminalMirror::clear() noexcept {
    for (auto& cell : screen_.cells) cell = {0x20, 0x07};
    ++screen_.sequence;
    row_ = 0;
    column_ = 0;
    attribute_ = 0x07;
    parse_state_ = ParseState::text;
    parameter_count_ = 0;
    parameter_present_ = false;
    private_sequence_ = unsupported_sequence_ = false;
    auto_wrap_ = true;
}

void ControlPanelTerminalMirror::newline() noexcept {
    column_ = 0;
    if (row_ + 1U < kControlPanelTerminalRows) {
        ++row_;
        return;
    }
    std::move(screen_.cells.begin() + static_cast<std::ptrdiff_t>(kControlPanelTerminalColumns),
        screen_.cells.end(), screen_.cells.begin());
    std::fill(screen_.cells.end() - static_cast<std::ptrdiff_t>(kControlPanelTerminalColumns),
        screen_.cells.end(), ControlPanelTerminalCell{0x20, attribute_});
}

void ControlPanelTerminalMirror::put(const std::uint8_t character) noexcept {
    if (column_ >= kControlPanelTerminalColumns) {
        if (auto_wrap_) newline();
        else column_ = kControlPanelTerminalColumns - 1;
    }
    screen_.cells[row_ * kControlPanelTerminalColumns + column_] = {character, attribute_};
    ++column_;
    if (!auto_wrap_) column_ = std::min(column_, kControlPanelTerminalColumns - 1);
    ++screen_.sequence;
}

void ControlPanelTerminalMirror::apply_control_sequence(const std::uint8_t final_byte) noexcept {
    if (unsupported_sequence_) return;
    if (private_sequence_) {
        if (parameter_count_ == 1 && parameters_[0] == 7 &&
            (final_byte == 'h' || final_byte == 'l')) auto_wrap_ = final_byte == 'h';
        return;
    }
    const auto parameter = [this](const std::size_t index, const std::uint16_t fallback) {
        return index < parameter_count_ && parameters_[index] != 0 ? parameters_[index] : fallback;
    };
    if (final_byte == 'H' || final_byte == 'f') {
        row_ = std::min<std::size_t>(parameter(0, 1) - 1U, kControlPanelTerminalRows - 1U);
        column_ = std::min<std::size_t>(parameter(1, 1) - 1U, kControlPanelTerminalColumns - 1U);
    } else if (final_byte == 'J' && parameter(0, 0) == 2) {
        for (auto& cell : screen_.cells) cell = {0x20, attribute_};
        row_ = column_ = 0;
        ++screen_.sequence;
    } else if (final_byte == 'K') {
        const auto mode = parameter(0, 0);
        const std::size_t begin = row_ * kControlPanelTerminalColumns + (mode == 1 || mode == 2 ? 0 : std::min(column_, kControlPanelTerminalColumns));
        const std::size_t end = row_ * kControlPanelTerminalColumns + (mode == 1 ? std::min(column_ + 1, kControlPanelTerminalColumns) : kControlPanelTerminalColumns);
        std::fill(screen_.cells.begin() + static_cast<std::ptrdiff_t>(begin),
            screen_.cells.begin() + static_cast<std::ptrdiff_t>(end),
            ControlPanelTerminalCell{0x20, attribute_});
        ++screen_.sequence;
    } else if (final_byte == 'm') {
        if (parameter_count_ == 0) {
            attribute_ = 0x07;
        }
        for (std::size_t index = 0; index < parameter_count_; ++index) {
            const auto value = parameters_[index];
            if (value == 0) attribute_ = 0x07;
            else if (value == 1) attribute_ |= 0x08;
            else if (value == 22) attribute_ &= 0xf7;
            else if (value == 5) attribute_ |= 0x80; // Static rendition bit; no forced terminal mode.
            else if (value == 25) attribute_ &= 0x7f;
            else if (value == 39) attribute_ = static_cast<std::uint8_t>((attribute_ & 0xf8U) | 7);
            else if (value == 49) attribute_ &= 0x8f;
            else if (value >= 30 && value <= 37) {
                static constexpr std::array<std::uint8_t, 8> colors{
                    0, 4, 2, 6, 1, 5, 3, 7};
                attribute_ = static_cast<std::uint8_t>((attribute_ & 0xf8U) |
                    colors[static_cast<std::size_t>(value - 30)]);
            } else if (value >= 40 && value <= 47) {
                static constexpr std::array<std::uint8_t, 8> colors{
                    0, 4, 2, 6, 1, 5, 3, 7};
                attribute_ = static_cast<std::uint8_t>((attribute_ & 0x8fU) |
                    (colors[static_cast<std::size_t>(value - 40)] << 4U));
            }
        }
    }
}

void ControlPanelTerminalMirror::consume(const std::string_view bytes) noexcept {
    for (const unsigned char byte : bytes) {
        // The caller plays CSI | music; a local mirror must not render its
        // notes as text or play them again. Streaming state needs no buffer.
        if(parse_state_==ParseState::music) {
            if(byte==0x0eU || byte==0x0fU)parse_state_=ParseState::text;
            else if(byte==0x1bU)parse_state_=ParseState::escape;
            continue;
        }
        if (parse_state_ == ParseState::escape) {
            if (byte == '[') {
                parse_state_ = ParseState::control_sequence;
                parameters_.fill(0);
                parameter_count_ = 1;
                parameter_present_ = false;
                private_sequence_ = unsupported_sequence_ = false;
            } else {
                parse_state_ = ParseState::text;
            }
            continue;
        }
        if (parse_state_ == ParseState::control_sequence) {
            if(byte=='|' && !parameter_present_ && !private_sequence_ && !unsupported_sequence_ && parameter_count_==1) {
                parse_state_=ParseState::music;continue;
            }
            if (byte == '?' && !parameter_present_ && parameter_count_ == 1) {
                private_sequence_ = true;
                continue;
            }
            if (byte >= '0' && byte <= '9') {
                parameter_present_ = true;
                auto& value = parameters_[parameter_count_ - 1U];
                value = static_cast<std::uint16_t>(std::min<unsigned int>(999,
                    static_cast<unsigned int>(value) * 10U + (byte - '0')));
                continue;
            }
            if (byte == ';') {
                if (parameter_count_ < parameters_.size()) ++parameter_count_;
                else unsupported_sequence_ = true;
                continue;
            }
            if (byte >= 0x40U && byte <= 0x7eU) {
                apply_control_sequence(byte);
                parse_state_ = ParseState::text;
            } else unsupported_sequence_ = true;
            continue;
        }
        if (byte == 0x1bU) {
            parse_state_ = ParseState::escape;
        } else if (byte == '\r') {
            column_ = 0;
        } else if (byte == '\n') {
            newline();
        } else if (byte == '\b') {
            if (column_ != 0) --column_;
        } else if (byte == '\t') {
            // With wrapping disabled the last column cannot advance. Bound
            // expansion by the original tab stop, not the clamped cursor.
            const auto count = 8U - column_ % 8U;
            for (std::size_t index = 0; index < count; ++index) put(0x20);
        } else if (byte == 0x0fU) {
            // Match ANSI-BBS SI display, including the caller music cleanup.
            put(byte);
        } else if (byte >= 0x20U && byte != 0x7fU) {
            put(byte);
        }
    }
}

ControlPanelTerminalSnapshot ControlPanelTerminalMirror::snapshot() const noexcept {
    return screen_;
}

std::array<std::string, 4> control_panel_node_banner(
    const std::uint16_t node_number, const std::string_view abbreviated_state,
    const std::string_view product_version) {
    constexpr std::string_view ornament{">>>>>>>><<<<<<<<"};
    const std::string node = "NODE " + std::to_string(node_number) + " " +
        std::string(abbreviated_state.substr(0, 5));
    const std::string left = centered(">[  SF-X BBS  ]<", 16);
    const std::string right = ">[" + centered(node, 12) + "]<";
    return {
        std::string(ornament) + centered("SPITFIRE-X Bulletin Board System", 48) +
            std::string(ornament),
        std::string(ornament) + centered("In memory of Mike Woltz", 48) +
            std::string(ornament),
        left + centered("Copyright (C) 2026 Spitfire-X BBS Project", 48) + right,
        std::string(ornament) + centered("Spitfire-X BBS v" + std::string(product_version), 48) +
            std::string(ornament)};
}

std::optional<ControlPanelTerminalSnapshot> parse_control_panel_ansi_asset(
    const std::string_view bytes) noexcept {
    constexpr std::size_t sauce_size = 128;
    if (bytes.size() < sauce_size) return std::nullopt;
    const std::size_t sauce = bytes.size() - sauce_size;
    if (bytes.substr(sauce, 7) != "SAUCE00") return std::nullopt;
    const std::uint32_t data_size = little_u32(bytes.data() + sauce + 90U);
    if (data_size > sauce || little_u16(bytes.data() + sauce + 96U) != 80U ||
        little_u16(bytes.data() + sauce + 98U) != 25U) return std::nullopt;
    ControlPanelTerminalMirror mirror;
    mirror.consume(bytes.substr(0, data_size));
    return mirror.snapshot();
}

ControlPanelTerminalSnapshot compose_control_panel_node_view(
    const ControlPanelTerminalSnapshot& owner_template,
    const ControlPanelNodeViewFields& fields) noexcept {
    auto screen = owner_template;
    replace_characters(screen, 3, 72, 3, fields.abbreviated_state.substr(0, 3), 0x0f);
    replace_characters(screen, 6, 29, 51, fields.work_path, 0x08);
    replace_characters(screen, 7, 29, 51, fields.display_path, 0x08);
    replace_characters(screen, 8, 29, 51, fields.message_path, 0x08);
    replace_characters(screen, 9, 29, 51, fields.protocol_path, 0x08);
    replace_characters(screen, 10, 29, 51, fields.configuration_result, 0x08);
    replace_characters(screen, 11, 29, 51, fields.database_result, 0x08);
    replace_characters(screen, 12, 29, 51, fields.node_readiness_result, 0x08);
    replace_characters(screen, 13, 29, 51, fields.listener_result, 0x08);
    replace_characters(screen, 14, 0, 8, fields.health_summary_result, 0x0f);
    replace_characters(screen, 15, 2, 78, fields.ready_result, 0x07);
    ++screen.sequence;
    return screen;
}

ControlPanelScreenSaverFrame compose_control_panel_screen_saver(
    const ControlPanelTerminalSnapshot& owner_demo,
    const std::uint64_t movement_index,
    const std::size_t visible_columns,
    const std::size_t visible_rows) noexcept {
    ControlPanelScreenSaverFrame frame;
    for (auto& cell : frame.screen.cells) cell = {0x20, 0x07};
    frame.color_index = static_cast<std::uint8_t>(movement_index % 3U);
    constexpr std::size_t frame_width = 36;
    constexpr std::size_t frame_height = 5;
    const std::size_t bounded_columns = std::clamp(visible_columns,
        frame_width, kControlPanelTerminalColumns);
    const std::size_t bounded_rows = std::clamp(visible_rows,
        frame_height, kControlPanelTerminalRows);
    frame.left = static_cast<std::size_t>((4U + movement_index * 17U) %
        (bounded_columns - frame_width + 1U));
    frame.top = static_cast<std::size_t>((2U + movement_index * 7U) %
        (bounded_rows - frame_height + 1U));
    constexpr std::size_t source_left = 21;
    const std::size_t source_top = 2U + static_cast<std::size_t>(frame.color_index) * 6U;
    for (std::size_t row = 0; row < frame_height; ++row) {
        for (std::size_t column = 0; column < frame_width; ++column) {
            frame.screen.cells[(frame.top + row) * kControlPanelTerminalColumns +
                frame.left + column] = owner_demo.cells[(source_top + row) *
                kControlPanelTerminalColumns + source_left + column];
        }
    }
    frame.screen.sequence = movement_index + 1U;
    return frame;
}

std::string serialize_control_panel_ui_preferences(
    const ControlPanelUiPreferences& preferences) {
    std::string result = "SFX_CONTROL_PANEL_UI=5\nleft=" + std::to_string(preferences.left) +
        "\ntop=" + std::to_string(preferences.top) +
        "\nwidth=" + std::to_string(preferences.width) +
        "\nheight=" + std::to_string(preferences.height) +
        "\nsystem_monitor=" + (preferences.system_monitor_visible ? "1\n" : "0\n") +
        "system_monitor_grayscale=" + (preferences.system_monitor_grayscale ? "1\n" : "0\n") +
        "modem_lights=" + (preferences.modem_lights ? "1\n" : "0\n") +
        "system_monitor_width=" + std::to_string(preferences.system_monitor_width) + "\n";
    for (std::size_t index = 0; index < preferences.node_column_widths.size(); ++index)
        result += "node_column_" + std::to_string(index) + "=" +
            std::to_string(preferences.node_column_widths[index]) + "\n";
    return result;
}

std::optional<ControlPanelUiPreferences> parse_control_panel_ui_preferences(
    const std::string_view serialized) noexcept {
    const bool version_one = serialized.substr(0, 23) == "SFX_CONTROL_PANEL_UI=1\n";
    const bool version_two = serialized.substr(0, 23) == "SFX_CONTROL_PANEL_UI=2\n";
    const bool version_three = serialized.substr(0, 23) == "SFX_CONTROL_PANEL_UI=3\n";
    const bool version_four = serialized.substr(0, 23) == "SFX_CONTROL_PANEL_UI=4\n";
    const bool version_five = serialized.substr(0, 23) == "SFX_CONTROL_PANEL_UI=5\n";
    if (!version_one && !version_two && !version_three && !version_four && !version_five) return std::nullopt;
    const auto left = integer_field(preference_field(serialized, "left"));
    const auto top = integer_field(preference_field(serialized, "top"));
    const auto width = integer_field(preference_field(serialized, "width"));
    const auto height = integer_field(preference_field(serialized, "height"));
    const auto monitor = preference_field(serialized, "system_monitor");
    if (!left || !top || !width || !height || *width < 720 || *width > 10000 ||
        *height < 480 || *height > 10000 || (monitor != "0" && monitor != "1")) return std::nullopt;
    ControlPanelUiPreferences result;
    result.left = *left; result.top = *top; result.width = *width; result.height = *height;
    result.system_monitor_visible = monitor == "1";
    if (version_three || version_four || version_five) {
        const auto grayscale = preference_field(serialized, "system_monitor_grayscale");
        if (grayscale != "0" && grayscale != "1") return std::nullopt;
        result.system_monitor_grayscale = grayscale == "1";
    }
    if (version_four || version_five) {
        const auto modem = preference_field(serialized, "modem_lights");
        if (modem != "0" && modem != "1") return std::nullopt;
        result.modem_lights = modem == "1";
    }
    if (version_five) {
        const auto value = integer_field(preference_field(serialized, "system_monitor_width"));
        if (!value || *value < 251 || *value > 520) return std::nullopt;
        result.system_monitor_width = *value;
    }
    if (version_two || version_three || version_four || version_five) {
        for (std::size_t index = 0; index < result.node_column_widths.size(); ++index) {
            const auto value = integer_field(preference_field(serialized,
                "node_column_" + std::to_string(index)));
            if (!value || *value < 40 || *value > 2000) return std::nullopt;
            result.node_column_widths[index] = *value;
        }
    }
    // Preserve strict V3/V4 validation while supplying only the missing defaults.
    if (version_three || version_four || version_five) {
        auto canonical = serialize_control_panel_ui_preferences(result);
        if (!version_five) {
            canonical.replace(0, canonical.find('\n'), "SFX_CONTROL_PANEL_UI=4");
            const std::string width_line = "system_monitor_width=251\n";
            canonical.erase(canonical.find(width_line), width_line.size());
        }
        if (version_three) {
            canonical.replace(0, canonical.find('\n'), "SFX_CONTROL_PANEL_UI=3");
            const std::string line = "modem_lights=0\n";
            canonical.erase(canonical.find(line), line.size());
        }
        if (canonical != serialized) return std::nullopt;
    }
    return result;
}

ControlPanelSqliteMetricSnapshot ControlPanelSqliteRateTracker::sample(
    const ControlPanelSqlitePagerCounters& counters,
    const std::uint64_t monotonic_millis) noexcept {
    ControlPanelSqliteMetricSnapshot result;
    result.available = counters.available;
    if (!counters.available) { reset(); return result; }
    if (!previous_ || previous_->connection_generation != counters.connection_generation ||
        monotonic_millis <= previous_millis_ || counters.cache_hits < previous_->cache_hits ||
        counters.cache_misses < previous_->cache_misses ||
        counters.cache_writes < previous_->cache_writes) {
        previous_ = counters;
        previous_millis_ = monotonic_millis;
        return result;
    }
    const std::uint64_t elapsed = monotonic_millis - previous_millis_;
    const std::uint64_t hits = counters.cache_hits - previous_->cache_hits;
    const std::uint64_t misses = counters.cache_misses - previous_->cache_misses;
    const std::uint64_t writes = counters.cache_writes - previous_->cache_writes;
    const std::uint64_t reads = hits > std::numeric_limits<std::uint64_t>::max() - misses
        ? std::numeric_limits<std::uint64_t>::max() : hits + misses;
    result.interval_available = true;
    result.page_reads_per_second = reads > std::numeric_limits<std::uint64_t>::max() / 1000U
        ? std::numeric_limits<std::uint64_t>::max() : reads * 1000U / elapsed;
    result.page_writes_per_second = writes > std::numeric_limits<std::uint64_t>::max() / 1000U
        ? std::numeric_limits<std::uint64_t>::max() : writes * 1000U / elapsed;
    if (reads != 0) {
        result.cache_hit_available = true;
        result.cache_hit_basis_points = static_cast<std::uint16_t>(std::min<std::uint64_t>(10000,
            hits > std::numeric_limits<std::uint64_t>::max() / 10000U
                ? 10000U : hits * 10000U / reads));
    }
    previous_ = counters;
    previous_millis_ = monotonic_millis;
    return result;
}

void ControlPanelSqliteRateTracker::reset() noexcept {
    previous_.reset();
    previous_millis_ = 0;
}

bool control_panel_monitor_should_poll(
    const int selected_primary_tab, const bool monitor_visible) noexcept {
    return selected_primary_tab == 0 && monitor_visible;
}

double sqlite_activity_graph_maximum(const std::vector<double>& values) noexcept {
    double peak = 0.0;
    for (const auto value : values) if (std::isfinite(value)) peak = std::max(peak, value);
    // 1,000 is a display reference, not a performance or warning threshold.
    // Expand in stable steps and leave at least 25% of the plot as headroom.
    double maximum = 1000.0;
    while (peak > maximum * 0.75 && maximum <= std::numeric_limits<double>::max() / 2.0)
        maximum *= 2.0;
    return maximum;
}

double disk_activity_graph_maximum(const std::vector<double>& values) noexcept {
    double peak = 0.0;
    for (const double value : values)
        if (std::isfinite(value)) peak = std::max(peak, value);
    double maximum = 64.0 * 1024.0;
    while (peak > maximum) {
        if (maximum > std::numeric_limits<double>::max() / 4.0)
            return std::numeric_limits<double>::max();
        maximum *= 4.0;
    }
    return maximum;
}

BoundedMetricHistory::BoundedMetricHistory(const std::size_t capacity) : capacity_(capacity) {
    values_.reserve(capacity);
}

void BoundedMetricHistory::push(const double value) noexcept {
    if (capacity_ == 0) return;
    if (values_.size() == capacity_) values_.erase(values_.begin());
    values_.push_back(std::max(0.0, value));
}

const std::vector<double>& BoundedMetricHistory::values() const noexcept { return values_; }

}  // namespace sfx::application
