#pragma once
#include "sfx/application/local_file_administration.hpp"
#include "sfx/application/file_management.hpp"
#include "sfx/application/ftn_hatch_catalog.hpp"
#include <filesystem>
#include <memory>
namespace sfx::platform::win32 {
// Pins a verified CURRENT existing database and exclusive installation guard
// for this session's whole lifetime. Never starts Runtime, creates/migrates a
// database, repairs a selected library, or exposes raw storage/handles to GUI.
class OfflineFileAdministration final {
public:
    static std::unique_ptr<OfflineFileAdministration> begin(const std::filesystem::path&,
        const std::wstring& owner,std::string& diagnostic);
    ~OfflineFileAdministration();
    // Stop jobs/storage, capture portable namespace, and report preserved work.
    // The UI calls this on its worker before closing; destructor is fallback.
    std::string finish() noexcept;
    OfflineFileAdministration(const OfflineFileAdministration&)=delete;
    application::LocalFileConfigurationSnapshot configuration(const application::LocalFileConfigurationQuery&);
    application::StorageResult<application::FileConfiguration> configuration_catalog();
    std::optional<application::FtnHatchCatalog> ftn_hatch_catalog();
    application::FileManagementReadSnapshot query(const application::FileManagementReadRequest&);
    application::StorageResult<application::ManagedFilePage> page(const application::FileManagementQuery&,application::FileId,std::size_t);
    application::StorageResult<application::ManagedFile> detail(application::FileId);
    application::StorageResult<std::vector<application::FileOperation>> operations(application::Identifier128,std::size_t);
    application::StorageResult<std::vector<application::FileImport>> imports(application::Identifier128,std::size_t);
    application::StorageResult<std::vector<application::FileIntake>> intake(application::Identifier128,std::size_t);
    application::FileManagementJobStatus start(const application::FileManagementTask&);
    application::FileManagementJobStatus status(application::Identifier128);
    application::FileManagementJobStatus cancel(application::Identifier128);
private:
    struct Impl;explicit OfflineFileAdministration(std::unique_ptr<Impl>);std::unique_ptr<Impl> impl_;
};
} // namespace sfx::platform::win32
