// Durable host-only data encoding. No owner IPC message decodes this type.
namespace ftn_forward_wire {
constexpr std::size_t maximum=2U*1024U*1024U;
bool job(Writer& w,const FtnOutboundJob& j){
    if(!j.id.valid()||!j.operation.valid()||j.claim.valid()||j.generation.valid()||j.state!=FtnOutboundState::queued||j.attempts||!j.reason.empty()||!j.echo_tag.empty()||j.flavor!=FtnMailFlavor::normal||j.file.size>kFtnTransferMaximumBytes||j.file.timestamp>UINT32_MAX||!valid_ftn_code(j.network)||!valid_ftn_code(j.link)||!valid_file_name(j.file.name))return false;
    ftn_held_wire::id(w,j.id);ftn_held_wire::id(w,j.operation);w.u16(j.ordinal);w.u8(static_cast<std::uint8_t>(j.kind));w.u64(j.file.size);w.u64(j.file.timestamp);w.raw(j.digest.data(),32);return w.text(j.network,48)&&w.text(j.link,48)&&w.text(j.file.name,128);
}
bool job(Reader& r,FtnOutboundJob& j){std::uint8_t kind{};if(!ftn_held_wire::id(r,j.id)||!ftn_held_wire::id(r,j.operation)||!r.u16(j.ordinal)||!r.u8(kind)||!r.u64(j.file.size)||!r.u64(j.file.timestamp)||!r.raw(j.digest.data(),32)||!r.text(j.network,48)||!r.text(j.link,48)||!r.text(j.file.name,128))return false;j.kind=static_cast<FtnOutboundKind>(kind);return true;}
}
std::string ftn_forward_peer_identity_material(const FtnLink& link){std::string out="SFX/FTNForwardPeer/v1";const auto add=[&](const std::string& value){out+='|';out+=std::to_string(value.size());out+=':';out+=value;};add(link.network);add(link.code);add(link.address);out+='|';out+=std::to_string(link.additional_addresses.size());for(const auto& value:link.additional_addresses)add(value);return out;}
std::vector<std::uint8_t> encode_ftn_file_forward_plan(const FtnFileForwardPlan& p){
    if(!p.publication.valid()||p.created<1||!valid_ftn_code(p.network)||!parse_ftn_address(p.local_address)||p.source_peer_identity==FtnDigest{}||p.targets.size()>2048||p.metadata_template.empty()||p.metadata_template.size()>65535)return {};
    const auto parsed=decode_ftn_tic(p.metadata_template);if(!parsed.tic||!parsed.tic->password.empty()||parsed.tic->to)return {};
    Writer w;w.u8(2);ftn_held_wire::id(w,p.publication);w.u64(p.created);w.raw(p.source_peer_identity.data(),32);if(!w.text(p.network,48)||!w.text(p.local_address,128)||!w.text(p.metadata_template,65535))return {};w.u16(static_cast<std::uint16_t>(p.targets.size()));std::set<std::string> links;std::set<std::array<std::uint8_t,16>> ids;
    for(std::size_t i=0;i<p.targets.size();++i){const auto& t=p.targets[i];auto key=t.link;for(auto& ch:key)if(ch>='A'&&ch<='Z')ch=static_cast<char>(ch+32);
        if(!valid_ftn_code(t.link)||!parse_ftn_address(t.address)||!links.insert(key).second||!ids.insert(t.payload.id.bytes).second||!ids.insert(t.tic.id.bytes).second||t.payload.operation.bytes!=p.publication.bytes||t.tic.operation.bytes!=p.publication.bytes||t.payload.network!=p.network||t.tic.network!=p.network||t.payload.link!=t.link||t.tic.link!=t.link||t.payload.kind!=FtnOutboundKind::payload||t.tic.kind!=FtnOutboundKind::tic||t.payload.ordinal!=i*2||t.tic.ordinal!=i*2+1||t.payload.file.timestamp!=static_cast<std::uint64_t>(std::min<std::int64_t>(UINT32_MAX,p.created))||t.tic.file.timestamp!=t.payload.file.timestamp||!t.tic.file.size||t.tic.file.size>65536)return {};
        if(!w.text(t.link,48)||!w.text(t.address,128))return {};
        w.raw(t.credential_generation.data(),32);w.raw(t.peer_identity.data(),32);if(!ftn_forward_wire::job(w,t.payload)||!ftn_forward_wire::job(w,t.tic))return {};
    }auto bytes=w.take();return bytes.size()<=ftn_forward_wire::maximum?bytes:std::vector<std::uint8_t>{};
}
std::optional<FtnFileForwardPlan> decode_ftn_file_forward_plan(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>ftn_forward_wire::maximum)return {};
    Reader r(bytes);std::uint8_t version{};std::uint16_t count{};FtnFileForwardPlan p;
    if(!r.u8(version)||version!=2||!ftn_held_wire::id(r,p.publication)||!nonnegative64(r,p.created)||!r.raw(p.source_peer_identity.data(),32)||!r.text(p.network,48)||!r.text(p.local_address,128)||!r.text(p.metadata_template,65535)||!r.u16(count)||count>2048)return {};
    for(unsigned i=0;i<count;++i){FtnFileForwardTarget t;if(!r.text(t.link,48)||!r.text(t.address,128)||!r.raw(t.credential_generation.data(),32)||!r.raw(t.peer_identity.data(),32)||!ftn_forward_wire::job(r,t.payload)||!ftn_forward_wire::job(r,t.tic))return {};p.targets.push_back(std::move(t));}if(!r.complete()||encode_ftn_file_forward_plan(p).empty())return {};return p;
}
