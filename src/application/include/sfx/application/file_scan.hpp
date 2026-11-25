#pragma once
#include "sfx/application/file.hpp"

namespace sfx::application {
inline constexpr std::size_t kFileScanOutputBytes = 65536;
inline constexpr std::size_t kFileScanDetectionMaximum = 32;
inline constexpr std::string_view kClamAvEngineVersion = "1.4.7";

// Evidence from one supervised invocation of the pinned external scanner. The
// native adapter owns paths, process identity, deadlines and complete capture;
// no caller-supplied output or exit code may cross this trusted boundary.
struct FileScanEvidence {
    bool started{}, exited{}, timed_out{}, cancelled{}, output_complete{};
    std::uint32_t exit_code{};
    std::string_view expected_output_path, standard_output, standard_error;
};
// Strict one-file output grammar. Exit zero alone is insufficient. Unsupported
// output, stale definitions, limits, encrypted content and partial scans HOLD.
// This parser grants no filesystem deletion or catalog-commit authority.
FileScanResult classify_clamav_scan(const FileScanEvidence&);

// Optional pending-work activity only; never used during binary transfer and
// never presented as bytes or percentage completion.
std::string file_scan_activity(std::uint64_t elapsed_milliseconds);
} // namespace sfx::application
