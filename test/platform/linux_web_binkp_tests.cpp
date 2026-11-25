#include "linux_web_binkp.hpp"
#include <iostream>
#include <stdexcept>
#include <sodium.h>
namespace app=sfx::application;namespace web=sfx::apps::runtime::web;
namespace {
unsigned checks{};void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Clock:app::IClock{std::uint64_t now{1000};std::uint64_t monotonic_millis()const noexcept override{return now;}std::int64_t unix_seconds()const noexcept override{return 1700000000;}};
}
int main(){try{
    check(sodium_init()>=0,"crypto ready");Clock clock;unsigned calls{};bool fail{};app::Identifier128 epoch;epoch.bytes[0]=1;
    web::WebBinkpActions actions(clock,[&](const auto& request,auto expected,auto revision){++calls;check(expected==epoch&&revision==7&&request.network=="test"&&request.link=="hub","review pins epoch revision and exact connection keys");if(fail)throw std::runtime_error("uncertain");app::FtnRuntimeResponse out;out.code=app::FtnConfigurationCode::ok;return out;});
    app::FtnRuntimeRequest poll;poll.action=app::FtnRuntimeAction::poll;poll.network="test";poll.link="hub";
    auto review=actions.prepare("owner",poll,epoch,7);check(review&&!review->submitted&&calls==0,"prepare never polls");
    check(!actions.current("other")&&!actions.read("other",review->handle,true),"receipts and submission bound to browser session");
    check(actions.cancel("owner",review->handle)&&calls==0&&!actions.read("owner",review->handle,true),"cancelled review cannot poll");
    auto next=actions.prepare("owner",poll,epoch,7);check(next&&next->handle!=review->handle,"new review gets new handle");
    auto result=actions.read("owner",next->handle,true);check(result&&result->submitted&&result->completed&&calls==1,"submit executes once");
    check(actions.read("owner",next->handle,true)->completed&&calls==1,"lost reply cannot duplicate poll");
    check(actions.current("owner")->completed&&calls==1,"read-only check never executes");
    check(!actions.cancel("owner",next->handle),"submitted work cannot be labelled cancelled");
    fail=true;next=actions.prepare("owner",poll,epoch,7);result=actions.read("owner",next->handle,true);
    check(result&&result->submitted&&!result->completed&&calls==2,"exception retains unknown receipt");
    check(!actions.prepare("owner",poll,epoch,7)&&!actions.cancel("owner",next->handle),"unknown action cannot be replaced");
    check(!actions.read("owner",next->handle,true)->completed&&calls==2,"unknown action never retries");
    clock.now+=600001;check(!actions.read("owner",next->handle,true)&&calls==2,"expired handle cannot replay");
    auto invalid=poll;invalid.include_hold=true;check(!actions.prepare("owner",invalid,epoch,7),"no implicit release of held outbound mail");
    invalid=poll;invalid.network="../other";check(!actions.prepare("owner",invalid,epoch,7),"invalid routing key refused");
    check(!actions.prepare("owner",poll,{},7)&&!actions.prepare("owner",poll,epoch,0),"empty epoch and revision refused");
    invalid={};check(!actions.prepare("owner",invalid,epoch,7),"read actions cannot become dispatch actions");
    invalid.action=app::FtnRuntimeAction::scan;check(actions.prepare("owner",invalid,epoch,7).has_value(),"scan review has no connection target");
    app::FtnRuntimeResponse snapshot;snapshot.code=app::FtnConfigurationCode::ok;snapshot.saved_revision=7;snapshot.scanned_messages=9007199254740993ULL;
    app::FtnSessionStatus session;session.serial=9007199254740993ULL;session.sent_bytes=UINT64_MAX;session.send={"test.zip",UINT64_MAX,9007199254740993ULL,true,true};snapshot.sessions.push_back(session);
    app::FtnQueuePage queue;queue.unscanned=UINT64_MAX;queue.ingress[3]=9007199254740993ULL;snapshot.queues=queue;
    const auto json=web::web_binkp_snapshot(snapshot);check(json.at("scanned_messages")=="9007199254740993"&&json.at("sessions")[0].at("serial")=="9007199254740993","64-bit IDs and counters stay decimal strings");
    check(json.at("sessions")[0].at("send").at("size")=="18446744073709551615"&&json.at("queues").at("unscanned")=="18446744073709551615","file and queue sizes retain all bits");
    check(json.at("sessions")[0].at("send").at("awaiting_ack")==true&&json.at("queues").at("ingress")[3]=="9007199254740993","ACK and held custody observations retained");
    snapshot.queues.reset();check(web::web_binkp_snapshot(snapshot).at("queues").is_null(),"unavailable queue is not fabricated empty");
    std::cout<<"PASS "<<checks<<" BinkP receipt and presentation checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
