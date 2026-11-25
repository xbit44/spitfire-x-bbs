#pragma once
#include "sfx/application/menu_session.hpp"
#include "sfx/application/classic_information_storage.hpp"

namespace sfx::application {
struct BulletinEntry {
    unsigned number{};
    std::optional<std::int64_t> modified_utc_100ns;
};
bool information_updated(const std::optional<std::int64_t>& modified_utc_100ns,
    const std::optional<std::int64_t>& previous_login_utc_seconds) noexcept;
std::vector<BulletinEntry> available_bulletins(const MenuSessionPorts& ports,
    const MenuSessionSnapshot& state);
enum class BulletinOutcome { continued, goodbye, failed };
// Numbered regular pages only; artwork never contributes routing authority.
BulletinOutcome run_bulletin_session(const MenuSessionPorts& ports,
    std::optional<std::int64_t> previous_login, bool skip_if_absent=false);
// No caller-account write or unread checkpoint: compare to PREVIOUS login.
bool offer_login_newsletter(const MenuSessionPorts& ports,
    std::optional<std::int64_t> previous_login);
bool show_login_door_bulletin_updates(const MenuSessionPorts&);
std::optional<std::string> run_other_board_session(const MenuSessionPorts&,ICallerTerminal&,
    const std::function<StorageResult<OtherBoardPage>(std::int64_t)>&,
    const std::function<bool(const OtherBoardValues&)>&,
    const std::function<bool(const OtherBoardRecord&)>& remove = {});
bool show_system_information(const MenuSessionPorts&,const SystemInformationCounters&,
    std::string opening_date);
} // namespace sfx::application
