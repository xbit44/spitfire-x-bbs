#pragma once
#include "sfx/application/file_management.hpp"
#include "sfx/application/local_file_administration.hpp"
#include "sfx/application/ftn_hatch_catalog.hpp"
#include <filesystem>
#include <memory>
namespace sfx::platform::win32 {
// Presentation uses ONLY typed ports. No GUI SQL, source-file adoption, native
// success flags or caller transports. A failed online link never silently
// becomes a second offline host. Reload/reconnect is an explicit UI action.
class IFileAdministrationClient {
public:
    virtual ~IFileAdministrationClient()=default;
    virtual bool online() const noexcept=0;
    virtual std::string finish(){return {};}
    virtual std::optional<application::FileConfiguration> configuration()=0;
    virtual application::FileManagementReadSnapshot read(const application::FileManagementReadRequest&)=0;
    virtual application::FileManagementJobStatus start(const application::FileManagementTask&)=0;
    virtual application::FileManagementJobStatus status(application::Identifier128)=0;
    virtual application::FileManagementJobStatus cancel(application::Identifier128)=0;
    virtual std::optional<application::FtnHatchCatalog> ftn_hatch_catalog(){return {};}
};
std::unique_ptr<IFileAdministrationClient> open_file_administration_client(
    const std::filesystem::path&,const std::wstring& owner,std::string& diagnostic);
} // namespace sfx::platform::win32
