#include "linux_web_binkp.hpp"

namespace sfx::apps::runtime::web {
namespace app=application;using Json=nlohmann::json;
Json web_binkp_snapshot(const app::FtnRuntimeResponse& value){
    const auto number=[](auto n){return std::to_string(n);};
    const auto progress=[&](const app::FtnFileProgress& p){return Json{{"name",p.name},{"size",number(p.size)},{"position",number(p.position)},{"active",p.active},{"awaiting_ack",p.awaiting_ack}};};
    Json sessions=Json::array(),connections=Json::array(),activity=Json::array(),queues=nullptr;
    for(const auto& s:value.sessions)sessions.push_back({{"serial",number(s.serial)},{"active",s.active},{"outbound",s.outbound},{"completed",s.completed},{"peer_verified",s.peer_verified},{"network",s.network},{"link",s.link},{"reason",s.reason},{"peer_address",s.peer_address},{"endpoint",s.endpoint},{"received_bytes",number(s.received_bytes)},{"sent_bytes",number(s.sent_bytes)},{"received_files",s.received_files},{"sent_files",s.sent_files},{"send",progress(s.send)},{"receive",progress(s.receive)}});
    for(const auto& c:value.connections)connections.push_back({{"network",c.network},{"link",c.link},{"name",c.name},{"enabled",c.enabled},{"outbound",c.outbound}});
    for(const auto& a:value.activity)activity.push_back({{"utc",number(a.utc_seconds)},{"session",number(a.session)},{"network",a.network},{"link",a.link},{"message",a.message}});
    if(value.queues){const auto& q=*value.queues;Json incoming=Json::array(),outgoing=Json::array(),rows=Json::array();
        for(auto n:q.ingress)incoming.push_back(number(n));
        for(auto n:q.outbound)outgoing.push_back(number(n));
        for(const auto& row:q.rows)rows.push_back({{"outbound",row.outbound},{"network",row.network},{"link",row.link},{"file",row.file},{"reason",row.reason},{"state",row.state},{"bytes",number(row.bytes)}});
        queues={{"ingress",incoming},{"outbound",outgoing},{"unscanned",number(q.unscanned)},{"more",q.more},{"rows",rows}};
    }
    return {{"code",static_cast<unsigned>(value.code)},{"mode",static_cast<unsigned>(value.mode)},{"saved_revision",number(value.saved_revision)},{"active_revision",number(value.active_revision)},
        {"capable",value.capable},{"paused",value.paused},{"serial_recovery_required",value.serial_recovery_required},{"classic_listening",value.classic_listening},{"tls_listening",value.tls_listening},{"processing",value.processing},{"classic_port",value.classic_port},{"tls_port",value.tls_port},
        {"diagnostic",value.diagnostic},{"classic_error",value.classic_error},{"tls_error",value.tls_error},{"processing_error",value.processing_error},{"processed_receipts",number(value.processed_receipts)},{"held_receipts",number(value.held_receipts)},{"imported_messages",number(value.imported_messages)},{"replayed_messages",number(value.replayed_messages)},{"scanned_messages",number(value.scanned_messages)},
        {"sessions",sessions},{"connections",connections},{"more_connections",value.more_connections},{"queues",queues},{"activity",activity}};
}
WebBinkpActions::WebBinkpActions(app::IClock& clock,Execute execute):clock_(clock),execute_(std::move(execute)){}
void WebBinkpActions::expire(){const auto now=clock_.monotonic_millis();for(auto i=states_.begin();i!=states_.end();){if(now-i->second.touched>=600000)i=states_.erase(i);else ++i;}}
std::optional<WebBinkpReview> WebBinkpActions::prepare(std::string owner,const app::FtnRuntimeRequest& request,app::Identifier128 epoch,std::int64_t revision){
    std::lock_guard<std::mutex> lock(mutex_);expire();
    if(owner.empty()||owner.size()>128||!epoch.valid()||revision<=0||request.include_hold||
        (request.action!=app::FtnRuntimeAction::poll&&request.action!=app::FtnRuntimeAction::scan)||app::encode_ftn_runtime_request(request).empty())return {};
    const auto prior=states_.find(owner);if(prior!=states_.end()&&prior->second.review.submitted&&!prior->second.review.completed)return {};
    if(prior==states_.end()&&states_.size()>=32)return {};
    auto& state=states_[std::move(owner)];state={WebBinkpReview{browser_token(),epoch,revision,request,false,false,{}},clock_.monotonic_millis()};return state.review;
}
std::optional<WebBinkpReview> WebBinkpActions::read(std::string_view owner,std::string_view handle,bool submit){
    std::lock_guard<std::mutex> lock(mutex_);expire();const auto found=states_.find(std::string(owner));
    if(found==states_.end()||!browser_secret_equal(found->second.review.handle,handle))return {};
    auto& state=found->second;state.touched=clock_.monotonic_millis();
    if(submit&&!state.review.submitted){state.review.submitted=true;try{state.review.result=execute_(state.review.request,state.review.epoch,state.review.revision);state.review.completed=true;}catch(...){}}
    return state.review;
}
std::optional<WebBinkpReview> WebBinkpActions::current(std::string_view owner){std::lock_guard<std::mutex> lock(mutex_);expire();const auto found=states_.find(std::string(owner));return found==states_.end()?std::nullopt:std::optional<WebBinkpReview>(found->second.review);}
bool WebBinkpActions::cancel(std::string_view owner,std::string_view handle){
    std::lock_guard<std::mutex> lock(mutex_);expire();const auto found=states_.find(std::string(owner));if(found==states_.end()||!browser_secret_equal(found->second.review.handle,handle)||found->second.review.submitted)return false;states_.erase(found);return true;
}
}
