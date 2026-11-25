#pragma once
#include "clamav_download.hpp"
#include <string>

namespace sfx::apps::setup::clamav_https {
inline constexpr char official_url[]="https://www.clamav.net/downloads/production/clamav-1.4.7.win.win32.zip";
// Internal native qualification seam, not a setting/IPC/CLI or trust bootstrap.
// Production calls open(pump) ONLY: fixed official URL and compiled pinned CA.
// Local TLS tests exercise this same reader with an ephemeral loopback CA.
std::unique_ptr<ClamAvDownloadReader> open(const ClamAvDownloadPump&,
    const char* fixture_url=nullptr,const std::string* fixture_ca=nullptr,
    long deadline_ms=600000);
}
