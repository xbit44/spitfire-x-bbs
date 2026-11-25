#include "caller_runtime_accounting.hpp"
#include <iostream>
#include <stdexcept>

namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F> bool fails(F&& operation){try{operation();return false;}catch(const std::runtime_error&){return true;}}
app::Identifier128 id(unsigned n){app::Identifier128 value;value.bytes[0]=static_cast<std::uint8_t>(n);return value;}
struct Clock final:app::IClock {
    std::uint64_t now{1000};
    std::int64_t unix_seconds() const noexcept override{return 1794139200+static_cast<std::int64_t>(now/1000);}
    std::uint64_t monotonic_millis() const noexcept override{return now;}
};
app::ChatParticipant participant(unsigned n){
    app::ChatParticipant value;value.identity={id(n),n,static_cast<std::uint16_t>(n)};
    value.name="Caller "+std::to_string(n);value.level=10;value.safe=true;return value;
}
void run(){
    Clock clock;app::ChatHub chat;const auto caller=participant(1),peer=participant(2);
    check(chat.attach(caller)&&chat.attach(peer),"real ChatHub participants");
    unsigned writes{};std::int64_t private_seconds{};bool fail_persist=false;
    rt::CallerRuntimeAccounting accounting(clock.now,[](std::int64_t)->std::optional<std::string>{return "2026-11-08 12:00:00";},clock,chat,
        [&](app::Identifier128 session,std::int64_t seconds,const auto& days){
            check(session==caller.identity.session&&days.size()==1&&days[0].first=="2026-11-08"&&days[0].second==seconds,"private usage carries exact session and BBS local day");
            if(fail_persist)return app::StorageStatus{app::StorageErrorCode::io,"fixture private write failed"};
            ++writes;private_seconds=seconds;return app::StorageStatus::success();
        });
    clock.now=11000;auto snapshot=accounting.snapshot(caller.identity,false);
    check(snapshot.total_seconds==10&&snapshot.daily_seconds.size()==1&&snapshot.daily_seconds[0].second==10&&!snapshot.outcome,"normal monotonic time charged once");
    const auto op=chat.operator_start(caller.identity,id(3),0,false,clock.now);
    check(op.code==app::ChatResult::ok&&chat.claim_operator(caller.identity,clock.now),"operator takeover claimed at safe boundary");
    clock.now=16000;snapshot=accounting.snapshot(caller.identity,false);
    check(snapshot.total_seconds==10,"operator-initiated Chat pauses calling charge");
    chat.operator_end(op.token,id(3),clock.now);clock.now=17000;
    check(accounting.snapshot(caller.identity,false).total_seconds==11,"calling charge resumes without billing operator pause");
    const auto invite=chat.invite(caller.identity,peer.identity,clock.now);
    check(invite.code==app::ChatResult::ok&&chat.answer(peer.identity,invite.request,true,clock.now)==app::ChatResult::ok,"private caller Chat accepted");
    clock.now=19000;snapshot=accounting.snapshot(caller.identity,false);
    check(snapshot.total_seconds==13&&writes==1&&private_seconds==2,"private Chat charges calling time and independent private allowance");
    (void)accounting.snapshot(caller.identity,false);
    check(writes==1,"unchanged snapshot does not duplicate private accounting write");
    clock.now=20000;fail_persist=true;
    check(fails([&]{(void)accounting.snapshot(caller.identity,false);}),"private accounting failure cannot become successful finalization");
    fail_persist=false;snapshot=accounting.snapshot(caller.identity,false);
    check(snapshot.total_seconds==14&&writes==2&&private_seconds==3,"failed private checkpoint remains retryable without double charge");
    chat.end(caller.identity,clock.now);clock.now=21000;snapshot=accounting.snapshot(caller.identity,true);
    check(snapshot.total_seconds==15&&writes==2&&snapshot.outcome==app::SessionOutcomeCode::timed_out,"timeout outcome retained and ended private Chat stops charging");
    clock.now=0;check(fails([&]{(void)accounting.snapshot(caller.identity,false);}),"backwards monotonic clock refuses uncertain accounting");
    Clock other;rt::CallerRuntimeAccounting unavailable(other.now,[](std::int64_t)->std::optional<std::string>{return {};},other,chat,
        [](app::Identifier128,std::int64_t,const auto&){return app::StorageStatus::success();});
    other.now=3000;check(fails([&]{(void)unavailable.snapshot({},false);}),"missing BBS local date fails instead of assigning a guessed day");
}
}
int main(){try{run();std::cout<<"PASS "<<checks<<" shared caller accounting checks\n";return 0;}
catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
