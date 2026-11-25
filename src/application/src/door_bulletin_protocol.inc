std::vector<std::uint8_t> encode_door_bulletin_query(const LocalDoorBulletinQuery& q){
    if(static_cast<unsigned>(q.kind)>2||q.revision<0||q.offset>512U*1024U||(q.offset&&!q.revision)||(q.kind==LocalDoorBulletinQueryKind::catalog?q.id.value!=0:!valid_door_bulletin_id(q.id)))return {};
    Writer w;w.u8(static_cast<std::uint8_t>(q.kind));w.u64(static_cast<std::uint64_t>(q.revision));w.u32(q.offset);w.u16(q.id.value);w.u8(q.ansi?1:0);return w.take();
}
std::optional<LocalDoorBulletinQuery> decode_door_bulletin_query(const std::vector<std::uint8_t>& bytes){
    Reader r(bytes);LocalDoorBulletinQuery q;std::uint8_t kind{},ansi{};if(!r.u8(kind)||kind>2||!nonnegative64(r,q.revision)||!r.u32(q.offset)||!r.u16(q.id.value)||!r.u8(ansi)||ansi>1||!r.complete())return {};q.kind=static_cast<LocalDoorBulletinQueryKind>(kind);q.ansi=ansi!=0;if(encode_door_bulletin_query(q).empty())return {};return q;
}
