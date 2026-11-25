#pragma once
#include "sfx/archive/archive_engine.hpp"
#include <string_view>

namespace sfx::archive {
inline constexpr std::uint64_t qwk_zip_limit = 16777216;
inline constexpr std::uint64_t qwk_expanded_limit = 67108864;
struct QwkArchiveFile { std::string name, bytes; };
struct QwkArchiveResult {
    Result result;
    std::vector<QwkArchiveFile> files;
    std::string zip;
};
// Memory-only packet envelope. Never extracts a caller path, executes an entry,
// invokes a shell or uses backup metadata/ownership to adopt packet contents.
// A Runtime adapter pins the protected staging-file identity before this call.
QwkArchiveResult read_qwk_reply_zip(std::string_view zip, std::string_view board_id,
    CancelCheck cancelled = {});
QwkArchiveResult write_qwk_zip(const std::vector<QwkArchiveFile>& files,
    CancelCheck cancelled = {});
} // namespace sfx::archive
