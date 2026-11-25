#pragma once
#include "sfx/platform/linux/file_payload_store.hpp"
#include "sfx/application/file_management.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"

namespace sfx::platform::linux_host {
// Collect only protected pending operation receipts for stopped-board Prepare.
// This updates custody evidence only; it never completes business operations.
void append_file_manager_prepare_proofs(application::IFileStorage&,const application::FileConfigurationActor&,
    const application::FileConfiguration&,FileBindingPrepareProofs&,const std::function<bool()>& cancelled={});
enum class FileManagerBoundary { before_change, destination_created, after_rename, after_copy, after_change, before_commit, after_staging_purge, import_identity_recorded, import_permissions_protected };
struct NativeFileOperationOutcome {
    bool completed{}, cancelled{}, journal_confirmed{};
    application::FileId result;
    std::string detail;
};
struct NativeFileImportChoice {
    application::Identifier128 operation;
    application::FileAreaId area;
    std::filesystem::path source;
    application::FileImportMode mode{application::FileImportMode::copy};
    std::string filename, public_uploader; // blank: original name/public importer
    std::optional<application::FileDescriptions> descriptions;
    bool use_diz{true}, allow_duplicate_content{}, sysop_only{};
};
// Read-only bounded cursor. A candidate is not a snapshot or permission to
// import: preview/import must recheck the selected native object and catalog.
class FileUnregisteredScan final {
public:
    ~FileUnregisteredScan();
    FileUnregisteredScan(FileUnregisteredScan&&) noexcept;
    FileUnregisteredScan& operator=(FileUnregisteredScan&&) noexcept;
    FileUnregisteredScan(const FileUnregisteredScan&)=delete;
    FilePayloadNamePage next(std::size_t limit=100,const std::function<bool()>& keep_going = {});
private:
    struct Impl;
    explicit FileUnregisteredScan(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
    friend class LinuxFileManager;
};
using FilePayloadPresence=application::FilePayloadPresence;
struct FileReconciliationEntry { application::ManagedFile record; FilePayloadPresence presence{FilePayloadPresence::unavailable}; };
struct FileReconciliationPage { std::vector<FileReconciliationEntry> entries; application::FileId next; };
// Trusted host coordinator. Paths are derived from current protected Config,
// never supplied by ordinary callers or accepted as a UI success/deletion bit.
class LinuxFileManager final {
public:
    using Progress=std::function<bool(std::uint64_t,std::uint64_t)>;
    using Boundary=std::function<void(FileManagerBoundary)>;
    LinuxFileManager(application::IFileStorage&,application::FileConfigurationActor,
        std::filesystem::path installation,std::shared_ptr<IFilePayloadBindings> bindings,Boundary = {});
    NativeFileOperationOutcome execute(const application::FileOperationRequest&,const Progress& = {});
    NativeFileOperationOutcome reconcile(application::Identifier128,const Progress& = {});
    NativeFileOperationOutcome cancel_unchanged(application::Identifier128,const Progress& = {});
    NativeFileOperationOutcome scan_files(bool pending,const Progress& = {},std::vector<std::string>* added_pages = nullptr);
    // Read-only native preview. Caller/UI edits only the bounded metadata before
    // confirmation; execution rechecks the exact source identity and all paths.
    std::optional<application::FileImportRequest> preview_import(const NativeFileImportChoice&,const Progress& = {});
    NativeFileOperationOutcome import_file(const application::FileImportRequest&,const Progress& = {});
    NativeFileOperationOutcome reconcile_import(application::Identifier128,const Progress& = {});
    NativeFileOperationOutcome cancel_import_unchanged(application::Identifier128,const Progress& = {});
    FileUnregisteredScan find_unregistered(application::FileAreaId);
    // Read-only preview; errors are unavailable, never inferred absence. Only
    // an explicit confirmed operation may mark missing or restore a record.
    FileReconciliationPage inspect_files(const application::FileManagementQuery&,application::FileId after,std::size_t limit,const Progress& = {});
    std::optional<application::FileIntakePurge> preview_intake_purge(application::Identifier128,const Progress& = {});
    NativeFileOperationOutcome purge_held_intake(const application::FileIntakePurge&,const Progress& = {});
private:
    FilePayloadDirectory area(const application::FileConfiguration&,application::FileAreaId) const;
    NativeFileOperationOutcome run(const application::FileOperation&,bool recovery,const Progress&);
    NativeFileOperationOutcome hold(application::Identifier128);
    FilePayloadDirectory import_source(const application::FileConfiguration&,const application::FileImportRequest&,bool adopt=false,const Progress& progress={}) const;
    NativeFileOperationOutcome run_import(const application::FileImport&,bool,const Progress&);
    NativeFileOperationOutcome hold_import(application::Identifier128);
    void reconcile_removal(const std::filesystem::path&,const application::FilePayloadIdentity&);
    application::IFileStorage& storage_;
    application::FileConfigurationActor actor_;
    std::filesystem::path installation_;
    std::shared_ptr<IFilePayloadBindings> bindings_;
    FilePayloadDirectory removed_;
    Boundary boundary_;
};
} // namespace sfx::platform::linux_host
