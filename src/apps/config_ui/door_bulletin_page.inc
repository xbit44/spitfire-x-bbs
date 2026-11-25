std::optional<std::vector<std::uint8_t>> load_door_bulletin_bytes(State& s,app::LocalDoorBulletinQuery query){
    std::vector<std::uint8_t> bytes;std::uint32_t total{};
    for(unsigned part=0;part<10;++part){app::LocalControlFrame response;const auto payload=app::encode_door_bulletin_query(query);if(payload.empty()||!online_exchange(s,request(s,app::LocalControlMessageType::door_bulletin_query_request,payload),app::LocalControlMessageType::door_bulletin_query_response,response))return {};
        const auto page=app::decode_door_configuration_snapshot(response.payload);if(!page||page->code!=app::LocalAccountResultCode::ok||page->total>512U*1024U||page->offset!=bytes.size()||(query.revision&&query.revision!=page->revision)||(total&&total!=page->total))return {};
        if(!total){total=page->total;bytes.reserve(total);}bytes.insert(bytes.end(),page->bytes.begin(),page->bytes.end());if(bytes.size()==total)return bytes;query.revision=page->revision;query.offset=static_cast<std::uint32_t>(bytes.size());
    }return {};
}
std::optional<app::DoorBulletinCatalog> load_door_bulletin_catalog(State& s){
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline){const auto result=win32::offline_door_bulletin_configuration(s.root,s.owner);return result&&result.value?result.value:std::nullopt;}
    if(s.mode!=State::ConnectionMode::online)return {};const auto bytes=load_door_bulletin_bytes(s,{});return bytes?app::decode_door_bulletin_catalog(*bytes):std::nullopt;
}
app::LocalDoorResult send_door_bulletin_change(State& s,const app::DoorBulletinChange& change){
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline)return win32::offline_change_door_bulletin(s.root,s.owner,change);
    if(s.mode!=State::ConnectionMode::online)return {};const auto payload=app::encode_door_bulletin_change(change);if(payload.empty())return {app::LocalAccountResultCode::invalid_request};app::LocalControlFrame response;
    if(!online_exchange(s,request(s,app::LocalControlMessageType::door_bulletin_change_request,payload),app::LocalControlMessageType::door_bulletin_change_response,response))return {};
    return app::decode_door_configuration_result(response.payload).value_or(app::LocalDoorResult{});
}
std::optional<app::DoorBulletinPreview> load_door_bulletin_preview(State& s,app::DoorBulletinId id,bool ansi,std::int64_t revision){
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline)return win32::offline_preview_door_bulletin(s.root,s.owner,id,ansi,revision);
    if(s.mode!=State::ConnectionMode::online)return {};app::LocalDoorBulletinQuery query;query.kind=app::LocalDoorBulletinQueryKind::preview;query.id=id;query.ansi=ansi;query.revision=revision;const auto bytes=load_door_bulletin_bytes(s,query);return bytes?app::decode_door_bulletin_preview(*bytes):std::nullopt;
}
void show_door_bulletins(State& s){
    if(!s.door_bulletins){sfx::config_ui::DoorBulletinWindowPorts ports;ports.load=[&s]{return load_door_bulletin_catalog(s);};ports.save=[&s](const auto& change){return send_door_bulletin_change(s,change);};ports.preview=[&s](auto id,bool ansi,std::int64_t revision){return load_door_bulletin_preview(s,id,ansi,revision);};
        ports.doors=[&s]{std::vector<std::pair<app::DoorId,std::string>> choices;if(const auto catalog=load_door_catalog(s))for(const auto& door:catalog->doors)if(door.kind==app::DoorExecutionKind::local)choices.emplace_back(door.id,door.name);return choices;};ports.operation=[]{win32::WindowsSecureIdentifierGenerator generator;return generator.generate();};
        s.door_bulletins=std::make_unique<sfx::config_ui::DoorBulletinWindow>(GetModuleHandleW(nullptr),s.window,s.appearance,std::move(ports));}
    if(!s.door_bulletins->show())set_status(s,L"Door Bulletins could not open. Check the BBS connection and try again.");
}
