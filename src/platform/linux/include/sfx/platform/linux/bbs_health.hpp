#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace sfx::platform::linux_host {
enum class BbsHealthState {ok,needs_attention,could_not_check};
struct BbsHealthFinding {BbsHealthState state;std::string subject,detail,next_step;};
struct BbsHealthReport {
    bool stopped{};std::vector<BbsHealthFinding> findings;std::optional<std::filesystem::path> saved_report;
    std::string text()const;
};
// Exclusive stopped inspection. Never starts SQLite storage, repairs, migrates,
// prepares file identities, clears pending recovery or runs configured programs.
// Only a new private dated Logs report and installation exclusion lock are used.
BbsHealthReport run_bbs_health_check(const std::filesystem::path&,
    const std::function<bool()>& cancelled={});
}
