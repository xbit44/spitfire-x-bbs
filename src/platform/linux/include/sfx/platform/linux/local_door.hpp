#pragma once
#include "sfx/application/door_execution.hpp"
#include "sfx/application/door_drop.hpp"
#include <filesystem>

namespace sfx::platform::linux_host {
struct DoorOptions {
    // Standard I/O on Linux may use a raw PTY for terminal-aware native games.
    bool pseudo_terminal{true};
    // Optional extracted runtime prefix (contains usr/libexec, usr/lib and
    // usr/share). Otherwise use <board>/Bin/Dosemu or the system installation.
    std::filesystem::path dosemu_root;
    std::filesystem::path dosemu_binary;
    // Optional explicit identity pin. The executable is always checked and
    // held open through exec; ordinary setup does not require typing a hash.
    std::string dosemu_sha256;
    // Optional spacing for legacy DOS serial input readers (0..100 ms).
    // Zero preserves normal throughput. Native/standard I/O is unaffected.
    // A saved per-game DosInputIntervalMs in sfxexec.ini takes precedence.
    unsigned dos_serial_input_interval_ms{};
};
inline constexpr const char* dosemu_source="7e7f3da9596ce8035274843f61dc6c71e9733d3a";
inline constexpr const char* dosemu_version="2.0pre9-10399";
void ensure_door_nodes(const std::filesystem::path& root,unsigned count);
application::DoorProgramResult run_local_door(const std::filesystem::path& root,
    const application::DoorDefinition&,application::DoorDropContext,
    const application::DoorChannel&,const DoorOptions& = {},
    std::vector<std::uint8_t> prefix={},bool discard_lf=false);
} // namespace sfx::platform::linux_host
