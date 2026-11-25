#include "linux_web_caller_actions.hpp"

namespace sfx::apps::runtime::web {
namespace app=application;
WebCallerActions::WebCallerActions(app::IClock& clock,Operator op,Notice notice):clock_(clock),operator_(std::move(op)),notice_(std::move(notice)){}
WebCallerActions::~WebCallerActions(){clear();}
void WebCallerActions::end_view(OperatorState& state)noexcept{
    try{app::ChatOperatorRequest request;request.action=app::ChatOperatorAction::end;request.view=state.identity;request.token=state.view.snapshot.token;if(request.token)(void)operator_(request);}catch(...){}
}
void WebCallerActions::expire(){
    const auto now=clock_.monotonic_millis();
    for(auto it=notices_.begin();it!=notices_.end();){if(now-it->second.touched>=600000)it=notices_.erase(it);else ++it;}
    for(auto it=operators_.begin();it!=operators_.end();){if(now-it->second.touched>=70000){end_view(it->second);it=operators_.erase(it);}else ++it;}
}
std::optional<WebNoticeReview> WebCallerActions::prepare_notice(std::string owner,const app::ChatNoticeRequest& request){
    std::lock_guard<std::mutex> lock(mutex_);expire();
    if(owner.empty()||owner.size()>128||app::encode_chat_notice(request).empty())return {};
    const auto previous=notices_.find(owner);
    if(previous!=notices_.end()&&previous->second.review.submitted&&!previous->second.review.completed)return {};
    if(previous==notices_.end()&&notices_.size()>=32)return {};
    auto& state=notices_[std::move(owner)];state={WebNoticeReview{browser_token(),request,app::ChatResult::unavailable,false,false},clock_.monotonic_millis()};return state.review;
}
std::optional<WebNoticeReview> WebCallerActions::notice(std::string_view owner,std::string_view handle,bool send){
    std::lock_guard<std::mutex> lock(mutex_);expire();const auto found=notices_.find(std::string(owner));
    if(found==notices_.end()||!browser_secret_equal(found->second.review.handle,handle))return {};
    auto& state=found->second;state.touched=clock_.monotonic_millis();
    if(send&&!state.review.submitted){
        state.review.submitted=true;
        try{state.review.result=notice_(state.review.request);state.review.completed=true;}catch(...){/* Retain unknown, never replay. */}
    }
    return state.review;
}
std::optional<WebNoticeReview> WebCallerActions::current_notice(std::string_view owner){
    std::lock_guard<std::mutex> lock(mutex_);expire();const auto found=notices_.find(std::string(owner));
    return found==notices_.end()?std::nullopt:std::optional<WebNoticeReview>(found->second.review);
}
bool WebCallerActions::cancel_notice(std::string_view owner,std::string_view handle){
    std::lock_guard<std::mutex> lock(mutex_);expire();const auto found=notices_.find(std::string(owner));
    if(found==notices_.end()||!browser_secret_equal(found->second.review.handle,handle)||found->second.review.submitted)return false;
    notices_.erase(found);return true;
}
std::optional<WebOperatorView> WebCallerActions::start(std::string owner,app::ChatOperatorRequest request){
    std::lock_guard<std::mutex> lock(mutex_);expire();
    if(owner.empty()||owner.size()>128||operators_.count(owner)||operators_.size()>=32||
        (request.action!=app::ChatOperatorAction::start&&request.action!=app::ChatOperatorAction::answer))return {};
    do{randombytes_buf(request.view.bytes.data(),request.view.bytes.size());}while(!request.view.valid());
    if(app::encode_chat_operator_request(request).empty())return {};
    OperatorState state;state.identity=request.view;state.view.handle=browser_token();state.touched=clock_.monotonic_millis();
    state.view.snapshot=operator_(request);
    if(state.view.snapshot.code==app::ChatResult::ok&&state.view.snapshot.token)operators_.emplace(std::move(owner),state);
    return state.view;
}
std::optional<WebOperatorView> WebCallerActions::poll(std::string_view owner,std::uint64_t acknowledged){
    std::lock_guard<std::mutex> lock(mutex_);expire();const auto found=operators_.find(std::string(owner));if(found==operators_.end())return {};
    auto& state=found->second;app::ChatOperatorRequest request;request.action=app::ChatOperatorAction::poll;request.view=state.identity;request.token=state.view.snapshot.token;request.sequence=acknowledged;
    const auto result=operator_(request);
    // Keep the original token even when the Runtime reports unavailable. A
    // later caller or restarted hub must never become this view's target.
    const auto token=state.view.snapshot.token;state.view.snapshot=result;state.view.snapshot.token=token;state.touched=clock_.monotonic_millis();return state.view;
}
app::ChatResult WebCallerActions::send(std::string_view owner,std::string_view handle,std::uint64_t sequence,std::string_view text){
    std::lock_guard<std::mutex> lock(mutex_);expire();const auto found=operators_.find(std::string(owner));
    if(found==operators_.end()||!browser_secret_equal(found->second.view.handle,handle)||!sequence)return app::ChatResult::unavailable;
    auto& state=found->second;
    if(sequence!=state.view.next_send||sequence==UINT64_MAX)return app::ChatResult::conflict;
    app::ChatOperatorRequest request;request.action=app::ChatOperatorAction::send;request.view=state.identity;request.token=state.view.snapshot.token;request.sequence=sequence;request.text=std::string(text);
    if(app::encode_chat_operator_request(request).empty())return app::ChatResult::invalid;
    const auto result=operator_(request);if(result.code==app::ChatResult::ok)++state.view.next_send;state.touched=clock_.monotonic_millis();return result.code;
}
bool WebCallerActions::end(std::string_view owner,std::string_view handle){
    std::lock_guard<std::mutex> lock(mutex_);expire();const auto found=operators_.find(std::string(owner));
    if(found==operators_.end()||!browser_secret_equal(found->second.view.handle,handle))return false;
    end_view(found->second);operators_.erase(found);return true;
}
void WebCallerActions::clear(){std::lock_guard<std::mutex> lock(mutex_);for(auto& item:operators_)end_view(item.second);operators_.clear();notices_.clear();}
}
