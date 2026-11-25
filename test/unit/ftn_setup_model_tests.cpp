#include "sfx/application/ftn_setup.hpp"
#include "sfx/application/ftn_mail.hpp"
#include "sfx/application/ftn_nodelist.hpp"
#include "sfx/application/message_source.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace sfx::application;
namespace {unsigned checks{};void expect(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}}
int main(int argc,char** argv){try{
    FtnConfiguration c;FtnNetwork f;f.code="fido";f.name="FidoNet";f.address="42:100/49";
    expect(add_ftn_setup_network(c,f,{{17,"fidonet"}}).succeeded(),"create draft network");
    expect(c.networks[0].message_group==17&&c.networks[0].new_group_name.empty(),"reuse named group");
    expect(c.networks[0].msg_netmail_enabled==false,"new network explicitly off, no legacy inheritance");
    FtnNetwork x;x.code="x";x.name="X-League";x.address="64200:100/49";
    expect(add_ftn_setup_network(c,x,{}).succeeded()&&c.networks[1].new_group_name=="X-League","new group stays draft");
    expect(!add_ftn_setup_network(c,f,{}).succeeded()&&c.networks.size()==2,"duplicate refuses without mutation");
    for(const auto* tag:{"SYSOP","FIDOTEST","MYSTIC"})expect(add_ftn_setup_echo(c,"fido",tag).succeeded(),"add echo");
    expect(add_ftn_setup_echo(c,"x","GAME").succeeded(),"add other network echo");
    expect(c.message_echoes[0].require_real_name&&!c.message_echoes[3].require_real_name,"real name defaults by network");
    for(const auto& e:c.message_echoes)expect(e.maximum_messages==500U&&!e.maximum_age_days&&e.ansi_colors==false&&!e.pipe_colors,"new echo defaults");
    expect(add_ftn_setup_echo(c,"fido","fidotest","replace?","replace?").changed==0&&c.message_echoes[1].local_name=="FIDOTEST","duplicate preserves settings/name");
    c.message_echoes[0].access_level=c.message_echoes[0].read_level=c.message_echoes[0].post_level=999;c.message_echoes[0].maximum_age_days=90;
    const auto before=encode_ftn_configuration(c);
    expect(apply_ftn_echo_options(c,"fido",{"SYSOP"},{}).changed==0&&encode_ftn_configuration(c)==before,"no selected fields is no-op");
    FtnEchoBulkOptions bulk;bulk.copy_count=true;bulk.maximum_messages=1000;
    expect(apply_ftn_echo_options(c,"fido",{"SYSOP","FIDOTEST","MYSTIC"},bulk).changed==3,"count-only applies explicit targets");
    expect(c.message_echoes[0].access_level==999&&c.message_echoes[0].read_level==999&&c.message_echoes[0].post_level==999&&c.message_echoes[0].maximum_age_days==90U,"preserve sysop permissions/age");
    expect(c.message_echoes[1].access_level==10&&!c.message_echoes[1].maximum_age_days&&!c.message_echoes[1].update_local_permissions,"preserve ordinary permissions/age");
    expect(c.message_echoes[3].maximum_messages==500U,"network isolation");
    const auto after=encode_ftn_configuration(c);expect(after[0]==7,"codec7");
    expect(!apply_ftn_echo_options(c,"fido",{"FIDOTEST","GAME"},bulk).succeeded()&&encode_ftn_configuration(c)==after,"cross-network target refuses entire change");
    bulk.maximum_messages=0;expect(!apply_ftn_echo_options(c,"fido",{"FIDOTEST"},bulk).succeeded()&&encode_ftn_configuration(c)==after,"invalid selected value no mutation");
    bulk={};bulk.copy_age=true;bulk.maximum_age_days=30;expect(apply_ftn_echo_options(c,"fido",{"MYSTIC"},bulk).changed==1&&c.message_echoes[2].maximum_age_days==30U&&c.message_echoes[2].maximum_messages==1000U,"independent age only");
    const auto decoded=decode_ftn_configuration(encode_ftn_configuration(c));expect(decoded&&decoded->message_echoes[0].maximum_age_days==90U&&decoded->message_echoes[0].update_local_retention==1,"new codec roundtrip");
    FtnConfiguration legacy;legacy.networks.push_back(f);FtnMessageEcho e;e.network="fido";e.tag="LEGACY";legacy.message_echoes.push_back(e);
    for(std::uint8_t v=2;v<=6;++v){const auto bytes=encode_ftn_configuration_legacy(legacy,v);const auto old=decode_ftn_configuration(bytes);expect(!bytes.empty()&&bytes[0]==v&&old&&!old->networks[0].msg_netmail_enabled&&!old->message_echoes[0].ansi_colors,"legacy codec exact forms readable");}
    legacy.networks[0].default_link="hub";expect(encode_ftn_configuration_legacy(legacy,6).empty(),"new semantics cannot alias old receipt");
    auto& net=c.networks[0];net.enabled=true;FtnLink hub;hub.code="hub";hub.network="fido";hub.name="Hub";hub.address="42:100/81";hub.host="hub.example";hub.enabled=true;c.links.push_back(hub);net.default_link="hub";
    expect(validate_ftn_configuration(c).empty(),"hub config valid");
    expect(ftn_route_link(c,"fido",*parse_ftn_address("2:3/4"))=="hub","ordinary mail routes via hub");
    expect(!ftn_route_link(c,"fido",*parse_ftn_address("42:100/49")),"own address never routed to hub");
    expect(!ftn_route_link(c,"x",*parse_ftn_address("2:3/4")),"no cross-network fallback");
    FtnLink peer=hub;peer.code="peer";peer.address="2:3/4";c.links.push_back(peer);expect(ftn_route_link(c,"fido",*parse_ftn_address("2:3/4"))=="peer","direct configured peer wins");
    c.routes.push_back({"fido","2:3/4","hub"});expect(ftn_route_link(c,"fido",*parse_ftn_address("2:3/4"))=="hub","explicit route wins");
    FtnConfiguration msg;msg.settings.msg_netmail_enabled=true;msg.settings.msg_netmail_directory="D:\\NetMail";
    FtnNetwork old;old.code="old";old.name="Legacy";old.address="1:1/1";old.enabled=true;msg.networks.push_back(old);
    auto off=old;off.code="off";off.msg_netmail_enabled=false;msg.networks.push_back(off);
    auto game=old;game.code="game";game.msg_netmail_enabled=true;game.msg_netmail_directory="d:/netmail/";msg.networks.push_back(game);
    const auto shared=ftn_stored_directories(msg);expect(shared.size()==1&&shared[0].networks==std::vector<std::string>({"game"}),"legacy shared enable is ignored; only explicit per-network On scans");
    msg.networks[2].msg_netmail_directory="D:\\Games\\NetMail";expect(ftn_stored_directories(msg).size()==1,"independent game directory without shared fallback");
    msg.settings.msg_netmail_enabled=false;expect(ftn_stored_directories(msg).size()==1&&ftn_stored_directories(msg)[0].networks==std::vector<std::string>({"game"}),"per-network policy independent of old global enable");
    const std::string nodes=";A nodelist\r\nZone,42,Z,L,S,-Unpublished-,300\r\nHost,100,H,L,S,-Unpublished-,300\r\n,81,Hub,L,S,-Unpublished-,300,INA:hub.example,IBN:24777\r\n,49,Us,L,S,-Unpublished-,300,INA:us.example,IBN\r\n";
    const auto endpoint=ftn_nodelist_endpoint(nodes,*parse_ftn_address("42:100/81"));expect(endpoint&&endpoint->host=="hub.example"&&endpoint->port==24777,"network nodelist INA and IBN port");
    expect(!ftn_nodelist_endpoint(nodes,*parse_ftn_address("43:100/81")),"nodelist zone isolation");
    expect(!ftn_nodelist_endpoint(nodes+",81,Duplicate,L,S,-Unpublished-,300,INA:other.example,IBN\n",*parse_ftn_address("42:100/81")),"ambiguous listed peer refuses");
    const auto hidden=ftn_nodelist_endpoint("Zone,42,Z,L,S,-,300\nHost,100,H,L,S,-,300\nHold,81,H,L,S,-,300,INA:hub.example,IBN\n",*parse_ftn_address("42:100/81"));expect(!hidden,"Hold node not dialled");
    expect(!ftn_nodelist_endpoint("Zone,42,Z,L,S,-,300\nHost,100,H,L,S,-,300\nHost,invalid,H,L,S,-,300\n,81,H,L,S,-,300,INA:wrong.example,IBN\n",*parse_ftn_address("42:100/81")),"malformed hierarchy cannot reuse previous network scope");
    expect(!ftn_nodelist_endpoint("Zone,42,Z,L,S,-,300\nHost,100,H,L,S,-,300\nUnknown,81,H,L,S,-,300,INA:wrong.example,IBN\n",*parse_ftn_address("42:100/81")),"unknown record is not a listed peer");
    MessageSource source;source.format=MessageSourceFormat::ansi_canvas;source.bytes="\x1b[31mRed\x1b[0m |10Pipe\r\nSecond";
    const auto original=validate_message_source(source,kMaximumMessageLines);expect(bool(original),"original supported ANSI validates");
    const auto colored=message_source_rendition(source,false,true),plain=message_source_rendition(source,false,false),pipes=message_source_rendition(source,true,false);
    expect(colored&&plain&&pipes&&colored->cells[0].attribute==4&&plain->cells[0].attribute==7,"ANSI policy affects rendition only");
    expect(pipes->cells[4].attribute==10&&plain->text.canonical_lf.find("|10")!=std::string::npos,"pipe policy independent of ANSI policy");
    expect(source.bytes=="\x1b[31mRed\x1b[0m |10Pipe\r\nSecond"&&original.off.extent==plain->extent,"original source and layout preserved");
    std::vector<FtnEchoListEntry> batch;for(unsigned i=0;i<600;++i)batch.push_back({"BULK."+std::to_string(i),"Description",i,true});
    auto many=c;expect(add_ftn_setup_echoes(many,"fido",batch).changed==600,"hundreds of selected entries added in one draft batch");
    const auto original_many=encode_ftn_configuration(many);expect(add_ftn_setup_echoes(many,"fido",batch).changed==0&&encode_ftn_configuration(many)==original_many,"batch duplicates preserve existing values");
    auto invalid=batch;invalid.push_back({"BAD/TAG","",0,true});expect(!add_ftn_setup_echoes(many,"fido",invalid).succeeded()&&encode_ftn_configuration(many)==original_many,"invalid batch has no partial additions");
    std::vector<std::string> targets;for(const auto& echo:many.message_echoes)if(echo.network=="fido")targets.push_back(echo.tag);FtnEchoBulkOptions count_only;count_only.copy_count=true;count_only.maximum_messages=1000;expect(apply_ftn_echo_options(many,"fido",targets,count_only).changed==targets.size()&&many.message_echoes[0].access_level==999,"hundreds of explicit targets preserve sysop level");
    if(argc==2){unsigned files{};for(const auto& entry:std::filesystem::recursive_directory_iterator(argv[1])){auto extension=entry.path().extension().string();for(auto& ch:extension)if(ch>='a'&&ch<='z')ch=static_cast<char>(ch-'a'+'A');if(!entry.is_regular_file()||extension!=".NA")continue;std::ifstream in(entry.path(),std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(in)),{});const auto list=parse_ftn_echo_list(bytes);expect(list.complete&&!list.entries.empty(),"owner NA corpus parses bounded entries");++files;}expect(files==14,"all fourteen owner NA samples covered");}
    std::cout<<"PASS "<<checks<<" setup model/selected-field/codec/hub/MSG checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
