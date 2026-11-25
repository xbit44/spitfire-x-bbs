#include "sfx/application/ftn_echo_import.hpp"
#include <iostream>
#include <algorithm>
#include <stdexcept>
using namespace sfx::application;
namespace {unsigned checks{};void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}}
int main(){try{
    FtnConfiguration c;FtnNetwork n;n.code="FIDO";n.name="FidoNet";n.address="42:100/49";n.enabled=true;c.networks.push_back(n);
    const auto list=parse_ftn_echo_list("ECHO.ONE First\nECHO.TWO Second\n");std::vector<FtnEchoImportRequirements> levels(2);
    const auto first=plan_ftn_echo_import(c,"FIDO",list,levels,3,0,"FidoNet",false);
    check(first.issues.empty()&&first.created==2,"initial import is a pure staged plan");
    levels[0]={50,60,70};const auto retry=plan_ftn_echo_import(first.configuration,"FIDO",list,levels,3,41,"",false);
    check(retry.issues.empty()&&retry.configuration.networks[0].message_group==41&&retry.configuration.networks[0].new_group_name.empty(),"retry honors corrected existing group for staged echoes");
    check(retry.configuration.message_echoes.size()==2&&retry.created==2&&retry.configuration.message_echoes[0].access_level==50&&retry.configuration.message_echoes[0].read_level==60&&retry.configuration.message_echoes[0].post_level==70,"retry updates staged levels without duplicating tags");
    const auto full=plan_ftn_echo_import(first.configuration,"FIDO",list,levels,997,41,"",false);check(full.issues.empty()&&full.created==2,"staged retry counts each pending conference once at exact capacity");
    auto saved=retry.configuration;saved.message_echoes[0].local_echo=101;saved.message_echoes[1].local_echo=102;saved.message_echoes[0].local_name="Custom Name";saved.message_echoes[0].access_level=999;saved.message_echoes[0].maximum_messages=1000;saved.message_echoes[0].ansi_colors=true;
    const auto again=plan_ftn_echo_import(saved,"FIDO",list,levels,999,41,"",false);check(again.issues.empty()&&again.created==0&&again.unchanged==2&&again.configuration.message_echoes[0].access_level==999&&again.configuration.message_echoes[0].local_name=="Custom Name"&&again.configuration.message_echoes[0].maximum_messages==1000U&&again.configuration.message_echoes[0].ansi_colors==true,"saved echo permissions/name/limits/colors survive ordinary reimport");
    const std::vector<std::pair<std::int64_t,std::string>> groups{{41,"FidoNet"},{51,"X-League"},{61,"Special Fido"}};
    const auto destination=resolve_ftn_echo_import_destination(c,"FIDO",groups,{});check(destination.issues.empty()&&destination.group==41&&destination.name=="FidoNet","legacy unbound network reuses matching group");
    auto assigned=c;assigned.networks[0].message_group=61;const auto explicit_group=resolve_ftn_echo_import_destination(assigned,"FIDO",groups,{});check(explicit_group.issues.empty()&&explicit_group.group==61&&explicit_group.name=="Special Fido","existing explicit association takes priority over network name");
    const auto absent=resolve_ftn_echo_import_destination(c,"FIDO",{},{});check(absent.issues.empty()&&!absent.group&&absent.name=="FidoNet","missing group is only a proposed final-Save creation");
    const auto mapped=resolve_ftn_echo_import_destination(saved,"FIDO",groups,{{101,41},{102,41}});check(mapped.issues.empty()&&mapped.group==41,"existing mapped group retained");
    auto unbound=saved;unbound.networks[0].message_group=0;const auto mapped_other=resolve_ftn_echo_import_destination(unbound,"FIDO",groups,{{101,61},{102,61}});check(mapped_other.issues.empty()&&mapped_other.group==61,"legacy intentional mapped group is not silently moved");
    check(!resolve_ftn_echo_import_destination(unbound,"FIDO",groups,{{101,41},{102,61}}).issues.empty(),"mixed legacy mappings require review rather than moving content");
    check(!resolve_ftn_echo_import_destination(saved,"FIDO",groups,{{101,41}}).issues.empty(),"missing target cannot become an implicit new echo");
    check(!resolve_ftn_echo_import_destination(c,"FIDO",{{41,"FidoNet"},{51,"FIDONET"}},{}).issues.empty(),"ambiguous matching groups refused");
    check(!resolve_ftn_echo_import_destination(c,"UNKNOWN",groups,{}).issues.empty(),"unknown network refused");
    auto two=c;auto x=n;x.code="XL";x.name="X-League";x.address="64200:100/49";two.networks.push_back(x);FtnMessageEcho old;old.network="XL";old.tag="ECHO.ONE";old.local_echo=501;old.access_level=999;two.message_echoes.push_back(old);
    const auto isolated=plan_ftn_echo_import(two,"FIDO",list,levels,3,41,"",false);check(isolated.issues.empty()&&isolated.created==2&&isolated.configuration.message_echoes[0].network=="XL"&&isolated.configuration.message_echoes[0].local_echo==501&&isolated.configuration.message_echoes[0].access_level==999&&isolated.configuration.networks[1].message_group==0,"same-tag other network and its settings stay untouched");
    check(resolve_ftn_echo_import_destination(two,"XL",groups,{{501,51}}).group==51,"X-League resolves independently");
    const auto raw=encode_ftn_configuration(c);(void)resolve_ftn_echo_import_destination(c,"FIDO",groups,{});check(encode_ftn_configuration(c)==raw,"resolution is read-only");
    auto hub_config=c;FtnLink hub;hub.network="FIDO";hub.code="HUB";hub.name="Test hub";hub.address="42:100/1";hub.host="127.0.0.1";hub.enabled=true;hub_config.links.push_back(hub);hub_config.networks[0].default_link=hub.code;
    const auto linked=plan_ftn_echo_import(hub_config,"FIDO",list,levels,3,41,"",false);
    check(linked.issues.empty()&&linked.configuration.permissions.size()==2,"new imported echoes link the network hub automatically");
    check(std::all_of(linked.configuration.permissions.begin(),linked.configuration.permissions.end(),[](const auto& p){return p.network=="FIDO"&&p.link=="HUB"&&p.receive&&p.send&&!p.manage;}),"automatic hub exchange never enables remote AreaFix management");
    auto exception=linked.configuration;exception.message_echoes[0].local_echo=101;exception.permissions[0].receive=false;exception.permissions[0].send=false;
    const auto preserved=plan_ftn_echo_import(exception,"FIDO",list,levels,5,41,"",false);
    check(!preserved.configuration.permissions[0].receive&&!preserved.configuration.permissions[0].send,"saved receive/send exceptions survive reimport");
    auto pass=hub_config;FtnMessageEcho pass_echo;pass_echo.network="FIDO";pass_echo.tag="SAVED.PASS";pass_echo.store_locally=false;pass.message_echoes.push_back(pass_echo);
    const auto with_pass=plan_ftn_echo_import(pass,"FIDO",list,levels,3,41,"",false);
    check(with_pass.configuration.permissions.size()==2&&std::none_of(with_pass.configuration.permissions.begin(),with_pass.configuration.permissions.end(),[](const auto& p){return p.tag=="SAVED.PASS";}),"adding echoes preserves an existing pass-through echo's absent grant");
    auto multiple=hub_config;multiple.networks[0].default_link.clear();auto second=hub;second.code="SECOND";multiple.links.push_back(second);
    check(plan_ftn_echo_import(multiple,"FIDO",list,levels,3,41,"",false).configuration.permissions.empty(),"ambiguous connections never receive automatic broad grants");
    std::cout<<"PASS "<<checks<<" import retry checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
