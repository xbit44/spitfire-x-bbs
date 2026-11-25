#pragma once
#include "sfx/application/file_storage.hpp"
#include "sfx/application/ftn_hatch.hpp"
#include "sfx/application/local_account_administration.hpp"

namespace sfx::application {
// List pages omit extended text; detail requests return the complete record.
// Forty compact records fit one 64KiB private frame, including maximum names.
inline constexpr std::size_t kFileManagementRecordPage=40,kFileManagementPendingPage=12;
enum class FileManagementQueryKind : std::uint8_t { records,detail,operations,imports,intake,hatches };
struct FileManagementReadRequest {
    FileManagementQueryKind kind{FileManagementQueryKind::records};
    FileManagementQuery filter;
    FileId after,selection;
    Identifier128 pending_after;
};
struct FileManagementPending {
    Identifier128 operation;
    FileId file;
    FileAreaId area;
    std::uint8_t state{},action{};
    std::string filename,detail;
    std::optional<FtnHatchRequest> hatch{}; // original reviewed choices, never secrets/native proof
};
struct FileManagementReadSnapshot {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};
    FileManagementQueryKind kind{FileManagementQueryKind::records};
    std::vector<ManagedFile> records{};
    FileId next{};
    std::vector<FileManagementPending> pending{};
    Identifier128 pending_next{};
};
bool valid_file_management_read_request(const FileManagementReadRequest&) noexcept;
bool valid_file_management_read_snapshot(const FileManagementReadSnapshot&) noexcept;
std::vector<std::uint8_t> encode_file_management_read_request(const FileManagementReadRequest&);
std::optional<FileManagementReadRequest> decode_file_management_read_request(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_file_management_read_snapshot(const FileManagementReadSnapshot&);
std::optional<FileManagementReadSnapshot> decode_file_management_read_snapshot(const std::vector<std::uint8_t>&);
class IFtnMailStorage;
class LocalFileManagementQueries final {
public:
    LocalFileManagementQueries(IFileStorage&,LocalAccountAuthority,IFtnMailStorage* ftn=nullptr);
    LocalFileManagementQueries(IFileStorage&,MessageActor);
    FileManagementReadSnapshot snapshot(const FileManagementReadRequest&);
private:
    IFileStorage& storage_;LocalAccountAuthority authority_;IFtnMailStorage* ftn_{};
    std::optional<MessageActor> caller_;
};
enum class FileManagementTaskKind : std::uint8_t {
    operation, preview_import, import_file, reconcile_operation, cancel_operation,
    reconcile_import, cancel_import, preview_intake_purge, purge_intake, reupload_block,
    discover_unregistered,continue_unregistered,cancel_unregistered,inspect_files,
    discover_import_folder,open_view,page_view,read_view_member,view_archive_index,close_view,
    reconcile_intake,scan_pending,scan_offline,scan_report,hatch_file,reconcile_hatch
};
enum class FilePayloadPresence : std::uint8_t { present,missing,changed,unavailable,removed,pending };
struct FileManagementDiscoveryPage {
    Identifier128 cursor; // this page's job; next request names THIS exact page
    FileAreaId area;
    std::vector<std::string> names;
    bool finished{};
    std::string source_folder{}; // native canonical selected folder; owner only
};
struct FileManagementInspectionPage {
    FileManagementReadSnapshot catalog;
    std::vector<FilePayloadPresence> presence;
};
inline constexpr std::size_t kFileViewColumns=80,kFileViewRows=25,kFileViewArchivePage=20;
struct FileViewCell {std::uint8_t glyph{32},attribute{7};};
struct FileViewArchiveEntry {
    std::uint16_t index{}; // zero-based index of the same held ZIP directory
    std::string name;
    std::uint64_t bytes{};
    bool directory{},encrypted{},readable{};
};
// Read-only INERT display data, never original ANSI/control bytes or an
// extraction/native pathname. Total means rows for text, entries for a ZIP.
struct FileManagementViewPage {
    Identifier128 cursor;
    FileId file;
    std::int64_t revision{};
    std::string filename,member;
    bool archive{};
    std::uint16_t page{};
    std::uint32_t total{};
    std::vector<FileViewCell> cells;
    std::vector<FileViewArchiveEntry> entries;
};
struct FileImportChoice {
    Identifier128 operation;
    FileAreaId area;
    std::string source_path,filename,public_uploader;
    FileImportMode mode{FileImportMode::copy};
    std::optional<FileDescriptions> descriptions;
    bool use_diz{true},allow_duplicate_content{},sysop_only{};
};
struct FileReuploadBlockRequest {
    Identifier128 operation;
    FileId file;
    std::int64_t expected_revision{};
    bool blocked{};
};
// An asynchronous job identity is distinct from the durable mutation/import
// identity. Preview and acceptance use different jobs but the SAME import
// intent, while uncertain start responses retry the SAME exact job and task.
struct FileManagementTask {
    Identifier128 job;
    FileManagementTaskKind kind{FileManagementTaskKind::operation};
    FileOperationRequest operation;
    FileImportChoice choice;
    FileImportRequest import;
    Identifier128 target;
    FileIntakePurge purge;
    FileReuploadBlockRequest block;
    FileAreaId discovery_area;
    FileManagementReadRequest inspection_request;
    std::string discovery_folder;
    FileId view_file;
    FtnHatchRequest hatch;
    bool view_archive{};
    std::uint16_t view_page{},view_member{};
};
enum class FileManagementJobState : std::uint8_t { running,completed,held,cancelled,refused,failed };
struct FileManagementJobStatus {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};
    Identifier128 job;
    FileManagementTaskKind kind{FileManagementTaskKind::operation};
    FileManagementJobState state{FileManagementJobState::refused};
    // Actual current native pass, not an invented percentage of a multi-pass
    // hash/copy/verify transaction. Progress can restart at a new pass.
    std::uint64_t bytes_done{},bytes_total{},elapsed_millis{};
    bool cancellation_requested{},journal_confirmed{};
    FileId result;
    std::uint64_t duplicate_contents{}; // preview warning only; never overwrite authority
    std::string detail;
    std::optional<FileImportRequest> preview;
    std::optional<FileIntakePurge> purge_preview;
    std::optional<FileManagementDiscoveryPage> discovery;
    std::optional<FileManagementInspectionPage> inspection;
    std::optional<FileManagementViewPage> view;
    // Matched-owner report chunks, never paths or success authority. The scan
    // result advertises page count; read-only scan_report fetches exact pages.
    struct Report {std::uint16_t page{},pages{};};
    std::optional<Report> report;
};
bool valid_file_management_view(const FileManagementViewPage&) noexcept;
bool valid_file_management_operation(const FileOperationRequest&) noexcept;
bool valid_file_management_task(const FileManagementTask&) noexcept;
bool valid_file_management_status(const FileManagementJobStatus&) noexcept;
// Canonical, bounded private-owner payloads. Never contain an authority bit or
// caller/helper success assertion; native hosts verify every object/commit.
std::vector<std::uint8_t> encode_file_management_task(const FileManagementTask&);
std::optional<FileManagementTask> decode_file_management_task(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_file_management_status(const FileManagementJobStatus&);
std::optional<FileManagementJobStatus> decode_file_management_status(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_file_management_job(Identifier128);
std::optional<Identifier128> decode_file_management_job(const std::vector<std::uint8_t>&);
} // namespace sfx::application
