#pragma once
#include "sfx/application/file.hpp"

namespace sfx::application {

struct FileCatalog {
    std::int64_t revision{};
    std::vector<FileGroup> groups;
    std::vector<FileArea> areas; // authorized DTO only; no native paths
};
struct FileSecurityRule {
    std::uint16_t level{};
    std::int64_t revision{1};
    bool inherit{true}, daily_exempt{}, ratio_exempt{};
    FileCountRule counts;
};
struct FileCallerExemption {
    AccountId account{};
    std::int64_t revision{1};
    bool daily_exempt{}, ratio_exempt{};
};
struct FileConfiguration {
    FilePolicy policy;
    std::vector<FileGroup> groups;
    std::vector<FileAreaConfiguration> areas;
    std::vector<FileSecurityRule> security_rules;
};
struct FilePayloadIdentity {
    std::uint64_t bytes{};
    std::array<std::uint8_t, 32> sha256{};
    std::string volume, file_id;
};
struct FileObjectIdentity { std::string volume, file_id; };
struct FileGroupDeletionItem {
    FileAreaId area;
    FileId file; // zero for an unregistered on-disk file
    std::string name;
    FilePayloadIdentity payload;
};
struct FileGroupDeletionState {
    FileGroupId group;Identifier128 operation;bool delete_disk_files{},completed{};
    std::vector<FileGroupDeletionItem> items;
};
enum class FileConfigurationAction : std::uint8_t { policy, group, area, security_rule, caller_exemption,
    insert_area_before,insert_area_after,move_area_up,move_area_down,preview_group_delete,begin_group_delete,finish_group_delete };
struct FileConfigurationChange {
    Identifier128 operation;
    FileConfigurationAction action{FileConfigurationAction::policy};
    std::int64_t expected_catalog_revision{};
    FilePolicy policy{};
    FileGroup group{};
    FileAreaConfiguration area{};
    FileSecurityRule security{};
    FileCallerExemption exemption{};
    FileAreaId anchor{};
    std::int64_t anchor_revision{};
    bool delete_disk_files{};
    std::vector<FileGroupDeletionItem> deletion_items{}; // native-only plan, never accepted from IPC
};
struct FileChangeOutcome { std::int64_t revision{}, identity{}; bool replayed{}; };
struct FileCallerState {
    std::int64_t since_utc{}, last_seen_publication{}, revision{1};
    FileAreaId current_area;
    bool lightbar{true}, new_file_scan{true};
    std::optional<std::int64_t> scan_completed_utc;
};
struct FileScanArea { FileAreaId area; bool selected{true}; std::int64_t revision{1}; };
struct FileScanSeen { FileId file; std::int64_t publication_sequence{}; };
struct FileQuery {
    FileSearchScope scope{FileSearchScope::area};
    FileAreaId area;
    FileGroupId group;
    FileSort sort{FileSort::filename};
    std::optional<FileSearchKind> search;
    std::string query;
    bool downloadable_only{}, new_only{};
    std::int64_t since_utc{}, after_publication{}, publication_fence{};
    bool unseen_only{}, selected_only{}; // native New File Scan, not arbitrary caller SQL
};
struct FileQueryCursor {
    std::string filename_key;
    std::int64_t sort_value{};
    FileId file;
};
struct FilePage {
    std::vector<FileEntry> entries;
    std::optional<FileQueryCursor> next;
    std::int64_t publication_fence{};
    std::uint64_t total_files{}, total_bytes{}; // complete authorized query scope
    std::int64_t last_updated_utc{};
};
struct FileCallerStatistics {
    FileCountState counts;
    std::uint64_t upload_bytes{}, download_bytes{};
    FileCountRule rule;
    bool daily_exempt{}, ratio_exempt{};
};
// Native, handle-verified payload identity. This is not a caller IPC payload.
// Host retains the safe parent/file handles through the corresponding commit.
struct FileRegistration {
    Identifier128 operation;
    FileAreaId area;
    std::int64_t expected_area_revision{};
    std::string filename;
    FileDescriptions descriptions;
    std::string public_uploader;
    FilePayloadIdentity payload;
    FileOrigin origin{FileOrigin::sysop_import};
    std::optional<AccountId> uploader;
    std::int64_t original_upload_utc{};
    bool sysop_only{}, allow_duplicate_content{};
};
struct FileDownloadReservation {
    Identifier128 operation;
    FileEntry file;
    FilePayloadIdentity payload;
    std::string bbs_local_date;
    bool replayed{}, completed{};
};
struct FileDownloadStart {
    FileDownloadAllowance allowance{FileDownloadAllowance::allowed};
    std::optional<FileDownloadReservation> reservation;
};
struct FileTransferOutcome { bool completed{}, replayed{}; };

enum class FileIntakeState : std::uint8_t { receiving, ready, prepared, accepted, deleted, held };
// Private native-host journal, never a caller list/search DTO or arbitrary path.
struct FileIntake {
    Identifier128 operation;
    AccountId account{};
    Identifier128 session;
    std::string stage_key, filename, bbs_local_date, public_uploader;
    FileAreaId area;
    std::int64_t area_revision{}, created_utc{};
    std::uint64_t maximum_bytes{};
    bool private_submission{}, scan_required{}, transfer_complete{}, sysop_only{};
    FileIntakeState state{FileIntakeState::receiving};
    FilePayloadIdentity payload;
    FileDescriptions descriptions;
    FileScanResult scan;
    FileId accepted_file;
    FileAreaId placement_area; // distinct from retained intended area
    std::int64_t placement_revision{};
    std::optional<FileObjectIdentity> staged_object, placed_object;
    bool staging_clean{}; // accepted payload's redundant incoming copy is absent
    // Native-only LEGACY opaque basename, bound by the durable private intake
    // key. New receives leave this empty and retain their original final name.
    // Never a caller DTO/path; old accepted/pending rows remain recoverable.
    std::string placement_name;
};
inline const std::string& file_intake_payload_name(const FileIntake& r) noexcept { return r.placement_name.empty()?r.filename:r.placement_name; }
struct FileIntakeStart {
    Identifier128 operation;
    FileAreaId area;
    bool private_submission{};
    std::string bbs_local_date;
};
struct FileIntakeReceived {
    Identifier128 operation;
    std::string filename;
    FilePayloadIdentity payload;
    FileScanResult scan;
    bool transfer_complete{};
};
// Explicit Sysop purge intent, distinct from automatic positive-malware deletion.
// Optional objects are actual native preview observations; absence is not an
// error fallback. Persist both before removing either so interrupted purge can
// reconcile exact original objects without touching later replacements.
struct FileIntakePurge {
    Identifier128 operation;
    std::optional<FilePayloadIdentity> staging, placed;
    bool completed{};
};

enum class FileManagementSearch : std::uint8_t { any, filename, descriptions, uploader };
struct FileManagementQuery {
    FileAreaId area; // zero = all; this interface is Sysop/installation-owner only
    std::optional<FileCatalogState> state;
    FileManagementSearch field{FileManagementSearch::any};
    std::string search;
};
struct ManagedFile {
    FileEntry file;
    FilePayloadIdentity payload;
    std::string removed_key; // host derives the durable private removal path
    bool purged{}, operation_pending{};
    FileAreaId intended_area; // private management context for reviewed uploads
    bool reupload_blocked{}; // content policy, independent of removal/purge
    std::string native_basename; // opaque private placement only; empty = legacy/name
};
inline const std::string& file_managed_payload_name(const ManagedFile& r) noexcept { return r.native_basename.empty()?r.file.name:r.native_basename; }
struct ManagedFilePage { std::vector<ManagedFile> files; FileId next; };
// Bounded exact-name reconciliation. Private Sysop data, never a caller DTO.
struct FileNameStatus { std::string filename; FileId registered; bool pending{}; bool staging_only_hold{}; };
enum class FileOperationAction : std::uint8_t { metadata, rename, copy, move, remove, restore, purge, mark_missing };
enum class FileOperationState : std::uint8_t { prepared, completed, held, cancelled };
struct FileOperationRequest {
    Identifier128 operation;
    FileOperationAction action{FileOperationAction::metadata};
    FileId file;
    std::int64_t expected_revision{};
    FileAreaId destination;
    std::int64_t expected_destination_revision{};
    std::string filename;
    FileDescriptions descriptions;
    std::string public_uploader;
    bool sysop_only{}, allow_duplicate_content{};
};
struct FileOperation {
    FileOperationRequest request;
    ManagedFile source;
    FileOperationState state{FileOperationState::prepared};
    FileId result;
    std::string detail;
    std::optional<FileObjectIdentity> placed_object;
};
// A trusted native observation, NOT a client-supplied success/deletion flag.
struct FileOperationCompletion {
    FilePayloadIdentity source, destination;
    bool source_absent{}, destination_present{};
};
enum class FileImportMode : std::uint8_t { in_place, copy, move };
struct FileImportRequest {
    FileRegistration registration; // payload is the locked SOURCE at preparation
    FileImportMode mode{FileImportMode::in_place};
    std::string source_path; // private native journal; never a caller DTO
};
struct FileImport {
    FileImportRequest request;
    FileOperationState state{FileOperationState::prepared};
    std::optional<FileObjectIdentity> placed_object;
    FileId result;
    std::string detail;
};

// Deliberately independent from message/QWK storage and their frozen limits.
class IFileStorage {
public:
    virtual ~IFileStorage() = default;
    virtual StorageResult<FileCatalog> file_catalog(const FileActor&) = 0;
    virtual StorageResult<FileConfiguration> file_configuration(const FileConfigurationActor&) = 0;
    virtual StorageResult<FileGroupDeletionState> file_group_deletion(const FileConfigurationActor&,FileGroupId) { return {{StorageErrorCode::not_found,"File Group deletion unavailable"},{}}; }
    virtual StorageResult<FileCallerExemption> file_caller_exemption(const FileConfigurationActor&,AccountId) = 0;
    virtual StorageResult<std::uint64_t> file_content_duplicate_count(const FileConfigurationActor&,const FilePayloadIdentity&) = 0;
    virtual StorageResult<FileChangeOutcome> change_file_configuration(const FileConfigurationActor&, const FileConfigurationChange&) = 0;
    virtual StorageResult<FileCallerState> file_caller_state(const FileActor&) = 0;
    virtual StorageResult<FileCallerState> save_file_caller_state(const FileActor&, const FileCallerState&) = 0;
    virtual StorageResult<std::vector<FileScanArea>> file_scan_areas(const FileActor&) = 0;
    virtual StorageResult<FileScanArea> save_file_scan_area(const FileActor&, const FileScanArea&) = 0;
    // Trusted Runtime acknowledges only entries actually emitted. Merely fetching
    // a page must never mark its rows or any skipped/hidden area as seen.
    virtual StorageStatus acknowledge_file_scan(const FileActor&, const std::vector<FileScanSeen>&) = 0;
    virtual StorageStatus complete_file_scan(const FileActor&, const std::vector<FileAreaId>&, std::int64_t fence, std::int64_t since_utc) = 0;
    virtual StorageResult<FilePage> file_page(const FileActor&, const FileQuery&, const FileQueryCursor&, std::size_t limit) = 0;
    virtual StorageResult<FileEntry> file_information(const FileActor&, FileId) = 0;
    virtual StorageResult<FileChangeOutcome> edit_own_file_description(const FileActor&, Identifier128, FileId, std::int64_t revision, const FileDescriptions&) = 0;
    virtual StorageResult<FileEntry> register_file(const FileConfigurationActor&, const FileRegistration&) = 0;
    virtual StorageResult<FileCallerStatistics> file_caller_statistics(const FileActor&, std::string_view bbs_local_date) = 0;
    virtual StorageResult<FileDownloadStart> reserve_file_download(const FileActor&, Identifier128, FileId, std::string_view bbs_local_date) = 0;
    // Trusted Runtime calls only on a verified terminal helper outcome; never
    // deserialize a success flag from the caller or the ordinary Config UI.
    virtual StorageResult<FileTransferOutcome> finish_file_download(const FileActor&, Identifier128, const FilePayloadIdentity&, bool success) = 0;
    virtual StorageStatus recover_file_transfers(const FileConfigurationActor&) = 0;
    // Native host creates this record BEFORE creating durable receive storage.
    virtual StorageResult<FileIntake> begin_file_intake(const FileActor&, const FileIntakeStart&) = 0;
    virtual StorageResult<FileIntake> file_intake(const FileConfigurationActor&, Identifier128) = 0;
    // Native objects are bound before the first write or placement, never from
    // a caller/UI success assertion. Bindings cannot be reassigned on retry.
    virtual StorageStatus bind_file_intake_object(const FileActor&, Identifier128, bool placement, const FileObjectIdentity&) = 0;
    virtual StorageStatus finish_file_intake_cleanup(const FileConfigurationActor&, Identifier128, const FileObjectIdentity&) = 0;
    virtual StorageResult<FileIntake> record_file_intake(const FileActor&, const FileIntakeReceived&) = 0;
    virtual StorageResult<FileIntake> prepare_file_intake(const FileActor&, Identifier128, const FileDescriptions&, bool sysop_only) = 0;
    // The host holds verified final directory/payload handles across this commit.
    virtual StorageResult<FileEntry> commit_file_intake(const FileActor&, Identifier128, const FilePayloadIdentity&) = 0;
    virtual StorageResult<std::vector<FileIntake>> pending_file_intake(const FileConfigurationActor&, std::size_t limit, Identifier128 after = {}) = 0;
    // Native recovery/deletion receipt only; never trust a caller/UI success bit.
    virtual StorageStatus resolve_file_intake(const FileConfigurationActor&, Identifier128, const FilePayloadIdentity&, bool deleted, std::string_view detail) = 0;
    virtual StorageStatus recover_file_intake(const FileConfigurationActor&) = 0;
    virtual StorageResult<FileIntakePurge> file_intake_purge(const FileConfigurationActor&,Identifier128) = 0;
    virtual StorageStatus prepare_file_intake_purge(const FileConfigurationActor&,const FileIntakePurge&) = 0;
    virtual StorageStatus finish_file_intake_purge(const FileConfigurationActor&,const FileIntakePurge&) = 0;
    virtual StorageResult<ManagedFilePage> managed_file_page(const FileConfigurationActor&,const FileManagementQuery&,FileId after,std::size_t limit) = 0;
    virtual StorageResult<ManagedFile> managed_file(const FileConfigurationActor&,FileId) = 0;
    virtual StorageResult<std::vector<FileNameStatus>> file_name_status(const FileConfigurationActor&,FileAreaId,const std::vector<std::string>&) = 0;
    virtual StorageResult<FileChangeOutcome> set_file_reupload_block(const FileConfigurationActor&,Identifier128,FileId,std::int64_t revision,bool blocked) = 0;
    virtual StorageResult<FileOperation> prepare_file_operation(const FileConfigurationActor&,const FileOperationRequest&) = 0;
    virtual StorageResult<FileOperation> file_operation(const FileConfigurationActor&,Identifier128) = 0;
    // Bind a newly created native destination before copying bytes. Never a UI
    // assertion; replay requires the identical held native object.
    virtual StorageStatus bind_file_operation_destination(const FileConfigurationActor&,Identifier128,const FileObjectIdentity&) = 0;
    virtual StorageResult<FileOperation> commit_file_operation(const FileConfigurationActor&,Identifier128,const FileOperationCompletion&) = 0;
    // Verified unchanged source plus absent intended destination only. An
    // interrupted destructive operation cannot be cancelled by a UI flag.
    virtual StorageStatus cancel_file_operation(const FileConfigurationActor&,Identifier128,const FileOperationCompletion&) = 0;
    virtual StorageStatus hold_file_operation(const FileConfigurationActor&,Identifier128,std::string_view detail) = 0;
    virtual StorageResult<std::vector<FileOperation>> pending_file_operations(const FileConfigurationActor&,Identifier128 after,std::size_t limit) = 0;
    virtual StorageResult<std::string> file_importer_name(const FileConfigurationActor&) = 0;
    virtual StorageResult<FileImport> prepare_file_import(const FileConfigurationActor&,const FileImportRequest&) = 0;
    virtual StorageResult<FileImport> file_import(const FileConfigurationActor&,Identifier128) = 0;
    virtual StorageResult<std::vector<FileImport>> pending_file_imports(const FileConfigurationActor&,Identifier128 after,std::size_t limit) = 0;
    virtual StorageStatus bind_file_import_destination(const FileConfigurationActor&,Identifier128,const FileObjectIdentity&) = 0;
    virtual StorageResult<FileEntry> commit_file_import(const FileConfigurationActor&,Identifier128,const FileOperationCompletion&) = 0;
    virtual StorageStatus hold_file_import(const FileConfigurationActor&,Identifier128,std::string_view) = 0;
    virtual StorageStatus cancel_file_import(const FileConfigurationActor&,Identifier128,const FileOperationCompletion&) = 0;
};
} // namespace sfx::application
