#include "sfx/application/event_sounds.hpp"
#include "sfx/application/operational_log.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
namespace app=sfx::application;
unsigned checks{};
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
app::Identifier128 id(unsigned n){app::Identifier128 value;value.bytes[0]=static_cast<std::uint8_t>(n);return value;}
void classification(){
    using K=app::OperationalKind;using S=app::SoundEvent;
    const K loud[]={K::assignment_quarantined,K::runtime_failed,K::listener_failed,K::durable_journal_degraded,K::backup_failed,K::feed_update_failed,K::message_maintenance_problem,K::file_maintenance_problem};
    for(unsigned i=0;i<=static_cast<unsigned>(K::file_maintenance_problem);++i){
        app::OperationalEvent e;e.kind=static_cast<K>(i);bool expected{};for(auto k:loud)expected=expected||k==e.kind;
        require(app::operational_sound_event(e)==(expected?std::optional<S>(S::operational_alert):std::nullopt),"exact operational allowlist; routine status/refusals silent");
    }
    app::OperationalEvent e;e.kind=K::authenticated;e.context.account_id=42;
    e.context.transport=app::OperationalTransport::ssh;require(app::operational_sound_event(e)==S::ssh_login,"authenticated SSH");
    e.context.transport=app::OperationalTransport::telnet;require(app::operational_sound_event(e)==S::telnet_login,"authenticated Telnet");
    e.context.account_id=0;require(!app::operational_sound_event(e),"no unauthenticated login");
    e.kind=K::session_failed;e.outcome=app::OperationalOutcome::storage_failure;require(app::operational_sound_event(e)==S::operational_alert,"typed storage failure");
    e.kind=K::file_maintenance_problem;
    for(const auto code:{"clamav_detection_deleted","clamav_detection_retained"}){e.context.diagnostic_code=code;require(!app::operational_sound_event(e),"malware not double routed");}
}
void stream(){
    using S=app::SoundEvent;app::EventSoundStream stream(id(1));app::EventSoundConfiguration config;config.paths[0]="C:\\Sounds\\login.wav";
    app::SoundPollRequest a{id(2),{},0,false};
    stream.publish(S::telnet_login,100);
    auto first=stream.poll(a,config,101);require(first.owner&&first.events.empty()&&first.through==1&&first.configuration==config,"attach no history, authoritative mappings");
    a.epoch=first.epoch;a.after=first.through;
    stream.publish(S::telnet_login,200);stream.publish(S::telnet_logout,201);stream.publish(S::telnet_login,202);
    auto batch=stream.poll(a,config,203);require(batch.events.size()==3&&batch.through==4,"distinct logins not coalesced");
    require(stream.poll(a,config,204).events.empty(),"duplicate poll cannot replay");
    app::SoundPollRequest b{id(3),id(1),0,false};require(!stream.poll(b,config,205).owner,"single active audio owner");
    require(!stream.poll({},config,205).owner,"invalid identity rejected");
    a.after=batch.through;
    stream.publish(S::security_alert,1000);stream.publish(S::security_alert,1001);stream.publish(S::operational_alert,1001);
    batch=stream.poll(a,config,1002);require(batch.events.size()==2,"same alert coalesced, categories independent");a.after=batch.through;
    stream.publish(S::page_sysop,1003);batch=stream.poll(a,config,4004);require(batch.events.empty(),"old live item expires");
    stream.publish(S::ssh_login,10000);batch=stream.poll(a,config,10001);require(batch.events.empty(),"lease expiry drops missed history");a.after=batch.through;
    stream.publish(S::ssh_logout,10002);batch=stream.poll(a,config,10003);require(batch.events.size()==1,"live events resume");
    b.after=0;batch=stream.poll(b,config,16000);require(batch.owner&&batch.events.empty(),"new owner after expiry no replay");
    for(unsigned i=0;i<100;++i)stream.publish(S::rlogin_entry,16001+i);
    b.after=batch.through;batch=stream.poll(b,config,16101);require(batch.events.size()==32,"stream memory bounded");
    require(batch.events.front().sequence<batch.events.back().sequence,"ordered delivery");
    b.after=batch.through+100;require(stream.poll(b,config,16102).events.empty(),"future cursor no replay");
    app::EventSoundStream restarted(id(4));batch=restarted.poll(b,config,16103);require(batch.epoch==id(4)&&batch.events.empty(),"Runtime epoch restart");
    app::EventSoundStream startup(id(5));startup.publish(S::operational_alert,50,true);
    a={id(2),{},0,true};batch=startup.poll(a,config,51);require(batch.events.size()==1,"fresh Desktop receives own prerequisite failure");
    a.epoch=batch.epoch;require(startup.poll(a,config,52).events.empty(),"initial failure delivered once");
    require(startup.poll({id(3),{},0,true},config,6000).events.empty(),"later desktop cannot replay startup");
    app::EventSoundStream simultaneous(id(6));a={id(2),{},0,false};batch=simultaneous.poll(a,config,1);a.epoch=batch.epoch;
    std::thread x([&]{for(unsigned i=0;i<100;++i)simultaneous.publish(S::ssh_login,2);});
    std::thread y([&]{for(unsigned i=0;i<100;++i)simultaneous.publish(S::telnet_login,2);});x.join();y.join();
    batch=simultaneous.poll(a,config,3);require(batch.through==200&&batch.events.size()==32,"concurrent callers bounded and sequence safe");
}
void codecs(){
    app::SoundPollRequest request{id(1),id(2),9,true};auto wire=app::encode_sound_poll_request(request);
    require(wire.size()==41&&app::decode_sound_poll_request(wire).has_value(),"request roundtrip");
    for(std::size_t n=0;n<wire.size();++n)require(!app::decode_sound_poll_request({wire.begin(),wire.begin()+n}),"truncated request rejected");
    wire.push_back(0);require(!app::decode_sound_poll_request(wire),"request trailing bytes");wire.pop_back();wire.back()=2;require(!app::decode_sound_poll_request(wire),"invalid bool");
    request.owner={};require(app::encode_sound_poll_request(request).empty(),"null owner encode rejected");
    app::SoundPollSnapshot snapshot;snapshot.owner=true;snapshot.epoch=id(2);snapshot.through=4;
    snapshot.configuration.paths[9]="C:\\Sounds\\binkp.wav";
    snapshot.events={{3,900,app::SoundEvent::ssh_login},{4,901,app::SoundEvent::ssh_logout}};
    wire=app::encode_sound_poll_snapshot(snapshot);auto decoded=app::decode_sound_poll_snapshot(wire);
    require(decoded&&decoded->events.size()==2&&decoded->configuration==snapshot.configuration&&decoded->through==4,"snapshot roundtrip");
    for(std::size_t n=0;n<wire.size();++n)require(!app::decode_sound_poll_snapshot({wire.begin(),wire.begin()+n}),"truncated snapshot rejected");
    wire.push_back(0);require(!app::decode_sound_poll_snapshot(wire),"snapshot trailing bytes");wire.pop_back();wire.back()=255;require(!app::decode_sound_poll_snapshot(wire),"invalid event kind");
    snapshot.owner=false;require(app::encode_sound_poll_snapshot(snapshot).empty(),"unauthorized events rejected");snapshot.owner=true;
    snapshot.events[1].sequence=3;require(app::encode_sound_poll_snapshot(snapshot).empty(),"duplicate sequence rejected");snapshot.events[1].sequence=5;require(app::encode_sound_poll_snapshot(snapshot).empty(),"future sequence rejected");
    snapshot.events.clear();snapshot.configuration.paths[0]="\\\\server\\a.wav";require(app::encode_sound_poll_snapshot(snapshot).empty(),"unsafe path rejected");
}
}
int main(){try{classification();stream();codecs();std::cout<<"PASS "<<checks<<" event sound checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
