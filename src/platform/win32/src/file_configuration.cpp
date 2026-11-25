#include "sfx/platform/win32/file_configuration.hpp"
#include "sfx/platform/win32/file_payload_store.hpp"
#include "sfx/platform/win32/file_manager.hpp"
#include "sfx/application/ftn_work.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <sodium.h>

namespace sfx::platform::win32 {
namespace {
struct ConfigurationLease final : application::IFileConfigurationLease {
    std::optional<FilePayloadDirectory> directory;
};
}
std::unique_ptr<application::IFileConfigurationLease> WindowsFileConfigurationPaths::acquire(
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
        {L"Bin",L"Config",L"Data",L"Display",L"Backup",L"Logs",L"Work",L"Doors",L"BotGate"},owner_);
    return lease;
}
#include "file_group_deletion.inc"
} // namespace sfx::platform::win32
