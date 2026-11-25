#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
namespace sfx::platform::win32 {
enum class BbsHealthState {ok,needs_attention,could_not_check};
struct BbsHealthFinding {BbsHealthState state;std::wstring subject,detail,next_step;};
struct BbsHealthReport {
    bool stopped{};std::vector<BbsHealthFinding> findings;std::optional<std::filesystem::path> saved_report;
    std::wstring text()const;
};
// Manual stopped inspection. Opens immutable/read-only SQLite; never starts
// storage, migrates, repairs access, clears holds or creates live board files.
// The only filesystem output is a new exclusive-create dated Logs text report.
BbsHealthReport run_bbs_health_check(const std::filesystem::path&,const std::wstring& current_owner,
    const std::function<bool()>& cancelled={});
}
