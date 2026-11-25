#pragma once
#include "sfx/archive/archive_engine.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <functional>
#include <optional>

namespace sfx::apps::setup {
namespace install = platform::win32::installation;
enum class Operation { fresh, rebuild, maintenance, uninstall, uninstall_full, uninstall_missing };
inline bool removing(Operation op) noexcept {
    return op == Operation::uninstall || op == Operation::uninstall_full || op == Operation::uninstall_missing;
}
struct Plan {
    install::Path root;
    install::Path backup;
    install::Path staging;
    archive::Timestamp backup_timestamp;
    std::wstring transaction;
    Operation operation{Operation::fresh};
    std::optional<install::Receipt> previous;
    std::uint64_t reviewed_root_id{0};
    DWORD reviewed_volume{0};
};
struct Options { bool start_menu{true}; bool desktop{true}; install::Path clamav_package; bool automatic_clamav{false};
    std::optional<bool> service_mode; // obsolete qualification input; nonempty is refused
    bool install_uninstall_entry{false}; // Only the shipping Setup frontend registers itself.
};
using Progress = std::function<void(const std::wstring&)>;
using Pump = std::function<bool()>; // false requests cancellation; never caller input.

enum class PlanningPhase { total, embedded_payload, location, receipt,
    installed_files, installed_boundaries, installed_program, source_recognition, details,
    installed_open_security, installed_native_identity, installed_read, installed_sha };
struct PlanningTiming {
    PlanningPhase phase;
    std::size_t role{0};
    std::uint64_t microseconds{0};
    bool completed{false};
};
struct PlanningCallbacks {
    Pump keep_going;
    std::function<void(PlanningPhase)> status;
    std::function<void(const PlanningTiming&)> timing;
};
class PlanningCancelled final : public std::runtime_error {
public: PlanningCancelled():std::runtime_error("setup_planning_cancelled"){}
};
class PlanningObservationFailure final : public std::runtime_error {
public: PlanningObservationFailure():std::runtime_error("setup_planning_observer_failed"){}
};

void verify_payload();
Plan inspect_plan(const install::Path& root, bool uninstall);
Plan inspect_plan(const install::Path& root, bool uninstall, const PlanningCallbacks& callbacks);
Plan inspect_registered_uninstall(const std::wstring& id, const PlanningCallbacks&);
// Ordinary coordinator; returns false only for actual UAC cancellation.
bool execute(const Plan& plan, const Options& options, HWND window, Progress progress, Pump pump);
int worker(const std::wstring& pipe, DWORD parent_process) noexcept;
std::wstring summary(const Plan& plan);
// Format only after the existing verified commit/finalization succeeds.
std::wstring completion_text(const Plan& plan,const Options& options);
} // namespace sfx::apps::setup
