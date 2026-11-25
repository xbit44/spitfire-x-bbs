#pragma once
#include "sfx/archive/archive_engine.hpp"
#include "sfx/application/file.hpp"

namespace sfx::archive {
struct FileZipSource {
    std::uint64_t bytes{};
    // A trusted host holds the immutable native file. No path extraction or
    // external archive process; every requested read is at most64KiB.
    std::function<std::vector<std::uint8_t>(std::uint64_t,std::size_t)> read;
    CancelCheck cancelled;
};
struct FileZipEntry {std::string name;std::uint64_t bytes{};bool directory{},encrypted{},readable{};};
struct FileZipInspection {Result result;std::vector<FileZipEntry> entries;bool encrypted{};};
struct FileZipText {Result result;std::string bytes;};
struct FileDizResult {Result result;std::optional<application::FileDescriptions> descriptions;bool encrypted{};};
FileZipInspection inspect_file_zip(const FileZipSource&);
// Index is from an inspection of the same held source, not a native pathname.
// Revalidates the selected entry; returned bytes are still UNTRUSTED content.
FileZipText read_file_zip_entry(const FileZipSource&,std::size_t index,std::size_t maximum=262144);
FileDizResult read_file_diz(const FileZipSource&);
} // namespace sfx::archive
