#pragma once
#include "sfx/application/account_storage.hpp"
#include "sfx/application/menu.hpp"
#include "sfx/application/terminal.hpp"
#include <functional>

namespace sfx::application {
struct CallerProfileRules {
    bool aliases_allowed{true}, location_required{false}, email_required{false};
    bool sex_required{false};
};
bool valid_caller_profile_change(const AccountProfile& previous,const AccountProfile& candidate,
    const CallerProfileRules& rules);
using CallerLocalFormatter=std::function<std::optional<std::string>(std::int64_t)>;
// Uses real UTC boundaries, including 23/25-hour days, in the configured zone.
std::optional<std::pair<std::int64_t,std::int64_t>> caller_local_day_range(
    const CallerLocalFormatter& format,std::int64_t now,bool yesterday);
struct CallerUtilityPorts {
    ICallerTerminal& terminal;
    AccountId account_id;
    CallerProfileRules rules;
    unsigned page_rows{20};
    std::function<std::optional<AccountRecord>()> account;
    std::function<bool(const AccountRecord&,const AccountProfile&)> save_profile;
    std::function<StorageResult<std::vector<SessionRecord>>(const HistoryQuery&)> history;
    std::function<StorageResult<std::vector<CallerDirectoryEntry>>(const CallerDirectoryQuery&)> directory;
    CallerLocalFormatter local_time;
    std::function<std::int64_t()> utc_now;
    std::function<std::optional<bool>()> ghost;
    std::function<bool()> toggle_ghost;
    CallerUtilityPorts(ICallerTerminal& target,AccountId identity):terminal(target),account_id(identity) {}
};
// All identity and write authority are host-supplied, never selected by caller input.
std::optional<std::string> run_caller_utility(MenuAction action,const CallerUtilityPorts& ports);
} // namespace sfx::application
