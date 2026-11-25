#pragma once
#include "sfx/application/file_management.hpp"
#include "sfx/platform/linux/file_payload_store.hpp"
#include <filesystem>
#include <functional>
#include <memory>
namespace sfx::platform::linux_host {
// A single immutable native source owned by the authenticated owner worker.
// Never executes content, invokes a shell, extracts entries, or modifies files.
class LinuxFileOwnerView final {
public:
    using Progress=std::function<bool(std::uint64_t,std::uint64_t)>;
    LinuxFileOwnerView(application::IFileStorage&,std::filesystem::path,
        std::shared_ptr<IFilePayloadBindings>,application::FileId,bool archive,const Progress& = {},application::FileConfigurationActor actor={true,{}});
    ~LinuxFileOwnerView();
    LinuxFileOwnerView(const LinuxFileOwnerView&)=delete;
    application::FileManagementViewPage page(std::uint16_t);
    application::FileManagementViewPage member(std::uint16_t,const Progress& = {});
    application::FileManagementViewPage archive_index(std::uint16_t);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace sfx::platform::linux_host
