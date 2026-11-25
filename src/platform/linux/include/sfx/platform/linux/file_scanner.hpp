#pragma once
#include "sfx/platform/linux/file_payload_store.hpp"
#include "sfx/application/file_scan.hpp"

namespace sfx::platform::linux_host {
struct NativeFileScanResult {
    application::FileScanResult result;
    std::uint32_t exit_code{0xffffffffU};
    std::uint64_t elapsed_milliseconds{};
    bool process_reaped{true};
    std::string diagnostic;
};
// Trusted host only. Paths are derived from the selected portable installation;
// no caller/browser path, shell command, global installation or daemon is used.
// The original lease remains owned by intake throughout the supervised scan.
class LinuxFileScanner final {
public:
    explicit LinuxFileScanner(std::filesystem::path installation);
    NativeFileScanResult scan(FilePayloadLease&,std::uint64_t maximum_file_bytes,
        const std::function<bool()>& cancelled = {},
        const std::function<void(std::uint64_t)>& pending_activity = {},
        std::uint32_t deadline_milliseconds = 180000) const;
private:
    std::filesystem::path root_;
};
}
