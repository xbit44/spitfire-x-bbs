#pragma once
#include "sfx/application/door_bulletin_session.hpp"
#include <filesystem>
namespace sfx::platform::win32 {
application::TrustedDisplayAssetReadResult read_door_bulletin_source(const std::filesystem::path& installation,
    const std::wstring& owner,const application::DoorBulletinSource&,
    const std::function<void()>& native_test_before_read={});
application::DoorBulletinSourcePorts door_bulletin_sources(const std::filesystem::path&,const std::wstring& owner,
    std::function<void(application::DoorBulletinId,std::string_view)> diagnostic={});
}
