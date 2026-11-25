#pragma once
#include "sfx/application/door_storage.hpp"
#include "sfx/application/menu_session.hpp"
#include "sfx/application/door_bulletin_preview.hpp"
#include <memory>

namespace sfx::application {
struct DoorBulletinFileCapture {
    TrustedDisplayAssetReadResult asset;
    std::string diagnostic;
    DoorBulletinFileCapture(TrustedDisplayAssetReadResult a={},std::string d={}):asset(std::move(a)),diagnostic(std::move(d)){}
};
struct DoorBulletinSourcePorts {
    std::function<std::unique_ptr<ITrustedDisplayAssetReader>()> display_reader;
    std::function<DoorBulletinFileCapture(const DoorBulletinSource&)> generated_reader;
    std::function<void(DoorBulletinId,std::string_view)> diagnostic;
};
struct DoorBulletinRefreshResult {
    StorageStatus status;
    std::optional<RenderedTrustedDisplayProgram> display;
    DoorBulletinPublicationResult publication;
    std::string diagnostic;
};
DoorBulletinRefreshResult refresh_door_bulletin(IDoorStorage&,const MessageConfigurationActor&,
    DoorBulletinId,TrustedDisplaySelectionRequest,const DoorBulletinSourcePorts&,
    std::int64_t expected_definition_revision=0);
using DoorBulletinRefresh=std::function<DoorBulletinRefreshResult(DoorBulletinId,TrustedDisplaySelectionRequest,std::int64_t)>;
DoorBulletinPreview preview_door_bulletin(IDoorStorage&,DoorBulletinId,TrustedDisplaySelectionRequest,const DoorBulletinSourcePorts&,std::int64_t expected_catalog_revision=0);
std::string door_bulletin_diagnostic_code(std::string_view);
bool run_door_bulletin_session(IDoorStorage&,const MenuSessionPorts&,
    const std::function<std::optional<MessageActor>()>&,const DoorBulletinRefresh&,
    MenuInputDecoder* invoker_input=nullptr);
} // namespace sfx::application
