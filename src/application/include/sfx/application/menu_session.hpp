#pragma once
#include "sfx/application/display.hpp"
#include "sfx/application/menu_storage.hpp"
#include "sfx/application/phase3a_storage.hpp"
#include "sfx/application/file_session.hpp"
#include "sfx/application/door.hpp"
#include "sfx/application/door_bulletin.hpp"
#include "sfx/application/chat.hpp"
#include <functional>

namespace sfx::application {
// Selected B console alert. No parser/mute changes or ASCII fallback.
std::string caller_inactivity_music(GraphicsMode graphics);
class CallerTimeWarnings final {
public:
    bool pending(std::optional<std::int64_t> seconds) const noexcept;
    std::optional<std::string> take(std::optional<std::int64_t> seconds);

private:
    int last_threshold_{301};
};
struct MenuSessionSnapshot {
    MenuAccess access;
    std::uint64_t configuration_revision{};
    MenuPreferenceRecord preferences;
    GraphicsMode graphics{GraphicsMode::ascii};
    TrustedDisplayRenderContext context;
    unsigned columns{80};
    bool lightbars_enabled{true};
    std::uint8_t default_color{7};
    std::uint8_t input_color{11};
    std::uint8_t selected_foreground{0}, selected_background{7};
    bool compact_footer{false};
    std::string message_group, message_echo;
    std::string file_group, file_area;
    std::uint16_t file_area_number{};
    unsigned rows{25};
};
enum class DoorMenuExit { main_menu, goodbye, quick_goodbye, confirmed_goodbye };
struct MenuSessionPorts {
    std::function<std::optional<MenuSessionSnapshot>()> snapshot;
    std::function<TrustedDisplaySelectionResult(const TrustedDisplaySelectionRequest&)> artwork;
    std::function<bool(const TerminalDisplayProgram&)> display;
    std::function<bool(std::string_view)> write;
    // Owns transport polling, live revocation, AFK, shutdown and notice overlays.
    std::function<std::optional<MenuInput>(MenuInputDecoder&, const std::function<bool()>&)> read;
    std::function<bool(const UpdateMenuPreferenceRequest&)> save_preferences;
    // Saved preference only; does not expose or start a File transfer engine.
    std::function<std::optional<TransferPreferenceRecord>()> transfer_preference;
    std::function<bool(const UpdateTransferPreferenceRequest&)> save_transfer_preference;
    // Implemented native actions only. Never invoked for deferred commands.
    std::function<std::optional<std::string>(MenuAction)> action;
    std::function<std::optional<FileSessionEffect>(MenuAction,MenuId)> file_action;
    std::function<std::optional<bool>(MenuId,MenuId)> file_navigation;
    // null = caller lifetime ended; explicit menu/normal/quick logoff result.
    std::function<std::optional<DoorMenuExit>()> doors;
    // Chat distinguishes Main, confirmed Goodbye and immediate Quick Goodbye.
    std::function<std::optional<ChatExit>(MenuAction)> chat;
    std::function<std::optional<MenuDoorCommands>(MenuId)> door_commands;
    std::function<std::optional<std::string>(const MenuDoorSelection&)> door_command;
    std::function<std::int64_t()> utc_now;
    std::optional<std::int64_t> previous_login_utc;
    std::function<bool()> login_message_scan;
    std::function<bool()> login_file_scan;
    // Pass the exact native prompt so a safe notice can restore it without
    // redrawing/clearing authored artwork or disturbing a pending command.
    std::function<bool(std::string_view)> safe_boundary;
    // Shared typed notice surface: menu content precedes these rows, and the
    // complete native status/command footer follows them. No fixed row number.
    std::function<std::vector<MenuTextRow>()> notifications;
    // Enter-only operational overlays may interrupt help without making it a
    // safe surface for ordinary arrivals or deferred AFK presentation.
    std::function<std::vector<MenuTextRow>()> operational_notifications;
    // Typed location publication, independent of authored menu text. Optional
    // presentation only; the lifetime-stable assignment owns stale-call gates.
    std::function<void(MenuId)> menu_entered;
    // Authenticated logon after scans; voluntary logoff before goodbye. Never
    // called recursively for carrier loss, time expiry or event failure.
    std::function<bool(DoorAutomaticEvent)> door_event;
    std::function<std::optional<std::vector<DoorBulletinNotice>>(std::int64_t)> door_bulletin_updates;
    std::function<std::optional<std::string>()> calling_time_warning;
};
// True only for a caller-requested normal Goodbye. Null/failure/cancellation
// exits false and leaves the caller session's established lifetime owner intact.
bool run_menu_session(const MenuSessionPorts& ports, MenuId initial_menu=MenuId::main,
    bool login_information=false);
TrustedDisplayIdentity menu_display_identity(MenuId id) noexcept;
TerminalDisplayProgram trim_menu_artwork_padding(TerminalDisplayProgram program);
std::string menu_duration_words(std::uint64_t seconds);
std::vector<MenuTextRow> menu_footer_rows(const MenuSessionSnapshot&,std::string prompt,MenuId menu=MenuId::main);
std::string caller_inactivity_warning(GraphicsMode graphics, const MenuPreferences& preferences,
    std::uint8_t default_color, std::uint8_t input_color);
} // namespace sfx::application
