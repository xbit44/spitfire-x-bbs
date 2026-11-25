#include "sfx/application/chat.hpp"
#include "sfx/application/chat_session.hpp"
#include "sfx/application/chat_notice.hpp"
#include <fstream>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace sfx::application;
namespace {
unsigned checks{};
void check(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
ChatParticipant caller(unsigned node,bool sysop=false){
    ChatParticipant p;p.identity.session.bytes[0]=static_cast<std::uint8_t>(node);p.identity.account=node;p.identity.node=static_cast<std::uint16_t>(node);
    p.level=sysop?500:10;p.sysop_threshold=500;p.sysop=sysop;p.name="Caller "+std::to_string(node);p.activity="Main Menu";p.safe=true;return p;
}
void routing(){
    ChatHub hub;ChatConfiguration config;config.rooms.push_back({2,"Games",true,{DoorSecurityMode::minimum,10,false}});
    check(hub.configure(config),"configure");auto a=caller(1),b=caller(2),admin=caller(3,true);admin.preferences.ghost=true;
    check(hub.attach(a)&&hub.attach(b)&&hub.attach(admin),"attach unique callers");check(!hub.attach(a),"refuse duplicate node");
    check(hub.visible(a.identity).size()==2,"Ghost hidden globally");check(hub.visible(admin.identity).size()==3,"Sysop sees true presence");
    hub.poll(a.identity,true);hub.poll(b.identity,true);hub.poll(admin.identity,true);
    check(hub.join(a.identity,1)==ChatResult::ok&&hub.join(admin.identity,1)==ChatResult::ok,"join");
    check(hub.visible(a.identity,true).size()==2,"Ghost visible in joined room");check(hub.visible(a.identity).size()==2,"global rule unchanged in room");
    check(hub.join(b.identity,2)==ChatResult::ok,"join separate room");hub.poll(a.identity,true);
    check(hub.text(a.identity,b.identity,"private",true)==ChatResult::unavailable,"whisper wrong room");
    check(hub.text(a.identity,b.identity,"private")==ChatResult::ok,"private global send");check(hub.poll(b.identity,false).empty(),"unsafe delivery deferred");
    const auto delivered=hub.poll(b.identity,true);check(delivered.size()==1&&delivered[0].text=="private","safe private delivery only recipient");check(hub.poll(admin.identity,true).empty(),"no private broadcast");
    b.preferences.dnd=true;check(hub.refresh(b),"set DND");check(hub.text(a.identity,b.identity,"no")==ChatResult::dnd,"ordinary DND blocked");
    check(hub.text(a.identity,b.identity,"no",false,true)==ChatResult::dnd,"caller cannot forge override");
    check(hub.text(admin.identity,b.identity,"yes",false,true)==ChatResult::ok,"Sysop one attempt override");check(hub.participant(b.identity)->preferences.dnd,"override preserves DND");hub.poll(b.identity,true);
    check(hub.invite(a.identity,b.identity,0).code==ChatResult::dnd,"DND refuses before wait");
    b.preferences.dnd=false;hub.refresh(b);hub.safe(b.identity,false);check(hub.invite(a.identity,b.identity,0).code==ChatResult::busy,"busy refuses");hub.safe(b.identity,true);
    const auto request=hub.invite(a.identity,b.identity,100);check(request.code==ChatResult::ok,"invite");check(hub.answer(b.identity,request.request,true,200)==ChatResult::ok,"consent");
    check(hub.peer(a.identity)==b.identity&&hub.peer(b.identity)==a.identity,"private pair");check(!hub.participant(a.identity)->room&&!hub.participant(b.identity)->room,"private leaves public rooms");
    hub.poll(a.identity,true);hub.poll(b.identity,true);check(hub.say(a.identity,"hello")==ChatResult::ok,"private speech");check(hub.poll(b.identity,true)[0].text=="Caller 1: hello","private prefix");check(hub.poll(admin.identity,true).size()==1,"only public leave visible to third party");
    hub.end(a.identity);check(!hub.peer(a.identity)&&!hub.peer(b.identity),"both end");check(hub.participant(a.identity)->room==1&&hub.participant(b.identity)->room==2,"restore own rooms");
    hub.poll(a.identity,true);hub.poll(b.identity,true);const auto late=hub.invite(a.identity,b.identity,1000);hub.tick(61000);check(hub.answer(b.identity,late.request,true,61001)==ChatResult::unavailable,"late accept refused");
    const auto cancelled=hub.invite(a.identity,b.identity,70000);hub.cancel(a.identity,cancelled.request);check(hub.answer(b.identity,cancelled.request,true,70001)==ChatResult::unavailable,"cancelled accept refused");
    hub.detach(b.identity);auto replacement=caller(2);replacement.identity.session.bytes[1]=1;check(hub.attach(replacement),"node reused");check(hub.text(a.identity,b.identity,"stale")==ChatResult::unavailable,"no reroute to replacement");
    bool committed=false;config.rooms[0].name="Renamed";check(hub.change_configuration(config,[&]{committed=true;return true;})==ChatResult::busy&&!committed,"occupied prevents commit");
    hub.leave(a.identity);hub.leave(admin.identity);check(hub.change_configuration(config,[&]{committed=true;return true;})==ChatResult::ok&&committed,"empty edit commits");check(hub.configuration().revision==2,"advance revision");
    config.revision=2;config.rooms.erase(config.rooms.begin());check(hub.change_configuration(config,[]{return true;})==ChatResult::invalid,"entry cannot delete");
}
void formats(){
    ChatNoticeRequest notice;notice.text="Controlled notice";const auto notice_bytes=encode_chat_notice(notice);check(!notice_bytes.empty()&&decode_chat_notice(notice_bytes)->text==notice.text,"bounded all-caller notice roundtrip");
    notice.text="bad\ncontrol";check(encode_chat_notice(notice).empty(),"notice rejects newline injection");notice.text=std::string(513,'x');check(encode_chat_notice(notice).empty(),"notice enforces maximum");
    for(std::size_t length=0;length<notice_bytes.size();++length)check(!decode_chat_notice({notice_bytes.begin(),notice_bytes.begin()+length}),"partial notice frame refused");
    ChatConfiguration config;check(config.rooms[0].access.level==10&&config.exclude_sysop_statistics,"defaults");const auto bytes=encode_chat_configuration(config);check(!bytes.empty(),"encode");const auto decoded=decode_chat_configuration(bytes);check(decoded&&encode_chat_configuration(*decoded)==bytes,"roundtrip");
    for(std::size_t n=0;n<bytes.size();++n)check(!decode_chat_configuration({bytes.begin(),bytes.begin()+n}),"truncated rejected");
    auto extra=bytes;extra.push_back(0);check(!decode_chat_configuration(extra),"trailing rejected");config.commands.push_back({MenuId::main,'C',1,"Door",true,{}});check(!valid_chat_configuration(config),"built-in protected");config.commands[0].key='K';check(valid_chat_configuration(config),"unused letter valid");config.commands.push_back(config.commands[0]);check(!valid_chat_configuration(config),"same-menu duplicate refused");
    check(chat_safe_text("a\x1b[2J\r\nb")=="a[2Jb","controls stripped");check(chat_node(2)=="02"&&chat_node(2,128)=="002","node width");
    check(valid_pchat_frame(stock_pchat_frame()),"stock frame valid");check(!valid_pchat_frame("\x1b[2;3Hsecret"),"art cannot overwrite live header");check(!valid_pchat_frame("\x1b[24;1H"),"row24 reserved");check(!valid_pchat_frame("\x1b[1;80H"),"column80 reserved");check(!valid_pchat_frame("\x1b[1;23r"),"no scroll takeover");
    const auto emote=parse_chat_emotes("wave|TARGET|{caller} waves to {target}\n");check(emote.size()==1,"case insensitive emote");check(render_chat_emote(emote[0],"{target}","Other")=="{target} waves to Other","substitution text stays inert");
    std::ifstream file("distribution/Display/chat-emote.txt",std::ios::binary);const std::string stock((std::istreambuf_iterator<char>(file)),{});check(parse_chat_emotes(stock).size()==33,"all33 emotes retained");
}
void bounded_invitation_queue(){
    ChatHub hub;auto a=caller(1),b=caller(2);check(hub.attach(a)&&hub.attach(b),"bounded invitation callers attached");hub.poll(a.identity,true);hub.poll(b.identity,true);
    const auto request=hub.invite(a.identity,b.identity,100);check(request.code==ChatResult::ok,"bounded invitation pending");
    for(unsigned n=0;n<64;++n){check(hub.text(b.identity,a.identity,"Queued "+std::to_string(n))==ChatResult::ok,"bounded accepted text");check(hub.poll_request(a.identity,request.request).empty(),"wait does not drain queued text");}
    check(hub.text(b.identity,a.identity,"overflow")==ChatResult::full,"invitation cannot grow a second text queue");
    hub.cancel(a.identity,request.request);check(hub.poll_request(a.identity,request.request+1).empty(),"unrelated request cannot consume lifecycle event");
    const auto ended=hub.poll_request(a.identity,request.request);check(ended.size()==1&&ended[0].kind==ChatEventKind::ended,"cancel bypasses full text queue through reserved control space");
    const auto texts=hub.poll(a.identity,true);check(texts.size()==64&&texts.front().text=="Queued 0"&&texts.back().text=="Queued 63","all accepted text preserved in order until safe delivery");
    check(hub.text(b.identity,a.identity,"after drain")==ChatResult::ok,"queue admits new text after safe drain");hub.poll(a.identity,true);
    a.sysop=true;a.preferences.ghost=true;hub.refresh(a);b.sysop=true;hub.refresh(b);
    check(hub.text(b.identity,a.identity,"privileged queued text")==ChatResult::ok,"Sysop may initially address Ghost");
    b.sysop=false;hub.refresh(b);check(hub.poll(a.identity,true).empty(),"queued private delivery rechecks sender visibility after privilege loss");
}
}
namespace {
#include "chat_operator_checks.inc"
void private_limits(){
    {ChatHub unequal;auto longer=caller(1),shorter=caller(2);longer.private_chat_minutes=2;shorter.private_chat_minutes=1;
        check(unequal.attach(longer)&&unequal.attach(shorter),"unequal private limits attached");
        const auto pending=unequal.invite(longer.identity,shorter.identity,100);check(unequal.answer(shorter.identity,pending.request,true,200)==ChatResult::ok,"unequal limits consent");
        unequal.tick(180200);check(!unequal.peer(longer.identity)&&unequal.private_usage_millis(longer.identity,180200)==60000&&unequal.private_usage_millis(shorter.identity,180200)==60000,"late tick uses earliest peer deadline independent of node order");}
    ChatHub hub;auto a=caller(1),b=caller(2);a.private_chat_minutes=1;
    check(hub.attach(a)&&hub.attach(b),"limited private callers attached");hub.safe(b.identity,true);
    auto invite=hub.invite(a.identity,b.identity,100);check(hub.answer(b.identity,invite.request,true,500)==ChatResult::ok,"limited private consent");
    hub.tick(60499);check(hub.peer(a.identity).has_value()&&hub.private_usage_millis(a.identity,60499)==59999,"per-chat boundary not early");
    hub.tick(60500);check(!hub.peer(a.identity)&&!hub.peer(b.identity)&&hub.private_usage_millis(a.identity,60500)==60000,"per-chat ends both without disconnect");
    invite=hub.invite(a.identity,b.identity,70000);check(hub.answer(b.identity,invite.request,true,70100)==ChatResult::ok,"new chat receives new per-chat window");
    hub.end(a.identity,72100);check(hub.private_usage_millis(a.identity,80000)==62000&&hub.private_usage_millis(b.identity,80000)==62000,"elapsed counters accumulate only active time");
    b.private_seconds_remaining_today=0;hub.refresh(b);check(hub.invite(a.identity,b.identity,90000).code==ChatResult::denied,"daily limit refuses recipient before invite");
    b.private_seconds_remaining_today=20;hub.refresh(b);invite=hub.invite(a.identity,b.identity,100000);b.private_seconds_remaining_today=0;hub.refresh(b);
    check(hub.answer(b.identity,invite.request,true,100100)==ChatResult::denied&&!hub.invite_pending(a.identity,invite.request),"daily limit rechecked at consent");
    b.private_seconds_remaining_today=1;hub.refresh(b);invite=hub.invite(a.identity,b.identity,110000);check(hub.answer(b.identity,invite.request,true,110100)==ChatResult::ok,"daily remaining permits chat");
    b.private_seconds_remaining_today=0;hub.refresh(b);hub.tick(111100);check(!hub.peer(a.identity)&&hub.private_usage_millis(a.identity,111100)==63000,"daily exhaustion restores both contexts");
    b.private_seconds_remaining_today.reset();a.private_chat_minutes.reset();hub.refresh(a);hub.refresh(b);invite=hub.invite(a.identity,b.identity,120000);check(hub.answer(b.identity,invite.request,true,120100)==ChatResult::ok,"unlimited defaults restored");hub.tick(7200100);check(hub.peer(a.identity).has_value(),"unlimited private chat remains connected");
    hub.detach(b.identity,7201100);check(!hub.peer(a.identity)&&hub.private_usage_millis(a.identity,7201100)==7144000,"disconnect charges active private time once");
    hub.operator_available(true);Identifier128 view;view.bytes[0]=71;auto op=hub.operator_start(a.identity,view,0,false,7202100);hub.safe(a.identity,true);
    // Operator/Page chat has its separate time policy and no Node Chat charge.
    if(op.code!=ChatResult::ok)op=hub.operator_start(a.identity,view,0,false,7202200);
    check(op.code==ChatResult::ok&&hub.claim_operator(a.identity,7202201),"operator chat allowed after Node Chat limit");
    hub.operator_end(op.token,view,7203000);check(hub.private_usage_millis(a.identity,7203000)==7144000,"operator chat never charged to private Node Chat counter");
}
}
namespace {
#include "chat_followup_checks.inc"
}
int main(){try{routing();formats();bounded_invitation_queue();operators();operator_codecs();private_limits();followup_checks();std::cout<<"PASS "<<checks<<" Chat model/session checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
