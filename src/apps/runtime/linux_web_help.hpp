#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::apps::runtime::web {
struct HelpAsset { std::vector<std::uint8_t> bytes; std::string content_type; };
// Installed documentation only. No caller-supplied absolute paths or links.
std::optional<HelpAsset> read_help_asset(const std::filesystem::path& root,
    std::string_view relative);
}
