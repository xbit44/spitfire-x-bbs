#include "sfx/application/ftn.hpp"
#include "ftn_legacy_fixtures.hpp"
#include "sfx/application/ftn_configuration_transport.hpp"
#include "sfx/application/ftn_echo_import.hpp"
#include "sfx/application/ftn_hatch_catalog.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace sfx::application;
namespace {
unsigned checks{};
void check(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
FtnConfiguration own_network(){FtnConfiguration c;FtnNetwork n;n.code="MY-NET";n.name="My Own Network";n.address="42:100/1@my-net";n.enabled=true;c.networks.push_back(n);return c;}
void identities(){
    for(const auto s:{"1:2/3","65535:65535/65535.65535@network","42:1/0"})check(parse_ftn_address(s).has_value(),"valid FTN identity");
    for(const auto s:{"0:2/3","1:0/3","1:2/-3","1:2/3.65536","1:2/3/4","1:2/3@","1:2/3@net..name","1:2/3@net@other","1:2/3\n","1:2/3.1.2","65536:2/3"})check(!parse_ftn_address(s),"malformed identity refused");
    check(format_ftn_address(*parse_ftn_address("01:002/003.0@NET"))=="1:2/3@net","canonical identity");
    for(const auto s:{"../net","net/name","net.name","net:name","CON\\file","CON","prn","Lpt1","com9",""})check(!valid_ftn_code(s),"unsafe network path code refused");
    check(valid_ftn_code("my-net_2"),"stable path code");
    check(valid_ftn_echo_list_url("http://example.invalid/list.na")&&valid_ftn_echo_list_url("https://example.invalid/list.na?version=2"),"explicit HTTP HTTPS");
    for(const auto s:{"file:///list.na","http://user:secret@example.invalid/list","https:///list.na","https://example.invalid/a b","https://example.invalid/a\\b","http://example.invalid/#fragment"})check(!valid_ftn_echo_list_url(s),"unsafe list URL refused");
}
void policies(){
    auto c=own_network();check(validate_ftn_configuration(c).empty(),"complete own network without hub or echoes");
    c.networks[0].name="R\xc3\xa9seau \xd0\x9b";check(validate_ftn_configuration(c).empty(),"UTF8 names do not mistake continuation bytes for controls");
    c.networks[0].name="unsafe\xc2\x9b";check(!validate_ftn_configuration(c).empty(),"Unicode control injection refused");c=own_network();
    check(c.settings.mode==FtnMailerMode::native&&c.settings.allow_direct_local_netmail,"approved fresh defaults");
    check(c.networks[0].keep_processed&&c.networks[0].retention_days==30,"retention defaults");
    FtnLink link;link.code="downlink";link.network="MY-NET";link.name="Downlink";link.address="42:100/2@my-net";link.use=FtnConnectionUse::inbound_only;link.enabled=true;link.port=0;
    c.links.push_back(link);check(validate_ftn_configuration(c).empty(),"inbound only without outbound host");
    c.links[0].use=FtnConnectionUse::both;check(!validate_ftn_configuration(c).empty(),"outbound needs endpoint");
    c.links[0].host="host.invalid";c.links[0].port=24554;check(validate_ftn_configuration(c).empty(),"outbound endpoint valid");
    check(!ftn_areafix_enabled(c.links[0]),"fresh blank AreaFix off");
    c.links[0].credentials.areafix={1,2,3};check(ftn_areafix_enabled(c.links[0]),"password unspecified defaults on");
    c.links[0].areafix_enabled=false;check(!ftn_areafix_enabled(c.links[0]),"explicit Off retained");
    c.links[0].credentials.areafix={4,5,6};check(!ftn_areafix_enabled(c.links[0]),"password replacement preserves Off");
    c.links[0].areafix_enabled=true;c.links[0].credentials.areafix.clear();check(!validate_ftn_configuration(c).empty(),"enabled AreaFix requires own credential");
    c.links[0].areafix_enabled=false;
    auto other=c.networks[0];other.code="OTHER";other.name="Other network";c.networks.push_back(other);check(validate_ftn_configuration(c).empty(),"overlapping address belongs to explicit network");
    c.networks[1].code="my-net";check(!validate_ftn_configuration(c).empty(),"case duplicate stable network code refused");
    c=own_network();auto n=c.networks[0];n.code="unfinished";n.address.clear();n.enabled=false;c.networks.push_back(n);check(validate_ftn_configuration(c).empty(),"finish later disabled draft");
    c.networks.back().enabled=true;check(!validate_ftn_configuration(c).empty(),"incomplete identity cannot activate");
    c.networks.back().enabled=false;c.networks.back().address="42:";c.networks.back().echo_list_url="http://";
    check(validate_ftn_configuration(c).empty(),"finish later retains incomplete inert input");
    c.networks.back().enabled=true;check(!validate_ftn_configuration(c).empty(),"incomplete saved fields require correction before activation");
    c=own_network();c.networks[0].enabled=false;FtnLink unfinished;unfinished.code="peer";unfinished.network="MY-NET";unfinished.name="Peer";unfinished.enabled=true;unfinished.address="42:";c.links.push_back(unfinished);
    check(validate_ftn_configuration(c).empty(),"disabled setup preserves incomplete link intent");c.networks[0].enabled=true;
    check(!validate_ftn_configuration(c).empty(),"network activation revalidates all enabled connections");
}
void lists_and_wire(){
    auto list=parse_ftn_echo_list(";comment\r\nECHO.ONE First\r\necho.two Second\nECHO.ONE Duplicate\nBAD/TAG nope\n");
    check(list.complete&&list.entries.size()==2&&list.issues.size()==2,"list preview separates duplicates and invalid rows");
    check(list.entries[0].selected&&list.entries[1].selected&&list.entries[1].tag=="ECHO.TWO","all usable initially selected");
    std::string large;for(unsigned i=0;i<2400;++i)large+="ECHO."+std::to_string(i)+" Description\n";
    const auto many=parse_ftn_echo_list(large);check(many.entries.size()==2400&&many.complete,"large list not silently truncated to conference count");
    auto c=own_network();for(const auto& e:many.entries)c.message_echoes.push_back({"MY-NET",e.tag,e.description});
    check(c.message_echoes[0].access_level==10&&c.message_echoes[0].read_level==10&&c.message_echoes[0].post_level==10,"conference defaults10");
    auto bytes=encode_ftn_configuration(c);check(bytes.size()>65536,"large configuration must use bounded IPC pages");
    const auto decoded=decode_ftn_configuration(bytes);check(decoded&&encode_ftn_configuration(*decoded)==bytes,"large catalog roundtrip");
    c=own_network();bytes=encode_ftn_configuration(c);
    for(std::size_t i=0;i<bytes.size();++i)check(!decode_ftn_configuration({bytes.begin(),bytes.begin()+i}),"truncated configuration refused");
    bytes.push_back(0);check(!decode_ftn_configuration(bytes),"trailing bytes refused");
    bytes[0]=9;check(!decode_ftn_configuration(bytes),"unknown format refused");
    c.message_echoes.push_back({"MY-NET","LOCAL","Description"});FtnEchoPermission permission;permission.network="MY-NET";permission.link="missing";permission.tag="LOCAL";permission.receive=true;c.permissions.push_back(permission);check(!validate_ftn_configuration(c).empty(),"missing link cannot grant echo access");
}
void tls_codec_compatibility(){
    using namespace sfx::test::ftn_legacy;
    for(const auto* fixture:{&v2,&v3,&v4}){
        const auto c=decode_ftn_configuration(*fixture);check(c.has_value(),"frozen B legacy configuration fixture accepted");
        check(c->revision==1&&c->settings.port==24600&&c->settings.bind_address=="127.0.0.1"&&c->networks.size()==1&&c->networks[0].address=="42:100/1@legacy","legacy read preserves revision and custom classic endpoint");
        check(!c->settings.tls_listener_enabled&&c->settings.tls_port==24555,"legacy read does not enable TLS or mutate its new defaults");
        check(c->settings.tls_certificate==((*fixture)[0]>=3?"C:/Legacy/cert.pem":"")&&c->networks[0].description==((*fixture)[0]>=4?"Frozen B description":""),"defined legacy extension fields preserved exactly");
        const auto current=encode_ftn_configuration(*c);check(!current.empty()&&current[0]==7&&decode_ftn_configuration(current).has_value(),"explicit new encoding is version7");
    }
    auto undefined=v2;undefined[0]=1;check(!decode_ftn_configuration(undefined),"undefined version1 is not invented");
    auto c=own_network();check(!c.settings.tls_listener_enabled&&c.settings.tls_port==24555,"TLS is explicitly Off by default");
    c.settings.tls_listener_enabled=true;check(!validate_ftn_configuration(c).empty(),"enabled TLS listener needs certificate and key paths");
    c.settings.tls_certificate="C:/Fixture/cert.pem";c.settings.tls_private_key="C:/Fixture/key.pem";check(validate_ftn_configuration(c).empty(),"TLS configuration can save before native activation qualification");
    const auto encoded=encode_ftn_configuration(c);const auto decoded=decode_ftn_configuration(encoded);check(decoded&&decoded->settings.tls_listener_enabled&&decoded->settings.tls_port==24555,"explicit TLS5 roundtrip");
    const auto legacy6=encode_ftn_configuration_legacy(c,6);check(!legacy6.empty(),"legacy-compatible TLS encoding retained");
    auto malformed=legacy6;malformed[malformed.size()-6]=2;check(!decode_ftn_configuration(malformed),"nonboolean TLS flag rejected");malformed=legacy6;malformed[malformed.size()-5]=malformed[malformed.size()-4]=0;check(!decode_ftn_configuration(malformed),"zero TLS port rejected");
    auto v5=legacy6;v5.resize(v5.size()-3);v5[0]=5;const auto old5=decode_ftn_configuration(v5);check(old5&&old5->settings.tls_listener_enabled&&!old5->settings.msg_netmail_enabled&&old5->settings.msg_netmail_directory.empty(),"frozen v5-shaped configuration retains TLS and defaults MSG scanner Off");
    auto msg=c;msg.settings.msg_netmail_enabled=true;check(validate_ftn_configuration(msg).empty(),"retired shared MSG setting cannot block unrelated edits");msg.settings.msg_netmail_directory="D:\\IREX\\OUT";const auto msg_roundtrip=decode_ftn_configuration(encode_ftn_configuration(msg));check(msg_roundtrip&&msg_roundtrip->settings.msg_netmail_enabled&&msg_roundtrip->settings.msg_netmail_directory==msg.settings.msg_netmail_directory,"codec6 retains exact MSG directory and enable state");
        c.settings.tls_port=c.settings.port;check(!validate_ftn_configuration(c).empty(),"simultaneous classic and TLS ports cannot overlap");c.settings.listener_enabled=false;check(validate_ftn_configuration(c).empty(),"same saved port is valid when classic listener is disabled");
    c.settings.mode=FtnMailerMode::disabled;check(validate_ftn_configuration(c).empty()&&decode_ftn_configuration(encode_ftn_configuration(c))->settings.tls_listener_enabled,"Disabled retains dormant explicit TLS choice");
}
void publication(){
    auto durable=own_network();unsigned writes{},previews{};
    FtnConfigurationPublisher publisher([&]{return std::optional<FtnConfiguration>(durable);},[&](const FtnTransferToken&,const FtnConfiguration& c){
        FtnConfigurationResponse r;if(c.revision!=durable.revision){r.code=FtnConfigurationCode::conflict;return r;}
        durable=c;++durable.revision;++writes;r.code=FtnConfigurationCode::ok;r.revision=durable.revision;return r;
    },{},[&](const FtnTransferToken& token,const FtnConfiguration& c){check(token[0]==3&&c.revision==durable.revision&&c.message_echoes.size()==2400,"preview receives exact fully uploaded draft and token");++previews;return FtnConfigurationResponse{FtnConfigurationCode::ok,c.revision,0,1,{7}};});
    FtnConfigurationRequest q;q.token[0]=1;
    auto response=publisher.handle(q,0);check(response.code==FtnConfigurationCode::ok&&response.revision==1,"read pinned catalog");
    const auto request=encode_ftn_configuration_request(q);check(request.size()<65536&&decode_ftn_configuration_request(request).has_value(),"request frame bounded");
    check(decode_ftn_configuration_response(encode_ftn_configuration_response(response)).has_value(),"response frame roundtrip");
    for(std::size_t i=0;i<request.size();++i)check(!decode_ftn_configuration_request({request.begin(),request.begin()+i}),"truncated page envelope refused");
    auto other=q;other.token[0]=2;check(publisher.handle(other,1).code==FtnConfigurationCode::busy,"reader token isolation");
    auto next=durable;for(unsigned i=0;i<2400;++i)next.message_echoes.push_back({"MY-NET","TAG."+std::to_string(i),"Description"});
    const auto bytes=encode_ftn_configuration(next);q.operation=FtnConfigurationOperation::begin;q.token[0]=3;q.revision=1;q.total=static_cast<std::uint32_t>(bytes.size());
    check(publisher.handle(q,2).code==FtnConfigurationCode::ok,"begin bounded revision upload");
    q.operation=FtnConfigurationOperation::commit;check(publisher.handle(q,3).code==FtnConfigurationCode::invalid&&!writes,"missing pages cannot commit");
    q.operation=FtnConfigurationOperation::preview_retention;check(publisher.handle(q,3).code==FtnConfigurationCode::invalid&&!previews&&!writes,"partial upload cannot receive retention review");
    q.operation=FtnConfigurationOperation::append;q.bytes={bytes.begin(),bytes.begin()+kFtnConfigurationPageBytes};q.offset=1;
    check(publisher.handle(q,4).code==FtnConfigurationCode::conflict&&!writes,"out of order page refused");q.offset=0;
    auto now=std::uint64_t{5};
    for(std::size_t offset=0;offset<bytes.size();offset+=kFtnConfigurationPageBytes){
        q.offset=static_cast<std::uint32_t>(offset);const auto end=std::min(bytes.size(),offset+kFtnConfigurationPageBytes);q.bytes={bytes.begin()+offset,bytes.begin()+end};
        check(publisher.handle(q,now++).code==FtnConfigurationCode::ok,"upload page");
        check(publisher.handle(q,now++).code==FtnConfigurationCode::ok,"identical page retry idempotent");
    }
    q.operation=FtnConfigurationOperation::preview_retention;q.offset=0;q.bytes.clear();check(decode_ftn_configuration_request(encode_ftn_configuration_request(q)).has_value(),"retention preview request roundtrip");other=q;other.token[0]=4;check(publisher.handle(other,now++).code==FtnConfigurationCode::conflict&&!previews,"another token cannot review upload");check(publisher.handle(q,now++).bytes==std::vector<std::uint8_t>{7}&&previews==1&&!writes&&durable.revision==1,"review leaves saved configuration unchanged");check(publisher.handle(q,now++).code==FtnConfigurationCode::ok&&previews==2&&!writes,"lost preview reply retries same draft without committing");
    q.operation=FtnConfigurationOperation::commit;check(publisher.handle(q,now++).code==FtnConfigurationCode::ok&&writes==1&&durable.revision==2,"complete upload commits once");
    check(publisher.handle(q,now++).code==FtnConfigurationCode::conflict&&writes==1,"commit replay cannot duplicate mutation");
    q.operation=FtnConfigurationOperation::begin;q.revision=1;check(publisher.handle(q,now++).code==FtnConfigurationCode::conflict,"stale revision begin refused");
    q.revision=2;check(publisher.handle(q,now++).code==FtnConfigurationCode::ok,"next draft begins");
    q.operation=FtnConfigurationOperation::cancel;q.total=0;check(publisher.handle(q,now++).code==FtnConfigurationCode::ok&&writes==1,"cancel preserves durable catalog");
    q.operation=FtnConfigurationOperation::begin;q.total=static_cast<std::uint32_t>(bytes.size());check(publisher.handle(q,now++).code==FtnConfigurationCode::ok,"abandoned draft begins");
    q.operation=FtnConfigurationOperation::commit;check(publisher.handle(q,now+60001).code==FtnConfigurationCode::conflict&&writes==1,"abandoned draft expires");
}
}
void imports(){
    auto c=own_network();std::string text;for(unsigned i=0;i<2400;++i)text+="ECHO."+std::to_string(i)+" Description "+std::to_string(i)+"\n";
    auto list=parse_ftn_echo_list(text);std::vector<FtnEchoImportRequirements> requirements(list.entries.size());
    auto plan=plan_ftn_echo_import(c,"MY-NET",list,requirements,0,0,"My Messages",false);check(!plan.issues.empty()&&plan.selected==2400,"oversized selected import refuses complete plan rather than partial success");
    for(auto& e:list.entries)e.selected=false;
    list.entries[7].selected=list.entries[2301].selected=true;plan=plan_ftn_echo_import(c,"MY-NET",list,requirements,997,0,"My Messages",false);
    check(plan.issues.empty()&&plan.created==2&&plan.selected==2&&plan.configuration.message_echoes.size()==2,"only checked echoes enter the plan across hidden pages");check(plan.configuration.message_echoes[0].access_level==10&&plan.configuration.message_echoes[0].read_level==10&&plan.configuration.message_echoes[0].post_level==10,"new conference10/10/10");
    check(plan.configuration.message_echoes[0].ansi_colors==false&&!plan.configuration.message_echoes[0].require_real_name,"advanced import seeds colors Off and non-Fido Real Name Off");auto fido=c;fido.networks[0].name="FidoNet";const auto fido_plan=plan_ftn_echo_import(fido,"MY-NET",list,requirements,997,0,"My Messages",false);check(fido_plan.issues.empty()&&fido_plan.configuration.message_echoes[0].require_real_name&&fido_plan.configuration.message_echoes[0].ansi_colors==false,"advanced Fido import seeds Real Name On independently of ANSI");
    c=plan.configuration;c.message_echoes[0].ansi_colors=true;c.message_echoes[0].require_real_name=true;c.message_echoes[0].local_echo=91;c.message_echoes[0].local_name="Keep Custom Name";c.message_echoes[0].access_level=50;c.message_echoes[1].local_echo=92;list.entries[7].description="Changed description";
    plan=plan_ftn_echo_import(c,"MY-NET",list,requirements,999,0,"unused",false);check(plan.issues.empty()&&plan.created==0&&plan.unchanged==2&&plan.configuration.message_echoes[0].local_name=="Keep Custom Name"&&plan.configuration.message_echoes[0].access_level==50,"default reimport preserves existing name and restrictions");check(plan.configuration.message_echoes[0].ansi_colors==true&&plan.configuration.message_echoes[0].require_real_name,"reimport preserves existing explicit color and Real Name choices");
    plan=plan_ftn_echo_import(c,"MY-NET",list,requirements,999,0,"unused",true);check(plan.issues.empty()&&plan.text_updates==2&&plan.configuration.message_echoes[0].description=="Changed description"&&plan.configuration.message_echoes[0].update_local_name&&plan.configuration.message_echoes[0].access_level==50,"explicit text update never resets existing requirements");
    list.entries[2301].selected=false;plan=plan_ftn_echo_import(c,"MY-NET",list,requirements,999,0,"unused",false);check(plan.configuration.message_echoes.size()==2,"unchecked or absent entries never delete saved echoes");
    auto missing=c;missing.networks.clear();check(!plan_ftn_echo_import(missing,"MY-NET",list,requirements,0,0,"new",false).issues.empty(),"unknown network import fails");
    auto long_list=parse_ftn_echo_list(std::string(65,'A')+" Long name\n");check(!plan_ftn_echo_import(c,"MY-NET",long_list,std::vector<FtnEchoImportRequirements>(1),0,0,"new",false).issues.empty(),"long echo tag cannot silently become a truncated conference name");
}
void hatch_projection(){
    auto c=own_network();c.networks[0].description="Optional description";c.settings.tls_certificate="Config/cert.pem";c.settings.tls_private_key="Config/key.pem";
    FtnLink link;link.code="HUB";link.network="MY-NET";link.name="Permitted Hub";link.address="42:100/2";link.host="localhost";link.enabled=true;link.credentials.session={1,2,3,4,5};c.links.push_back(link);c.file_echoes.push_back({"my-net","UTILITY.FILES","Files",INT64_C(1099511627778)});c.permissions.push_back({"my-net","hub","utility.files",true,false,true,false});
    const auto full=decode_ftn_configuration(encode_ftn_configuration(c));check(full&&full->networks[0].description==c.networks[0].description&&full->settings.tls_private_key==c.settings.tls_private_key,"current codec retains description and TLS paths");
    const auto projection=ftn_hatch_catalog(c);check(projection.destinations.size()==1&&projection.destinations[0].network=="MY-NET"&&projection.destinations[0].echo=="UTILITY.FILES"&&projection.destinations[0].link=="HUB","hatch projection resolves case-insensitive references to canonical identities");const auto bytes=encode_ftn_hatch_catalog(projection);const auto decoded=decode_ftn_hatch_catalog(bytes);check(decoded&&decoded->echoes[0].file_area==INT64_C(1099511627778),"hatch projection preserves64bit area identity");check(std::search(bytes.begin(),bytes.end(),link.credentials.session.begin(),link.credentials.session.end())==bytes.end(),"hatch projection never contains credentials");
    auto bad=projection;bad.destinations.push_back(bad.destinations[0]);check(encode_ftn_hatch_catalog(bad).empty(),"duplicate hatch destination refused");bad=projection;bad.networks[0].name="Injected\ncontrol";check(encode_ftn_hatch_catalog(bad).empty(),"hatch display control injection refused");bad=projection;bad.destinations[0].name="\xc0\xaf";check(encode_ftn_hatch_catalog(bad).empty(),"hatch invalid UTF8 refused");bad=projection;bad.echoes[0].network="unknown";check(encode_ftn_hatch_catalog(bad).empty(),"hatch orphan reference refused");for(std::size_t i=0;i<bytes.size();++i)check(!decode_ftn_hatch_catalog({bytes.begin(),bytes.begin()+i}),"truncated hatch projection refused");
    auto writable=c;FtnReadOnlyPublisher publisher([&]()->std::optional<FtnReadOnlyPublisher::Snapshot>{return FtnReadOnlyPublisher::Snapshot{writable.revision,encode_ftn_hatch_catalog(ftn_hatch_catalog(writable))};});FtnConfigurationRequest q;q.token[0]=1;const auto first=publisher.handle(q,1);check(first.code==FtnConfigurationCode::ok,"purpose-limited read snapshot");++writable.revision;check(publisher.handle(q,2).revision==first.revision,"read-only snapshot stays pinned");q.operation=FtnConfigurationOperation::begin;q.revision=1;q.total=1;check(publisher.handle(q,3).code==FtnConfigurationCode::invalid,"read-only publisher rejects mutation");q.operation=FtnConfigurationOperation::cancel;q.total=0;check(publisher.handle(q,4).code==FtnConfigurationCode::ok,"read-only cancel releases snapshot");q.operation=FtnConfigurationOperation::read;q.revision=0;check(publisher.handle(q,5).revision==writable.revision,"next read sees latest revision");
}
int main(){try{identities();policies();lists_and_wire();tls_codec_compatibility();publication();imports();hatch_projection();std::cout<<"PASS "<<checks<<" FTN foundation checks\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
