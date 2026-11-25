#pragma once

#include <array>
#include <string_view>

namespace sfx::config_ui {
// Storage slots are private UI implementation details. Persist and link by ID.
struct Route { std::wstring_view id, category, title; int page; };
inline constexpr std::array<Route, 31> routes{{
    {L"general.board_identity", L"General", L"Board Identity", 0},
    {L"general.regional_time", L"General", L"Regional & Time", 13},
    {L"general.displays_menus", L"General", L"Displays & Menus", 17},
    {L"general.system_information", L"General", L"System Information", 18},
    {L"messages.local_echoes", L"Messages", L"Groups & Local Echoes", 19},
    {L"messages.clone_permissions", L"Messages", L"Clone Options", 20},
    {L"files.groups_areas", L"Files", L"Groups & Areas", 21},
    {L"files.transfer_policy", L"Files", L"Transfer Policy", 22},
    {L"files.count_rules", L"Files", L"Security-level Count Rules", 23},
    {L"files.caller_exemptions", L"Files", L"Caller Exemptions", 24},
    {L"doors.manager", L"Doors", L"Doors", 26},
    {L"chat.rooms", L"Chat", L"Chat Rooms", 27},
    {L"ftn.networks", L"FTN Networking", L"Networks", 29},
    {L"system.runtime_behavior", L"System", L"Runtime Behavior", 14},
    {L"system.nodes", L"System", L"Nodes", 1},
    {L"system.system_password", L"System", L"System Password", 7},
    {L"users.accounts", L"Users & Access", L"Accounts", 6},
    {L"users.security_levels", L"Users & Access", L"Security Levels", 8},
    {L"users.new_user_registration", L"Users & Access", L"New User Registration", 15},
    {L"users.caller_password_policy", L"Users & Access", L"Caller Password Policy", 16},
    {L"connections.telnet_ssh", L"Connections", L"Telnet & SSH", 2},
    {L"connections.caller_idle_limits", L"Connections", L"Caller Idle Limits", 41},
    {L"connections.external_client", L"Connections", L"External Client", 3},
    {L"security.botgate", L"Security", L"BBS Security", 11},
    {L"security.connection_limits", L"Security", L"Connections", 9},
    {L"security.throttle_recovery", L"Security", L"Failed Login Attempts", 10},
    {L"security.clamav", L"Security", L"ClamAV", 25},
    {L"tools.backup_restore", L"Tools & Maintenance", L"Backup & Restore", 4},
    {L"tools.scheduled_jobs", L"Tools & Maintenance", L"Scheduled Jobs", 5},
    {L"tools.logging", L"Tools & Maintenance", L"Logging", 12},
    {L"tools.bbs_health", L"Tools & Maintenance", L"BBS Health Check", 40},
}};
inline constexpr Route ftn_network_route{L"ftn.network",L"FTN Networking",L"Network Settings",30};
inline constexpr Route ftn_connections_route{L"ftn.connections",L"FTN Networking",L"Connections",31};
inline constexpr std::array<Route,7> ftn_editor_routes{{
    {L"ftn.echoes",L"FTN Networking",L"Message Echoes",32},
    {L"ftn.files",L"FTN Networking",L"File Echoes",33},
    {L"ftn.routing",L"FTN Networking",L"Routing & Polling",34},
    {L"ftn.import",L"FTN Networking",L"Import Echo List",35},
    {L"ftn.held",L"FTN Networking",L"Held Mail",36},
    {L"ftn.review",L"FTN Networking",L"Review Network Setup",37},
    {L"ftn.permissions",L"FTN Networking",L"Connection Echo Permissions",38}
}};
inline constexpr const Route& route(std::wstring_view id) {
    if(id==L"ftn.msg_netmail") id=L"ftn.networks"; // Retired route opens the network list.
    if(id==L"ftn.network")return ftn_network_route;
    if(id==L"ftn.connections")return ftn_connections_route;
    for(const auto& r:ftn_editor_routes)if(r.id==id)return r;
    if(id==L"security.bbs_security") id=L"security.botgate";
    for (const auto& value : routes) if (value.id == id) return value;
    return routes.front();
}
inline constexpr bool security_page(int page) { return (page>=9 && page<=11)||page==25; }
inline constexpr std::array<int,4> security_tab_pages{{11,9,10,25}};
inline constexpr Route commands_route{L"general.displays_menus",L"General",L"Displays & Menus > Door Commands",28};
inline constexpr const Route& route(int page) {
    if(page==30)return ftn_network_route;
    if(page==31)return ftn_connections_route;
    for(const auto& r:ftn_editor_routes)if(r.page==page)return r;
    if(page==28)return commands_route;
    for (const auto& value : routes) if (value.page == page) return value;
    return routes.front();
}
} // namespace sfx::config_ui
