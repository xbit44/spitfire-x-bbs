#pragma once
#include "sfx/application/door.hpp"
#include <map>

namespace sfx::application {
enum class DoorDropPathStyle { windows, posix };
// A captured, nonsecret projection. No credential or account-update fields.
struct DoorDropContext {
    std::uint32_t account{};
    std::uint16_t node{}, nodes{1}, security{}, columns{80}, rows{25};
    std::string alias, user_key, board, sysop{"Sysop"}, board_code{"SFX"};
    std::string software{"Spitfire-X BBS"}, encoding{"IBM437"}, language{"en-US"};
    std::string node_path, drop_path, startup_path;
    std::string date, clock, logon_date, logon_clock, deadline_utc;
    std::uint32_t seconds_today{}, logon_seconds_today{}, elapsed_seconds{};
    std::optional<std::uint64_t> remaining_seconds;
    bool ansi{}, sysop_access{}, dos{};
    DoorIo io{DoorIo::standard};
    std::uint64_t socket{};
    std::string file_time, software_version, door_code, door_name;
    bool doordrop_utf8{};
    DoorDropPathStyle path_style{DoorDropPathStyle::windows};
};
struct DoorDropImage {std::string name;std::vector<std::uint8_t> bytes;};
struct DoorDropResult {
    std::vector<DoorDropImage> files;
    // Values are exact unquoted door-visible paths, not shell fragments.
    std::map<std::string,std::string> environment;
    std::string diagnostic;
    bool valid() const noexcept {return diagnostic.empty();}
};
DoorDropResult make_door_drop_files(DoorDropFile,const DoorDropContext&);

struct DoorDriverSettings {
    double yield_interval_ms{1.0};
    bool can_disconnect{true};
    bool allow_x64_dos{}; // Separate runtime required; never installed implicitly.
    // Optional parent-side Linux DOS UART/FOSSIL pacing (whole 0..100 ms).
    // Absent preserves legacy adapter options/default zero; Windows accepts
    // copied settings but does not apply this Linux-only capability.
    std::optional<unsigned> dos_input_interval_ms;
};
// Shared file followed by a startup override. Unknown keys/sections are refused
// rather than silently appearing to configure an unsupported driver capability.
std::optional<DoorDriverSettings> parse_door_driver_settings(std::string_view shared,
    std::string_view startup,std::string* diagnostic=nullptr);
std::string encode_door_driver_settings(const DoorDriverSettings&);
} // namespace sfx::application
