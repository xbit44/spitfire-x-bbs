// Bounded owner-only observation: no credentials, message contents, native paths,
// custody proofs, or private caller data cross this protocol.
namespace {
bool ftn_runtime_text(std::string_view text,std::size_t maximum){
    if(text.size()>maximum)return false;
    for(std::size_t i=0;i<text.size();){const auto first=static_cast<unsigned char>(text[i++]);if(first<128){if(first<32||first==127)return false;continue;}
        unsigned count{},value{},minimum{};if(first>=0xc2&&first<=0xdf){count=1;value=first&31;minimum=0x80;}else if(first>=0xe0&&first<=0xef){count=2;value=first&15;minimum=0x800;}else if(first>=0xf0&&first<=0xf4){count=3;value=first&7;minimum=0x10000;}else return false;
        if(count>text.size()-i)return false;
        for(unsigned n=0;n<count;++n){const auto c=static_cast<unsigned char>(text[i++]);if((c&0xc0)!=0x80)return false;value=(value<<6)|(c&63);}if(value<minimum||value>0x10ffff||(value>=0xd800&&value<=0xdfff)||(value>=0x80&&value<=0x9f)||value==0x2028||value==0x2029)return false;
    }return true;
}
bool ftn_runtime_request_valid(const FtnRuntimeRequest& r){
    if(static_cast<unsigned>(r.action)>4||r.offset>1000000)return false;
    if(r.action==FtnRuntimeAction::poll)return valid_ftn_code(r.network)&&valid_ftn_code(r.link)&&!r.offset;
    if(!r.network.empty()||!r.link.empty()||r.include_hold)return false;
    return r.action==FtnRuntimeAction::queues||r.action==FtnRuntimeAction::connections||!r.offset;
}
}
std::vector<std::uint8_t> encode_ftn_runtime_request(const FtnRuntimeRequest& r){
    if(!ftn_runtime_request_valid(r))return {};
    Writer w;w.u8(1);w.u8(static_cast<std::uint8_t>(r.action));w.u32(r.offset);w.u8(r.include_hold);w.text(r.network,48);w.text(r.link,48);return w.take();
}
std::optional<FtnRuntimeRequest> decode_ftn_runtime_request(const std::vector<std::uint8_t>& bytes){
    Reader r(bytes);FtnRuntimeRequest value;std::uint8_t version{},action{},hold{};
    if(bytes.size()>128||!r.u8(version)||version!=1||!r.u8(action)||!r.u32(value.offset)||!r.u8(hold)||hold>1||!r.text(value.network,48)||!r.text(value.link,48)||!r.complete())return {};
    value.action=static_cast<FtnRuntimeAction>(action);value.include_hold=hold!=0;return ftn_runtime_request_valid(value)?std::optional<FtnRuntimeRequest>{value}:std::nullopt;
}
std::vector<std::uint8_t> encode_ftn_runtime_response(const FtnRuntimeResponse& v){
    if(static_cast<unsigned>(v.code)>4||static_cast<unsigned>(v.mode)>2||v.saved_revision<0||v.active_revision<0||v.sessions.size()>16||v.connections.size()>64||v.activity.size()>32||(v.queues&&v.queues->rows.size()>64))return {};
    Writer w;const auto text=[&](std::string_view s,std::size_t max){return ftn_runtime_text(s,max)&&w.text(s,max);};
    const bool completions=std::any_of(v.sessions.begin(),v.sessions.end(),[](const auto& s){return !s.last_sent.name.empty()||!s.last_received.name.empty();});
    w.u8(completions?4:3);w.u8(static_cast<std::uint8_t>(v.code));w.u8(static_cast<std::uint8_t>(v.mode));w.u64(v.saved_revision);w.u64(v.active_revision);
    for(const auto flag:{v.capable,v.paused,v.serial_recovery_required,v.classic_listening,v.tls_listening,v.processing,v.more_connections})w.u8(flag);
    w.u16(v.classic_port);w.u16(v.tls_port);for(const auto* s:{&v.diagnostic,&v.classic_error,&v.tls_error,&v.processing_error})if(!text(*s,96))return {};
    for(const auto count:{v.processed_receipts,v.held_receipts,v.imported_messages,v.replayed_messages,v.scanned_messages})w.u64(count);
    w.u8(static_cast<std::uint8_t>(v.sessions.size()));for(const auto& s:v.sessions){if(!s.serial)return {};w.u64(s.serial);for(const auto flag:{s.active,s.outbound,s.completed,s.peer_verified})w.u8(flag);if(!text(s.network,96)||!text(s.link,96)||!text(s.reason,96))return {};w.u64(s.received_bytes);w.u64(s.sent_bytes);w.u32(s.received_files);w.u32(s.sent_files);for(const auto* p:{&s.send,&s.receive}){if(p->size>1073741824ULL||p->position>p->size||!text(p->name,128))return {};w.u64(p->size);w.u64(p->position);w.u8(p->active);w.u8(p->awaiting_ack);}if(!text(s.peer_address,96)||!text(s.endpoint,260))return {};}
    w.u8(static_cast<std::uint8_t>(v.connections.size()));for(const auto& c:v.connections){if(!valid_ftn_code(c.network)||!valid_ftn_code(c.link)||!text(c.network,48)||!text(c.link,48)||!text(c.name,195))return {};w.u8(c.enabled);w.u8(c.outbound);}
    w.u8(v.queues.has_value());if(v.queues){const auto& q=*v.queues;for(const auto n:q.ingress)w.u64(n);for(const auto n:q.outbound)w.u64(n);w.u64(q.unscanned);w.u8(q.more);w.u8(static_cast<std::uint8_t>(q.rows.size()));for(const auto& row:q.rows){if(!row.id.valid()||row.state>(row.outbound?5:4))return {};w.raw(row.id.bytes.data(),16);w.u8(row.outbound);w.u8(row.state);w.u64(row.bytes);if(!text(row.network,96)||!text(row.link,96)||!text(row.file,128)||!text(row.reason,64))return {};}}
    w.u8(static_cast<std::uint8_t>(v.activity.size()));for(const auto& a:v.activity){if(!a.utc_seconds||a.utc_seconds>253402300799ULL||!text(a.network,96)||!text(a.link,96)||!text(a.message,192))return {};w.u64(a.utc_seconds);w.u64(a.session);}
    if(completions)for(const auto& s:v.sessions)for(const auto* p:{&s.last_sent,&s.last_received}){
        if(p->size>1073741824ULL||p->position!=p->size||p->active||p->awaiting_ack||(p->name.empty()&&p->size)||!text(p->name,128))return {};
        w.u64(p->size);
    }
    auto bytes=w.take();return bytes.size()<=kLocalControlMaximumPayloadBytes?bytes:std::vector<std::uint8_t>{};
}
std::optional<FtnRuntimeResponse> decode_ftn_runtime_response(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>kLocalControlMaximumPayloadBytes)return {};
    Reader r(bytes);FtnRuntimeResponse v;std::uint8_t version{},code{},mode{},count{};
    const auto flag=[&](bool& to){std::uint8_t n{};if(!r.u8(n)||n>1)return false;to=n!=0;return true;};
    if(!r.u8(version)||(version!=3&&version!=4)||!r.u8(code)||!r.u8(mode)||!nonnegative64(r,v.saved_revision)||!nonnegative64(r,v.active_revision))return {};
    v.code=static_cast<FtnConfigurationCode>(code);v.mode=static_cast<FtnMailerMode>(mode);
    for(auto* b:{&v.capable,&v.paused,&v.serial_recovery_required,&v.classic_listening,&v.tls_listening,&v.processing,&v.more_connections})if(!flag(*b))return {};
    if(!r.u16(v.classic_port)||!r.u16(v.tls_port))return {};
    for(auto* s:{&v.diagnostic,&v.classic_error,&v.tls_error,&v.processing_error})if(!r.text(*s,96))return {};
    for(auto* n:{&v.processed_receipts,&v.held_receipts,&v.imported_messages,&v.replayed_messages,&v.scanned_messages})if(!r.u64(*n))return {};
    if(!r.u8(count)||count>16)return {};
    for(unsigned i=0;i<count;++i){FtnSessionStatus s;if(!r.u64(s.serial)||!flag(s.active)||!flag(s.outbound)||!flag(s.completed)||!flag(s.peer_verified)||!r.text(s.network,96)||!r.text(s.link,96)||!r.text(s.reason,96)||!r.u64(s.received_bytes)||!r.u64(s.sent_bytes)||!r.u32(s.received_files)||!r.u32(s.sent_files))return {};for(auto* p:{&s.send,&s.receive})if(!r.text(p->name,128)||!r.u64(p->size)||!r.u64(p->position)||!flag(p->active)||!flag(p->awaiting_ack))return {};if(!r.text(s.peer_address,96)||!r.text(s.endpoint,260))return {};v.sessions.push_back(std::move(s));}
    if(!r.u8(count)||count>64)return {};
    for(unsigned i=0;i<count;++i){FtnConnectionStatus c;if(!r.text(c.network,48)||!r.text(c.link,48)||!r.text(c.name,195)||!flag(c.enabled)||!flag(c.outbound))return {};v.connections.push_back(std::move(c));}
    bool queued{};if(!flag(queued))return {};if(queued){v.queues.emplace();auto& q=*v.queues;for(auto& n:q.ingress)if(!r.u64(n))return {};for(auto& n:q.outbound)if(!r.u64(n))return {};if(!r.u64(q.unscanned)||!flag(q.more)||!r.u8(count)||count>64)return {};for(unsigned i=0;i<count;++i){FtnQueueRow row;if(!r.raw(row.id.bytes.data(),16)||!flag(row.outbound)||!r.u8(row.state)||!r.u64(row.bytes)||!r.text(row.network,96)||!r.text(row.link,96)||!r.text(row.file,128)||!r.text(row.reason,64))return {};q.rows.push_back(std::move(row));}}
    if(!r.u8(count)||count>32)return {};
    for(unsigned i=0;i<count;++i){FtnActivity a;if(!r.text(a.network,96)||!r.text(a.link,96)||!r.text(a.message,192)||!r.u64(a.utc_seconds)||!r.u64(a.session))return {};v.activity.push_back(std::move(a));}
    if(version==4)for(auto& s:v.sessions)for(auto* p:{&s.last_sent,&s.last_received}){
        if(!r.text(p->name,128)||!r.u64(p->size))return {};
        p->position=p->size;
    }
    if(!r.complete()||encode_ftn_runtime_response(v).empty())return {};
    return v;
}
