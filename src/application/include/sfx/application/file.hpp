#pragma once

#include "sfx/application/message_storage.hpp"

namespace sfx::application {

struct FileGroupId { std::int64_t value{}; };
struct FileAreaId { std::int64_t value{}; };
struct FileId { std::int64_t value{}; };
inline bool operator==(FileGroupId a, FileGroupId b) noexcept { return a.value == b.value; }
inline bool operator==(FileAreaId a, FileAreaId b) noexcept { return a.value == b.value; }
inline bool operator==(FileId a, FileId b) noexcept { return a.value == b.value; }

inline constexpr std::uint64_t kDefaultCallerFileBytes = 100ULL * 1024 * 1024;
inline constexpr std::uint64_t kMaximumCallerFileBytes = 1024ULL * 1024 * 1024;
inline constexpr std::size_t kFileNameBytes = 128, kFileShortDescriptionBytes = 80;
inline constexpr std::size_t kFileLongDescriptionBytes = 16384;
inline constexpr std::size_t kFileNameKeyBytes = kFileNameBytes*3;
inline constexpr std::size_t kFileQueueMaximum = 256, kFilePageMaximum = 100;
inline constexpr std::uint16_t kFileAreaMaximum = 999;

// File actors are created by authenticated native hosts, never by caller input.
// Storage rechecks the live account, policy and session for every operation.
using FileActor = MessageActor;
using FileConfigurationActor = MessageConfigurationActor;

enum class FileAreaKind : std::uint8_t { ordinary, review_uploads, private_sysop };
enum class FileSort : std::uint8_t { filename, newest, oldest, size, downloads };
enum class FileSearchScope : std::uint8_t { area, group, all };
enum class FileSearchKind : std::uint8_t { filename, descriptions };
enum class FileOrigin : std::uint8_t { caller_upload, sysop_import, sysop_copy, ftn };
enum class FileCatalogState : std::uint8_t { available, removed, missing };

struct FilePermissions {
    std::uint16_t access{1}, upload{1}, download{1};
    bool uploads_enabled{};
    bool downloads_sysop_only{};
};
struct FileGroup {
    FileGroupId id;
    std::string name;
    std::int64_t order{}, revision{1};
    std::uint16_t access{1};
    bool enabled{true};
    Identifier128 deletion_operation{}; // native owner deletion recovery, never caller authority
    bool delete_disk_files{};
};
struct FileArea {
    FileAreaId id;
    FileGroupId group;
    std::uint16_t number{}; // permanent identity, not a visible-list position
    std::string name, description;
    std::int64_t order{}, revision{1};
    FileAreaKind kind{FileAreaKind::ordinary};
    FilePermissions permissions;
    bool enabled{true}, own_description_edit{}, backup_contents{};
    bool ratio_uploads{true}, ratio_downloads{true};
    std::optional<std::uint64_t> maximum_upload_bytes; // absent inherits global
    bool sysop_review{}; // ordinary destination routes acceptance to Uploads
};
// Native configuration only. Caller DTOs contain no absolute filesystem paths.
struct FileAreaConfiguration {
    FileArea area;
    std::string storage_path; // root-relative default or explicitly validated absolute path
};
struct FileRatio {
    std::uint32_t downloads{}, uploads{}; // no invented or prefilled selection
};
struct FileCountRule {
    std::optional<std::uint32_t> daily_downloads;
    std::optional<FileRatio> ratio;
};
struct FilePolicy {
    std::int64_t revision{1}, catalog_revision{1};
    std::uint64_t maximum_upload_bytes{kDefaultCallerFileBytes};
    FileCountRule counts; // both Off on a fresh installation
    bool lightbar_default{true}, scan_uploads{};
};
struct FileAuthority {
    std::uint16_t level{}, sysop_threshold{};
    bool valid() const noexcept;
    bool sysop() const noexcept;
};
bool valid_file_group(const FileGroup&) noexcept;
bool valid_file_area(const FileArea&) noexcept;
bool valid_file_count_rule(const FileCountRule&) noexcept;
bool valid_file_policy(const FilePolicy&) noexcept;
bool file_area_visible(const FileGroup&, const FileArea&, FileAuthority) noexcept;
bool file_area_uploadable(const FileGroup&, const FileArea&, FileAuthority,
                          bool private_submission) noexcept;
bool file_area_downloadable(const FileGroup&, const FileArea&, FileAuthority) noexcept;
std::uint64_t file_upload_limit(const FilePolicy&, const FileArea&) noexcept;

struct FileDescriptions { std::string short_text, long_text; };
bool valid_file_utf8(std::string_view) noexcept;
// Pinned Unicode17 simple default fold; not locale-dependent, not normalization.
std::optional<std::string> file_case_key(std::string_view);
bool valid_file_name(std::string_view) noexcept;
bool valid_file_descriptions(const FileDescriptions&) noexcept;
// Untrusted DIZ/metadata is text, never commands or terminal escapes. The caller
// command parser, not this conversion, owns the historical leading-backslash flag.
std::string sanitize_file_text(std::string_view, std::size_t maximum, bool multiline);
std::optional<FileDescriptions> file_diz_descriptions(std::string_view);
FileDescriptions file_import_descriptions(std::string_view name,
                                         std::optional<std::string_view> diz);
bool file_name_matches(std::string_view name, std::string_view query) noexcept;
std::optional<std::vector<std::string>> file_description_terms(std::string_view);
bool file_description_matches(const FileDescriptions&, const std::vector<std::string>&);

struct FileEntry {
    FileId id;
    FileAreaId area;
    std::int64_t revision{1}, publication_sequence{}, published_utc{}, uploaded_utc{};
    std::string name;
    FileDescriptions descriptions;
    std::string public_uploader; // retained public attribution, not a private profile
    std::optional<AccountId> uploader;
    std::uint64_t bytes{}, downloads{};
    std::array<std::uint8_t, 32> sha256{};
    FileOrigin origin{FileOrigin::caller_upload};
    FileCatalogState state{FileCatalogState::available};
    bool sysop_only{};
    bool new_to_caller{}; // read-only caller query annotation, never a commit flag
};
bool file_entry_visible(const FileGroup&, const FileArea&, const FileEntry&, FileAuthority) noexcept;
bool file_description_editable(const FileGroup&, const FileArea&, const FileEntry&,
                              FileAuthority, AccountId) noexcept;
bool file_entry_less(const FileEntry&, const FileEntry&, FileSort) noexcept;

struct FileCountState {
    std::uint64_t successful_uploads{}, successful_downloads{};
    std::uint64_t eligible_uploads{}, eligible_downloads{}, today_downloads{};
    std::uint64_t reserved_downloads{}, reserved_eligible_downloads{}, reserved_today_downloads{};
};
enum class FileDownloadAllowance : std::uint8_t { allowed, daily_limit, ratio_limit, invalid_policy };
std::uint64_t file_ratio_entitlement(FileRatio, std::uint64_t eligible_uploads) noexcept;
FileDownloadAllowance file_download_allowance(const FileCountRule&, const FileCountState&,
    bool ratio_participating, bool daily_exempt, bool ratio_exempt) noexcept;

// Queues hold identities/metadata only. Clearing/removing never touches payloads.
struct FileUploadItem { std::string name; FileAreaId destination; FileDescriptions descriptions; bool sysop_only{}; bool prefer_diz{true}; };
class FileUploadQueue final {
public:
    bool add(FileUploadItem);
    bool remove(std::size_t zero_based);
    void clear() noexcept { items_.clear(); }
    const std::vector<FileUploadItem>& items() const noexcept { return items_; }
private:
    std::vector<FileUploadItem> items_;
};
class FileDownloadQueue final {
public:
    bool add(FileId);
    bool remove(FileId);
    void clear() noexcept { items_.clear(); }
    const std::vector<FileId>& items() const noexcept { return items_; }
private:
    std::vector<FileId> items_;
};

enum class FileScanVerdict : std::uint8_t { not_scanned, clean, detected, incomplete, error, timeout };
struct FileScanResult {
    FileScanVerdict verdict{FileScanVerdict::error};
    std::vector<std::string> detections;
    std::string detail;
};
enum class IncomingFileAction : std::uint8_t { validate_and_commit, delete_detected_staging, durable_private_hold };
// This decision concerns only NEW owned caller staging, never existing files.
IncomingFileAction incoming_file_action(bool scanning_enabled, const FileScanResult&) noexcept;
std::string file_scan_report(const FileScanResult&, bool committed, bool deletion_succeeded);

} // namespace sfx::application
