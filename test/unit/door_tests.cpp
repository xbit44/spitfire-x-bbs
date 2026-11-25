#include "sfx/application/door.hpp"
#include "sfx/application/rlogin.hpp"
#include "sfx/application/caller_login_secret.hpp"
#include "sfx/application/caller_assignment.hpp"
#include "sfx/application/time.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace app=sfx::application;
namespace {
unsigned checks{};
void check(bool v,const char* message){++checks;if(!v)throw std::runtime_error(message);}
app::DoorDefinition door(std::uint16_t id=1){app::DoorDefinition d;d.id=id;d.name="Example Door";d.enabled=true;d.rlogin.host="example.invalid";return d;}
struct Bridge {
    std::uint64_t now{};std::vector<std::uint8_t> input,output;std::string sent,displayed;
    bool end{},stall{},trickle{};unsigned keyboard{};std::size_t cursor{},remote_cursor{};
    app::RloginBridgePorts ports(){
        using O=app::InteractiveStreamOutcome;
        app::RloginBridgePorts p;
        p.now=[&]{return now;};p.wait=[&]{now+=10;};p.keyboard=[&]{++keyboard;};
        p.stop=[&]()->std::optional<app::RloginEnd>{if(now>10000)return app::RloginEnd::operator_terminated;return {};};
        p.caller_read=[&](std::size_t n){if(cursor==input.size())return app::InteractiveStreamReadResult{O::timed_out,{}};
            n=std::min(n,input.size()-cursor);std::vector<std::uint8_t> b(input.begin()+static_cast<std::ptrdiff_t>(cursor),input.begin()+static_cast<std::ptrdiff_t>(cursor+n));cursor+=n;return app::InteractiveStreamReadResult{O::completed,std::move(b)};};
        p.remote_read=[&](std::size_t n){if(remote_cursor==output.size())return app::InteractiveStreamReadResult{end?O::disconnected:O::timed_out,{}};
            n=std::min(n,output.size()-remote_cursor);std::vector<std::uint8_t> b(output.begin()+static_cast<std::ptrdiff_t>(remote_cursor),output.begin()+static_cast<std::ptrdiff_t>(remote_cursor+n));remote_cursor+=n;return app::InteractiveStreamReadResult{O::completed,std::move(b)};};
        p.remote_write=[&](std::string_view b){if(stall)return app::InteractiveStreamWriteResult{O::timed_out,0};const auto n=trickle?std::size_t(1):b.size();sent.append(b.data(),n);if(trickle)now+=1000;return app::InteractiveStreamWriteResult{O::completed,n};};
        p.caller_write=[&](std::string_view b){displayed+=b;return app::InteractiveStreamWriteResult{O::completed,b.size()};};return p;
    }
};
}
#include "door_owner_fixes.inc"
#include "door_followup.inc"
#include "door_local_programs.inc"
#include "door_category_delete.inc"
int main(){try{
    category_delete_model_checks();
    owner_policy_and_order_checks();
    followup_secret_checks();
    local_door_model_checks();
    app::DoorCatalog c;check(app::validate_door_catalog(c).valid() && c.doors.empty() && c.entries.empty(),"fresh root only, no test destinations");
    c.doors={door()};c.entries={{0,7,app::DoorEntryKind::door,1,"Play",""}};
    check(app::validate_door_catalog(c).valid(),"permanent definition and gapped placement");
    c.menus.push_back({1,"Games"});check(app::copy_door_entry(c,0,7,1,42),"copy shares definition");
    check(c.doors.size()==1 && c.entries.back().number==42 && c.entries.back().target==1,"no auto renumber or target duplication");
    check(app::duplicate_door_definition(c,1,2) && !c.doors.back().enabled && c.entries.size()==2,"duplicate disabled and unplaced");
    check(app::duplicate_door_menu(c,1,2,"Other Games") && c.entries.size()==3,"menu duplicate copies references only");
    c.entries.push_back({0,8,app::DoorEntryKind::submenu,1,"Games",""});
    c.menus.push_back({3,"Nested"});c.entries.push_back({1,99,app::DoorEntryKind::submenu,3,"Nested",""});
    check(app::validate_door_catalog(c).valid(),"root plus two submenu displays");
    auto bad=c;bad.entries.push_back({3,1,app::DoorEntryKind::submenu,2,"Too deep",""});check(!app::validate_door_catalog(bad).valid(),"reject fourth menu display");
    bad=c;bad.entries.push_back({3,1,app::DoorEntryKind::submenu,1,"Cycle",""});check(!app::validate_door_catalog(bad).valid(),"reject cycle");
    bad=c;bad.entries.push_back(c.entries.front());check(!app::validate_door_catalog(bad).valid(),"reject duplicate local number");
    bad=c;bad.doors.erase(bad.doors.begin());check(!app::validate_door_catalog(bad).valid(),"reject dead references");
    auto d=door();
    for(const auto mode:{app::DoorSecurityMode::minimum,app::DoorSecurityMode::maximum,app::DoorSecurityMode::exact}){
        d.security_mode=mode;d.security_level=40;
        for(std::uint16_t level=1;level<=999;++level){const bool expected=mode==app::DoorSecurityMode::minimum?level>=40:mode==app::DoorSecurityMode::maximum?level<=40:level==40;
            check(app::door_accessible(d,{level,90})==expected,"inclusive numeric policy over full domain");}
    }
    d.sysop_only=true;check(app::door_accessible(d,{90,90}) && !app::door_accessible(d,{89,90}),"configurable Sysop threshold replaces numeric rule");
    d.enabled=false;check(!app::door_accessible(d,{999,90}),"disabled still denied to Sysop");
    for(const auto level:{0,1000})check(!app::door_accessible(door(),{static_cast<std::uint16_t>(level),90}),"invalid caller domain denied");
    check(app::select_door_entry(c,c,0,7,{10,90}).has_value(),"fresh visible selection");bad=c;++bad.revision;
    check(!app::select_door_entry(c,bad,0,7,{10,90}),"stale catalog selection blocked");
    auto encoded=app::encode_door_catalog(c);auto decoded=app::decode_door_catalog(encoded);
    check(decoded && app::encode_door_catalog(*decoded)==encoded,"catalog lossless codec");
    for(std::size_t n=0;n<encoded.size();++n){auto short_record=encoded;short_record.resize(n);check(!app::decode_door_catalog(short_record),"all truncations rejected");}
    auto extra=encoded;extra.push_back(0);check(!app::decode_door_catalog(extra),"trailing bytes rejected");
    app::RloginCallerIdentity caller{"Caller",92,1700000000,"BOARD"};auto target=door().rlogin;
    target.server_identity=app::RloginIdentity::configured;target.server_name="Remote";target.terminal_type="xtrn=GAME";target.prompt_credential=true;
    auto frame=app::rlogin_handshake(target,caller,"RemotePass","",{});
    const char expected[]={0,'R','e','m','o','t','e','P','a','s','s',0,'R','e','m','o','t','e',0,'x','t','r','n','=','G','A','M','E',0};
    check(frame && frame->view()==std::string_view(expected,sizeof(expected)),"exact four-field traditional handshake");
    check(!app::rlogin_handshake(target,caller,"","",{}),"no credential fallback");
    target.authentication=app::RloginAuthentication::synchronet_caller_hash;
    std::string hashed_input;
    const auto hash=[&](std::string_view s)->std::optional<std::array<std::uint8_t,20>>{hashed_input=s;std::array<std::uint8_t,20> value{};value[0]=0xab;value[19]=0xff;return value;};
    frame=app::rlogin_handshake(target,caller,"Password","Pepper",hash);
    check(frame && hashed_input=="Password921700000000BOARDPepper","current Synchronet h concatenation with board salt");
    check(frame->view().substr(1,40)=="ab000000000000000000000000000000000000ff","lowercase forty-character SHA1 wire value");
    target.salt="SALT";frame=app::rlogin_handshake(target,caller,"Password","P",hash);
    check(frame && hashed_input=="Password921700000000SALTP","explicit salt replaces board salt");
    target.authentication=app::RloginAuthentication::synchronet_predefined_hash;
    check(!app::rlogin_handshake(target,caller,"Predefined","unexpected",hash),"H mode cannot silently use h pepper");
    frame=app::rlogin_handshake(target,caller,"Predefined","",hash);
    check(frame && hashed_input=="Predefined921700000000SALT","H uses configured password plus same caller metadata and empty pepper");
    const auto dimensions=app::rlogin_window_size(25,132);check(dimensions[0]==255 && dimensions[4]==0 && dimensions[5]==25 && dimensions[7]==132,"RFC window frame");
    {
        struct Clock final:app::IClock{std::int64_t unix_seconds()const noexcept override{return 1700000000;}std::uint64_t monotonic_millis()const noexcept override{return 1000;}} clock;
        app::Identifier128 epoch;epoch.bytes[0]=1;app::FourNodePool pool(epoch);
        auto first=pool.allocate("127.0.0.1"),second=pool.allocate("127.0.0.2");check(first.lease&&second.lease,"two ordinary independent caller assignments");
        app::CallerAssignment one(std::move(*first.lease),pool,clock,true,"127.0.0.1"),two(std::move(*second.lease),pool,clock,false,"127.0.0.2");
        one.authenticated("Caller",1);two.authenticated("Other",2);
        const auto old=one.begin_door(1,"Example Door"),peer=two.begin_door(1,"Example Door");check(old&&peer&&one.snapshot().state==sfx::core::NodeState::door&&two.snapshot().state==sfx::core::NodeState::door,"same shared Door supports simultaneous callers");
        const auto identity=one.identity();app::AlphaNodeCommandRequest command{identity.node,app::AlphaNodeCommandKind::terminate_door,identity.runtime_epoch,identity.generation,old};
        check(app::decode_node_command_request(app::encode_node_command_request(command)).has_value(),"operator command binds caller and Door launch generations");
        command.expected_door_launch=0;check(app::encode_node_command_request(command).empty(),"unbound Door termination rejected");command.expected_door_launch=old;command.command=app::AlphaNodeCommandKind::disconnect;check(app::encode_node_command_request(command).empty(),"Disconnect Caller remains a distinct command");
        check(one.request_door_termination(old)&&one.door_termination_requested(old)&&!two.door_termination_requested(peer)&&!one.stopping.load(),"Terminate Door neither disconnects caller nor terminates peer");
        one.end_door(old);check(one.snapshot().caller_connected&&one.snapshot().account_id==1&&!one.snapshot().door_id,"caller retained after Door end");
        const auto current=one.begin_door(2,"Next Door");check(current>old&&!one.request_door_termination(old),"stale termination cannot affect replacement Door");one.end_door(old);check(one.snapshot().door_launch_sequence==current,"stale cleanup cannot clear new launch");one.end_door(current);
        one.transport_closed();check(one.retire(true),"normal first assignment cleanup");auto replacement=pool.allocate_exact(identity.node,"127.0.0.1");check(replacement.lease.has_value(),"normal node reuse");
        app::CallerAssignment next(std::move(*replacement.lease),pool,clock,true,"127.0.0.1");next.authenticated("Next",3);const auto fresh=next.begin_door(1,"Replacement Caller");check(fresh&&!one.request_door_termination(old)&&!next.door_termination_requested(fresh),"old assignment cannot terminate replacement caller's Door");
        next.end_door(fresh);next.transport_closed();check(next.retire(true),"replacement cleanup");two.end_door(peer);two.transport_closed();check(two.retire(true)&&pool.snapshot().active==0,"all synthetic caller leases normally reaped");
    }
    Bridge b;b.input={27,'[','A',0x80,0xfe,13,'q',29,29,'x',29,'d'};auto result=app::run_rlogin_bridge(b.ports());
    check(result.end==app::RloginEnd::gateway_escape && b.sent.empty(),"gateway escape never sends escape or coalesced bytes to remote");
    b={};b.input={27,'[','A',0x80,0xfe,13,'q',29,29,'x'};result=app::run_rlogin_bridge(b.ports());
    const char keys[]={27,'[','A',static_cast<char>(0x80),static_cast<char>(0xfe),13,'q',29,'x'};
    check(b.sent==std::string(keys,sizeof(keys)) && b.keyboard==1,"ANSI/CP437/game keys pass byte-for-byte, doubled escape literal");
    b={};b.input={29};result=app::run_rlogin_bridge(b.ports());check(b.sent==std::string(1,29),"lone gateway prefix has bounded lookahead");
    b={};b.output.assign(2*1024*1024,0xfe);b.end=true;result=app::run_rlogin_bridge(b.ports());
    check(result.end==app::RloginEnd::remote_closed && b.displayed.size()==b.output.size() && result.inbound_high_water<=65536,"large remote output drains before exact-menu recovery");
    b={};b.input.assign(200000,'a');b.stall=true;result=app::run_rlogin_bridge(b.ports());
    check(result.end==app::RloginEnd::backpressure_timeout && result.outbound_high_water<=65536 && b.cursor<=65536,"bounded outbound pressure timeout without input loss/overflow");
    b={};b.input.assign(200000,'b');b.trickle=true;result=app::run_rlogin_bridge(b.ports());check(result.end==app::RloginEnd::backpressure_timeout,"trickle cannot extend oldest-byte deadline");
    b={};b.end=true;result=app::run_rlogin_bridge(b.ports());check(result.end==app::RloginEnd::remote_closed && !b.keyboard,"remote traffic/idle do not reset keyboard AFK");
    std::cout<<"PASS "<<checks<<" Door/RLogin checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
