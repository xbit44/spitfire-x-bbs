#pragma once
#include "sfx/application/door_storage.hpp"
#include "sfx/application/menu_session.hpp"
#include "sfx/application/door_execution.hpp"
#include "sfx/application/door_bulletin_session.hpp"

namespace sfx::application {
struct DoorSessionPorts {
    IDoorStorage& storage;
    const MenuSessionPorts& menu;
    std::function<std::optional<MessageActor>()> actor;
    std::function<std::optional<DoorEnd>(const DoorLaunch&)> launch;
    std::function<void(DoorId,DoorAutomaticEvent,DoorEnd)> event_outcome{};
    DoorBulletinRefresh bulletin_refresh{};
};
std::optional<DoorMenuExit> run_door_session(const DoorSessionPorts&);
bool run_door_events(const DoorSessionPorts&,DoorAutomaticEvent);
} // namespace sfx::application
