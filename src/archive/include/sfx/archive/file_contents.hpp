#pragma once
#include "sfx/archive/archive_engine.hpp"
#include <optional>

namespace sfx::archive {
inline constexpr const char* file_contents_index = "FileContents/INDEX.txt";
struct ContentArea {
    std::int64_t id{}, revision{};
    std::string name, configured_path;
};
struct ContentFile {
    std::int64_t area{};
    std::string name;
    std::uint64_t bytes{};
    std::string sha256;
    // Empty archive/digest means this restore point. Otherwise an exact sibling
    // managed archive and its full SHA-256, never a path supplied for extraction.
    std::string archive, archive_sha256, member;
};
struct ContentIndex {
    std::vector<ContentArea> areas;
    std::vector<ContentFile> files;
};
std::optional<ContentIndex> decode_content_index(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_content_index(const ContentIndex&);
// Legacy core-only archives have no index. Invalid present indexes must not be
// treated as legacy or silently ignored by retention/restore.
bool has_content_index(const BackupInspection&);
ContentIndex inspect_content_index(const std::filesystem::path&, const BackupInspection&);
} // namespace sfx::archive
