#include "linux_web_ftn_records.hpp"
#include <iostream>

namespace {
namespace app=sfx::application;namespace web=sfx::apps::runtime::web;using Json=nlohmann::json;
unsigned checks{};
void check(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
template<class F>void refuses(F action){bool refused{};try{action();}catch(const std::exception&){refused=true;}check(refused,"invalid FTN edit refused");}
Json secrets(){Json out=Json::object();for(const auto* name:{"session","packet","areafix","tic"})out[name]={{"action","keep"},{"value",""}};return out;}
Json request(const char* action,const app::FtnConfiguration& c,const char* network,const char* record,Json values,Json passwords=Json::object()){
    return {{"revision",std::to_string(c.revision)},{"action",action},{"network",network},{"record",record},{"values",std::move(values)},{"secrets",std::move(passwords)}};
}
}
int main(){try{
    app::FtnConfiguration current;current.settings.mode=app::FtnMailerMode::disabled;current.settings.listener_enabled=false;current.settings.msg_netmail_enabled=true;current.settings.msg_netmail_directory="/legacy/shared";
    const auto settings=web::web_ftn_settings(current.settings);check(!settings.contains("msg_netmail_enabled")&&!settings.contains("msg_netmail_directory"),"no retired shared MSG editor");
    auto change=request("settings",current,"","",settings);change["values"]["port"]="ignored while disabled";change["values"]["tls_port"]=0;change["values"]["tls_certificate"]=nullptr;
    auto candidate=web::web_ftn_record_update(change,current,{});check(candidate.settings.port==current.settings.port&&candidate.settings.tls_port==current.settings.tls_port&&candidate.settings.msg_netmail_directory=="/legacy/shared","inactive listener and legacy fields preserved");
    change=request("settings",current,"","",settings);change["values"]["mode"]=0;change["values"]["listener_enabled"]=true;change["values"]["port"]=24556;change["values"]["bind_address"]="127.0.0.1";
    candidate=web::web_ftn_record_update(change,current,{});check(candidate.settings.listener_enabled&&candidate.settings.port==24556&&candidate.settings.bind_address=="127.0.0.1","explicit classic listener values");
    change["values"]["port"]=false;refuses([&]{web::web_ftn_record_update(change,current,{});});
    change=request("settings",current,"","",settings);change["values"]["mode"]=0;change["values"]["tls_listener_enabled"]=true;change["values"]["tls_certificate"]="/owned/certificate.pem";change["values"]["tls_private_key"]="/owned/key.pem";change["values"]["tls_port"]=24556;
    candidate=web::web_ftn_record_update(change,current,{});check(candidate.settings.tls_listener_enabled&&candidate.settings.tls_port==24556,"TLS settings explicit");
    change["values"]["listener_enabled"]=true;change["values"]["port"]=24556;refuses([&]{web::web_ftn_record_update(change,current,{});});
    change=request("settings",current,"","",settings);change["values"]["mode"]=1;change["values"]["bind_address"]="";change["values"]["port"]=nullptr;change["values"]["tls_listener_enabled"]=true;change["values"]["tls_port"]=nullptr;change["values"]["tls_certificate"]=nullptr;
    candidate=web::web_ftn_record_update(change,current,{});check(candidate.settings.mode==app::FtnMailerMode::external_binkd&&candidate.settings.bind_address.empty()&&candidate.settings.port==current.settings.port&&candidate.settings.tls_certificate==current.settings.tls_certificate,"External accepts blank inactive listen address and preserves unused saved transport values");
    app::FtnNetwork network;network.code="TEST";network.name="Test Network";network.msg_netmail_enabled=false;const auto fields=web::web_ftn_network(network);
    change=request("network_add",current,"TEST","",fields);candidate=web::web_ftn_record_update(change,current,{});
    check(candidate.networks.size()==1&&!candidate.networks[0].enabled&&current.networks.empty(),"unfinished network staged disabled without changing source");
    refuses([&]{web::web_ftn_record_update(change,candidate,{});});current=candidate;
    change=request("network_save",current,"MISSING","",fields);refuses([&]{web::web_ftn_record_update(change,current,{});});
    change=request("network_save",current,"TEST","",fields);change["values"]["enabled"]=true;refuses([&]{web::web_ftn_record_update(change,current,{});});
    change["values"]["address"]="2:100/1@test";change["values"]["additional_addresses"]=Json::array({"2:100/2@test"});candidate=web::web_ftn_record_update(change,current,{});
    check(candidate.networks[0].enabled&&candidate.networks[0].additional_addresses.size()==1,"enabled network requires valid distinct addresses");
    change["values"]["additional_addresses"]=Json::array({"2:100/1@test"});refuses([&]{web::web_ftn_record_update(change,current,{});});
    change["values"]["additional_addresses"]=Json::array();change["values"]["echo_list_source"]=1;refuses([&]{web::web_ftn_record_update(change,current,{});});
    change["values"]["echo_list_file"]="/owned/list.na";change["values"]["msg_netmail_enabled"]=true;refuses([&]{web::web_ftn_record_update(change,current,{});});
    change["values"]["msg_netmail_directory"]="/owned/NetMail";change["values"]["message_group"]="9007199254740993";change["values"]["retention_days"]=0;
    candidate=web::web_ftn_record_update(change,current,{});check(candidate.networks[0].msg_netmail_enabled==true&&candidate.networks[0].message_group==9007199254740993LL&&!candidate.networks[0].retention_days,"explicit per-network MSG, exact group identity and Forever retention");current=candidate;
    app::FtnLink link;link.network="TEST";link.code="PEER";link.name="Peer";link.address="2:100/3@test";link.host="127.0.0.1";link.enabled=true;link.areafix_enabled=false;
    change=request("link_add",current,"TEST","PEER",web::web_ftn_link(link),secrets());candidate=web::web_ftn_record_update(change,current,{});
    check(candidate.links.size()==1&&candidate.links[0].credentials.session.empty()&&candidate.permissions.empty(),"new connection does not grant echo permissions or invent credentials");current=candidate;
    auto keep=secrets();change=request("link_save",current,"TEST","PEER",web::web_ftn_link(current.links[0]),keep);
    current.links[0].credentials.session={7,8,9};current.links[0].credentials.packet={4,5};unsigned seals{};
    const web::FtnSeal protect=[&](std::string_view n,std::string_view l,unsigned kind,std::string_view plain)->std::vector<std::uint8_t>{check(n=="TEST"&&l=="PEER"&&!plain.empty(),"credential purpose remains exact stable identity");++seals;return {static_cast<std::uint8_t>(kind+1),42};};
    candidate=web::web_ftn_record_update(change,current,protect);check(candidate.links[0].credentials.session==current.links[0].credentials.session&&seals==0,"unreadable unchanged credential bytes retained without decryption");
    change["secrets"]["session"]={{"action","replace"},{"value","new session"}};change["secrets"]["packet"]={{"action","clear"},{"value",""}};
    candidate=web::web_ftn_record_update(change,current,protect);check(candidate.links[0].credentials.session==std::vector<std::uint8_t>({1,42})&&candidate.links[0].credentials.packet.empty()&&seals==1,"separate credential replace and clear");
    change["secrets"]["packet"]={{"action","replace"},{"value","123456789"}};refuses([&]{web::web_ftn_record_update(change,current,protect);});
    change["secrets"]=secrets();change["secrets"]["session"]={{"action","replace"},{"value",std::string(129,'a')}};refuses([&]{web::web_ftn_record_update(change,current,protect);});
    change["secrets"]["session"]={{"action","replace"},{"value","nonascii\xc3\xa9"}};refuses([&]{web::web_ftn_record_update(change,current,protect);});
    change["secrets"]["session"]={{"action","keep"},{"value","unexpected plaintext"}};refuses([&]{web::web_ftn_record_update(change,current,protect);});
    change["secrets"]["session"]={{"action","replace"},{"value","secret"}};refuses([&]{web::web_ftn_record_update(change,current,{});});
    change["secrets"]=secrets();change["values"]["areafix"]=1;refuses([&]{web::web_ftn_record_update(change,current,protect);});
    change["secrets"]["areafix"]={{"action","replace"},{"value","areafix"}};candidate=web::web_ftn_record_update(change,current,protect);check(candidate.links[0].areafix_enabled==true&&candidate.permissions.empty(),"AreaFix password and enable do not grant echo access");
    change["values"]["forward_missing_echoes"]=true;change["values"]["forward_via_link"]="MISSING";refuses([&]{web::web_ftn_record_update(change,current,protect);});
    change=request("link_save",current,"TEST","PEER",web::web_ftn_link(current.links[0]),secrets());change["values"]["use"]=0;change["values"]["host"]="";change["values"]["port"]=0;
    candidate=web::web_ftn_record_update(change,current,protect);check(candidate.links[0].use==app::FtnConnectionUse::inbound_only&&candidate.links[0].host.empty(),"inbound-only peer needs no outgoing endpoint");
    for(const auto& revision:Json::array({1,"0","01","2","9223372036854775807"})){auto bad=change;bad["revision"]=revision;refuses([&]{web::web_ftn_record_update(bad,current,protect);});}
    for(const auto& value:Json::array({-1,3,true,"1"})){auto bad=change;bad["values"]["use"]=value;refuses([&]{web::web_ftn_record_update(bad,current,protect);});}
    auto bad=change;bad["values"]["credentials"]=Json::array({1,2});refuses([&]{web::web_ftn_record_update(bad,current,protect);});bad=change;bad["extra"]=true;refuses([&]{web::web_ftn_record_update(bad,current,protect);});
    const auto catalog=web::web_ftn_network_list(current);check(catalog.at("networks")[0].at("connections")==1&&!web::web_ftn_link(current.links[0]).contains("credentials"),"ordinary catalog never exposes protected credential bytes");
    check(current.links[0].credentials.session==std::vector<std::uint8_t>({7,8,9})&&current.settings.msg_netmail_directory=="/legacy/shared","all candidates leave original source unchanged");
    app::FtnMessageEcho echo;echo.network="TEST";echo.tag="TEST.CHAT";echo.local_name="Test Chat";echo.ansi_colors=false;
    change=request("echo_add",current,"TEST","",web::web_ftn_echo(echo));candidate=web::web_ftn_record_update(change,current,{});
    check(candidate.message_echoes.size()==1&&candidate.permissions.size()==1&&candidate.permissions[0].receive&&candidate.permissions[0].send&&!candidate.permissions[0].manage,"new message echo uses existing Windows hub helper without AreaFix grant");
    check(current.message_echoes.empty()&&current.permissions.empty(),"echo draft leaves source unchanged");current=candidate;
    current.message_echoes[0].local_echo=9007199254740993LL;current.permissions[0].send=false;
    change=request("echo_save",current,"TEST","TEST.CHAT",web::web_ftn_echo(current.message_echoes[0]));change["values"]["maximum_messages"]=nullptr;change["values"]["maximum_age_days"]=30;change["values"]["update_local_retention"]=2;
    candidate=web::web_ftn_record_update(change,current,{});check(candidate.message_echoes[0].local_echo==9007199254740993LL&&!candidate.message_echoes[0].maximum_messages&&candidate.message_echoes[0].maximum_age_days==30&&candidate.message_echoes[0].update_local_retention==2&&!candidate.permissions[0].send,"saved exact mapping, selected retention update and hub exception preserved");
    change["values"]["tag"]="RENAMED";refuses([&]{web::web_ftn_record_update(change,current,{});});change["values"]["tag"]="TEST.CHAT";
    change["values"]["store_locally"]=false;refuses([&]{web::web_ftn_record_update(change,current,{});});change["values"]["local_echo"]="0";
    candidate=web::web_ftn_record_update(change,current,{});check(!candidate.message_echoes[0].store_locally&&!candidate.message_echoes[0].local_echo,"explicit pass-through requires clearing local mapping");
    for(const auto& value:Json::array({0,-1,1000000000,true,"500"})){auto invalid=change;invalid["values"]["maximum_messages"]=value;refuses([&]{web::web_ftn_record_update(invalid,current,{});});}
    app::FtnFileEcho file;file.network="TEST";file.tag="TEST.FILES";file.file_area=9007199254740993LL;
    change=request("file_add",current,"TEST","",web::web_ftn_file(file));candidate=web::web_ftn_record_update(change,current,{});
    check(candidate.file_echoes.size()==1&&candidate.file_echoes[0].file_area==9007199254740993LL&&candidate.permissions.size()==current.permissions.size(),"file echo retains exact area and grants no automatic access");current=candidate;
    change["values"]["file_area"]="0";refuses([&]{web::web_ftn_record_update(change,current,{});});
    app::FtnRoute route;route.network="TEST";route.destination="2:100/99@TEST";route.via_link="PEER";route.flavor=app::FtnMailFlavor::hold;
    change=request("route_add",current,"TEST","",web::web_ftn_route(route));candidate=web::web_ftn_record_update(change,current,{});check(candidate.routes[0].destination=="2:100/99@test"&&candidate.routes[0].flavor==app::FtnMailFlavor::hold,"route destination canonical and Hold preserved");current=candidate;
    refuses([&]{web::web_ftn_record_update(change,current,{});});change=request("route_save",current,"TEST","2:100/99@test",web::web_ftn_route(current.routes[0]));change["values"]["via_link"]="MISSING";refuses([&]{web::web_ftn_record_update(change,current,{});});
    auto inbound=current;inbound.links[0].use=app::FtnConnectionUse::inbound_only;change["values"]["via_link"]="PEER";refuses([&]{web::web_ftn_record_update(change,inbound,{});});
    change=request("permissions",current,"TEST","PEER",{{"selected",Json::array({Json{{"tag","TEST.FILES"},{"file_echo",true}}})},{"receive",true},{"send",false},{"manage",false}});
    candidate=web::web_ftn_record_update(change,current,{});check(candidate.permissions.size()==2&&!candidate.permissions[0].send&&candidate.permissions[1].receive&&!candidate.permissions[1].send,"explicit selected file access leaves message exception unchanged");current=candidate;
    change["values"]["manage"]=true;refuses([&]{web::web_ftn_record_update(change,current,{});});change["values"]["manage"]=false;change["values"]["selected"][0]["tag"]="MISSING";refuses([&]{web::web_ftn_record_update(change,current,{});});
    change["values"]["selected"]=Json::array();refuses([&]{web::web_ftn_record_update(change,current,{});});
    candidate=web::web_ftn_record_update(request("echo_remove",current,"TEST","TEST.CHAT",Json::object()),current,{});
    check(candidate.message_echoes.empty()&&candidate.file_echoes.size()==1&&candidate.permissions.size()==1&&candidate.permissions[0].file_echo,"message removal only removes dependent message access");
    candidate=web::web_ftn_record_update(request("file_remove",current,"TEST","TEST.FILES",Json::object()),current,{});
    check(candidate.file_echoes.empty()&&candidate.message_echoes.size()==1&&candidate.permissions.size()==1&&!candidate.permissions[0].file_echo,"file mapping removal only removes file access");
    current.networks[0].default_link="PEER";
    candidate=web::web_ftn_record_update(request("link_remove",current,"TEST","PEER",Json::object()),current,{});
    check(candidate.links.empty()&&candidate.routes.empty()&&candidate.permissions.empty()&&candidate.networks[0].default_link.empty()&&candidate.message_echoes.size()==1&&candidate.file_echoes.size()==1,"connection removal cascades settings while keeping echo mappings");
    candidate=web::web_ftn_record_update(request("network_remove",current,"TEST","",Json::object()),current,{});
    check(candidate.networks.empty()&&candidate.links.empty()&&candidate.routes.empty()&&candidate.message_echoes.empty()&&candidate.file_echoes.empty()&&candidate.permissions.empty(),"network removal only edits unpublished FTN catalog");
    check(current.networks.size()==1&&current.message_echoes.size()==1&&current.file_echoes.size()==1&&current.links.size()==1,"removal candidates leave source intact");
    auto clone_source=current;auto untouched=clone_source.message_echoes[0];untouched.tag="TEST.OTHER";untouched.local_echo=0;clone_source.message_echoes.push_back(untouched);
    Json clone_values={{"selected",Json::array({"TEST.CHAT"})},{"copy_count",true},{"maximum_messages",nullptr},{"copy_age",false},{"maximum_age_days",nullptr},{"copy_access",false},{"access_level",10},{"copy_read",true},{"read_level",30},{"copy_post",false},{"post_level",10}};
    change=request("echo_clone",clone_source,"TEST","",clone_values);candidate=web::web_ftn_record_update(change,clone_source,{});
    check(!candidate.message_echoes[0].maximum_messages&&candidate.message_echoes[0].read_level==30&&candidate.message_echoes[0].update_local_permissions&&(candidate.message_echoes[0].update_local_retention&1),"clone uses shared helper and explicit mapped-conference update intent");
    check(candidate.message_echoes[0].access_level==clone_source.message_echoes[0].access_level&&candidate.message_echoes[0].maximum_age_days==clone_source.message_echoes[0].maximum_age_days&&web::web_ftn_echo(candidate.message_echoes[1])==web::web_ftn_echo(untouched),"unchecked clone fields and unselected echo unchanged");
    check(clone_source.message_echoes[0].maximum_messages==500&&clone_source.message_echoes[0].read_level==10,"clone preview candidate does not mutate original draft");
    for(const auto& targets:Json::array({Json::array(),Json::array({"MISSING"}),Json::array({"TEST.CHAT","test.chat"})})){auto invalid=change;invalid["values"]["selected"]=targets;refuses([&]{web::web_ftn_record_update(invalid,clone_source,{});});}
    auto no_mask=change;no_mask["values"]["copy_count"]=false;no_mask["values"]["copy_read"]=false;refuses([&]{web::web_ftn_record_update(no_mask,clone_source,{});});
    auto mixed=clone_source;mixed.networks[0].code="TeSt";mixed.message_echoes[0].network="tEsT";mixed.message_echoes[0].tag="test.Chat";
    change=request("echo_clone",mixed,"test","",clone_values);const auto preview=web::web_ftn_clone_preview(change,mixed);
    check(preview.size()==1&&preview[0]["tag"]=="test.Chat"&&preview[0]["read_level"]==30,"clone preview includes mixed-case stored network and echo without rewriting identity");
    change=request("echo_save",mixed,"TEST","TEST.CHAT",web::web_ftn_echo(mixed.message_echoes[0]));change["values"]["description"]="Edited lower-case mapping";candidate=web::web_ftn_record_update(change,mixed,{});
    check(candidate.message_echoes[0].tag=="test.Chat"&&candidate.message_echoes[0].network=="tEsT"&&candidate.message_echoes[0].local_echo==mixed.message_echoes[0].local_echo,"ordinary mapped echo edit preserves stored identity spelling");
    candidate=web::web_ftn_record_update(request("echo_remove",mixed,"test","TEST.CHAT",Json::object()),mixed,{});check(candidate.message_echoes.size()==1&&candidate.permissions.size()==1&&candidate.permissions[0].file_echo,"case-insensitive selected removal clears matching dependent permissions only");
    change=request("echo_presentation",clone_source,"TEST","",{{"selected",Json::array({"TEST.CHAT"})},{"real_name",true},{"ansi",false},{"pipe",true}});candidate=web::web_ftn_record_update(change,clone_source,{});
    check(candidate.message_echoes[0].require_real_name&&candidate.message_echoes[0].ansi_colors==false&&candidate.message_echoes[0].pipe_colors&&candidate.message_echoes[0].update_local_permissions&&web::web_ftn_echo(candidate.message_echoes[1])==web::web_ftn_echo(clone_source.message_echoes[1]),"selected setup presentation marks mapped pipe update and leaves other echoes unchanged");
    change["values"]["selected"]=Json::array({"TEST.CHAT","MISSING"});refuses([&]{web::web_ftn_record_update(change,clone_source,{});});check(!clone_source.message_echoes[0].pipe_colors,"failed bulk setup presentation leaves source untouched");
    auto setup=clone_source;setup.links[0].areafix_enabled=true;setup.links[0].credentials.areafix={3,42};setup.permissions[0].receive=true;
    change=request("setup_finish",setup,"TEST","",{{"enabled",true},{"queue",true},{"hub","PEER"},{"selected",Json::array({"TEST.CHAT"})}});candidate=web::web_ftn_record_update(change,setup,{});
    check(candidate.setup_subscriptions&&candidate.setup_subscriptions->tags==std::vector<std::string>{"TEST.CHAT"}&&!setup.setup_subscriptions,"explicit queue is only a command in unpublished final candidate");
    auto no_receive=setup;no_receive.permissions[0].receive=false;refuses([&]{web::web_ftn_record_update(change,no_receive,{});});
    change["values"]["enabled"]=false;refuses([&]{web::web_ftn_record_update(change,setup,{});});
    change["values"]={{"enabled",false},{"queue",false},{"hub",""},{"selected",Json::array()}};candidate=web::web_ftn_record_update(change,candidate,{});
    check(!candidate.networks[0].enabled&&!candidate.setup_subscriptions&&candidate.message_echoes.size()==setup.message_echoes.size(),"finish later disables network and clears command while retaining staged echoes");
    std::cout<<"PASS "<<checks<<" FTN web record checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<" "<<error.what()<<'\n';return 1;}}
