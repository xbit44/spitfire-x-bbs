std::vector<std::uint8_t> encode_door_configuration_query(const LocalDoorQuery& q){
    if(q.revision<0 || q.offset>kLocalDoorCatalogMaximumBytes || (q.offset==0)!=(q.revision==0))return {};
    Writer w;w.u64(static_cast<std::uint64_t>(q.revision));w.u32(q.offset);return w.take();
}
std::optional<LocalDoorQuery> decode_door_configuration_query(const std::vector<std::uint8_t>& b){
    Reader r(b);LocalDoorQuery q;if(!nonnegative64(r,q.revision)||!r.u32(q.offset)||!r.complete()||encode_door_configuration_query(q).empty())return {};return q;
}
std::vector<std::uint8_t> encode_door_configuration_snapshot(const LocalDoorSnapshot& s){
    if(!valid_local_door_snapshot(s))return {};
    Writer w;w.u8(static_cast<std::uint8_t>(s.code));w.u64(static_cast<std::uint64_t>(s.revision));w.u32(s.offset);w.u32(s.total);w.u32(static_cast<std::uint32_t>(s.bytes.size()));
    if(!s.bytes.empty())w.raw(s.bytes.data(),s.bytes.size());
    return w.take();
}
std::optional<LocalDoorSnapshot> decode_door_configuration_snapshot(const std::vector<std::uint8_t>& b){
    if(b.size()>kLocalControlMaximumPayloadBytes)return {};
    Reader r(b);LocalDoorSnapshot s;std::uint8_t code{};std::uint32_t count{};
    if(!r.u8(code)||!nonnegative64(r,s.revision)||!r.u32(s.offset)||!r.u32(s.total)||!r.u32(count)||count>kLocalDoorChunkBytes)return {};
    s.code=static_cast<LocalAccountResultCode>(code);s.bytes.resize(count);if((count&&!r.raw(s.bytes.data(),count))||!r.complete()||!valid_local_door_snapshot(s))return {};return s;
}
std::vector<std::uint8_t> encode_door_configuration_change(const DoorConfigurationChange& c){
    if(!valid_door_change(c))return {};
    Writer w;w.raw(c.operation.bytes.data(),c.operation.bytes.size());w.u64(static_cast<std::uint64_t>(c.expected_revision));w.u8(static_cast<std::uint8_t>(c.action));w.u8(c.previous_number);w.u16(c.source_menu);w.u8(c.create_only?1:0);
    switch(c.action){
    case DoorConfigurationAction::save_door:{auto b=encode_door_definition(c.door);if(b.empty()||b.size()>32768)return {};w.u16(static_cast<std::uint16_t>(b.size()));w.raw(b.data(),b.size());break;}
    case DoorConfigurationAction::save_menu:w.u16(c.menu.id);if(!w.text(c.menu.name,64))return {};for(const auto& p:{c.menu.access,c.menu.execution}){w.u8(static_cast<std::uint8_t>(p.mode));w.u16(p.level);w.u8(p.sysop_only?1:0);}break;
    case DoorConfigurationAction::remove_door:w.u16(c.door.id);break;
    case DoorConfigurationAction::remove_menu:w.u16(c.menu.id);break;
    case DoorConfigurationAction::save_entry:w.u16(c.entry.menu);w.u8(c.entry.number);w.u8(static_cast<std::uint8_t>(c.entry.kind));w.u16(c.entry.target);if(!w.text(c.entry.title,64)||!w.text(c.entry.description,256))return {};break;
    case DoorConfigurationAction::remove_entry:w.u16(c.entry.menu);w.u8(c.entry.number);break;
    case DoorConfigurationAction::duplicate_menu:w.u16(c.menu.id);if(!w.text(c.menu.name,64))return {};break;
    }
    w.u8(c.move_entry?1:0);w.u8(static_cast<std::uint8_t>(c.order.size()));for(const auto n:c.order)w.u8(n);
    return w.take();
}
std::optional<DoorConfigurationChange> decode_door_configuration_change(const std::vector<std::uint8_t>& b){
    if(b.size()>kLocalControlMaximumPayloadBytes)return {};
    Reader r(b);DoorConfigurationChange c;std::uint8_t action{},create{};
    if(!r.raw(c.operation.bytes.data(),c.operation.bytes.size())||!nonnegative64(r,c.expected_revision)||!r.u8(action)||action>6||!r.u8(c.previous_number)||!r.u16(c.source_menu)||!r.u8(create)||create>1)return {};
    c.action=static_cast<DoorConfigurationAction>(action);c.create_only=create!=0;
    switch(c.action){
    case DoorConfigurationAction::save_door:{std::uint16_t count{};if(!r.u16(count)||!count||count>32768)return {};std::vector<std::uint8_t> record(count);if(!r.raw(record.data(),count))return {};auto d=decode_door_definition(record);if(!d)return {};c.door=std::move(*d);break;}
    case DoorConfigurationAction::save_menu:if(!r.u16(c.menu.id)||!r.text(c.menu.name,64))return {};for(auto* p:{&c.menu.access,&c.menu.execution}){std::uint8_t mode{},sysop{};if(!r.u8(mode)||!r.u16(p->level)||!r.u8(sysop)||sysop>1)return {};p->mode=static_cast<DoorSecurityMode>(mode);p->sysop_only=sysop!=0;}break;
    case DoorConfigurationAction::remove_door:if(!r.u16(c.door.id))return {};break;
    case DoorConfigurationAction::remove_menu:if(!r.u16(c.menu.id))return {};break;
    case DoorConfigurationAction::save_entry:{std::uint8_t kind{};if(!r.u16(c.entry.menu)||!r.u8(c.entry.number)||!r.u8(kind)||kind>1||!r.u16(c.entry.target)||!r.text(c.entry.title,64)||!r.text(c.entry.description,256))return {};c.entry.kind=static_cast<DoorEntryKind>(kind);break;}
    case DoorConfigurationAction::remove_entry:if(!r.u16(c.entry.menu)||!r.u8(c.entry.number))return {};break;
    case DoorConfigurationAction::duplicate_menu:if(!r.u16(c.menu.id)||!r.text(c.menu.name,64))return {};break;
    }
    std::uint8_t move{},count{};if(!r.u8(move)||move>1||!r.u8(count)||count>99)return {};c.move_entry=move!=0;
    for(unsigned i=0;i<count;++i){std::uint8_t n{};if(!r.u8(n))return {};c.order.push_back(n);}
    if(!r.complete()||!valid_door_change(c))return {};
    return c;
}
std::vector<std::uint8_t> encode_door_configuration_result(const LocalDoorResult& s){
    if(static_cast<unsigned>(s.code)>static_cast<unsigned>(LocalAccountResultCode::system_sysop_protected)||
        (s.code==LocalAccountResultCode::ok?s.result.revision<2:s.result.revision!=0||s.result.replayed))return {};
    Writer w;w.u8(static_cast<std::uint8_t>(s.code));w.u64(static_cast<std::uint64_t>(s.result.revision));w.u8(s.result.replayed?1:0);return w.take();
}
std::optional<LocalDoorResult> decode_door_configuration_result(const std::vector<std::uint8_t>& b){
    Reader r(b);LocalDoorResult s;std::uint8_t code{},replayed{};if(!r.u8(code)||!nonnegative64(r,s.result.revision)||!r.u8(replayed)||replayed>1||!r.complete())return {};
    s.code=static_cast<LocalAccountResultCode>(code);s.result.replayed=replayed!=0;if(encode_door_configuration_result(s).empty())return {};return s;
}
