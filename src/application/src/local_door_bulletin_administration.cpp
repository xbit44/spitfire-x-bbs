#include "sfx/application/local_door_bulletin_administration.hpp"
#include <algorithm>
namespace sfx::application {
namespace {
LocalAccountResultCode code(StorageErrorCode c){if(c==StorageErrorCode::conflict)return LocalAccountResultCode::conflict;if(c==StorageErrorCode::invalid_argument)return LocalAccountResultCode::invalid_request;if(c==StorageErrorCode::permission_denied||c==StorageErrorCode::not_found||c==StorageErrorCode::constraint)return LocalAccountResultCode::policy_rejected;return LocalAccountResultCode::storage_failure;}
}
void LocalDoorBulletinSnapshotCache::clear()noexcept {revision_=0;query_={};std::vector<std::uint8_t>().swap(bytes_);}
LocalDoorSnapshot LocalDoorBulletinSnapshotCache::query(IDoorStorage& storage,LocalAccountAuthority authority,const LocalDoorBulletinQuery& request,const LocalDoorBulletinPreview& preview){
    if(authority!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    if(static_cast<unsigned>(request.kind)>2||request.revision<0||request.offset>512U*1024U||(request.kind==LocalDoorBulletinQueryKind::catalog?request.id.value!=0:!valid_door_bulletin_id(request.id))||(request.offset&&!request.revision))return {LocalAccountResultCode::invalid_request};
    if(!request.offset){
        clear();const auto catalog=storage.door_bulletin_configuration({true,{}});if(!catalog||!catalog.value)return {code(catalog.status.code)};
        if(request.revision&&request.revision!=catalog.value->revision)return {LocalAccountResultCode::conflict};
        if(request.kind==LocalDoorBulletinQueryKind::catalog)bytes_=encode_door_bulletin_catalog(*catalog.value);
        else{if(!preview)return {LocalAccountResultCode::unavailable};auto result=preview(request.id,request.ansi,catalog.value->revision);if(request.kind==LocalDoorBulletinQueryKind::validate){result.cells.clear();result.rows=0;}bytes_=encode_door_bulletin_preview(result);}
        if(bytes_.empty()||bytes_.size()>512U*1024U){clear();return {LocalAccountResultCode::storage_failure};}query_=request;revision_=catalog.value->revision;
    }
    if(request.kind!=query_.kind||!(request.id==query_.id)||request.ansi!=query_.ansi||(request.revision&&request.revision!=revision_)||request.offset>=bytes_.size())return {LocalAccountResultCode::conflict};
    LocalDoorSnapshot out;out.code=LocalAccountResultCode::ok;out.revision=revision_;out.offset=request.offset;out.total=static_cast<std::uint32_t>(bytes_.size());const auto n=std::min(kLocalDoorChunkBytes,bytes_.size()-request.offset);out.bytes.assign(bytes_.begin()+request.offset,bytes_.begin()+request.offset+n);if(request.offset+n==bytes_.size())clear();return out;
}
LocalDoorResult administer_door_bulletin(IDoorStorage& storage,LocalAccountAuthority authority,const DoorBulletinChange& r){
    if(authority!=LocalAccountAuthority::installation_owner)return {LocalAccountResultCode::unauthorized};
    if(!valid_door_bulletin_change(r))return {LocalAccountResultCode::invalid_request};
    const auto result=storage.change_door_bulletin({true,{}},r);return result&&result.value?LocalDoorResult{LocalAccountResultCode::ok,*result.value}:LocalDoorResult{code(result.status.code)};
}
}
