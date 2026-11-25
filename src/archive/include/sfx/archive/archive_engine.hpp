#pragma once
#include "sfx/archive/physical_layout.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace sfx::archive {

inline constexpr const char* backup_info_name = "SFX-BACKUP-INFO.txt";
inline constexpr const char* backup_manifest_name = "SFX-BACKUP-MANIFEST.txt";

struct Limits final {
    std::uint64_t maximum_entries{100000};
    std::uint64_t maximum_file_bytes{64ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_total_bytes{512ULL * 1024ULL * 1024ULL * 1024ULL};
    std::size_t maximum_path_bytes{2048};
    std::size_t maximum_path_depth{64};
};

struct Timestamp final {
    std::string filename_component;
    std::string local;
    std::string utc_offset;
    std::string utc;
};

struct Metadata final {
    std::string backup_type;
    std::string transaction_id;
    std::string sfx_version;
    std::string source_generation;
    Timestamp created;
};

struct FileRecord final {
    std::string path;
    std::uint64_t size{};
    std::string sha256;
    // Manifest v2 binds the Linux owner-execute bit to this exact member.
    // Legacy v1 and Windows-produced records default to nonexecutable.
    bool owner_executable{};
};
// Reuse the archive's strict path/size/hash/collision parser when inspecting
// a loose legacy restore set. Parsing alone is not payload verification.
[[nodiscard]] bool decode_backup_manifest(const std::string&,std::vector<FileRecord>&);

enum class Error {
    none,
    cancelled,
    invalid_request,
    unsafe_path,
    limit_exceeded,
    source_changed,
    source_io,
    archive_io,
    archive_invalid,
    metadata_invalid,
    manifest_invalid,
    destination_exists,
    publication_failed,
};

struct Result final {
    Error error{Error::none};
    std::string detail;

    [[nodiscard]] bool ok() const noexcept { return error == Error::none; }
};

using CancelCheck = std::function<bool()>;
using Progress = std::function<void(std::uint64_t completed,
                                    std::uint64_t total,
                                    const std::string& path)>;
enum class BackupStage { inventory, writing, verification, publication };
struct BackupProgressEvent {
    BackupStage stage{BackupStage::inventory};
    std::uint64_t completed_bytes{},total_bytes{},completed_files{},total_files{};
    // Zero totals mean the inventory is still unknown, never a percentage.
};
using BackupProgress=std::function<void(const BackupProgressEvent&)>;

struct CreateBackupRequest final {
    struct SourceOverride final {
        std::string archive_path;
        std::filesystem::path native_path;
    };

    std::filesystem::path source_root;
    std::filesystem::path partial_path;
    std::filesystem::path destination_path;
    std::vector<std::string> included_root_names;
    std::vector<std::string> excluded_root_names;
    std::vector<std::string> excluded_relative_paths;
    std::vector<SourceOverride> source_overrides;
#if defined(__linux__)
    // Only a host-validated complete journal snapshot set enables this mode.
    // Captured overrides replace the closed native journal namespace, including
    // canonical rotations created by live writers after the snapshot boundary.
    bool linux_journal_snapshot_set{};
#endif
    Metadata metadata;
    Limits limits{};
    CancelCheck cancelled;
    Progress progress;
    BackupProgress backup_progress;
    int compression_level{6};
    bool store_clamav_definitions{false}; // Setup-only opt-in for compressed CVDs
    PhysicalLayout physical_layout{PhysicalLayout::canonical};
};

struct CreateZipRequest final {
    std::filesystem::path source_root;
    std::filesystem::path partial_path;
    std::filesystem::path destination_path;
    std::vector<std::filesystem::path> input_paths;
    std::vector<std::string> included_patterns;
    std::vector<std::string> excluded_patterns;
    bool recursive{false};
    int compression_level{6};
    Limits limits{};
    CancelCheck cancelled;
    Progress progress;
};

struct ZipInspection final {
    Result result;
    std::vector<FileRecord> files;
    std::uint64_t directory_count{};
    std::uint64_t total_bytes{};
};

struct BackupInspection final {
    Result result;
    Metadata metadata;
    std::vector<FileRecord> files;
    std::uint64_t total_bytes{};
};

struct ZipMember final {
    Result result;
    std::vector<std::uint8_t> bytes;
};
// Bounded single regular member; exact name, duplicate rejection and CRC read.
// Callers authenticating a larger backup/package retain their native archive
// lease and verify its manifest/hash separately. Never writes to the filesystem.
[[nodiscard]] ZipMember read_zip_member(const std::filesystem::path& archive_path,
    const std::string& member, std::size_t maximum_bytes = 32U * 1024U * 1024U);
// Streams one exact verified member to a trusted private staging sink. The
// caller must not publish the sink until both this result and its hash pass.
[[nodiscard]] Result stream_zip_member(const std::filesystem::path&,const std::string&,
    std::uint64_t expected_bytes,const std::function<bool(const std::uint8_t*,std::size_t)>&,
    const CancelCheck& cancelled = {});

struct ExtractRequest final {
    std::filesystem::path archive_path;
    std::filesystem::path destination_root;
    Limits limits{};
    bool skip_backup_control_files{false};
    std::vector<std::string> included_patterns;
    std::vector<std::string> excluded_patterns;
    CancelCheck cancelled;
    Progress progress;
    PhysicalLayout physical_layout{PhysicalLayout::canonical};
};

[[nodiscard]] Timestamp current_timestamp();
[[nodiscard]] std::filesystem::path backup_filename(const Timestamp& timestamp);
[[nodiscard]] Result create_backup(const CreateBackupRequest& request);
[[nodiscard]] Result create_zip(const CreateZipRequest& request);
[[nodiscard]] ZipInspection inspect_zip(const std::filesystem::path& archive_path,
                                        const Limits& limits = {});
[[nodiscard]] BackupInspection inspect_backup(
    const std::filesystem::path& archive_path,
    const Limits& limits = {},const CancelCheck& cancelled = {},const BackupProgress& progress = {});
[[nodiscard]] Result extract_zip(const ExtractRequest& request);
[[nodiscard]] const char* error_name(Error error) noexcept;

} // namespace sfx::archive
