#include "sfx/application/ftn_mail.hpp"
#include <iostream>
#include <stdexcept>
using namespace sfx::application;
namespace {
unsigned checks{};void check(bool v,const char* s){++checks;if(!v)throw std::runtime_error(s);}
FtnConfiguration config(){
    FtnConfiguration c;FtnNetwork n;n.code="TEST";n.name="Test only";n.address="2:100/1@test";n.enabled=true;c.networks.push_back(n);
    FtnLink l;l.code="PEER";l.network=n.code;l.name="Test peer";l.address="2:100/2@test";l.enabled=true;c.links.push_back(l);
    c.file_echoes.push_back({n.code,"FILES","Test files",42});c.permissions.push_back({n.code,l.code,"FILES",true,true,true,false});return c;
}
std::string tic(){return "Area FILES\r\nOrigin 2:100/2@test\r\nFrom 2:100/2@test\r\nTo 2:100/1@test\r\nFile demo.zip\r\nSize 9\r\nCrc CBF43926\r\nDesc First description\r\nLdesc Full description\r\nPath 2:100/2@test 1730000000 retained text\r\nSeenby 2:100/2@test\r\nX-Unknown retained\r\n";}
void policies(){
    auto c=config();const auto a=*parse_ftn_address("2:100/2@test");auto peer=resolve_ftn_peer(c,{a});check(peer&&peer->network=="TEST"&&peer->link=="PEER","unique network-scoped peer selected");
    check(!resolve_ftn_peer(c,{a},"OTHER","PEER"),"outbound peer hint cannot switch network");
    auto duplicate=c.links[0];duplicate.code="OTHER";c.links.push_back(duplicate);check(!resolve_ftn_peer(c,{a},"TEST","PEER"),"ambiguous AKA cannot be resolved by first or hint");c.links.pop_back();
    auto other=c.networks[0];other.code="SECOND";other.address="2:100/1@second";c.networks.push_back(other);duplicate.network="SECOND";duplicate.address="2:100/2@second";c.links.push_back(duplicate);
    check(resolve_ftn_peer(c,{a})->network=="TEST"&&!resolve_ftn_peer(c,{*parse_ftn_address("2:100/2")}),"domain-aware isolation; domainless collision refused");
    const auto second_aka=*parse_ftn_address("2:100/2@second");
    const auto outbound=resolve_ftn_peer(c,{second_aka,a},"TEST","PEER");
    check(outbound&&outbound->network=="TEST"&&outbound->link=="PEER","outbound selection keeps its link when a hub advertises another network");
    check(!resolve_ftn_peer(c,{second_aka,a}),"inbound claims that match two links remain ambiguous");
    check(!resolve_ftn_peer(c,{second_aka},"TEST","PEER"),"foreign AKA alone cannot select the outbound link");
    check(!resolve_ftn_peer(c,{*parse_ftn_address("2:100/2")},"TEST","PEER"),"one claim matching two links remains ambiguous outbound");
    check(ftn_echo_permission(c,"test","peer","files",true,true)&&!ftn_echo_permission(c,"TEST","PEER","OTHER",true,true),"explicit echo rights only");
    c.links[0].enabled=false;check(!ftn_echo_permission(c,"TEST","PEER","FILES",true,true),"revocation checked at use");c.links[0].enabled=true;
    check(ftn_route_link(c,"TEST",a)=="PEER"&&!ftn_route_link(c,"TEST",*parse_ftn_address("2:100/1@test")),"direct configured routing never loops local");
    c.routes.push_back({"TEST","3:300/9@test","PEER",FtnMailFlavor::normal});check(ftn_route_link(c,"TEST",*parse_ftn_address("3:300/9@test"))=="PEER","explicit route accepted");
    check(!ftn_route_link(c,"TEST",*parse_ftn_address("3:300/10@test")),"unknown transit never relayed by default");
    for(const auto input:{"","a","abcdefgh","longer-password"}){check(equal_ftn_secret(input,input),"equal secret accepted");check(!equal_ftn_secret(input,std::string(input)+"x"),"secret length mismatch refused");}
}
void tics(){
    check(ftn_crc32("123456789")==0xcbf43926U&&ftn_crc32("")==0,"independent published CRC32 vectors");
    const auto decoded=decode_ftn_tic(tic(),"test");check(decoded.tic.has_value(),"independent TIC fixture");const auto& t=*decoded.tic;
    check(t.size==9&&t.crc==0xcbf43926U&&t.path[0].timestamp==1730000000&&t.descriptions[0]=="First description"&&t.unknown.size()==1,"TIC metadata preserved");
    const auto wire=encode_ftn_tic(t);check(wire&&decode_ftn_tic(*wire,"test").tic->long_descriptions==t.long_descriptions,"writer emits bounded metadata");
    for(const auto bad:{"File ../escape.zip\r\n","File CON.zip\r\n","Crc CBF43927\r\n","From 2:100/2@test\r\n","Area OTHER\r\n","Pw secret\nPw other\n"})check(!decode_ftn_tic(tic()+bad,"test").tic,"duplicate singleton or unsafe path refused");
    check(!decode_ftn_tic(std::string(65537,'x')).tic&&!decode_ftn_tic("Area FILES\nFile demo.zip\n").tic,"bounded TIC and mandatory sender");
    auto c=config();FtnPeerMatch peer{"TEST","PEER"};
    check(admit_ftn_tic(c,peer,t,"",true,false)==FtnTicAdmission::accepted,"intentionally passwordless TIC needs same-link native proof");
    check(admit_ftn_tic(c,peer,t,"",false,false)==FtnTicAdmission::transport_proof_required,"unverified empty password held");
    check(admit_ftn_tic(c,peer,t,"",true,true)==FtnTicAdmission::transport_proof_required,"stock external binkd cannot invent same-link proof");
    c.links[0].credentials.tic={1};check(admit_ftn_tic(c,peer,t,"",true,false)==FtnTicAdmission::wrong_password,"failed secret unprotect cannot become passwordless");
    auto supplied=t;supplied.password="correct";
    check(admit_ftn_tic(c,peer,supplied,"correct",false,true)==FtnTicAdmission::accepted,"explicit TIC password authenticates external ingress");
    supplied.password="wrong";check(admit_ftn_tic(c,peer,supplied,"correct",true,false)==FtnTicAdmission::wrong_password,"wrong TIC password cannot fall back to session");supplied.password="correct";
    supplied.to=*parse_ftn_address("2:100/8@test");check(admit_ftn_tic(c,peer,supplied,"correct",true,false)==FtnTicAdmission::wrong_destination,"TIC destination rechecked");supplied.to=t.to;
    supplied.path.push_back({*parse_ftn_address(c.networks[0].address),1730000001,{}});check(admit_ftn_tic(c,peer,supplied,"correct",true,false)==FtnTicAdmission::loop,"local Path loop held");
    auto compatibility=tic();const auto at=compatibility.find("Crc ");compatibility.erase(at,14);const auto no_crc=decode_ftn_tic(compatibility,"test");check(no_crc.tic&&!no_crc.tic->crc,"authorized reader permits absent CRC without claiming wire verification");check(!encode_ftn_tic(*no_crc.tic),"writer always requires CRC");
    auto injected=t;injected.descriptions={"valid\r\nPw injected"};check(!encode_ftn_tic(injected),"outbound description cannot inject singleton credentials");
    auto secret=t;secret.password="separate-tic-secret";const auto private_wire=encode_ftn_tic(secret);check(private_wire&&decode_ftn_tic(*private_wire,"test").tic->password==secret.password,"bounded writer preserves exact separate TIC credential");
    secret.password=" padded ";check(!encode_ftn_tic(secret),"writer refuses nonroundtripping credential whitespace");
    secret.password=std::string(1,static_cast<char>(0xff));check(!encode_ftn_tic(secret),"writer refuses nonwire credential bytes");
    auto oversized=t;oversized.long_descriptions.assign(17,std::string(4096,'x'));check(!encode_ftn_tic(oversized),"writer stops at bounded total metadata limit before secret insertion");
    auto extra_password=t;extra_password.unknown.push_back("Pw alternate");check(!encode_ftn_tic(extra_password),"extension metadata cannot supply a hidden alternate password");
}
void areafix(){
    const auto parsed=parse_ftn_areafix("%HELP\r\n%LIST\n%QUERY\n%UNLINKED\n%STATUS\n+GENERAL\n-OLD\n%RESCAN GENERAL 25\n%PAUSE\n%RESUME\n%PASSWORD secret\n---\nIgnored\n");
    check(parsed&&parsed->size()==11&&(*parsed)[5].kind==FtnAreaFixKind::add&&(*parsed)[7].rescan_count==25&&parsed->back().kind==FtnAreaFixKind::unsupported,"bounded explicit command matrix; unknown refused truthfully");
    for(const auto bad:{"%RESCAN GENERAL 1001","%RESCAN GENERAL 0","%RESCAN ../bad 5","%LIST unexpected","+GENERAL extra","+bad/tag"})check(!parse_ftn_areafix(bad),"bad command arguments refused");
    std::string many;for(unsigned i=0;i<257;++i)many+="%HELP\n";check(!parse_ftn_areafix(many),"AreaFix per-request command cap");
}
void display(){
    FtnPackedMessage message;message.text="AREA:GENERAL\r\1MSGID: 2:1/1 stable\r\1X-UNKNOWN retained privately\rLiteral @EXEC:never@\r\x1b[31mArt\x1b[0m\rSEEN-BY: 1/1\r";FtnMessageMetadata meta;meta.area="GENERAL";meta.charset="IBMPC 2";
    const auto source=ftn_message_display_source(message,meta);check(source&&source->format==MessageSourceFormat::ansi_canvas&&source->bytes=="Literal @EXEC:never@\r\x1b[31mArt\x1b[0m\r","render view preserves art and literal tokens, hides control/routing paragraphs");
    check(message.text.find("X-UNKNOWN")!=message.text.npos,"raw wire source remains untouched");
    message.text="\xc3\xa9";meta.charset="UTF-8 4";check(ftn_message_display_source(message,meta)->bytes==std::string(1,static_cast<char>(0x82)),"explicit representable UTF8 converted through shared CP437 codec");
    message.text="\xf0\x9f\x98\x80";const auto original=message.text;const auto fallback=ftn_message_display_source(message,meta);check(fallback&&fallback->bytes==std::string(1,static_cast<char>(0xa8))&&message.text==original,"unrepresentable Unicode uses the accepted visible CP437 rendition while immutable wire bytes remain exact");
    meta.charset="UTF-16 4";check(!ftn_message_display_source(message,meta),"unsupported charset never guessed");
}
}
void areafix_plans(){
    auto c=config();c.links[0].areafix_enabled=true;c.links[0].credentials.areafix={1};FtnMessageEcho echo;echo.network="TEST";echo.tag="PERMITTED";echo.description="permitted";echo.local_echo=42;c.message_echoes.push_back(echo);echo.tag="PRIVATE";c.message_echoes.push_back(echo);c.permissions.push_back({"TEST","PEER","PERMITTED",false,false,false,true});c.permissions.push_back({"TEST","PEER","PRIVATE",false,false,false,false});
    const auto commands=*parse_ftn_areafix("+PERMITTED\r%LIST\r%RESCAN PERMITTED 100\r%PAUSE\r");auto denied=plan_ftn_areafix(c,{"TEST","PEER"},commands,false,false);check(!denied.authorized&&!denied.changed&&denied.rescans.empty()&&!denied.configuration.permissions[1].send,"session alone cannot authorize AreaFix subscription/list/rescan");
    auto accepted=plan_ftn_areafix(c,{"TEST","PEER"},commands,true,false);check(accepted.authorized&&accepted.changed&&accepted.paused&&accepted.configuration.permissions[1].send&&accepted.configuration.permissions[1].manage&&!accepted.configuration.permissions[1].receive,"separate credential permits only own managed send subscription");check(accepted.rescans.size()==1&&accepted.rescans[0].rescan_count==100,"bounded permitted local echo rescan planned");check(accepted.response.find("PRIVATE")==std::string::npos&&accepted.response.find("PERMITTED")!=std::string::npos,"list does not reveal nonpermitted echo");check(!c.permissions[1].send,"planning never mutates caller configuration before commit");
    auto removed=plan_ftn_areafix(accepted.configuration,{"TEST","PEER"},*parse_ftn_areafix("-PERMITTED\r+PRIVATE\r%RESCAN PRIVATE 100\r%RESUME\r"),true,true);check(!removed.configuration.permissions[1].send&&removed.configuration.permissions[1].manage&&!removed.configuration.permissions[2].send&&removed.rescans.empty()&&!removed.paused,"drop preserves manage policy and denied private operations make no grant");
    c.links[0].credentials.areafix.clear();check(!plan_ftn_areafix(c,{"TEST","PEER"},commands,true,false).authorized,"unset AreaFix secret cannot become passwordless access");c.links[0].credentials.areafix={1};c.links[0].areafix_enabled=false;check(!plan_ftn_areafix(c,{"TEST","PEER"},commands,true,false).authorized,"explicit AreaFix disable dominates credential");c.links[0].areafix_enabled=true;
    check(!plan_ftn_areafix(c,{"OTHER","PEER"},commands,true,false).authorized,"network mismatch cannot select another link");auto pass=c.message_echoes[0];pass.tag="TRANSIT";pass.store_locally=false;pass.local_echo=0;c.message_echoes.push_back(pass);c.permissions.push_back({"TEST","PEER","TRANSIT",false,false,false,true});auto hub=c.links[0];hub.code="HUB";hub.address="2:100/3@test";hub.credentials.areafix={2};c.links.push_back(hub);c.links[0].forward_via_link="HUB";c.permissions.push_back({"TEST","HUB","TRANSIT",false,true,false,false});
    const auto add=*parse_ftn_areafix("+TRANSIT\r+TRANSIT\r");check(plan_ftn_areafix(c,{"TEST","PEER"},add,true,false).upstream.empty(),"forward missing echoes remains opt-in");c.links[0].forward_missing_echoes=true;const auto forward=plan_ftn_areafix(c,{"TEST","PEER"},add,true,false);check(forward.upstream.size()==1&&forward.upstream[0].link=="HUB"&&forward.configuration.message_echoes.back().local_echo==0&&forward.configuration.links[0].credentials.areafix!=forward.configuration.links[1].credentials.areafix,"known pass-through request deduplicates provider and preserves separate envelopes");check(forward.response.find("acceptance has not been observed")!=std::string::npos,"prepared request never claims upstream acceptance");
    c.permissions.back().receive=false;const auto refused=plan_ftn_areafix(c,{"TEST","PEER"},add,true,false);check(refused.upstream.empty()&&!refused.configuration.permissions.back().receive,"remote request cannot override owner-disabled upstream receive permission");
    c.permissions.back().receive=true;c.links[1].use=FtnConnectionUse::outbound_only;check(plan_ftn_areafix(c,{"TEST","PEER"},add,true,false).upstream.empty(),"provider requires inbound echo direction as well as outbound request");c.links[1].use=FtnConnectionUse::both;c.message_echoes.back().store_locally=true;c.message_echoes.back().local_echo=84;const auto stored=plan_ftn_areafix(c,{"TEST","PEER"},add,true,false);check(stored.upstream.size()==1&&stored.configuration.message_echoes.back().local_echo==84,"existing local conference is not upstream membership and is never replaced");c.links[1].forward_missing_echoes=true;c.links[1].forward_via_link="PEER";check(plan_ftn_areafix(c,{"TEST","PEER"},add,true,false).upstream.empty(),"cyclic upstream forwarding refused");c.links[1].forward_missing_echoes=false;
    auto count=*parse_ftn_areafix("+PERMITTED\r%RESCAN PERMITTED 1000\r%RESCAN PERMITTED 1\r");check(plan_ftn_areafix(c,{"TEST","PEER"},count,true,false).rescans.size()==1,"per-request rescan maximum cannot multiply through commands");
}
int main(){try{policies();tics();areafix();areafix_plans();display();std::cout<<"PASS "<<checks<<" FTN policy/TIC/AreaFix checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
