#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {

enum class MenuId : std::uint8_t {
    // Retired menu values 3/4 stay unused; existing live menu IDs are stable.
    main, messages, files, doors=5, bulletins,
    door_bulletins, chat, settings, file_groups, file_areas, batch_transfer, qwk
};
enum class MenuAction : std::uint8_t {
    open, help, goodbye, unavailable, online, password, graphics, hotkey, color, transfer_preference,
    caller_profile, caller_directory, locate_caller, other_boards, system_information, quick_goodbye,
    message_change, message_read, message_browse, message_addressed, message_enter,
    message_personal, message_settings, message_search, comment_sysop,
    qwk_download, qwk_upload, qwk_settings, qwk_pointers,
    file_change, file_list, file_new, file_find, file_search, file_read, file_archive,
    file_download_add, file_upload_add, file_upload_view, file_download_view, file_both_view,
    file_upload_remove, file_download_remove, file_both_remove,
    file_upload_clear, file_download_clear, file_both_clear,
    file_upload_begin, file_download_begin, file_private, file_both_add, file_settings, chat_dnd, chat_notices, page_sysop,
    chat_room, chat_rooms, chat_text, sysop_utilities
};
struct MenuCommand {
    char key;
    std::string_view label;
    MenuAction action{MenuAction::unavailable};
    MenuId destination{MenuId::main};
    bool sysop_only{false};
};
struct MenuDefinition {
    MenuId id;
    std::string_view title;
    std::string_view asset_base;
    MenuId back;
    std::string_view empty_notice;
    std::vector<MenuCommand> commands;
};
struct MenuAccess {
    std::uint16_t security_level{};
    // Supplied by authoritative Runtime access, including independent SY auth.
    bool sysop_authorized{false};
    std::uint64_t revision{};
};
struct MenuSnapshot {
    MenuId menu{MenuId::main};
    std::uint64_t access_revision{};
    std::uint64_t configuration_revision{};
    std::vector<MenuCommand> commands;
};
// Captured named placements; both revisions must still match at launch.
// The storage adapter reauthorizes the caller and shared Door definition.
struct MenuDoorCommand { char key{}; std::string name; };
struct MenuDoorCommands {
    std::int64_t revision{},door_revision{};
    std::vector<MenuDoorCommand> commands;
};
struct MenuDoorSelection {
    MenuId menu{MenuId::main}; char key{};
    std::int64_t revision{},door_revision{};
};
bool primary_door_menu(MenuId) noexcept;
bool available_door_command_key(MenuId,char) noexcept;
enum class MenuRouteCode : std::uint8_t { command, redraw, invalid, denied, refresh };
struct MenuRoute {
    MenuRouteCode code{MenuRouteCode::invalid};
    MenuAction action{MenuAction::unavailable};
    MenuId destination{MenuId::main};
};
enum class MenuTextRole : std::uint8_t { heading, command, event, status, prompt, feedback, notification, inactivity_warning };
struct MenuTextRow {
    MenuTextRole role;
    std::string text;
    // Generated main-menu footer. Status uses the caller color and its prompt
    // has a fixed palette. Modal questions use their own system role.
    bool main_footer{false};
};

const MenuDefinition& menu_definition(MenuId id);
MenuSnapshot menu_snapshot(MenuId id, const MenuAccess& access, std::uint64_t configuration_revision);
MenuRoute route_menu(const MenuSnapshot& shown, unsigned char key, const MenuAccess& current,
                     std::uint64_t configuration_revision);
// Text/layout only: the terminal presentation adapter applies color roles.
// No socket, file, storage, ANSI escape, or authority comes from artwork here.
std::vector<MenuTextRow> builtin_menu_rows(const MenuSnapshot& snapshot,
    std::string_view date_time_line, std::string_view session_time_line,
    std::string_view event_line = {}, unsigned columns = 80);
std::string menu_prompt(const MenuSnapshot& snapshot);
inline constexpr std::string_view unavailable_menu_feature = "This feature is not available yet.";

struct MenuPreferences {
    // Zero means Use BBS default. 1..15 are fixed VGA foreground indexes.
    std::uint8_t color{0};
    bool hotkey{false};
    bool lightbar{true};
};
struct MenuColor { std::uint8_t value; std::string_view name; };
const std::array<MenuColor,16>& menu_colors() noexcept;
bool valid_menu_preferences(const MenuPreferences& preferences) noexcept;
std::string menu_text_sgr(std::uint8_t foreground, std::uint8_t background = 0);
// Generated system controls: gray brackets/labels, only short hotkeys white.
// Input is inert text; this helper never interprets embedded escape sequences.
std::string system_menu_text(std::string_view text, bool ansi);
std::string render_menu_rows(const std::vector<MenuTextRow>& rows, bool ansi,
    const MenuPreferences& preferences, std::uint8_t board_color = 7);

enum class MenuInputKind : std::uint8_t { pending, command, number, back, redraw, up, down, invalid, quick_goodbye, activate };
struct MenuInput { MenuInputKind kind{MenuInputKind::pending}; unsigned value{0}; };
// Menu-reader echo only; never use for secret/editor input. A numeric hotkey
// echoes its final raw digit because an earlier prefix was already displayed.
std::string menu_input_echo(unsigned char byte, std::string_view before,
    MenuInput decoded, std::string_view after);
// Byte-at-a-time decoder: transport polling, AFK and cancellation remain in
// Runtime. Escape tails never leak into command dispatch. Numeric input waits
// for Enter unless a menu explicitly supplies its complete selectable numbers.
class MenuInputDecoder final {
public:
    explicit MenuInputDecoder(bool hotkey = false, bool numbered = false, bool quick_goodbye = false, bool arrow_activate = false) noexcept;
    MenuInput feed(unsigned char byte);
    MenuInput idle(); // resolve a bare Escape at a transport poll boundary
    void reset(bool hotkey, bool numbered, bool quick_goodbye = false, bool arrow_activate = false) noexcept;
    void discard_pending() noexcept;
    // A completed modal action consumed its own acknowledgment. Keep CR/LF
    // coalescing, but do not suppress the next menu's independent Enter.
    void completed_action() noexcept;
    void adopt_pending_delimiter_lf(bool value) noexcept {discard_lf_=discard_lf_||value;}
    bool take_pending_delimiter_lf() noexcept {const bool value=discard_lf_;discard_lf_=false;return value;}
    void numeric_choices(std::vector<unsigned> choices);
    std::string_view pending_text() const noexcept { return text_; }
    bool selection_in_progress() const noexcept { return !text_.empty() || invalid_ || escape_!=0; }
private:
    bool hotkey_{false}, numbered_{false}, quick_goodbye_{false}, discard_lf_{false}, discard_enter_{false};
    bool arrow_activate_{}; // Door opt-in; bare Enter retains its locked Back action.
    unsigned escape_{0};
    bool invalid_{false};
    std::string text_;
    std::vector<unsigned> numeric_choices_;
};

struct NumberedMenuEntry {
    unsigned number{};
    std::string stable_id;
    std::string label;
};
struct NumberedMenuSnapshot {
    std::uint64_t access_revision{}, configuration_revision{};
    std::vector<NumberedMenuEntry> entries;
};
bool valid_numbered_menu(const NumberedMenuSnapshot& snapshot) noexcept;
// Reordered/removed/replaced items require redraw and a new explicit choice.
std::optional<std::string> select_numbered_menu(const NumberedMenuSnapshot& shown,
    unsigned number, const NumberedMenuSnapshot& current);

} // namespace sfx::application
