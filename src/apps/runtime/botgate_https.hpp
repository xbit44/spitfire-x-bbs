#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace sfx::apps::runtime::https {
inline constexpr char feed_url[]="https://raw.githubusercontent.com/borestad/blocklist-abuseipdb/refs/heads/main/abuseipdb-s100-30d.ipv4";
inline constexpr char bundle_hash[]="427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285";
inline constexpr char bundle_date[]="2026-11-05";
struct Result {
    std::optional<std::string> bytes;
    std::string error;
    long certificate_result{};
    long http_status{};
    int transport_result{};
    std::string certificate_summary;
};
// Runtime passes only feed_url. URL/CA overrides are not configurable or IPC
// inputs; the diagnostic harness uses them for local certificate fixtures.
Result get(const char* url,const std::atomic<bool>& stop,
    const std::function<void(std::uint64_t)>& progress,
    const std::string* fixture_ca=nullptr);
std::string implementation();
}
