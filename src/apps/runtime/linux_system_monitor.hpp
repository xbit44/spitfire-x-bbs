#pragma once
#include "sfx/application/control_panel.hpp"
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace sfx::apps::runtime {
struct LinuxMonitorCounters {
    std::map<std::string,std::array<std::uint64_t,8>> cpu;
    std::optional<std::array<std::uint64_t,2>> memory;
    std::map<std::string,std::array<std::uint64_t,2>> disks,network;
    bool disks_available{},network_available{};
};
struct LinuxMonitorSnapshot {
    bool cpu_present{},disks_present{},network_present{};
    application::ControlPanelMetricSnapshot host;
    std::vector<std::string> cores,disks,interfaces;
};
LinuxMonitorCounters parse_linux_monitor_counters(std::string_view stat,std::string_view memory,
    std::string_view disks,std::string_view network,const std::vector<std::string>& physical_disks);
LinuxMonitorCounters read_linux_monitor_counters();
std::optional<std::uint64_t> linux_boot_milliseconds() noexcept;
class LinuxMonitorRates final {
public:
    LinuxMonitorSnapshot sample(const LinuxMonitorCounters&,std::uint64_t now);
private:
    std::optional<LinuxMonitorCounters> previous_;
    std::uint64_t previous_ms_{};
};
}
