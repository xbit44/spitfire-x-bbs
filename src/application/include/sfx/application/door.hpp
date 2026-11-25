#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {

// Permanent IDs are independent of placements. Zero names only the root menu.
using DoorId = std::uint16_t;
using DoorMenuId = std::uint16_t;
inline constexpr std::size_t kDoorMaximumDefinitions = 999;
inline constexpr std::size_t kDoorMaximumMenuEntries = 99;
enum class DoorSecurityMode : std::uint8_t { minimum, maximum, exact };
struct DoorRequirements {
    DoorSecurityMode mode{DoorSecurityMode::minimum};
    std::uint16_t level{1};
    bool sysop_only{};
};
enum class RloginAuthentication : std::uint8_t {
    traditional, synchronet_caller_hash, synchronet_predefined_hash
};
enum class RloginIdentity : std::uint8_t { caller_alias, configured };
struct RloginTarget {
    std::string host;
    std::uint16_t port{513};
    RloginAuthentication authentication{RloginAuthentication::traditional};
    RloginIdentity server_identity{RloginIdentity::caller_alias};
    std::string server_name;
    // Traditional mode uses this explicit value when no credential is supplied.
    // Passwords and derived bearer credentials never belong in this field.
    std::string client_name;
    std::string terminal_type{"ansi"};
    std::string salt; // Empty selects the board's QWK ID, as Synchronet does.
    // Remote credentials and pepper are protected by the native installation
    // secret adapter. The portable/catalog layer never decrypts these blobs.
    std::vector<std::uint8_t> protected_credential, protected_pepper;
    // Legacy record/IPC field name retained. Uses the authenticated connection's
    // caller password automatically; it no longer requests a second password.
    bool prompt_credential{};
    std::uint16_t connection_timeout_seconds{10};
    bool quiet{true}, banner{}, pause{};
};
struct TelnetTarget {
    std::string host;
    std::uint16_t port{23};
    std::string terminal_type{"ansi"};
    std::uint16_t connection_timeout_seconds{10};
};
enum class DoorExecutionKind : std::uint8_t { rlogin=0, local=1, telnet=2 };
enum class DoorProgramType : std::uint8_t { dos, native };
enum class DoorIo : std::uint8_t { standard, fossil_or_uart, uart_only, fossil_only, socket };
enum class DoorDropFile : std::uint8_t {
    none, xtrn_dat, chain_txt, door_sys52, dorinfo_node, callinfo_bbs,
    pcboard_sys, sfdoors_dat, door_sys31, doorfile_sr, dorinfo1_def,
    tribbs_sys, door32_sys, bbsdev_drp, doordrop_ini
};
enum class DoorDropLocation : std::uint8_t { node, startup, temporary };
enum class DoorAutomaticEvent : std::uint8_t { none, logon, logoff };
struct DoorProgram {
    std::string startup_directory, command_line, cleanup_command;
    DoorProgramType type{DoorProgramType::dos};
    DoorIo io{DoorIo::fossil_or_uart};
    bool command_shell{};
    DoorDropFile drop_file{DoorDropFile::none};
    DoorDropLocation drop_location{DoorDropLocation::node};
    bool doordrop_utf8{};
};
struct DoorDefinition {
    DoorId id{};
    std::string name;
    bool enabled{};
    DoorSecurityMode security_mode{DoorSecurityMode::minimum};
    std::uint16_t security_level{1};
    bool sysop_only{};
    RloginTarget rlogin;
    TelnetTarget telnet;
    DoorRequirements execution;
    std::string code;
    DoorExecutionKind kind{DoorExecutionKind::rlogin};
    DoorProgram program;
    bool allow_multiple{true};
    DoorAutomaticEvent automatic_event{DoorAutomaticEvent::none};
    bool event_only{};
};
struct DoorMenuDefinition {
    DoorMenuId id{}; std::string name;
    DoorRequirements access{}, execution{};
};
enum class DoorEntryKind : std::uint8_t { door, submenu };
struct DoorMenuEntry {
    DoorMenuId menu{};
    std::uint8_t number{};
    DoorEntryKind kind{DoorEntryKind::door};
    std::uint16_t target{};
    std::string title, description;
};
struct DoorCatalog {
    std::int64_t revision{1};
    std::vector<DoorDefinition> doors;
    std::vector<DoorMenuDefinition> menus{{0,"Door Menu",{}, {}}};
    std::vector<DoorMenuEntry> entries;
};
// Definitions only. Placement is committed with the guarded first-run defaults.
enum class StarterDoorPlatform { windows, linux_native };
std::vector<DoorDefinition> fresh_install_starter_doors(StarterDoorPlatform platform=StarterDoorPlatform::windows);
struct DoorValidationError {
    std::string code;
    std::uint16_t identity{};
    std::uint8_t entry{};
};
struct DoorValidation {
    std::vector<DoorValidationError> errors;
    bool valid() const noexcept { return errors.empty(); }
};
struct DoorAuthority { std::uint16_t security_level{}, sysop_threshold{}; };
bool valid_door_text(std::string_view text, std::size_t maximum, bool empty = false) noexcept;
bool valid_rlogin_target(const RloginTarget&) noexcept;
bool valid_telnet_target(const TelnetTarget&,bool slot=false) noexcept;
bool valid_door_definition(const DoorDefinition&) noexcept;
bool valid_door_code(std::string_view) noexcept;
std::string effective_door_code(const DoorDefinition&);
std::string suggest_door_code(const DoorCatalog&, std::string_view name, DoorId);
bool door_has_command(const DoorDefinition&) noexcept;
DoorValidation validate_door_catalog(const DoorCatalog&);
bool door_accessible(const DoorDefinition&, DoorAuthority) noexcept;
bool valid_door_requirements(const DoorRequirements&) noexcept;
bool meets_door_requirements(const DoorRequirements&, DoorAuthority) noexcept;
bool door_executable(const DoorDefinition&, DoorAuthority) noexcept;
const DoorDefinition* find_door(const DoorCatalog&, DoorId) noexcept;
const DoorMenuDefinition* find_door_menu(const DoorCatalog&, DoorMenuId) noexcept;
std::vector<DoorMenuEntry> visible_door_entries(const DoorCatalog&, DoorMenuId, DoorAuthority);
// A selection is bound to its complete displayed placement and shared target.
// Any catalog edit requires a fresh screen; stale numeric input never launches.
std::optional<DoorMenuEntry> select_door_entry(const DoorCatalog& displayed,
    const DoorCatalog& current, DoorMenuId menu, std::uint8_t number, DoorAuthority);
std::optional<DoorId> next_unused_door_id(const DoorCatalog&) noexcept;
std::optional<DoorMenuId> next_unused_door_menu_id(const DoorCatalog&) noexcept;
bool copy_door_entry(DoorCatalog&, DoorMenuId source, std::uint8_t source_number,
    DoorMenuId destination, std::uint8_t destination_number);
bool duplicate_door_definition(DoorCatalog&, DoorId source, DoorId destination);
bool duplicate_door_menu(DoorCatalog&, DoorMenuId source, DoorMenuId destination, std::string name);

// Versioned, length-bounded, platform-independent storage/IPC records. These
// contain only protected secret blobs; no cleartext password serialization API.
std::vector<std::uint8_t> encode_door_definition(const DoorDefinition&);
std::optional<DoorDefinition> decode_door_definition(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_door_catalog(const DoorCatalog&);
std::optional<DoorCatalog> decode_door_catalog(const std::vector<std::uint8_t>&);

} // namespace sfx::application
