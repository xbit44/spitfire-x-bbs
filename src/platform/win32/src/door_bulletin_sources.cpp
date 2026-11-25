#include "sfx/platform/win32/door_bulletin_sources.hpp"
#include "sfx/platform/win32/file_payload_store.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/trusted_display_asset_reader.hpp"
#include <algorithm>
namespace sfx::platform::win32 {
namespace app=application;
app::TrustedDisplayAssetReadResult read_door_bulletin_source(const std::filesystem::path& root,
    const std::wstring& owner,const app::DoorBulletinSource& source,const std::function<void()>& native_test_before_read){
    installation::require(source.door_available&&source.definition.source==app::DoorBulletinSourceKind::door_file&&app::valid_door_bulletin_definition(source.definition),"door_bulletin_source_definition_unavailable");
    const auto work=source.work_directory.empty()?root:resolve_portable_owned_path(root,std::filesystem::u8path(source.work_directory));
    const auto target=work/std::filesystem::u8path(source.definition.relative_path);
    // Reuse current owner/ACL, all-ancestor pins, no reparse/hardlinks and
    // deny-write/delete lease. No create, repair, reserve or mutation path.
    auto directory=FilePayloadDirectory::private_store(target.parent_path(),owner);
    auto lease=directory.read(target.filename().u8string(),false);
    if(native_test_before_read)native_test_before_read();
    const auto size=lease.byte_count();installation::require(size<=app::kMaximumTrustedDisplaySourceBytes,"door_bulletin_source_oversized");
    std::string bytes;bytes.reserve(static_cast<std::size_t>(size));
    for(std::uint64_t offset=0;offset<size;){const auto piece=lease.read(offset,static_cast<std::size_t>(std::min<std::uint64_t>(65536,size-offset)));installation::require(!piece.empty(),"door_bulletin_source_short_read");bytes.append(piece.begin(),piece.end());offset+=piece.size();}
    installation::require(lease.byte_count()==size,"door_bulletin_source_changed_during_read");const auto identity=lease.native_object_identity();
    return {app::TrustedDisplayAssetReadCode::available,target.filename().u8string(),identity.volume+":"+identity.file_id,std::move(bytes),{}};
}
app::DoorBulletinSourcePorts door_bulletin_sources(const std::filesystem::path& root,const std::wstring& owner,
    std::function<void(app::DoorBulletinId,std::string_view)> diagnostic){
    app::DoorBulletinSourcePorts ports;ports.display_reader=[root]()->std::unique_ptr<app::ITrustedDisplayAssetReader>{return Win32TrustedDisplayAssetReader::open((root/L"Display").wstring());};
    ports.generated_reader=[root,owner](const app::DoorBulletinSource& source)->app::DoorBulletinFileCapture{try{return read_door_bulletin_source(root,owner,source);}catch(const std::exception& e){return {{app::TrustedDisplayAssetReadCode::unavailable,{},{},{},{}},e.what()};}};ports.diagnostic=std::move(diagnostic);return ports;
}
}
