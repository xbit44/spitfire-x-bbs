#include "sfx/platform/linux/door_bulletin_sources.hpp"
#include "sfx/application/ansi_playback_settings.hpp"
#include "sfx/platform/linux/trusted_display_asset_reader.hpp"
#include <algorithm>
#include <sys/stat.h>
namespace sfx::platform::linux_host {
namespace app=application;
app::TrustedDisplayAssetReadResult read_door_bulletin_source(const std::filesystem::path& root,const app::DoorBulletinSource& source,std::shared_ptr<IFilePayloadBindings> bindings,const std::function<void()>& native_test_after_capture){
    if(!source.door_available||source.definition.source!=app::DoorBulletinSourceKind::door_file||!app::valid_door_bulletin_definition(source.definition))throw std::runtime_error("door_bulletin_source_definition_unavailable");
    auto work_text=source.work_directory;std::replace(work_text.begin(),work_text.end(),'\\','/');auto work=work_text.empty()?root:std::filesystem::path(work_text);if(work.is_relative())work=root/work;if(bindings)work=bindings->resolve_path(work);
    auto directory=open_owned_directory(work,false);
    auto relative=source.definition.relative_path;std::replace(relative.begin(),relative.end(),'\\','/');const std::filesystem::path path(relative);
    // Every component remains relative to a no-follow descriptor. The shared
    // owned reader rejects hardlinks/nonregular/unsafe/oversized/changing files.
    for(const auto& part:path.parent_path())directory=open_owned_resource_directory(directory.get(),part.string());
    struct stat before{},after{};if(fstat(directory.get(),&before)!=0)throw std::runtime_error("door_bulletin_source_parent_unavailable");
    const auto leaf=path.filename().string();const auto first=read_owned_resource(directory.get(),leaf,app::kMaximumTrustedDisplaySourceBytes);
    if(native_test_after_capture)native_test_after_capture();
    const auto second=read_owned_resource(directory.get(),leaf,app::kMaximumTrustedDisplaySourceBytes);
    if(first!=second)throw std::runtime_error("door_bulletin_source_changed_between_captures");
    auto current=open_owned_directory(work,false);for(const auto& part:path.parent_path())current=open_owned_resource_directory(current.get(),part.string());
    if(fstat(current.get(),&after)!=0||before.st_dev!=after.st_dev||before.st_ino!=after.st_ino)throw std::runtime_error("door_bulletin_source_parent_changed_during_capture");
    return {app::TrustedDisplayAssetReadCode::available,leaf,"owned-door-source:"+std::to_string(source.definition.door)+":"+relative,
        {first.begin(),first.end()},{},app::ansi_playback_bps(work / path)};
}
app::DoorBulletinSourcePorts door_bulletin_sources(const std::filesystem::path& root,std::shared_ptr<IFilePayloadBindings> bindings,std::function<void(app::DoorBulletinId,std::string_view)> diagnostic){
    app::DoorBulletinSourcePorts ports;ports.display_reader=[root]()->std::unique_ptr<app::ITrustedDisplayAssetReader>{return LinuxTrustedDisplayAssetReader::open(root/"display");};ports.generated_reader=[root,bindings](const app::DoorBulletinSource& source)->app::DoorBulletinFileCapture{try{return read_door_bulletin_source(root,source,bindings);}catch(const std::exception& e){return {{app::TrustedDisplayAssetReadCode::unavailable,{},{},{},{}},e.what()};}};ports.diagnostic=std::move(diagnostic);return ports;
}
}
