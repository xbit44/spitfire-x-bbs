#include "linux_web_caller_actions.hpp"
#include <iostream>

namespace app=sfx::application;namespace web=sfx::apps::runtime::web;
namespace {
unsigned checks{};void check(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
struct Clock final:app::IClock {std::uint64_t now{1000};std::int64_t unix_seconds()const noexcept override{return 1794355200;}std::uint64_t monotonic_millis()const noexcept override{return now;}};
}
int main(){try{
    check(sodium_init()>=0,"random generator ready");Clock clock;app::ChatHub hub;check(hub.configure({}),"default Chat configuration");
    app::ChatParticipant caller;caller.identity.session.bytes[0]=1;caller.identity.account=2;caller.identity.node=2;caller.name="Test Caller";caller.safe=true;caller.level=20;
    check(hub.attach(caller),"synthetic exact caller attached");app::Identifier128 epoch;epoch.bytes[0]=7;app::NodeAssignmentIdentity assignment{epoch,2,10};
    unsigned sends{};bool throw_notice{};
    web::WebCallerActions actions(clock,[&](const app::ChatOperatorRequest& request){
        hub.tick(clock.now);using Action=app::ChatOperatorAction;
        if(request.action==Action::start||request.action==Action::answer){if(!request.target||!(*request.target==assignment))return app::ChatOperatorSnapshot{};return hub.operator_start(caller.identity,request.view,request.page,request.override_dnd,clock.now);}
        if(request.action==Action::poll)return hub.operator_poll(request.token,request.view,request.sequence,clock.now);
        if(request.action==Action::send){app::ChatOperatorSnapshot result;result.code=hub.operator_send(request.token,request.view,request.sequence,request.text,clock.now);return result;}
        if(request.action==Action::end){hub.operator_end(request.token,request.view,clock.now);return hub.operator_status();}
        return app::ChatOperatorSnapshot{};
    },[&](const app::ChatNoticeRequest& request){++sends;if(throw_notice)throw std::runtime_error("unknown notice result");return request.target&& !(*request.target==assignment)?app::ChatResult::unavailable:hub.notice(request.target?std::optional<app::ChatIdentity>(caller.identity):std::nullopt,request.room,request.text,request.override_dnd);});
    app::ChatNoticeRequest notice;notice.target=assignment;notice.text="Test notice";
    const auto review=actions.prepare_notice("owner-a",notice);check(review&&!review->submitted&&sends==0,"Prepare Notice does not send");
    check(!actions.current_notice("owner-b")&&!actions.notice("owner-b",review->handle,true),"other session cannot discover or submit notice");
    check(actions.cancel_notice("owner-a",review->handle)&&sends==0,"Cancel preserves zero sends");check(!actions.notice("owner-a",review->handle,true),"cancelled handle cannot submit");
    const auto final=actions.prepare_notice("owner-a",notice);check(final&&final->handle!=review->handle,"new review gets a new receipt");
    const auto sent=actions.notice("owner-a",final->handle,true);check(sent&&sent->completed&&sent->result==app::ChatResult::ok&&sends==1,"confirmed notice sent once");
    check(actions.notice("owner-a",final->handle,true)->completed&&sends==1,"lost response retry returns same result without duplicate");check(actions.notice("owner-a",final->handle,false)->submitted&&sends==1,"Check Result is read-only");
    check(!actions.cancel_notice("owner-a",final->handle),"submitted notice cannot be relabelled cancelled");
    auto invalid=notice;invalid.text="Line one\nLine two";check(!actions.prepare_notice("owner-a",invalid),"multiline notice refused");invalid.text=std::string(513,'x');check(!actions.prepare_notice("owner-a",invalid),"oversize notice refused");
    throw_notice=true;const auto unknown=actions.prepare_notice("owner-a",notice);check(unknown.has_value(),"new deliberate notice review");const auto uncertain=actions.notice("owner-a",unknown->handle,true);
    check(uncertain&&uncertain->submitted&&!uncertain->completed&&sends==2,"exception retains uncertain submitted state");
    check(!actions.prepare_notice("owner-a",notice)&&!actions.cancel_notice("owner-a",unknown->handle),"unknown notice cannot be silently replaced or cancelled");
    check(!actions.notice("owner-a",unknown->handle,true)->completed&&sends==2,"unknown notice is never replayed");throw_notice=false;
    app::ChatOperatorRequest start;start.action=app::ChatOperatorAction::start;start.target=assignment;
    const auto view=actions.start("operator-a",start);check(view&&view->snapshot.pending&&view->snapshot.token,"operator view starts with private server identity");
    check(!actions.start("operator-a",start),"duplicate Start cannot replace existing owner view");check(!actions.poll("operator-b",0),"other session cannot read operator chat");
    check(!actions.end("operator-b",view->handle),"other session cannot end operator chat");
    check(hub.claim_operator(caller.identity,clock.now),"caller claims chat at safe screen");const auto active=actions.poll("operator-a",0);check(active&&active->snapshot.active&&active->snapshot.paused,"operator-start chat pauses calling time");
    check(actions.send("operator-b",view->handle,1,"wrong owner")==app::ChatResult::unavailable,"other owner cannot inject text");
    check(actions.send("operator-a",view->handle,2,"out of order")==app::ChatResult::conflict,"out-of-order input refused");
    check(actions.send("operator-a",view->handle,1,"Hello caller")==app::ChatResult::ok,"operator sends one safe line");
    check(actions.send("operator-a",view->handle,1,"Hello caller")==app::ChatResult::conflict,"duplicate HTTP send cannot duplicate line");
    const auto checked=actions.poll("operator-a",0);check(checked&&checked->next_send==2,"lost send response reconciles next sequence read-only");
    check(std::count_if(checked->snapshot.lines.begin(),checked->snapshot.lines.end(),[](const auto& line){return line.text=="Sysop: Hello caller";})==1,"actual hub contains exactly one sent line");
    check(actions.send("operator-a",view->handle,2,"bad\x1b[31m")==app::ChatResult::invalid,"control escape text refused");
    check(actions.end("operator-a",view->handle)&&!hub.operator_active(caller.identity),"End closes only owned chat and returns caller");check(!actions.poll("operator-a",0),"ended browser view erased");
    const auto next=actions.start("operator-a",start);check(next&&next->handle!=view->handle,"new conversation has a new opaque handle");check(!actions.end("operator-a",view->handle),"old view handle cannot end new chat");
    clock.now+=70001;check(!actions.poll("operator-a",0),"abandoned web view expires without polling");check(!hub.operator_active(caller.identity),"abandoned chat ended in hub");
    clock.now+=600000;check(!actions.current_notice("owner-a")&&!actions.notice("owner-a",unknown->handle,true)&&sends==2,"expired notice receipt cannot trigger later resend");
    for(unsigned i=0;i<32;++i)check(actions.prepare_notice("owner-"+std::to_string(i),notice).has_value(),"bounded notice slot");
    check(!actions.prepare_notice("owner-overflow",notice),"notice owner map bounded");
    actions.clear();check(!actions.current_notice("owner-0"),"restore/session reset clears ephemeral notice drafts");
    std::cout<<"PASS "<<checks<<" browser caller-action ownership, receipt and ChatHub checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
