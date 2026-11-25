#pragma once
#include "sfx/application/local_file_administration.hpp"
#include "sfx/platform/linux/file_payload_store.hpp"
#include <filesystem>
#include <functional>

namespace sfx::platform::linux_host {
enum class FileGroupDeletionBoundary { file_deleted,before_retire };
// Called only inside the authenticated Runtime/exclusively held offline owner.
// Configuration never creates, relocates, adopts or repairs a selected library.
class LinuxFileConfigurationPaths final : public application::IFileConfigurationPaths {
public:
    LinuxFileConfigurationPaths(std::filesystem::path root,std::shared_ptr<IFilePayloadBindings> bindings,bool offline_deletion=false,std::function<void(FileGroupDeletionBoundary)> boundary={}):root_(std::move(root)),bindings_(std::move(bindings)),offline_deletion_(offline_deletion),boundary_(std::move(boundary)){}
    std::unique_ptr<application::IFileConfigurationLease> acquire(const application::FileConfiguration&,
        const application::FileConfigurationChange&) override;
    application::LocalFileConfigurationResult group_deletion(application::IFileStorage&,const application::FileConfigurationChange&) override;
private:
    std::filesystem::path root_;
    std::shared_ptr<IFilePayloadBindings> bindings_;
    bool offline_deletion_{};
    std::function<void(FileGroupDeletionBoundary)> boundary_;
};
} // namespace sfx::platform::linux_host
