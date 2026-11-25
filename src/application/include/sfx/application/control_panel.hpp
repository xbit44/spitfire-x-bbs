#pragma once

#include "sfx/core/node.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {

inline constexpr std::size_t kControlPanelTerminalColumns = 80;
inline constexpr std::size_t kControlPanelTerminalRows = 25;
inline constexpr std::size_t kControlPanelTerminalCells =
    kControlPanelTerminalColumns * kControlPanelTerminalRows;
inline constexpr std::size_t kControlPanelMaximumLogEntries = 32;

// Measured against the owner's Moebius 1.0.29 palette.js. The authoring
// palette is six-bit EGA; VGA expands each channel with (v << 2) | (v >> 4).
inline constexpr std::array<std::array<std::uint8_t, 3>, 16>
    kControlPanelMoebiusEgaPalette{{
        {{0,0,0}}, {{0,0,42}}, {{0,42,0}}, {{0,42,42}},
        {{42,0,0}}, {{42,0,42}}, {{42,21,0}}, {{42,42,42}},
        {{21,21,21}}, {{21,21,63}}, {{21,63,21}}, {{21,63,63}},
        {{63,21,21}}, {{63,21,63}}, {{63,63,21}}, {{63,63,63}}
    }};
constexpr std::uint8_t control_panel_vga_channel(const std::uint8_t value) noexcept {
    return static_cast<std::uint8_t>((value << 2U) | (value >> 4U));
}

struct ControlPanelTerminalCell {
    std::uint8_t character{0x20};
    std::uint8_t attribute{0x07};
};

struct ControlPanelTerminalSnapshot {
    std::array<ControlPanelTerminalCell, kControlPanelTerminalCells> cells{};
    std::uint64_t sequence{0};
};

struct ControlPanelTerminalGridLayout {
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t cell_width{0};
    std::int32_t cell_height{0};
    std::int32_t right{0};
    std::int32_t bottom{0};
};

std::optional<ControlPanelTerminalGridLayout> control_panel_terminal_grid_layout(
    std::int32_t pane_width, std::int32_t pane_height,
    std::int32_t measured_glyph_width, std::int32_t text_metric_height,
    std::int32_t external_leading) noexcept;

std::optional<ControlPanelTerminalGridLayout> control_panel_idle_reference_layout(
    std::int32_t pane_width, std::int32_t pane_height,
    std::int32_t maximum_cell_width) noexcept;

// UI-only device pixels. No DPI multiplier, caller geometry or wire state.
enum class NodeViewScale : std::uint16_t { auto_fit = 0, percent100 = 100,
    percent125 = 125, percent150 = 150, percent200 = 200 };
std::optional<NodeViewScale> node_view_scale(std::uint32_t value) noexcept;
struct NodeViewPixelSize { std::int32_t width{0}; std::int32_t height{0}; };
std::optional<NodeViewPixelSize> node_view_pixel_size(std::int32_t viewport_width,
    std::int32_t viewport_height, NodeViewScale scale) noexcept;

struct ScreenSaverPosition { std::int32_t x{0}; std::int32_t y{0}; };
// One instance per local Node View. Randomness has no security authority.
// A fixed seed provides deterministic tests; repaint never advances movement.
class ScreenSaverPlacement final {
public:
    void seed(std::uint64_t value) noexcept;
    ScreenSaverPosition place(std::int32_t maximum_x, std::int32_t maximum_y,
        std::uint64_t movement_index) noexcept;
private:
    std::uint64_t random_{1}, movement_{0};
    std::int32_t maximum_x_{-1}, maximum_y_{-1};
    ScreenSaverPosition position_;
    bool positioned_{false};
};

class ControlPanelTerminalMirror final {
public:
    ControlPanelTerminalMirror() noexcept;

    void clear() noexcept;
    void consume(std::string_view bytes) noexcept;
    ControlPanelTerminalSnapshot snapshot() const noexcept;
    std::size_t cursor_row() const noexcept {return row_<kControlPanelTerminalRows?row_:kControlPanelTerminalRows-1;}
    std::size_t cursor_column() const noexcept {return column_<kControlPanelTerminalColumns?column_:kControlPanelTerminalColumns-1;}

private:
    enum class ParseState : std::uint8_t { text, escape, control_sequence, music };

    void put(std::uint8_t character) noexcept;
    void newline() noexcept;
    void apply_control_sequence(std::uint8_t final_byte) noexcept;

    ControlPanelTerminalSnapshot screen_;
    std::size_t row_{0};
    std::size_t column_{0};
    std::uint8_t attribute_{0x07};
    ParseState parse_state_{ParseState::text};
    std::array<std::uint16_t, 8> parameters_{};
    std::size_t parameter_count_{0};
    bool parameter_present_{false};
    bool private_sequence_{false};
    bool unsupported_sequence_{false};
    bool auto_wrap_{true};
};

std::array<std::string, 4> control_panel_node_banner(
    std::uint16_t node_number, std::string_view abbreviated_state,
    std::string_view product_version);

std::optional<ControlPanelTerminalSnapshot> parse_control_panel_ansi_asset(
    std::string_view bytes) noexcept;

struct ControlPanelNodeViewFields {
    std::string abbreviated_state;
    std::string work_path;
    std::string display_path;
    std::string message_path;
    std::string protocol_path;
    std::string configuration_result;
    std::string database_result;
    std::string node_readiness_result;
    std::string listener_result;
    std::string health_summary_result;
    std::string ready_result;
};

