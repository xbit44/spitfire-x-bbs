#include "linux_web_ftn_held.hpp"
#include <charconv>
#include <limits>
#include <stdexcept>
namespace sfx::apps::runtime::web {
namespace {
namespace app=application;using Json=nlohmann::json;
void require(bool ok){if(!ok)throw std::runtime_error("ftn_held_request_invalid");}
void keys(const Json& j,std::initializer_list<const char*> names){require(j.is_object()&&j.size()==names.size());for(const auto* key:names)require(j.contains(key));}
std::string text(const Json& j,std::size_t limit){require(j.is_string());auto s=j.get<std::string>();require(s.size()<=limit&&s.find('\0')==std::string::npos);return s;}
std::int64_t number(const Json& j,std::int64_t maximum=std::numeric_limits<std::int64_t>::max()){
 const auto s=text(j,20);std::int64_t n{};const auto r=std::from_chars(s.data(),s.data()+s.size(),n);require(!s.empty()&&r.ec==std::errc{}&&r.ptr==s.data()+s.size()&&n>=0&&n<=maximum&&std::to_string(n)==s);return n;
}
template<std::size_t N>std::array<std::uint8_t,N> hex(const Json& j){const auto s=text(j,N*2);require(s.size()==N*2);std::array<std::uint8_t,N> out{};const auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a'+10);throw std::runtime_error("ftn_held_identity_invalid");};for(std::size_t i=0;i<N;++i)out[i]=static_cast<std::uint8_t>((digit(s[2*i])<<4)|digit(s[2*i+1]));return out;}
app::Identifier128 id(const Json& j,bool optional=false){app::Identifier128 out;const auto s=text(j,32);if(optional&&s.empty())return out;out.bytes=hex<16>(j);require(optional||out.valid());return out;}
template<std::size_t N>std::string hex_text(const std::array<std::uint8_t,N>& bytes){constexpr char digits[]="0123456789abcdef";std::string s;for(auto b:bytes){s+=digits[b>>4];s+=digits[b&15];}return s;}
std::string token(const app::Identifier128& value){return hex_text(value.bytes);}
Json resolution(const app::FtnRecipientResolution& r){return {{"operation",token(r.request.operation)},{"recipient",std::to_string(r.request.recipient)},{"recipient_revision",std::to_string(r.request.recipient_revision)},{"created",std::to_string(r.created)}};}
template<class T>Json retry(const T& r){return {{"operation",token(r.request.operation)},{"finished",r.finished},{"processed",r.processed},{"reason",r.reason},{"created",std::to_string(r.created)}};}
void valid(const app::FtnHeldControlRequest& q){require(!app::encode_ftn_held_request(q).empty());}
}
application::FtnHeldReviewQuery web_ftn_held_query(const Json& j){
 keys(j,{"ingress","after","offset","network","reason","find"});app::FtnHeldReviewQuery q;q.ingress=id(j.at("ingress"),true);q.after=id(j.at("after"),true);q.message_offset=static_cast<std::uint32_t>(number(j.at("offset"),4096));q.network=text(j.at("network"),48);q.reason=text(j.at("reason"),64);q.find=text(j.at("find"),140);valid(q);return q;
}
application::FtnHeldRecipientQuery web_ftn_held_recipients_query(const Json& j){keys(j,{"after","find"});app::FtnHeldRecipientQuery q;q.after=number(j.at("after"));q.find=text(j.at("find"),140);valid(q);return q;}
application::FtnHeldControlRequest web_ftn_held_action(const Json& j){
 require(j.is_object()&&j.contains("kind"));const auto kind=text(j.at("kind"),16);app::FtnHeldControlRequest result;
 if(kind=="resolve"){
  keys(j,{"kind","operation","held_message","previous","revision","recipient","recipient_revision"});app::FtnRecipientResolutionRequest q;q.operation=id(j.at("operation"));q.held_message=id(j.at("held_message"));q.previous_resolution=id(j.at("previous"),true);q.configuration_revision=number(j.at("revision"));q.recipient=number(j.at("recipient"));q.recipient_revision=number(j.at("recipient_revision"));result=q;
 }else if(kind=="reprocess"){
  keys(j,{"kind","operation","ingress","previous","revision"});app::FtnReprocessRequest q;q.operation=id(j.at("operation"));q.ingress=id(j.at("ingress"));q.previous_request=id(j.at("previous"),true);q.configuration_revision=number(j.at("revision"));result=q;
 }else if(kind=="tic"){
  keys(j,{"kind","operation","ingress","previous","revision","review"});app::FtnTicRetryRequest q;q.operation=id(j.at("operation"));q.ingress=id(j.at("ingress"));q.previous_request=id(j.at("previous"),true);q.configuration_revision=number(j.at("revision"));q.review=hex<32>(j.at("review"));result=q;
 }else throw std::runtime_error("ftn_held_action_invalid");valid(result);return result;
}
nlohmann::json web_ftn_held_reply(const app::FtnHeldControlResponse& response){
 const auto code=response.code;Json out={{"ok",code==app::FtnConfigurationCode::ok},{"unconfirmed",code==app::FtnConfigurationCode::unavailable},{"busy",code==app::FtnConfigurationCode::busy},{"kind","none"}};
 if(code!=app::FtnConfigurationCode::ok)return out;
 if(const auto* v=std::get_if<app::FtnHeldReview>(&response.value)){
  out["kind"]="review";out["revision"]=std::to_string(v->configuration_revision);out["more"]=v->more;out["recovery_required"]=v->serial_recovery_required;out["mode"]=static_cast<unsigned>(v->mode);out["rows"]=Json::array();out["messages"]=Json::array();out["retry"]=v->retry?retry(*v->retry):Json(nullptr);out["tic"]=nullptr;
  for(const auto& r:v->rows)out["rows"].push_back({{"ingress",token(r.ingress)},{"received",std::to_string(r.received_utc)},{"network",r.network},{"link",r.link},{"file",r.file},{"reason",r.reason},{"from",r.from},{"state",static_cast<unsigned>(r.state)},{"retry_pending",r.retry_pending},{"tic",r.tic},{"message_original",app::ftn_message_wire_name(r.file)}});
  for(const auto& m:v->messages)out["messages"].push_back({{"operation",token(m.operation)},{"index",m.message_index},{"network",m.network},{"from",m.from},{"to",m.to},{"origin",m.origin},{"destination",m.destination},{"echo",m.echo},{"reason",m.reason},{"local_private",m.local_private},{"committed",m.committed},{"resolution",m.resolution?resolution(*m.resolution):Json(nullptr)}});
  if(v->tic){const auto& t=*v->tic;out["tic"]={{"review",hex_text(t.review)},{"publication",token(t.publication)},{"claimed_from",t.claimed_from},{"echo",t.echo},{"payload",t.payload},{"phase",t.phase},{"forward_plan_present",t.forward_plan_present},{"retry",t.retry?retry(*t.retry):Json(nullptr)}};}
 }else if(const auto* v=std::get_if<app::FtnHeldRecipients>(&response.value)){out["kind"]="recipients";out["more"]=v->more;out["rows"]=Json::array();for(const auto& r:v->rows)out["rows"].push_back({{"id",std::to_string(r.id)},{"revision",std::to_string(r.revision)},{"name",r.name}});
 }else if(const auto* v=std::get_if<app::FtnRecipientResolution>(&response.value)){out["kind"]="resolve";out["result"]=resolution(*v);
 }else if(const auto* v=std::get_if<app::FtnReprocessRecord>(&response.value)){out["kind"]="reprocess";out["result"]=retry(*v);
 }else if(const auto* v=std::get_if<app::FtnTicRetryRecord>(&response.value)){out["kind"]="tic";out["result"]=retry(*v);
 }else{out["ok"]=false;out["unconfirmed"]=true;}
 return out;
}
}
