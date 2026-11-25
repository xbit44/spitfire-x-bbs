#include "sfx/platform/linux/file_configuration.hpp"
#include "sfx/platform/linux/file_payload_store.hpp"
#include "sfx/platform/linux/file_manager.hpp"
#include "sfx/application/ftn_work.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <sodium.h>
#include <stdexcept>

namespace sfx::platform::linux_host {
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
struct ConfigurationLease final : application::IFileConfigurationLease {
    std::optional<FilePayloadDirectory> directory;
    void verify() const override {if(directory)directory->verify();}
};
}
std::unique_ptr<application::IFileConfigurationLease> LinuxFileConfigurationPaths::acquire(
    const application::FileConfiguration& current,const application::FileConfigurationChange& change){
    if((change.action!=application::FileConfigurationAction::area&&change.action!=application::FileConfigurationAction::insert_area_before&&change.action!=application::FileConfigurationAction::insert_area_after)||!application::valid_file_configuration_change(change))return {};
    auto lease=std::make_unique<ConfigurationLease>();
    const auto& selected=change.area;
    const auto old=std::find_if(current.areas.begin(),current.areas.end(),[&](const auto& a){return a.area.id==selected.area.id;});
    if(selected.area.id.value){
        if(old==current.areas.end())return {};
        // A label/permission-only change neither selects nor adopts a new path.
        // Allow disabling or repairing metadata for a missing library; actual
        // payload use still requires the ordinary native object capability.
        if(old->storage_path==selected.storage_path)return lease;
    }
    std::vector<std::filesystem::path> others;
    for(const auto& a:current.areas)if(!(a.area.id==selected.area.id))others.push_back(std::filesystem::u8path(a.storage_path));
    lease->directory=FilePayloadDirectory::area(root_,std::filesystem::u8path(selected.storage_path),others,
        {"bin","config","data","display","backup","logs","work","doors","botgate"},bindings_);
    return lease;
}
#include "file_group_deletion.inc"
} // namespace sfx::platform::linux_host
