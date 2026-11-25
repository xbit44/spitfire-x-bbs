#pragma once
#include "sfx/application/door_bulletin_session.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
namespace sfx::platform::linux_host {
application::TrustedDisplayAssetReadResult read_door_bulletin_source(const std::filesystem::path& installation,
    const application::DoorBulletinSource&,std::shared_ptr<IFilePayloadBindings> bindings={},
    const std::function<void()>& native_test_after_capture={});
application::DoorBulletinSourcePorts door_bulletin_sources(const std::filesystem::path&,std::shared_ptr<IFilePayloadBindings>,
    std::function<void(application::DoorBulletinId,std::string_view)> diagnostic={});
}
