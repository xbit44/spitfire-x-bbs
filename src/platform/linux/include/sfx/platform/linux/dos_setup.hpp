#pragma once
#include "sfx/platform/linux/local_door.hpp"
#include <functional>

namespace sfx::platform::linux_host {
struct DosRuntimeProfile {std::uint64_t revision{};DoorOptions options;};
struct DosRuntimeCheck {DoorOptions options;std::string version;};
struct DosGameCheck {
    std::filesystem::path directory;
    std::vector<std::string> programs;
    std::string driver_identity,driver_text;
    bool driver_change{};
    unsigned input_interval_ms{};
};
// Board-owned configuration is revision checked and included in Config backups.
DosRuntimeProfile read_dos_runtime_profile(const std::filesystem::path& board);
void save_dos_runtime_profile(const std::filesystem::path& board,const DoorOptions&,
    std::uint64_t expected_revision);
// These bounded commands are for an authenticated Sysop's explicit setup action.
// They use the same pinned ELF/child capability and descriptor rules as Doors.
std::string run_dos_setup_command(const std::filesystem::path& executable,
    std::vector<std::string> arguments,const std::filesystem::path& working_directory,
    const std::function<bool()>& cancel,const std::function<void(std::string_view)>& output,
    unsigned timeout_seconds=5,const DoorOptions& environment={});
DosRuntimeCheck check_dos_runtime(const std::filesystem::path& board,DoorOptions,
    const std::function<bool()>& cancel={});
DosGameCheck inspect_dos_game(const std::filesystem::path& board,
    const std::filesystem::path& startup,std::optional<unsigned> input_interval_ms={});
void prepare_dos_game_driver(const std::filesystem::path& board,
    const std::filesystem::path& startup,std::string_view expected_identity,
    std::optional<unsigned> input_interval_ms={});
}
