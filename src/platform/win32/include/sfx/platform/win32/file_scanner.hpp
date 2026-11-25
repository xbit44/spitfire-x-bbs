#pragma once
#include "sfx/platform/win32/clamav_host.hpp"
#include "sfx/application/file_scan.hpp"
#include "sfx/platform/win32/file_payload_store.hpp"
#include <functional>

namespace sfx::platform::win32 {
struct ClamAvPinnedMember { const wchar_t* name; std::uint64_t bytes; const char* sha256; };
// Exact executable/DLL subset of the independently signature-verified official
// Current Win32 portable ZIP. Setup separately preserves all applicable licenses.
const std::vector<ClamAvPinnedMember>& clamav_program_members();
// Old members are used only to identify stopped components for maintenance.
const std::vector<ClamAvPinnedMember>& legacy_clamav_program_members();
struct NativeFileScanResult {
    application::FileScanResult result;
    std::uint32_t exit_code{0xffffffffU};
    std::uint64_t elapsed_milliseconds{};
    bool process_reaped{true}; // true also when no child was started
    std::string diagnostic; // fixed native phase, never raw scanner text/path
};
// Native host-only capability. The installed host must first verify Setup's
// optional-engine receipt/protected role; this layer independently pins all
// engine images and ancestors for the lifetime of the child. No shell/PATH
// program lookup, arbitrary arguments, recursive scan or scanner-side deletion.
class WindowsFileScanner final {
public:
    WindowsFileScanner(std::filesystem::path engine_directory, std::wstring owner);
    NativeFileScanResult scan(FilePayloadLease& input,
        const FilePayloadDirectory& definitions, const FilePayloadDirectory& empty_scratch,
        std::uint64_t maximum_file_bytes, const std::function<bool()>& cancelled = {},
        const std::function<void(std::uint64_t)>& pending_activity = {},
        std::uint32_t deadline_milliseconds = 180000) const;
private:
    std::filesystem::path engine_;
    std::wstring owner_;
};
} // namespace sfx::platform::win32
