    case T::door_bulletin_query_request:{
        const auto q=app::decode_door_bulletin_query(request.payload);if(!q)return {};
        const auto catalog=s.door_bulletin_configuration();if(!catalog||!catalog.value||(q->revision&&q->revision!=catalog.value->revision))return {};
        std::vector<std::uint8_t> bytes;if(q->kind==app::LocalDoorBulletinQueryKind::catalog)bytes=app::encode_door_bulletin_catalog(*catalog.value);
        else{if(q->offset)return {};auto preview=s.preview_door_bulletin(q->id,q->ansi,catalog.value->revision);if(q->kind==app::LocalDoorBulletinQueryKind::validate){preview.cells.clear();preview.rows=0;}bytes=app::encode_door_bulletin_preview(preview);}
        if(bytes.empty()||q->offset>=bytes.size())return {};app::LocalDoorSnapshot part;part.code=app::LocalAccountResultCode::ok;part.revision=catalog.value->revision;part.offset=q->offset;part.total=static_cast<std::uint32_t>(bytes.size());const auto end=std::min(bytes.size(),q->offset+app::kLocalDoorChunkBytes);part.bytes={bytes.begin()+q->offset,bytes.begin()+end};response.type=T::door_bulletin_query_response;response.payload=app::encode_door_configuration_snapshot(part);break;
    }
    case T::door_bulletin_change_request:{const auto change=app::decode_door_bulletin_change(request.payload);if(!change)return {};response.type=T::door_bulletin_change_response;response.payload=app::encode_door_configuration_result(s.change_door_bulletin(*change));break;}
