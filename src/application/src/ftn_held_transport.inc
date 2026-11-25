namespace ftn_held_wire {
bool text(Writer& w,std::string_view value,std::size_t maximum){return ftn_runtime_text(value,maximum)&&w.text(value,maximum);}
void id(Writer& w,const Identifier128& v){w.raw(v.bytes.data(),v.bytes.size());}
bool id(Reader& r,Identifier128& v){return r.raw(v.bytes.data(),v.bytes.size());}
bool flag(Reader& r,bool& v){std::uint8_t value{};if(!r.u8(value)||value>1)return false;v=value!=0;return true;}
bool resolution(Writer& w,const FtnRecipientResolutionRequest& v){if(!v.operation.valid()||!v.held_message.valid()||v.operation.bytes==v.previous_resolution.bytes||v.configuration_revision<1||v.recipient<1||v.recipient_revision<1)return false;id(w,v.operation);id(w,v.held_message);id(w,v.previous_resolution);w.u64(v.configuration_revision);w.u64(v.recipient);w.u64(v.recipient_revision);return true;}
bool resolution(Reader& r,FtnRecipientResolutionRequest& v){return id(r,v.operation)&&id(r,v.held_message)&&id(r,v.previous_resolution)&&nonnegative64(r,v.configuration_revision)&&nonnegative64(r,v.recipient)&&nonnegative64(r,v.recipient_revision);}
bool retry(Writer& w,const FtnReprocessRequest& v){if(!v.operation.valid()||!v.ingress.valid()||v.operation.bytes==v.previous_request.bytes||v.configuration_revision<1)return false;id(w,v.operation);id(w,v.ingress);id(w,v.previous_request);w.u64(v.configuration_revision);return true;}
bool retry(Reader& r,FtnReprocessRequest& v){return id(r,v.operation)&&id(r,v.ingress)&&id(r,v.previous_request)&&nonnegative64(r,v.configuration_revision);}
bool retry(Writer& w,const FtnTicRetryRequest& v){if(v.review==FtnDigest{}||!retry(w,FtnReprocessRequest{v.operation,v.ingress,v.previous_request,v.configuration_revision}))return false;w.raw(v.review.data(),v.review.size());return true;}
bool retry(Reader& r,FtnTicRetryRequest& v){return id(r,v.operation)&&id(r,v.ingress)&&id(r,v.previous_request)&&nonnegative64(r,v.configuration_revision)&&r.raw(v.review.data(),v.review.size());}
bool resolution_record(Writer& w,const FtnRecipientResolution& v){if(v.created<0||!resolution(w,v.request))return false;w.u64(v.created);return true;}
bool resolution_record(Reader& r,FtnRecipientResolution& v){return resolution(r,v.request)&&nonnegative64(r,v.created);}
bool retry_record(Writer& w,const FtnReprocessRecord& v){if(v.created<0||(v.processed&&!v.finished)||!retry(w,v.request))return false;w.u64(v.created);w.u8(v.finished);w.u8(v.processed);return text(w,v.reason,64);}
bool retry_record(Reader& r,FtnReprocessRecord& v){return retry(r,v.request)&&nonnegative64(r,v.created)&&flag(r,v.finished)&&flag(r,v.processed)&&r.text(v.reason,64);}
bool retry_record(Writer& w,const FtnTicRetryRecord& v){if(v.created<0||(v.processed&&!v.finished)||!retry(w,v.request))return false;w.u64(v.created);w.u8(v.finished);w.u8(v.processed);return text(w,v.reason,64);}
bool retry_record(Reader& r,FtnTicRetryRecord& v){return retry(r,v.request)&&nonnegative64(r,v.created)&&flag(r,v.finished)&&flag(r,v.processed)&&r.text(v.reason,64);}
bool tic_detail(Writer& w,const FtnHeldTicDetail& v,const Identifier128& ingress){if(v.review==FtnDigest{}||v.phase>6||(!v.publication.valid()&&(v.phase||v.forward_plan_present)))return false;w.raw(v.review.data(),v.review.size());id(w,v.publication);w.u8(v.phase);w.u8(v.forward_plan_present);if(!text(w,v.claimed_from,128)||!text(w,v.echo,128)||!text(w,v.payload,128))return false;w.u8(v.retry.has_value());return !v.retry||(v.retry->request.ingress.bytes==ingress.bytes&&retry_record(w,*v.retry));}
bool tic_detail(Reader& r,FtnHeldTicDetail& v){bool retried{};if(!r.raw(v.review.data(),v.review.size())||!id(r,v.publication)||!r.u8(v.phase)||!flag(r,v.forward_plan_present)||!r.text(v.claimed_from,128)||!r.text(v.echo,128)||!r.text(v.payload,128)||!flag(r,retried))return false;if(retried){v.retry.emplace();return retry_record(r,*v.retry);}return true;}
}
std::vector<std::uint8_t> encode_ftn_held_request(const FtnHeldControlRequest& request){
    using namespace ftn_held_wire;Writer w;w.u8(2);w.u8(static_cast<std::uint8_t>(request.index()));
    switch(request.index()){
    case 0:{const auto& q=std::get<0>(request);if((!q.network.empty()&&!valid_ftn_code(q.network))||q.message_offset>4096||(q.ingress.valid()?(q.after.valid()||!q.network.empty()||!q.reason.empty()||!q.find.empty()):q.message_offset!=0))return {};id(w,q.ingress);id(w,q.after);w.u32(q.message_offset);if(!text(w,q.network,48)||!text(w,q.reason,64)||!text(w,q.find,140))return {};break;}
    case 1:{const auto& q=std::get<1>(request);if(q.after<0)return {};w.u64(q.after);if(!text(w,q.find,140))return {};break;}
    case 2:if(!resolution(w,std::get<2>(request)))return {};break;
    case 3:if(!retry(w,std::get<3>(request)))return {};break;
    case 4:if(!retry(w,std::get<4>(request)))return {};break;
    default:return {};
    }return w.take();
}
std::optional<FtnHeldControlRequest> decode_ftn_held_request(const std::vector<std::uint8_t>& bytes){
    using namespace ftn_held_wire;if(bytes.size()>512)return {};Reader r(bytes);std::uint8_t version{},kind{};if(!r.u8(version)||version!=2||!r.u8(kind))return {};FtnHeldControlRequest out;
    switch(kind){
    case 0:{FtnHeldReviewQuery q;if(!id(r,q.ingress)||!id(r,q.after)||!r.u32(q.message_offset)||!r.text(q.network,48)||!r.text(q.reason,64)||!r.text(q.find,140))return {};out=q;break;}
    case 1:{FtnHeldRecipientQuery q;if(!nonnegative64(r,q.after)||!r.text(q.find,140))return {};out=q;break;}
    case 2:{FtnRecipientResolutionRequest q;if(!resolution(r,q))return {};out=q;break;}
    case 3:{FtnReprocessRequest q;if(!retry(r,q))return {};out=q;break;}
    case 4:{FtnTicRetryRequest q;if(!retry(r,q))return {};out=q;break;}
    default:return {};
    }if(!r.complete()||encode_ftn_held_request(out).empty())return {};return out;
}
std::vector<std::uint8_t> encode_ftn_held_response(const FtnHeldControlResponse& response){
    using namespace ftn_held_wire;if(static_cast<unsigned>(response.code)>4||(response.code==FtnConfigurationCode::ok)==(response.value.index()==0))return {};Writer w;w.u8(2);w.u8(static_cast<std::uint8_t>(response.code));w.u8(static_cast<std::uint8_t>(response.value.index()));
    switch(response.value.index()){
    case 0:break;
    case 1:{const auto& v=std::get<1>(response.value);if(v.configuration_revision<1||static_cast<unsigned>(v.mode)>2||v.rows.size()>32||v.messages.size()>32||(!v.messages.empty()&&v.rows.size()!=1)||(v.retry&&(v.rows.size()!=1||v.rows.front().tic))||(v.tic&&(v.rows.size()!=1||!v.rows.front().tic||!v.messages.empty()||v.retry))||(!v.messages.empty()&&v.rows.front().tic))return {};w.u64(v.configuration_revision);w.u8(static_cast<std::uint8_t>(v.mode));w.u8(v.serial_recovery_required);w.u8(v.more);w.u8(static_cast<std::uint8_t>(v.rows.size()));
        for(const auto& row:v.rows){if(!row.ingress.valid()||row.received_utc<0||static_cast<unsigned>(row.state)>4)return {};id(w,row.ingress);w.u64(row.received_utc);w.u8(static_cast<std::uint8_t>(row.state));w.u8(row.retry_pending);w.u8(row.tic);if(!text(w,row.network,48)||!text(w,row.link,48)||!text(w,row.file,128)||!text(w,row.reason,64)||!text(w,row.from,140))return {};}
        w.u8(static_cast<std::uint8_t>(v.messages.size()));for(const auto& m:v.messages){if(!m.operation.valid()||m.message_index>=4096||!valid_ftn_code(m.network))return {};id(w,m.operation);w.u16(m.message_index);w.u8(m.local_private);w.u8(m.committed);if(!text(w,m.network,48)||!text(w,m.from,140)||!text(w,m.to,140)||!text(w,m.origin,128)||!text(w,m.destination,128)||!text(w,m.echo,128)||!text(w,m.reason,64))return {};w.u8(m.resolution.has_value());if(m.resolution&&(m.resolution->request.held_message.bytes!=m.operation.bytes||!resolution_record(w,*m.resolution)))return {};}
        w.u8(v.retry.has_value());if(v.retry&&(v.retry->request.ingress.bytes!=v.rows.front().ingress.bytes||!retry_record(w,*v.retry)))return {};w.u8(v.tic.has_value());if(v.tic&&!tic_detail(w,*v.tic,v.rows.front().ingress))return {};break;}
    case 2:{const auto& v=std::get<2>(response.value);if(v.rows.size()>32)return {};w.u8(v.more);w.u8(static_cast<std::uint8_t>(v.rows.size()));for(const auto& row:v.rows){if(row.id<1||row.revision<1||row.name.empty())return {};w.u64(row.id);w.u64(row.revision);if(!text(w,row.name,140))return {};}break;}
    case 3:if(!resolution_record(w,std::get<3>(response.value)))return {};break;
    case 4:if(!retry_record(w,std::get<4>(response.value)))return {};break;
    case 5:if(!retry_record(w,std::get<5>(response.value)))return {};break;
    default:return {};
    }auto bytes=w.take();return bytes.size()<=kLocalControlMaximumPayloadBytes?bytes:std::vector<std::uint8_t>{};
}
std::optional<FtnHeldControlResponse> decode_ftn_held_response(const std::vector<std::uint8_t>& bytes){
    using namespace ftn_held_wire;if(bytes.size()>kLocalControlMaximumPayloadBytes)return {};Reader r(bytes);std::uint8_t version{},code{},kind{},count{};if(!r.u8(version)||version!=2||!r.u8(code)||!r.u8(kind))return {};FtnHeldControlResponse out;out.code=static_cast<FtnConfigurationCode>(code);
    switch(kind){
    case 0:break;
    case 1:{FtnHeldReview v;std::uint8_t mode{};if(!nonnegative64(r,v.configuration_revision)||!r.u8(mode)||!flag(r,v.serial_recovery_required)||!flag(r,v.more)||!r.u8(count)||count>32)return {};v.mode=static_cast<FtnMailerMode>(mode);for(unsigned i=0;i<count;++i){FtnHeldReviewRow row;std::uint8_t state{};if(!id(r,row.ingress)||!nonnegative64(r,row.received_utc)||!r.u8(state)||!flag(r,row.retry_pending)||!flag(r,row.tic)||!r.text(row.network,48)||!r.text(row.link,48)||!r.text(row.file,128)||!r.text(row.reason,64)||!r.text(row.from,140))return {};row.state=static_cast<FtnIngressState>(state);v.rows.push_back(std::move(row));}
        if(!r.u8(count)||count>32)return {};
        for(unsigned i=0;i<count;++i){FtnHeldReviewMessage m;bool resolved{};if(!id(r,m.operation)||!r.u16(m.message_index)||!flag(r,m.local_private)||!flag(r,m.committed)||!r.text(m.network,48)||!r.text(m.from,140)||!r.text(m.to,140)||!r.text(m.origin,128)||!r.text(m.destination,128)||!r.text(m.echo,128)||!r.text(m.reason,64)||!flag(r,resolved))return {};if(resolved){m.resolution.emplace();if(!resolution_record(r,*m.resolution))return {};}v.messages.push_back(std::move(m));}
        bool retried{};if(!flag(r,retried))return {};if(retried){v.retry.emplace();if(!retry_record(r,*v.retry))return {};}bool tic{};if(!flag(r,tic))return {};if(tic){v.tic.emplace();if(!tic_detail(r,*v.tic))return {};}out.value=std::move(v);break;}
    case 2:{FtnHeldRecipients v;if(!flag(r,v.more)||!r.u8(count)||count>32)return {};for(unsigned i=0;i<count;++i){FtnHeldRecipient row;if(!nonnegative64(r,row.id)||!nonnegative64(r,row.revision)||!r.text(row.name,140))return {};v.rows.push_back(std::move(row));}out.value=std::move(v);break;}
    case 3:{FtnRecipientResolution v;if(!resolution_record(r,v))return {};out.value=v;break;}
    case 4:{FtnReprocessRecord v;if(!retry_record(r,v))return {};out.value=v;break;}
    case 5:{FtnTicRetryRecord v;if(!retry_record(r,v))return {};out.value=v;break;}
    default:return {};
    }if(!r.complete()||encode_ftn_held_response(out).empty())return {};return out;
}