struct ControlPanelNodeViewLiveRange {
    std::size_t row{0};
    std::size_t column{0};
    std::size_t width{0};
};

// These are the only cells whose characters may differ from the owner ANSI.
// Written characters receive the foreground for their field role at composition
// time; their background nibble remains authored. Cleared, unwritten capacity
// retains its authored attributes. All cells outside these ranges stay byte-exact.
inline constexpr std::array<ControlPanelNodeViewLiveRange, 11>
    kControlPanelNodeViewLiveRanges{{
        {3, 72, 3},
        {6, 29, 51}, {7, 29, 51}, {8, 29, 51}, {9, 29, 51},
        {10, 29, 51}, {11, 29, 51}, {12, 29, 51}, {13, 29, 51},
        {14, 0, 8}, {15, 2, 78}
    }};

ControlPanelTerminalSnapshot compose_control_panel_node_view(
    const ControlPanelTerminalSnapshot& owner_template,
    const ControlPanelNodeViewFields& fields) noexcept;

struct ControlPanelScreenSaverFrame {
    ControlPanelTerminalSnapshot screen;
    std::size_t top{0};
    std::size_t left{0};
    std::uint8_t color_index{0};
};

ControlPanelScreenSaverFrame compose_control_panel_screen_saver(
    const ControlPanelTerminalSnapshot& owner_demo,
    std::uint64_t movement_index,
    std::size_t visible_columns = kControlPanelTerminalColumns,
    std::size_t visible_rows = kControlPanelTerminalRows) noexcept;

struct ControlPanelUiPreferences {
    std::int32_t left{80};
    std::int32_t top{80};
    std::int32_t width{1080};
    std::int32_t height{700};
    bool system_monitor_visible{false};
    bool system_monitor_grayscale{false};
    bool modem_lights{false};
    std::int32_t system_monitor_width{251}; // logical pixels, operator preference
    std::array<std::int32_t, 6> node_column_widths{{82, 120, 140, 105, 135, 180}};
};

struct ControlPanelMetricSnapshot {
    bool cpu_available{false};
    double total_cpu_percent{0};
    std::vector<double> per_core_cpu_percent;
    bool memory_available{false};
    std::uint64_t memory_used_bytes{0};
    std::uint64_t memory_available_bytes{0};
    std::uint64_t memory_total_bytes{0};
    bool storage_activity_available{false};
    double storage_activity_percent{0}; // Legacy slot; disk card now uses byte rates.
    double storage_read_bytes_per_second{0};
    double storage_write_bytes_per_second{0};
    bool network_available{false};
    double network_receive_bytes_per_second{0};
    double network_send_bytes_per_second{0};
    std::uint64_t network_received_bytes{0};
    std::uint64_t network_sent_bytes{0};
    std::string local_ip;
    bool database_volume_available{false}, backup_volume_available{false};
    bool shared_storage_volume{false};
    std::uint64_t database_volume_free_bytes{0}, backup_volume_free_bytes{0};
};

struct ControlPanelSqlitePagerCounters {
    bool available{false};
    std::uint64_t connection_generation{0};
    std::uint64_t cache_hits{0};
    std::uint64_t cache_misses{0};
    std::uint64_t cache_writes{0};
};

struct ControlPanelSqliteMetricSnapshot {
    bool available{false};
    bool interval_available{false};
    std::uint64_t page_reads_per_second{0};
    std::uint64_t page_writes_per_second{0};
    bool cache_hit_available{false};
    std::uint16_t cache_hit_basis_points{0};
};

class ControlPanelSqliteRateTracker final {
public:
    ControlPanelSqliteMetricSnapshot sample(
        const ControlPanelSqlitePagerCounters& counters,
        std::uint64_t monotonic_millis) noexcept;
    void reset() noexcept;

private:
    std::optional<ControlPanelSqlitePagerCounters> previous_;
    std::uint64_t previous_millis_{0};
};

std::string serialize_control_panel_ui_preferences(
    const ControlPanelUiPreferences& preferences);
std::optional<ControlPanelUiPreferences> parse_control_panel_ui_preferences(
    std::string_view serialized) noexcept;
bool control_panel_monitor_should_poll(
    int selected_primary_tab, bool monitor_visible) noexcept;

// Millisecond counters, never UTC or 32-bit tick arithmetic. Both durations
// include OS sleep/hibernate time, matching Windows GetTickCount64 semantics.
std::string compact_uptime(std::uint64_t milliseconds);
double uptime_reference_fraction(std::uint64_t bbs, std::uint64_t os) noexcept;

// Page operations/second, not a utilization percentage or database capacity.
// A stable floor keeps light activity legible without filling the graph.
double sqlite_activity_graph_maximum(const std::vector<double>& values) noexcept;

// Combined physical-disk read/write bytes per second. Linear powers-of-four
// ranges with a stable 64 KiB/s floor, labeled explicitly in the card.
double disk_activity_graph_maximum(const std::vector<double>& values) noexcept;

class BoundedMetricHistory final {
public:
    explicit BoundedMetricHistory(std::size_t capacity);
    void push(double value) noexcept;
    const std::vector<double>& values() const noexcept;

private:
    std::size_t capacity_{0};
    std::vector<double> values_;
};

}  // namespace sfx::application
