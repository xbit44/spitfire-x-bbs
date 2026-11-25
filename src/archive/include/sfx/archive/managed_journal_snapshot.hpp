#pragma once

#include <array>
#include <string_view>

namespace sfx::archive {

// This is the existing native journal namespace, not a wildcard exclusion.
// A coordinated host snapshot owns this complete set at its capture boundary.
inline bool is_linux_journal_snapshot_member(std::string_view path) noexcept {
    constexpr std::string_view directory = "Logs/";
    if (path.substr(0, directory.size()) != directory) return false;
    path.remove_prefix(directory.size());
    constexpr std::array<std::string_view, 3> active{
        "CALLERS.LOG", "HEYSYSOP.LOG", "SFXRuntime.log"};
    for (const auto name : active) if (path == name) return true;
    constexpr std::array<std::string_view, 3> bases{
        "CALLERS", "HEYSYSOP", "SFXRuntime"};
    for (const auto base : bases) {
        if (path.size() != base.size() + 1 + 19 + 1 + 32 + 4 ||
            path.substr(0, base.size()) != base || path[base.size()] != '-') continue;
        const auto suffix = path.substr(base.size() + 1);
        if (suffix[19] != '-' || suffix.substr(52) != ".LOG") return false;
        for (std::size_t i = 0; i < 19; ++i) {
            const auto c = suffix[i];
            if (i == 8 ? c != 'T' : i == 18 ? c != 'Z' : c < '0' || c > '9') return false;
        }
        for (std::size_t i = 20; i < 52; ++i) {
            const auto c = suffix[i];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
        }
        return true;
    }
    return false;
}

} // namespace sfx::archive
