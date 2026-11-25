#include "linux_web_ftn_held.hpp"
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;namespace web=sfx::apps::runtime::web;using Json=nlohmann::json;
namespace {unsigned checks{};void check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}template<class F>void refuses(F f){bool refused=false;try{f();}catch(const std::exception&){refused=true;}check(refused,"unsafe held request accepted");}}
int main(){try{
 const std::string one(32,'1'),two(32,'2'),zero(32,'0');
 Json query={{"ingress",""},{"after",""},{"offset","0"},{"network","TEST"},{"reason","ftn_recipient_unmatched"},{"find","Remote"}};
 auto q=web::web_ftn_held_query(query);check(!q.ingress.valid()&&q.network=="TEST"&&q.find=="Remote","bounded original filter");query["ingress"]=one;query["offset"]="42";query["network"]="";query["reason"]="";query["find"]="";q=web::web_ftn_held_query(query);check(q.ingress.valid()&&q.message_offset==42,"explicit selected original message page");
 for(const auto* bad:{"-1","01","65536","99999999999999999999"}){auto v=query;v["offset"]=bad;refuses([&]{web::web_ftn_held_query(v);});}
 auto bad=query;bad["proof"]=true;refuses([&]{web::web_ftn_held_query(bad);});bad=query;bad["find"]=std::string(141,'x');refuses([&]{web::web_ftn_held_query(bad);});
 auto recipients=web::web_ftn_held_recipients_query({{"after","12"},{"find","Alice"}});check(recipients.after==12&&recipients.find=="Alice","explicit recipient page without automatic choice");
 Json resolve={{"kind","resolve"},{"operation",one},{"held_message",two},{"previous",zero},{"revision","3"},{"recipient","7"},{"recipient_revision","8"}};
 const auto r=std::get<app::FtnRecipientResolutionRequest>(web::web_ftn_held_action(resolve));check(r.recipient==7&&r.recipient_revision==8&&r.configuration_revision==3&&!r.previous_resolution.valid(),"recipient authority keeps exact current revisions and prior decision");
 for(const auto* field:{"operation","held_message","revision","recipient","recipient_revision"}){auto v=resolve;v[field]=std::string(field)=="operation"||std::string(field)=="held_message"?zero:"0";refuses([&]{web::web_ftn_held_action(v);});}
 bad=resolve;bad["operation"]=std::string(32,'A');refuses([&]{web::web_ftn_held_action(bad);});bad=resolve;bad["recipient"]="9223372036854775808";refuses([&]{web::web_ftn_held_action(bad);});bad=resolve;bad["recipient"]=7;refuses([&]{web::web_ftn_held_action(bad);});
 bad=resolve;bad["body"]="private body";refuses([&]{web::web_ftn_held_action(bad);});
 Json retry={{"kind","reprocess"},{"operation",one},{"ingress",two},{"previous",zero},{"revision","4"}};check(std::holds_alternative<app::FtnReprocessRequest>(web::web_ftn_held_action(retry)),"original retry keeps its own opaque operation");
 auto tic=retry;tic["kind"]="tic";tic["review"]=std::string(64,'3');check(std::get<app::FtnTicRetryRequest>(web::web_ftn_held_action(tic)).review[0]==0x33,"TIC retry binds exact review digest");bad=tic;bad["review"]=std::string(64,'0');refuses([&]{web::web_ftn_held_action(bad);});
 app::FtnHeldReview review;review.configuration_revision=4;review.mode=app::FtnMailerMode::native;review.more=true;app::FtnHeldReviewRow row;row.ingress.bytes=r.held_message.bytes;row.file="held.pkt";row.reason="ftn_recipient_unmatched";review.rows.push_back(row);
 app::FtnHeldReviewMessage message;message.operation=r.held_message;message.from="Remote From";message.to="Original To";message.local_private=true;message.resolution=app::FtnRecipientResolution{r,123};review.messages.push_back(message);
 app::FtnHeldControlResponse response;response.code=app::FtnConfigurationCode::ok;response.value=review;auto json=web::web_ftn_held_reply(response);check(json["kind"]=="review"&&json["more"]==true&&json["messages"][0]["to"]=="Original To","review retains original recipient and metadata");check(json["messages"][0]["resolution"]["recipient"]=="7"&&!json["messages"][0].contains("body")&&!json["rows"][0].contains("proof")&&!json["rows"][0].contains("path"),"purpose-limited projection excludes body proof and native path");
 response.value=app::FtnReprocessRecord{std::get<app::FtnReprocessRequest>(web::web_ftn_held_action(retry)),10,false,false,{}};json=web::web_ftn_held_reply(response);check(json["kind"]=="reprocess"&&json["result"]["operation"]==one&&!json["result"]["finished"].get<bool>(),"queued retry is not completion");
 response.code=app::FtnConfigurationCode::unavailable;json=web::web_ftn_held_reply(response);check(!json["ok"].get<bool>()&&json["unconfirmed"].get<bool>(),"unavailable outcome remains explicitly unconfirmed");response.code=app::FtnConfigurationCode::conflict;json=web::web_ftn_held_reply(response);check(!json["ok"].get<bool>()&&!json["unconfirmed"].get<bool>(),"known conflict is distinct from lost reply");
 std::cout<<"PASS "<<checks<<" Held Mail web adapter checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<" "<<e.what()<<'\n';return 1;}}
