#include "sfx/application/local_door_administration.hpp"
#include <algorithm>

namespace sfx::application {
namespace {
LocalAccountResultCode door_admin_code(StorageErrorCode c){
    if(c==StorageErrorCode::conflict)return LocalAccountResultCode::conflict;
    if(c==StorageErrorCode::invalid_argument)return LocalAccountResultCode::invalid_request;
    if(c==StorageErrorCode::permission_denied || c==StorageErrorCode::not_found || c==StorageErrorCode::constraint)return LocalAccountResultCode::policy_rejected;
    return LocalAccountResultCode::storage_failure;
}
}
void LocalDoorSnapshotCache::clear() noexcept {revision_=0;std::vector<std::uint8_t>().swap(bytes_);}
LocalDoorSnapshot LocalDoorSnapshotCache::query(IDoorStorage& storage,LocalAccountAuthority authority,const LocalDoorQuery& q){
    if(authority!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    if(q.revision<0 || q.offset>kLocalDoorCatalogMaximumBytes || (!q.offset && q.revision) || (q.offset && !q.revision))return {LocalAccountResultCode::invalid_request};
    if(!q.offset){
        clear();const auto result=storage.door_configuration({true,{}});if(!result || !result.value)return {door_admin_code(result.status.code)};
        auto bytes=encode_door_catalog(*result.value);if(bytes.empty() || bytes.size()>kLocalDoorCatalogMaximumBytes)return {LocalAccountResultCode::storage_failure};
        revision_=result.value->revision;bytes_=std::move(bytes);
    }
    if((q.revision && q.revision!=revision_) || q.offset>=bytes_.size())return {LocalAccountResultCode::conflict};
    LocalDoorSnapshot s;s.code=LocalAccountResultCode::ok;s.revision=revision_;s.offset=q.offset;s.total=static_cast<std::uint32_t>(bytes_.size());
    const auto count=std::min(kLocalDoorChunkBytes,bytes_.size()-q.offset);
    s.bytes.assign(bytes_.begin()+q.offset,bytes_.begin()+q.offset+static_cast<std::ptrdiff_t>(count));
    if(q.offset+count==bytes_.size())clear();
    return s;
}
LocalDoorResult administer_door(IDoorStorage& storage,LocalAccountAuthority authority,const DoorConfigurationChange& c){
    if(authority!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    if(!valid_door_change(c))return {LocalAccountResultCode::invalid_request};
    const auto r=storage.change_door_configuration({true,{}},c);
    return r && r.value?LocalDoorResult{LocalAccountResultCode::ok,*r.value}:LocalDoorResult{door_admin_code(r.status.code)};
}
bool valid_local_door_snapshot(const LocalDoorSnapshot& s) noexcept {
    if(static_cast<unsigned>(s.code)>static_cast<unsigned>(LocalAccountResultCode::system_sysop_protected))return false;
    if(s.code!=LocalAccountResultCode::ok)return !s.revision&&!s.offset&&!s.total&&s.bytes.empty();
    return s.revision>=1 && s.total>=1 && s.total<=kLocalDoorCatalogMaximumBytes && s.offset<s.total &&
        !s.bytes.empty() && s.bytes.size()<=kLocalDoorChunkBytes && s.bytes.size()<=s.total-s.offset;
}
} // namespace sfx::application
