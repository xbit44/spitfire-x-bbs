#include "linux_caller_registry.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
app::Identifier128 id(unsigned value){app::Identifier128 result;result.bytes[0]=static_cast<std::uint8_t>(value);return result;}
struct Clock final:app::IClock {
    std::int64_t unix_seconds() const noexcept override{return 1700000000;}
    std::uint64_t monotonic_millis() const noexcept override{return 1000;}
};
void run(){
    Clock clock;app::FourNodePool pool(id(1));
    std::vector<std::shared_ptr<app::CallerAssignment>> assignments;
    std::vector<app::NodeAssignmentIdentity> retired;
    bool fail_retirement=false,admission_stopped=false;
    rt::LinuxCallerRegistry* registry_owner=nullptr;
    rt::LinuxCallerRegistry registry(id(1),{
        [&](app::NodeAssignmentIdentity exact,std::uint32_t timeout){
            check(timeout>0&&timeout<=10000,"one bounded revocation deadline");
            retired.push_back(exact);
            if(fail_retirement)return false;
            for(const auto& assignment:assignments)if(assignment->identity()==exact){
                check(assignment->stopping.load(),"cancellation precedes retirement wait");
                // Test simulates a joined worker; native caller integration uses
                // LinuxCallerListener::wait_for_retirement for the real proof.
                return registry_owner->forget(exact,true)&&assignment->retire(true);
            }
            return false;
        },
        [&]{admission_stopped=true;pool.stop();}});
    registry_owner=&registry;
    const auto assign=[&]{
        auto allocation=pool.allocate("127.0.0.1");check(allocation.lease.has_value(),"real bounded node lease");
        auto caller=std::make_shared<app::CallerAssignment>(std::move(*allocation.lease),pool,clock,false,"127.0.0.1");
        assignments.push_back(caller);check(registry.begin(caller),"register before authentication");return caller;
    };
    const auto clean=[&](const auto& caller){check(registry.forget(caller->identity(),true)&&caller->retire(true),"checked cleanup releases exact assignment");};
    auto delayed=assign();
    check(registry.revoke({app::SessionRevocationKind::account_all,42,{}}),"password reset recorded before handoff");
    check(!registry.publish(delayed->identity(),42,id(10),false)&&delayed->stopping.load(),"delayed old authentication cannot enter menus");
    check(!registry.account_online(42),"refused identity never shown online");clean(delayed);
    check(!registry.begin(delayed),"retired generation cannot be registered again");
    auto first=assign(),second=assign();
    check(registry.publish(first->identity(),42,id(11),false)&&registry.publish(second->identity(),42,id(12),false),"fresh authentication after reset can publish");
    check(registry.account_online(42),"actual published account online");
    check(registry.revoke({app::SessionRevocationKind::account_except_current,42,id(11)}),"revoke other sessions waits for exact retirement");
    check(retired.size()==1&&retired.back()==second->identity()&&!first->stopping.load(),"current caller retained and other caller retired");
    auto replacement=assign();
    check(replacement->identity().node==second->identity().node&&replacement->identity().generation>second->identity().generation,"node reused only with later generation");
    check(registry.publish(replacement->identity(),43,id(13),false),"new caller publishes on reused node");
    check(!registry.chat_identity(second->identity()),"stale node generation cannot address replacement Chat participant");
    const auto chat_target=registry.chat_identity(replacement->identity());
    check(chat_target&&chat_target->account==43&&chat_target->session==id(13),"Chat target binds exact published account and session");
    check(!registry.forget(second->identity(),true)&&registry.account_online(43),"stale cleanup cannot erase replacement");
    clean(first);check(!registry.account_online(42),"finished account no longer online");
    const auto before_sy=registry.authorization_checkpoint();
    check(registry.revoke({app::SessionRevocationKind::sysop_level_all,{},{}}),"System Password reset recorded");
    check(!registry.refresh_access(replacement->identity(),43,id(13),true,before_sy)&&replacement->stopping.load(),"SY challenge started before reset cannot publish elevated authority");clean(replacement);
    auto sysop=assign(),ordinary=assign();
    check(registry.publish(sysop->identity(),44,id(14),true)&&registry.publish(ordinary->identity(),45,id(15),false),"distinct Sysop and ordinary authorities");
    const app::ChatIdentity exact_sysop{id(14),44,sysop->identity().node};
    check(registry.sysop_authorized(exact_sysop)&&!registry.sysop_authorized({id(15),45,ordinary->identity().node}),"background Sysop jobs require actual completed SY authority");
    check(!registry.sysop_authorized({id(14),45,sysop->identity().node})&&!registry.sysop_authorized({id(15),44,sysop->identity().node}),"background Sysop capability is bound to exact account and session");
    check(!registry.sysop_disconnect({id(15),45,ordinary->identity().node},exact_sysop)&&!sysop->stopping.load(),"ordinary caller cannot request Sysop disconnect");
    check(!registry.sysop_disconnect(exact_sysop,{id(99),45,ordinary->identity().node})&&!ordinary->stopping.load(),"stale target session cannot disconnect a replacement caller");
    const auto prior=retired.size();
    check(registry.revoke({app::SessionRevocationKind::sysop_level_all,{},{}}),"revoke all actual Sysop sessions");
    check(retired.size()==prior+1&&retired.back()==sysop->identity()&&!ordinary->stopping.load(),"ordinary caller unaffected by System Password revocation");
    check(!registry.sysop_authorized(exact_sysop),"retired session cannot retain background Sysop authority");
    clean(ordinary);
    auto disconnect_sysop=assign(),disconnect_target=assign();
    check(registry.publish(disconnect_sysop->identity(),55,id(31),true)&&registry.publish(disconnect_target->identity(),56,id(32),false),"exact disconnect fixture authority");
    check(registry.sysop_disconnect({id(31),55,disconnect_sysop->identity().node},{id(32),56,disconnect_target->identity().node})&&disconnect_target->stopping.load()&&!disconnect_sysop->stopping.load(),"Sysop disconnect cancels only the confirmed exact caller");
    clean(disconnect_target);clean(disconnect_sysop);
    auto parallel_one=assign(),parallel_two=assign();
    check(registry.publish(parallel_one->identity(),48,id(21),true)&&registry.publish(parallel_two->identity(),48,id(22),true),"prepare level999 parallel callers");
    check(registry.enforce_single_session(45),"unrelated absent account does not cancel callers");
    check(!registry.enforce_single_session(48)&&parallel_one->stopping.load()&&parallel_two->stopping.load(),"loss of parallel permission cancels every existing session");
    clean(parallel_one);clean(parallel_two);
    auto single=assign();check(registry.publish(single->identity(),48,id(23),false)&&registry.enforce_single_session(48)&&!single->stopping.load(),"one ordinary session retains access");clean(single);
    auto downgraded=assign();const auto proof_checkpoint=registry.authorization_checkpoint();
    check(registry.publish(downgraded->identity(),49,id(24),true),"publish actual initial Sysop proof");
    check(registry.refresh_account_authority(49,true,false,false)&&!downgraded->stopping.load(),"management demotion preserves sole caller but consumes SY proof");
    check(!registry.sysop_authorized({id(24),49,downgraded->identity().node}),"demotion immediately removes background Sysop capability");
    check(!registry.refresh_access(downgraded->identity(),49,id(24),true,proof_checkpoint)&&downgraded->stopping.load(),"worker cannot restore consumed proof before management handles re-promotion");clean(downgraded);
    auto promoted=assign();check(registry.publish(promoted->identity(),50,id(25),false),"ordinary caller for management promotion");
    check(registry.refresh_account_authority(50,true,true,true)&&promoted->stopping.load(),"management promotion cancels caller without initial SY proof");clean(promoted);
    auto expired=assign();bool history=true;
    for(unsigned n=0;n<=app::SessionRevocationLedger::capacity;++n)
        history=registry.revoke({app::SessionRevocationKind::account_all,99,{}})&&history;
    check(history&&!registry.publish(expired->identity(),46,id(16),false),"expired bounded history refuses uncertain delayed login");clean(expired);
    check(!registry.revoke({app::SessionRevocationKind::account_all,{},{}})&&registry.healthy(),"malformed revocation neither succeeds nor invents a recovery failure");
    auto uncertain=assign();check(registry.publish(uncertain->identity(),47,id(17),false),"prepare retirement failure");
    fail_retirement=true;
    check(!registry.revoke({app::SessionRevocationKind::account_all,47,{}}),"retirement timeout/failure cannot report success");
    check(!registry.healthy()&&admission_stopped&&pool.snapshot().stopped,"uncertain cleanup stops further admission");
    check(registry.account_online(47),"failed assignment retained for recovery");
    check(!registry.publish(uncertain->identity(),47,id(18),false),"quarantine cannot publish another session");
    // Fixture-only cleanup, after failure observations; no product recovery reset.
    clean(uncertain);
}
}
int main(){try{run();std::cout<<"PASS "<<checks<<" Linux caller revocation/publication checks\n";return 0;}
catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
