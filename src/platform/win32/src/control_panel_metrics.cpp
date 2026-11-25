#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>

#include "sfx/platform/win32/control_panel_metrics.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace sfx::platform::win32 {
namespace {

std::uint64_t ticks(const LARGE_INTEGER value) noexcept {
    return static_cast<std::uint64_t>(value.QuadPart);
}

struct ProcessorPerformanceInformation {
    LARGE_INTEGER idle_time;
    LARGE_INTEGER kernel_time;
    LARGE_INTEGER user_time;
    LARGE_INTEGER dpc_time;
    LARGE_INTEGER interrupt_time;
    ULONG interrupt_count;
};

using NtQuerySystemInformationFunction = LONG (WINAPI*)(
    ULONG, void*, ULONG, ULONG*);

double percent(const std::uint64_t busy_delta, const std::uint64_t total_delta) noexcept {
    return total_delta == 0 ? 0.0 : std::min(100.0,
        100.0 * static_cast<double>(busy_delta) / static_cast<double>(total_delta));
}

}  // namespace

class WindowsControlPanelMetricSampler::Impl final {
public:
    explicit Impl(std::filesystem::path root) : installation_root_(std::move(root)) {
        started_ = std::chrono::steady_clock::now();
        previous_time_ = started_;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll != nullptr) query_processors_ = reinterpret_cast<NtQuerySystemInformationFunction>(
            GetProcAddress(ntdll, "NtQuerySystemInformation"));
        try {
            application::ControlPanelMetricSnapshot baseline;
            sample_cpu(baseline);
            sample_network(baseline, started_);
        } catch (...) {
        }
        if (PdhOpenQueryW(nullptr, 0, &disk_query_) == ERROR_SUCCESS &&
            PdhAddEnglishCounterW(disk_query_, L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec",
                0, &disk_read_) == ERROR_SUCCESS &&
            PdhAddEnglishCounterW(disk_query_, L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec",
                0, &disk_write_) == ERROR_SUCCESS &&
            PdhCollectQueryData(disk_query_) == ERROR_SUCCESS) {
            disk_query_ready_ = true;
            previous_disk_time_ = std::chrono::steady_clock::now();
        } else if (disk_query_ != nullptr) {
            PdhCloseQuery(disk_query_);
            disk_query_ = nullptr;
        }
    }

    ~Impl() {
        if (disk_query_ != nullptr) PdhCloseQuery(disk_query_);
    }

    void sample_cpu(application::ControlPanelMetricSnapshot& result) noexcept {
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            ULARGE_INTEGER i{}, k{}, u{};
            i.LowPart = idle.dwLowDateTime; i.HighPart = idle.dwHighDateTime;
            k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
            u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
            const std::uint64_t total = k.QuadPart + u.QuadPart;
            const std::uint64_t idle_value = i.QuadPart;
            if (have_total_cpu_ && total >= previous_total_ && idle_value >= previous_idle_) {
                const std::uint64_t total_delta = total - previous_total_;
                const std::uint64_t idle_delta = idle_value - previous_idle_;
                result.total_cpu_percent = percent(total_delta > idle_delta ? total_delta - idle_delta : 0,
                    total_delta);
                result.cpu_available = total_delta != 0;
            }
            previous_total_ = total;
            previous_idle_ = idle_value;
            have_total_cpu_ = true;
        }
        if (query_processors_ == nullptr) return;
        const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (count == 0 || count > 256) return;
        std::vector<ProcessorPerformanceInformation> current(count);
        ULONG returned = 0;
        if (query_processors_(8, current.data(), static_cast<ULONG>(current.size() *
                sizeof(current.front())), &returned) < 0 ||
            returned < current.size() * sizeof(current.front())) return;
        if (previous_cores_.size() == current.size()) {
            result.per_core_cpu_percent.reserve(current.size());
            for (std::size_t index = 0; index < current.size(); ++index) {
                const auto& now = current[index];
                const auto& before = previous_cores_[index];
                const std::uint64_t total = ticks(now.kernel_time) + ticks(now.user_time);
                const std::uint64_t old_total = ticks(before.kernel_time) + ticks(before.user_time);
                const std::uint64_t core_idle = ticks(now.idle_time);
                const std::uint64_t old_idle = ticks(before.idle_time);
                const std::uint64_t total_delta = total >= old_total ? total - old_total : 0;
                const std::uint64_t idle_delta = core_idle >= old_idle ? core_idle - old_idle : 0;
                result.per_core_cpu_percent.push_back(percent(
                    total_delta > idle_delta ? total_delta - idle_delta : 0, total_delta));
            }
        }
        previous_cores_ = std::move(current);
    }

    void sample_memory(application::ControlPanelMetricSnapshot& result) noexcept {
        MEMORYSTATUSEX memory{sizeof(memory)};
        if (!GlobalMemoryStatusEx(&memory)) return;
        result.memory_available = true;
        result.memory_total_bytes = memory.ullTotalPhys;
        result.memory_available_bytes = memory.ullAvailPhys;
        result.memory_used_bytes = memory.ullTotalPhys - memory.ullAvailPhys;
    }

    void sample_storage(application::ControlPanelMetricSnapshot& result) noexcept {
        if (!disk_query_ready_ || PdhCollectQueryData(disk_query_) != ERROR_SUCCESS) return;
        const auto now = std::chrono::steady_clock::now();
        const auto interval = now - previous_disk_time_;
        previous_disk_time_ = now;
        // Hidden monitoring does no polling. Re-prime after a gap instead of
        // presenting the whole hidden interval as a fresh one-second sample.
        if (interval > std::chrono::seconds(3)) return;
        PDH_FMT_COUNTERVALUE reads{}, writes{};
        const auto usable = [](const PDH_FMT_COUNTERVALUE& value) {
            return value.CStatus == PDH_CSTATUS_VALID_DATA ||
                value.CStatus == PDH_CSTATUS_NEW_DATA;
        };
        if (PdhGetFormattedCounterValue(disk_read_, PDH_FMT_DOUBLE, nullptr, &reads) !=
                ERROR_SUCCESS ||
            PdhGetFormattedCounterValue(disk_write_, PDH_FMT_DOUBLE, nullptr, &writes) !=
                ERROR_SUCCESS || !usable(reads) || !usable(writes) ||
            !std::isfinite(reads.doubleValue) || !std::isfinite(writes.doubleValue)) return;
        result.storage_activity_available = true;
        result.storage_read_bytes_per_second = std::max(0.0, reads.doubleValue);
        result.storage_write_bytes_per_second = std::max(0.0, writes.doubleValue);
    }

    void sample_volumes(application::ControlPanelMetricSnapshot& result) {
        struct Volume { std::wstring identity; std::uint64_t free{}; bool available{}; };
        const auto read = [](const std::filesystem::path& path) {
            Volume result;
            wchar_t mount[MAX_PATH]{}, identity[MAX_PATH]{};
            ULARGE_INTEGER available{}, total{}, free{};
            if (GetVolumePathNameW(path.c_str(), mount, MAX_PATH) &&
                GetVolumeNameForVolumeMountPointW(mount, identity, MAX_PATH) &&
                GetDiskFreeSpaceExW(mount, &available, &total, &free)) {
                result.identity = identity; result.free = available.QuadPart;
                result.available = true;
            }
            return result;
        };
        // The verified single-tree installation fixes these two locations.
        // No DB connection, samples table, log entry or remote request is used.
        const auto database = read(installation_root_ / L"Data");
        const auto backup = read(installation_root_ / L"Backup");
        result.database_volume_available = database.available;
        result.backup_volume_available = backup.available;
        result.database_volume_free_bytes = database.free;
        result.backup_volume_free_bytes = backup.free;
        result.shared_storage_volume = database.available && backup.available &&
            _wcsicmp(database.identity.c_str(), backup.identity.c_str()) == 0;
    }

    void sample_network(application::ControlPanelMetricSnapshot& result,
        const std::chrono::steady_clock::time_point now) noexcept {
        ULONG bytes = 15000;
        std::vector<std::uint8_t> buffer(bytes);
        auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        ULONG status = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addresses, &bytes);
        if (status == ERROR_BUFFER_OVERFLOW && bytes <= 1024U * 1024U) {
            buffer.resize(bytes);
            addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            status = GetAdaptersAddresses(AF_UNSPEC,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr, addresses, &bytes);
        }
        if (status != NO_ERROR) return;
        std::uint64_t received = 0, sent = 0;
        for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
            MIB_IF_ROW2 row{};
            row.InterfaceLuid = adapter->Luid;
            if (GetIfEntry2(&row) != NO_ERROR || row.OperStatus != IfOperStatusUp ||
                row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            received += row.InOctets;
            sent += row.OutOctets;
            if (result.local_ip.empty()) {
                for (auto* address = adapter->FirstUnicastAddress; address != nullptr;
                        address = address->Next) {
                    if (address->Address.lpSockaddr != nullptr &&
                        address->Address.lpSockaddr->sa_family == AF_INET) {
                        std::array<char, INET_ADDRSTRLEN> text{};
                        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(
                            address->Address.lpSockaddr);
                        if (InetNtopA(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr),
                                text.data(), static_cast<DWORD>(text.size())) != nullptr) {
                            result.local_ip = text.data();
                            break;
                        }
                    }
                }
            }
        }
        if (received == 0 && sent == 0 && result.local_ip.empty()) return;
        if (!have_network_) {
            network_base_received_ = received;
            network_base_sent_ = sent;
        } else {
            const double seconds = std::chrono::duration<double>(now - previous_time_).count();
            if (seconds > 0 && received >= previous_received_ && sent >= previous_sent_) {
                result.network_receive_bytes_per_second =
                    static_cast<double>(received - previous_received_) / seconds;
                result.network_send_bytes_per_second =
                    static_cast<double>(sent - previous_sent_) / seconds;
            }
        }
        result.network_available = true;
        result.network_received_bytes = received >= network_base_received_
            ? received - network_base_received_ : 0;
        result.network_sent_bytes = sent >= network_base_sent_
            ? sent - network_base_sent_ : 0;
        previous_received_ = received;
        previous_sent_ = sent;
        have_network_ = true;
    }

    application::ControlPanelMetricSnapshot sample() noexcept {
        application::ControlPanelMetricSnapshot result;
        try {
            const auto now = std::chrono::steady_clock::now();
            sample_cpu(result);
            sample_memory(result);
            sample_storage(result);
            sample_volumes(result);
            sample_network(result, now);
            previous_time_ = now;
        } catch (...) {
            return {};
        }
        return result;
    }

    NtQuerySystemInformationFunction query_processors_{nullptr};
    std::filesystem::path installation_root_;
    bool have_total_cpu_{false};
    std::uint64_t previous_total_{0};
    std::uint64_t previous_idle_{0};
    std::vector<ProcessorPerformanceInformation> previous_cores_;
    bool have_network_{false};
    std::uint64_t previous_received_{0};
    std::uint64_t previous_sent_{0};
    std::uint64_t network_base_received_{0};
    std::uint64_t network_base_sent_{0};
    std::chrono::steady_clock::time_point started_{};
    std::chrono::steady_clock::time_point previous_time_{};
    PDH_HQUERY disk_query_{nullptr};
    PDH_HCOUNTER disk_read_{nullptr};
    PDH_HCOUNTER disk_write_{nullptr};
    bool disk_query_ready_{false};
    std::chrono::steady_clock::time_point previous_disk_time_{};
};

WindowsControlPanelMetricSampler::WindowsControlPanelMetricSampler(
    std::filesystem::path installation_root)
    : impl_(std::make_unique<Impl>(std::move(installation_root))) {}

WindowsControlPanelMetricSampler::~WindowsControlPanelMetricSampler() = default;

application::ControlPanelMetricSnapshot WindowsControlPanelMetricSampler::sample() noexcept {
    return impl_->sample();
}

}  // namespace sfx::platform::win32
