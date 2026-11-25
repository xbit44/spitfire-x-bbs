#pragma once
#include "sfx/application/door_execution.hpp"
#include "sfx/application/door_drop.hpp"
#include <filesystem>

namespace sfx::apps::runtime {
struct LocalDoorCaller {
    std::uint32_t account{};
    std::uint16_t node{},security_level{};
    std::string alias,board_name;
    std::optional<std::uint64_t> remaining_seconds;
    bool ansi{};
    application::DoorDropContext drop_context;
    std::uint64_t generation{},sequence{};
    std::int64_t captured_utc_seconds{};
};
void ensure_door_node_directories(const std::filesystem::path&,const std::wstring& owner,unsigned count);
application::DoorProgramResult run_alpha_local_door(const std::filesystem::path& root,
    const std::wstring& owner,const application::DoorDefinition&,const LocalDoorCaller&,
    const application::DoorChannel&,std::vector<std::uint8_t> prefix={},bool discard_lf=false);
} // namespace sfx::apps::runtime
