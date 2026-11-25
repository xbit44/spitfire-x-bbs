#include "linux_web_operational.hpp"
#include "linux_web_messages.hpp"
#include "linux_web_file_catalog.hpp"
#include "linux_web_doors.hpp"
#include "sfx/application/door_bulletin_preview.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <iostream>
#include <stdexcept>
namespace {
namespace web=sfx::apps::runtime::web;
namespace app=sfx::application;
using Json=nlohmann::json;
unsigned checks{};
class Clock final:public app::IClock{public:std::int64_t unix_seconds()const noexcept override{return 0;}std::uint64_t monotonic_millis()const noexcept override{return 0;}};
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void refuses(F action){bool refused{};try{action();}catch(const std::exception&){refused=true;}check(refused,"invalid browser form refused");}
Json request(const Json& page){Json values=Json::object();for(const auto& field:page.at("fields"))values[field.at("key").get<std::string>()]=field.at("value");return {{"revision",page.at("revision")},{"values",values}};}
class PreviewReader final:public app::ITrustedDisplayAssetReader{
public:unsigned reads{};std::map<std::string,std::string> assets;
    app::TrustedDisplayAssetReadResult read_asset(std::string_view identity,std::size_t maximum)override{
        ++reads;const auto found=assets.find(std::string(identity));if(found==assets.end())return {app::TrustedDisplayAssetReadCode::not_found,{},{},{},{}};
        if(found->second.size()>maximum)throw std::runtime_error("preview fixture exceeds reader bound");
        return {app::TrustedDisplayAssetReadCode::available,std::string(identity),std::string(identity),found->second,{}};
    }
};
std::string preview_text(const Json& screen){std::string text;for(const auto& cell:screen.at("cells"))text+=static_cast<char>(cell[0].get<unsigned>());return text;}
}
int main(int argc,char** argv){try{
    check(argc==2&&sfx::platform::linux_host::LinuxBbsTimeZoneRuleProvider::initialize(argv[1]),"native time rules initialized");
    app::AlphaConfigurationSnapshot snapshot;snapshot.saved.bbs_time_zone="UTC";snapshot.saved.configuration_revision=9007199254740993ULL;snapshot.saved.security_policy_revision=1;snapshot.saved.security_policy_hash=std::string(64,'a');snapshot.active=snapshot.saved;snapshot.active.bbs_name="Active board";snapshot.saved.bbs_name="Saved board";
    const auto routes=web::configuration_routes();check(routes.size()==31,"canonical route inventory");unsigned available{};
    for(const auto& route:routes){if(!route.at("available").get<bool>())continue;++available;const auto id=route.at("id").get<std::string>();if(!web::operational_route(id))continue;const auto page=web::operational_page(id,snapshot);auto value=request(page);
        const auto unchanged=web::operational_update(id,value,snapshot);check(unchanged.expected_revision==snapshot.saved.configuration_revision&&unchanged.configuration.configuration_revision==9007199254740994ULL,"revision remains exact beyond JS integer precision");
        check(app::serialize_operational_configuration(unchanged.configuration)==app::serialize_operational_configuration([&]{auto expected=snapshot.saved;++expected.configuration_revision;return expected;}()),"page roundtrip preserves unrelated settings");
        value["values"]["unrelated"]=true;refuses([&]{web::operational_update(id,value,snapshot);});value=request(page);value["values"].erase(value["values"].begin());refuses([&]{web::operational_update(id,value,snapshot);});
        value=request(page);value["revision"]=9007199254740993ULL;refuses([&]{web::operational_update(id,value,snapshot);});
    }
    check(available==31,"thirty-one supporting configuration and recovery routes available");
    const auto page=web::operational_page("general.board_identity",snapshot);check(page["fields"][0]["value"]=="Saved board"&&page["active_fields"][0]["value"]=="Active board","saved and active values remain distinct");
    for(const auto& bad:std::vector<Json>{0,-1,129,UINT64_MAX,1.5,"3",true,nullptr}){auto value=request(web::operational_page("system.nodes",snapshot));value["values"]["logical_nodes"]=bad;refuses([&]{web::operational_update("system.nodes",value,snapshot);});}
    for(const auto& bad:{"0","01","-1","1x","18446744073709551615"}){auto value=request(page);value["revision"]=bad;refuses([&]{web::operational_update("general.board_identity",value,snapshot);});}
    auto security=request(web::operational_page("security.botgate",snapshot));security["values"]["challenge_seconds"]=121;refuses([&]{web::operational_update("security.botgate",security,snapshot);});security["values"]["challenge_seconds"]=4;refuses([&]{web::operational_update("security.botgate",security,snapshot);});
    auto nodes=request(web::operational_page("system.nodes",snapshot));nodes["values"]["logical_nodes"]=128;check(web::operational_update("system.nodes",nodes,snapshot).configuration.logical_nodes==128,"valid node boundary accepted");
    refuses([&]{web::operational_page("unknown.page",snapshot);});
    Json qwk{{"revision","2"},{"operation","abcdef0123456789abcdef0123456789"},{"board_id","SFXWEB"},{"maximum_messages",10000}};
    const auto policy=web::qwk_update(qwk);check(policy.expected_revision==2&&policy.operation.valid()&&policy.board_id=="SFXWEB"&&policy.maximum_messages==10000,"QWK policy uses separate exact operation and revision");
    for(const auto& bad:std::vector<Json>{0,-1,65536,UINT64_MAX,1.5,"100",true}){auto value=qwk;value["maximum_messages"]=bad;refuses([&]{web::qwk_update(value);});}
    for(const auto* bad:{"CON","123BBS","TOOLONGID","A/B"}){auto value=qwk;value["board_id"]=bad;refuses([&]{web::qwk_update(value);});}
    for(const auto* bad:{"","00000000000000000000000000000000","ABCDEFGH0123456789abcdef0123456789"}){auto value=qwk;value["operation"]=bad;refuses([&]{web::qwk_update(value);});}
    auto extra=qwk;extra["unrelated"]=true;refuses([&]{web::qwk_update(extra);});
    Clock clock;app::LocalClassicInformationSnapshot information;information.code=app::LocalAccountResultCode::ok;information.opening={1,946686600,{}};
    const auto utc=web::opening_date_page(information,"UTC",clock),pacific=web::opening_date_page(information,"America/Los_Angeles",clock);
    check(utc["fields"][0]["value"]=="2000-01-01"&&pacific["fields"][0]["value"]=="1999-12-31","unset opening date uses retained timestamp and BBS zone at date boundary");
    auto opening=request(utc);opening["values"]["opening_date"]="2000-02-29";const auto leap=web::opening_date_update(opening);check(leap.date=="2000-02-29"&&leap.expected_revision==1&&leap.action==app::ClassicInformationAction::opening_date,"historical leap date uses separate opening revision");
    for(const auto* bad:{"1900-02-29","2026-02-29","2026-04-31","11/09/2026","2026-11-09x",""}){auto value=opening;value["values"]["opening_date"]=bad;refuses([&]{web::opening_date_update(value);});}
    opening["revision"]="9223372036854775807";refuses([&]{web::opening_date_update(opening);});opening=request(utc);opening["values"]["unrelated"]=true;refuses([&]{web::opening_date_update(opening);});
    information.opening.configured_date="1987-06-12";check(web::opening_date_page(information,"America/Los_Angeles",clock)["fields"][0]["value"]=="1987-06-12","configured historical date is not shifted with time zone");
    information.code=app::LocalAccountResultCode::unavailable;refuses([&]{web::opening_date_page(information,"UTC",clock);});
    app::ChatConfiguration chat;chat.revision=9007199254740993LL;chat.rooms.front().name="Preserved room";app::ChatDoorCommand command;command.name="Preserved command";chat.commands.push_back(command);
    auto stats=web::statistics_page(chat);stats["exclude_sysop_statistics"]=false;const auto next=web::statistics_update(stats,chat);
    check(next.revision==chat.revision&&!next.exclude_sysop_statistics&&next.rooms.front().name=="Preserved room"&&next.commands.front().name=="Preserved command"&&next.entry==chat.entry,"statistics save keeps exact revision, rooms, commands and entry");
    stats["revision"]="1";check(web::statistics_update(stats,chat).revision==1,"stale statistics revision remains stale for authoritative CAS");
    for(const auto& bad:std::vector<Json>{"false",0,1,nullptr}){auto value=stats;value["exclude_sysop_statistics"]=bad;refuses([&]{web::statistics_update(value,chat);});}
    stats["rooms"]=Json::array();refuses([&]{web::statistics_update(stats,chat);});
    app::ChatConfiguration rooms;auto room_page=web::chat_room_page(rooms,{});check(room_page["record"]==1&&!room_page["can_remove"].get<bool>()&&room_page["fields"][1]["readonly"]==true,"entry room read-only enable and delete protection");
    auto add=request(web::chat_room_page(rooms,0));add["record"]=0;add["values"]["name"]="Lounge";add["values"]["security_mode"]=2;add["values"]["security_level"]=100;
    auto added=web::chat_room_update(add,rooms,false);check(added.rooms.size()==2&&added.rooms.back().id==2&&added.rooms.back().access.mode==app::DoorSecurityMode::exact&&added.exclude_sysop_statistics==rooms.exclude_sysop_statistics,"new room ID and exact security rule, unrelated statistics preserved");
    auto edit=request(web::chat_room_page(added,2));edit["record"]=2;edit["values"]["name"]="Renamed room";const auto edited=web::chat_room_update(edit,added,false);check(edited.rooms.front().name=="Lobby"&&edited.rooms.back().name=="Renamed room","selected record change cannot target entry room");
    const auto removed=web::chat_room_update(Json{{"revision","1"},{"record",2}},added,true);check(removed.rooms.size()==1&&removed.entry==1,"non-entry deletion preserves entry");
    for(const auto& bad:std::vector<Json>{0,1,3,-1,UINT64_MAX,"2"})refuses([&]{web::chat_room_update(Json{{"revision","1"},{"record",bad}},rooms,true);});
    auto entry=request(room_page);entry["record"]=1;entry["values"]["enabled"]=false;refuses([&]{web::chat_room_update(entry,rooms,false);});
    for(const auto& bad:std::vector<Json>{0,1000,-1,UINT64_MAX,"10"}){auto value=add;value["values"]["security_level"]=bad;refuses([&]{web::chat_room_update(value,rooms,false);});}
    for(const auto& bad:std::vector<Json>{-1,3,UINT64_MAX,"0"}){auto value=add;value["values"]["security_mode"]=bad;refuses([&]{web::chat_room_update(value,rooms,false);});}
    auto extra_room=add;extra_room["values"]["entry"]=2;refuses([&]{web::chat_room_update(extra_room,rooms,false);});
    const auto absent=web::chat_room_page(removed,2);check(absent["record"]==2&&!absent["can_remove"].get<bool>(),"removed record can show exact retained draft without rebinding");auto missing=request(absent);missing["record"]=2;missing["values"]["name"]="Do not recreate";refuses([&]{web::chat_room_update(missing,removed,false);});
    app::ChatHub hub;check(hub.configure(rooms),"shared live room hub configured");app::ChatParticipant participant;participant.identity.account=1;participant.identity.node=1;participant.identity.session.bytes[0]=1;participant.name="Caller";participant.level=10;participant.sysop_threshold=999;
    check(hub.attach(participant)&&hub.join(participant.identity,1)==app::ChatResult::ok,"actual shared hub has occupied entry room");
    entry=request(room_page);entry["record"]=1;entry["values"]["name"]="Changed lobby";bool committed=false;const auto occupied=web::chat_room_update(entry,rooms,false);check(hub.change_configuration(occupied,[&]{committed=true;return true;})==app::ChatResult::busy&&!committed,"typed browser room change cannot bypass occupied-room commit gate");hub.leave(participant.identity);check(hub.change_configuration(occupied,[&]{committed=true;return true;})==app::ChatResult::ok&&committed,"empty-room commit accepted");
    app::FilePolicy file_policy;file_policy.scan_uploads=true;const auto transfer=web::transfer_policy_page(file_policy,"abcdef0123456789abcdef0123456789");
    auto transfer_edit=request(transfer);transfer_edit["operation"]=transfer["operation"];transfer_edit["baseline"]=transfer["baseline"];
    check(transfer_edit["values"]["daily_files"].is_null()&&transfer_edit["values"]["ratio_downloads"].is_null()&&transfer_edit["values"]["ratio_uploads"].is_null(),"fresh Off counts have no invented numeric defaults");
    auto policy_change=web::transfer_policy_update(transfer_edit,file_policy);check(policy_change.policy.scan_uploads&&policy_change.policy.revision==file_policy.revision&&policy_change.expected_catalog_revision==file_policy.catalog_revision&&!policy_change.policy.counts.ratio,"transfer page preserves independent scanning and exact policy/catalog revisions");
    transfer_edit["values"]["ratio_enabled"]=true;refuses([&]{web::transfer_policy_update(transfer_edit,file_policy);});transfer_edit["values"]["ratio_downloads"]=3;refuses([&]{web::transfer_policy_update(transfer_edit,file_policy);});transfer_edit["values"]["ratio_uploads"]=1;transfer_edit["values"]["daily_enabled"]=true;refuses([&]{web::transfer_policy_update(transfer_edit,file_policy);});transfer_edit["values"]["daily_files"]=1000000;
    policy_change=web::transfer_policy_update(transfer_edit,file_policy);check(policy_change.policy.counts.ratio->downloads==3&&policy_change.policy.counts.ratio->uploads==1&&policy_change.policy.counts.daily_downloads==1000000,"explicit valid ratio and daily upper boundary accepted");
    auto newer_policy=file_policy;++newer_policy.revision;++newer_policy.catalog_revision;newer_policy.scan_uploads=false;
    const auto reconcile=web::transfer_policy_update(transfer_edit,newer_policy);check(reconcile.operation==policy_change.operation&&reconcile.policy.scan_uploads&&reconcile.policy.revision==policy_change.policy.revision&&reconcile.expected_catalog_revision==policy_change.expected_catalog_revision,"stale exact reconciliation reconstructs original request not newer unrelated policy");
    auto forged=transfer_edit;forged["baseline"]["scan_uploads"]=false;refuses([&]{web::transfer_policy_update(forged,file_policy);});
    for(const auto& bad:std::vector<Json>{0,-1,1073741825ULL,UINT64_MAX,"123"}){auto value=transfer_edit;value["values"]["maximum_upload_bytes"]=bad;refuses([&]{web::transfer_policy_update(value,file_policy);});}
    for(const auto& bad:std::vector<Json>{0,-1,1000001,UINT64_MAX,1.5,"2"}){auto value=transfer_edit;value["values"]["ratio_uploads"]=bad;refuses([&]{web::transfer_policy_update(value,file_policy);});}
    auto no_op=transfer_edit;no_op["operation"]="00000000000000000000000000000000";refuses([&]{web::transfer_policy_update(no_op,file_policy);});
    auto altered=transfer_edit;altered["values"]["scan_uploads"]=false;refuses([&]{web::transfer_policy_update(altered,file_policy);});
    altered=transfer_edit;altered["revision"]="2";refuses([&]{web::transfer_policy_update(altered,file_policy);});
    file_policy.counts.daily_downloads=20;file_policy.counts.ratio=app::FileRatio{3,1};file_policy.maximum_upload_bytes=16384;
    const auto clamav=web::clamav_policy_page(file_policy,"abcdef0123456789abcdef0123456789");auto scan=request(clamav);scan["operation"]=clamav["operation"];scan["baseline"]=clamav["baseline"];scan["values"]["scan_uploads"]=false;
    const auto scanning=web::transfer_policy_update(scan,file_policy,true);check(!scanning.policy.scan_uploads&&scanning.policy.maximum_upload_bytes==16384&&scanning.policy.counts.daily_downloads==20&&scanning.policy.counts.ratio->downloads==3&&scanning.policy.counts.ratio->uploads==1,"scanning save preserves exact unrelated transfer limits");
    for(const auto& bad:std::vector<Json>{"false",0,1,nullptr}){auto value=scan;value["values"]["scan_uploads"]=bad;refuses([&]{web::transfer_policy_update(value,file_policy,true);});}
    auto unsafe_scan=scan;unsafe_scan["values"]["maximum_upload_bytes"]=100;refuses([&]{web::transfer_policy_update(unsafe_scan,file_policy,true);});unsafe_scan=scan;unsafe_scan["baseline"]["daily_downloads"]=21;refuses([&]{web::transfer_policy_update(unsafe_scan,file_policy,true);});
    auto later_policy=file_policy;++later_policy.catalog_revision;++later_policy.revision;later_policy.counts.daily_downloads=99;
    const auto scan_replay=web::transfer_policy_update(scan,later_policy,true);check(scan_replay.operation==scanning.operation&&scan_replay.policy.counts.daily_downloads==20&&scan_replay.policy.revision==scanning.policy.revision,"scan reconciliation binds original counts not newer transfer edits");
    refuses([&]{web::transfer_policy_update(scan,file_policy,false);});refuses([&]{web::transfer_policy_update(transfer_edit,file_policy,true);});
    std::vector<app::SecurityLevelRecord> levels;app::SecurityLevelRecord level;level.level=10;level.label="Regular";levels.push_back(level);level.level=20;level.label="Retired";level.state=app::SecurityLevelState::retired;levels.push_back(level);level.level=999;level.label="Sysop";level.state=app::SecurityLevelState::active;levels.push_back(level);
    app::FileSecurityRule rule;rule.level=10;rule.revision=3;rule.inherit=false;rule.counts.daily_downloads=5;rule.counts.ratio=app::FileRatio{3,1};
    const auto rule_page=web::file_count_rule_page(file_policy,{rule},levels,{},"abcdef0123456789abcdef0123456789");
    check(rule_page["record"]==10&&rule_page["record_revision"]=="3"&&rule_page["records"].size()==2&&rule_page["can_save"]==true,"count selector uses active defined levels and exact rule identity");
    auto rule_edit=request(rule_page);rule_edit["kind"]="security_rule";rule_edit["record"]=10;rule_edit["record_revision"]=rule_page["record_revision"];rule_edit["operation"]=rule_page["operation"];
    auto changed_rule=web::file_count_rule_update(rule_edit);check(changed_rule.security.level==10&&changed_rule.security.revision==3&&changed_rule.security.counts.daily_downloads==5&&changed_rule.security.counts.ratio->downloads==3,"explicit count rule roundtrip");
    rule_edit["values"]["inherit"]=true;changed_rule=web::file_count_rule_update(rule_edit);check(changed_rule.security.inherit&&!changed_rule.security.counts.daily_downloads&&!changed_rule.security.counts.ratio&&!changed_rule.security.daily_exempt&&!changed_rule.security.ratio_exempt,"inherit request canonicalizes inactive explicit policy");
    const auto unconfigured=web::file_count_rule_page(file_policy,{rule},levels,999,"abcdef0123456789abcdef0123456789");check(unconfigured["record_revision"]=="1"&&unconfigured["fields"][0]["value"]==true,"absent override inherits with implicit revision one");
    const auto retired=web::file_count_rule_page(file_policy,{rule},levels,20,"abcdef0123456789abcdef0123456789");check(retired["record"]==20&&retired["can_save"]==false,"retired selected level remains distinct for draft review");
    for(const auto& bad:std::vector<Json>{0,1000,-1,"10",UINT64_MAX}){auto value=rule_edit;value["record"]=bad;refuses([&]{web::file_count_rule_update(value);});}
    for(const auto* bad:{"0","01","9223372036854775807"}){auto value=rule_edit;value["record_revision"]=bad;refuses([&]{web::file_count_rule_update(value);});}
    auto invalid_rule=rule_edit;invalid_rule["values"]["account"]=1;refuses([&]{web::file_count_rule_update(invalid_rule);});invalid_rule=rule_edit;invalid_rule["kind"]="caller_exemption";refuses([&]{web::file_count_rule_update(invalid_rule);});
    rule_edit["values"]["inherit"]=false;rule_edit["values"]["ratio_uploads"]=nullptr;refuses([&]{web::file_count_rule_update(rule_edit);});
    app::FileCallerExemption exemption;exemption.account=9007199254740993LL;exemption.revision=4;exemption.daily_exempt=true;
    const auto exempt_page=web::caller_exemption_page(file_policy,exemption,exemption.account,"Exact Caller",true,"abcdef0123456789abcdef0123456789");
    check(exempt_page["record"]=="9007199254740993"&&exempt_page["record_revision"]=="4"&&exempt_page["can_save"]==true,"caller ID remains exact beyond JavaScript integer precision");
    auto exempt=request(exempt_page);exempt["kind"]="caller_exemption";exempt["record"]=exempt_page["record"];exempt["record_revision"]=exempt_page["record_revision"];exempt["operation"]=exempt_page["operation"];
    const auto exemption_change=web::caller_exemption_update(exempt);check(exemption_change.exemption.account==exemption.account&&exemption_change.exemption.revision==4&&exemption_change.exemption.daily_exempt&&!exemption_change.exemption.ratio_exempt,"typed exemption preserves selected caller and exact independent revision");
    for(const auto& bad:std::vector<Json>{9007199254740993LL,0,"0","01","9223372036854775808"}){auto value=exempt;value["record"]=bad;refuses([&]{web::caller_exemption_update(value);});}
    auto invalid_exempt=exempt;invalid_exempt["values"]["security_level"]=999;refuses([&]{web::caller_exemption_update(invalid_exempt);});invalid_exempt=exempt;invalid_exempt["values"]["daily_exempt"]=1;refuses([&]{web::caller_exemption_update(invalid_exempt);});
    const auto unselected=web::caller_exemption_page(file_policy,{}, {},"",false,"abcdef0123456789abcdef0123456789");check(unselected["record"].is_null()&&!unselected["can_save"].get<bool>()&&unselected["fields"][0]["readonly"],"search does not silently select or save a caller");
    const auto unavailable=web::caller_exemption_page(file_policy,{},exemption.account,"Unavailable",false,"abcdef0123456789abcdef0123456789");check(unavailable["record"]=="9007199254740993"&&!unavailable["can_save"].get<bool>(),"missing caller draft never rebinds to another account");
    refuses([&]{web::caller_exemption_page(file_policy,exemption,123,"Wrong",true,"abcdef0123456789abcdef0123456789");});
    app::LocalSecuritySnapshot security_catalog;security_catalog.code=app::LocalAccountResultCode::ok;security_catalog.policy.revision=9007199254740993LL;security_catalog.policy.new_user_level=10;security_catalog.policy.sysop_threshold=999;security_catalog.levels=levels;
    const auto security_page=web::security_level_page(security_catalog,10);auto security_edit=request(security_page);security_edit["record"]=10;
    security_edit["values"]["call_minutes"]=60;security_edit["values"]["day_minutes"]=180;security_edit["values"]["chat_minutes"]=5;security_edit["values"]["chat_day_minutes"]=15;
    const auto level_change=web::security_level_update(security_edit,security_catalog);check(level_change.level==10&&level_change.minutes_per_call==60&&level_change.minutes_per_day==180&&level_change.private_chat_minutes==5&&level_change.private_chat_minutes_per_day==15&&level_change.new_user_level==10&&level_change.sysop_threshold==999,"security definition retains exact selection and defaults");
    for(const auto& bad:std::vector<Json>{-1,4294967296ULL,1.5,"60",true}){auto value=security_edit;value["values"]["call_minutes"]=bad;refuses([&]{web::security_level_update(value,security_catalog);});}
    for(const auto& bad:std::vector<Json>{" Leading","Trailing ","Line\nBreak",std::string(33,'a'),""}){auto value=security_edit;value["values"]["label"]=bad;refuses([&]{web::security_level_update(value,security_catalog);});}
    auto redirected=security_edit;redirected["values"]["level"]=999;refuses([&]{web::security_level_update(redirected,security_catalog);});
    redirected=security_edit;redirected["record"]=0;refuses([&]{web::security_level_update(redirected,security_catalog);});
    redirected=security_edit;redirected["revision"]="9007199254740992";refuses([&]{web::security_level_update(redirected,security_catalog);});
    auto new_level=request(web::security_level_page(security_catalog,0));new_level["record"]=0;new_level["values"]["level"]=30;new_level["values"]["label"]="Visitor";
    check(web::security_level_update(new_level,security_catalog).level==30,"new level uses unused number");new_level["values"]["level"]=20;refuses([&]{web::security_level_update(new_level,security_catalog);});
    const auto removed_level=web::security_level_page(security_catalog,20);check(removed_level["record"]==20&&!removed_level["can_save"].get<bool>(),"removed security selection never rebounds");
    auto unlimited=request(web::security_level_page(security_catalog,999));unlimited["record"]=999;unlimited["values"]["call_minutes"]=1;refuses([&]{web::security_level_update(unlimited,security_catalog);});unlimited["values"]["call_minutes"]=0;unlimited["values"]["chat_minutes"]=UINT32_MAX;check(web::security_level_update(unlimited,security_catalog).private_chat_minutes==UINT32_MAX,"level999 retains allowed independent private chat limit");
    auto defaults=request(web::security_level_page(security_catalog,1000));defaults["record"]=1000;check(web::security_level_update(defaults,security_catalog).action==app::LocalSecurityAction::set_defaults,"separate defaults form");defaults["values"]["sysop_threshold"]=20;refuses([&]{web::security_level_update(defaults,security_catalog);});
    Json removal{{"revision","9007199254740993"},{"record",10},{"replacement",999},{"affected_hash",std::string(64,'a')}};const auto retiring=web::security_level_removal(removal,security_catalog);check(retiring.level==10&&retiring.replacement==999&&retiring.new_user_level==999&&retiring.sysop_threshold==999,"removal binds reviewed caller hash and explicitly moves defaults");
    for(const auto& bad:std::vector<Json>{"",std::string(64,'A'),std::string(63,'a'),1}){auto value=removal;value["affected_hash"]=bad;refuses([&]{web::security_level_removal(value,security_catalog);});}
    for(const auto& bad:std::vector<Json>{0,10,20,1000,"999"}){auto value=removal;value["replacement"]=bad;refuses([&]{web::security_level_removal(value,security_catalog);});}
    auto missing_security=web::security_level_page(security_catalog,44);check(missing_security["record"]==44&&!missing_security["can_save"].get<bool>(),"missing security draft identity retained");
    Json message_limit{{"kind","line_limit"},{"policy","message"},{"revision","7"},{"operation","abcdef0123456789abcdef0123456789"},{"record",30},{"record_revision","2"},{"value",65535}};
    const auto line_change=web::message_utility_update("line_limit",message_limit);check(line_change.security_level==30&&line_change.maximum_lines==65535&&line_change.expected_limit_revision==2&&line_change.expected_configuration_revision==7,"body lines bind independent message catalog and limit revisions");
    for(const auto& bad:std::vector<Json>{0,-1,65536,1.5,"20",true,nullptr}){auto value=message_limit;value["value"]=bad;refuses([&]{web::message_utility_update("line_limit",value);});}
    auto private_colors=message_limit;private_colors["kind"]="private_colors";private_colors["record"]=nullptr;private_colors["value"]=true;
    const auto presentation=web::message_utility_update("private_colors",private_colors);check(presentation.private_pipe_colors&&presentation.expected_private_presentation_revision==2,"private presentation uses independent record revision");
    auto system_assignment=message_limit;system_assignment["kind"]="system_sysop";system_assignment["record"]="9007199254740993";system_assignment["value"]="9007199254740993";system_assignment["assignment_revision"]="3";
    const auto designation=web::message_utility_update("system_sysop",system_assignment);check(designation.sysop==9007199254740993LL&&designation.expected_assignment_revision==3&&designation.expected_account_revision==2,"System Sysop decimal ID and account/assignment revisions stay exact");
    for(const auto& bad:std::vector<Json>{9007199254740993LL,"0","01","9223372036854775808"}){auto value=system_assignment;value["record"]=bad;value["value"]=bad;refuses([&]{web::message_utility_update("system_sysop",value);});}
    auto wrong_assignment=system_assignment;wrong_assignment["value"]="2";refuses([&]{web::message_utility_update("system_sysop",wrong_assignment);});wrong_assignment=system_assignment;wrong_assignment["authority"]="owner";refuses([&]{web::message_utility_update("system_sysop",wrong_assignment);});
    auto wrong_presentation=private_colors;wrong_presentation["value"]=1;refuses([&]{web::message_utility_update("private_colors",wrong_presentation);});wrong_presentation=private_colors;wrong_presentation["record"]=30;refuses([&]{web::message_utility_update("private_colors",wrong_presentation);});
    refuses([&]{web::message_utility_update("unknown",message_limit);});refuses([&]{web::message_utility_update("private_colors",message_limit);});
    app::LocalAccountSnapshot account_snapshot;account_snapshot.code=app::LocalAccountResultCode::ok;account_snapshot.policy.revision=7;account_snapshot.policy.new_user_level=10;account_snapshot.policy.sysop_threshold=999;
    app::AccountRecord selected_account;selected_account.account_id=9007199254740993LL;selected_account.display_name="Exact Caller";selected_account.security_level=10;selected_account.revision=3;selected_account.purge_allowed=true;account_snapshot.accounts.push_back(selected_account);
    const auto editor=web::account_page(account_snapshot,selected_account.account_id);auto account_edit=request(editor);account_edit["record"]=editor["record"];account_edit["policy_revision"]=editor["policy_revision"];account_edit["values"]["location"]="Test City";
    const auto edit_account=web::account_update(account_edit);check(edit_account.account_id==selected_account.account_id&&edit_account.expected_revision==3&&edit_account.expected_policy_revision==7&&edit_account.profile.location=="Test City"&&edit_account.purge_allowed,"account editor binds exact decimal ID/account/policy revisions");
    for(const auto& bad:std::vector<Json>{"2025-02-29","13/1/2026","2026-13-01"}){auto value=account_edit;value["values"]["birth_date"]=bad;refuses([&]{web::account_update(value);});}
    auto bad_account=account_edit;bad_account["values"]["sex"]="Z";refuses([&]{web::account_update(bad_account);});bad_account=account_edit;bad_account["record"]=9007199254740993LL;refuses([&]{web::account_update(bad_account);});bad_account=account_edit;bad_account["values"]["password"]="not a profile field";refuses([&]{web::account_update(bad_account);});
    auto new_account=request(web::account_page(account_snapshot,0));new_account["record"]="0";new_account["policy_revision"]="7";new_account["values"]["name"]="New Caller";new_account["password"]="Fixture initial password 73";new_account["confirmation"]=new_account["password"];
    auto created_account=web::account_update(new_account);check(created_account.action==app::LocalAccountAction::create_user&&created_account.account_id==0&&created_account.password==created_account.confirmation,"new user has explicit initial secret outside ordinary fields");app::erase_local_account_secrets(created_account);check(created_account.password.empty()&&created_account.confirmation.empty(),"temporary typed account request secret erasure");
    new_account["confirmation"]="different";refuses([&]{web::account_update(new_account);});new_account["confirmation"]=new_account["password"];new_account["values"]["purge_allowed"]=true;refuses([&]{web::account_update(new_account);});
    Json reset{{"record","9007199254740993"},{"revision","3"},{"password","Fixture temporary password 83"},{"confirmation","Fixture temporary password 83"}};auto reset_account=web::account_action("reset_password",reset);check(reset_account.account_id==selected_account.account_id&&reset_account.action==app::LocalAccountAction::issue_temporary_password,"reset keeps original exact selected caller");app::erase_local_account_secrets(reset_account);
    auto deactivate=reset;deactivate.erase("password");deactivate.erase("confirmation");check(web::account_action("deactivate",deactivate).action==app::LocalAccountAction::lock,"deactivate invokes shared account lock");check(web::account_action("reactivate",deactivate).action==app::LocalAccountAction::unlock,"reactivate invokes shared account activation");check(web::account_action("delete",deactivate).action==app::LocalAccountAction::tombstone,"delete retains shared tombstone semantics");
    refuses([&]{web::account_action("delete",reset);});refuses([&]{web::account_action("unknown",deactivate);});
    account_snapshot.accounts.front().state=app::AccountState::tombstoned;const auto deleted_account=web::account_page(account_snapshot,selected_account.account_id);check(!deleted_account["can_save"].get<bool>()&&deleted_account["record"]=="9007199254740993","deleted account keeps exact selected ID without editable resurrection");
    const auto no_account=web::account_page(account_snapshot,{});check(no_account["record"].is_null()&&!no_account["can_save"].get<bool>(),"account list does not silently select or save a caller");
    account_snapshot.system_revision=9007199254740993LL;const auto system_page=web::system_password_page(account_snapshot);
    check(system_page["revision"]=="9007199254740993"&&system_page["fields"].empty()&&!system_page.contains("accounts"),"System Password page exposes exact revision without caller or credential data");
    Json system_secret{{"revision","9007199254740993"},{"password","Permanent fixture password 94"},{"confirmation","Permanent fixture password 94"}};
    auto system_change=web::system_password_update(system_secret);check(system_change.action==app::LocalAccountAction::replace_system_password&&system_change.account_id==0&&system_change.expected_revision==9007199254740993LL,"permanent replacement uses shared system authority and exact revision");app::erase_local_account_secrets(system_change);
    for(const auto& bad:std::vector<Json>{9007199254740993LL,"0","01","9223372036854775807"}){auto value=system_secret;value["revision"]=bad;refuses([&]{web::system_password_update(value);});}
    auto bad_system=system_secret;bad_system["confirmation"]="different";refuses([&]{web::system_password_update(bad_system);});bad_system=system_secret;bad_system["record"]="1";refuses([&]{web::system_password_update(bad_system);});bad_system=system_secret;bad_system["password"]=std::string(129,'x');bad_system["confirmation"]=bad_system["password"];refuses([&]{web::system_password_update(bad_system);});
    check(web::throttle_page()["fields"].size()==2,"Failed Login Attempts exposes selected scope and subject only");
    check(web::throttle_update({{"scope",0},{"subject","127.0.0.1"}}).scope==app::RateLimitScope::source,"canonical IPv4 selected scope");
    check(web::throttle_update({{"scope",0},{"subject","2001:db8::1"}}).subject=="2001:db8::1","canonical IPv6 selected scope");
    check(web::throttle_update({{"scope",1},{"subject","9007199254740993"}}).subject=="9007199254740993","exact account subject stays decimal string");
    check(web::throttle_update({{"scope",2},{"subject",""}}).scope==app::RateLimitScope::system_password&&web::throttle_update({{"scope",3},{"subject",""}}).scope==app::RateLimitScope::global,"shared scopes require no subject");
    for(const auto& value:std::vector<Json>{{{"scope",0},{"subject","localhost"}},{{"scope",0},{"subject","127.0.0.1/24"}},{{"scope",1},{"subject","01"}},{{"scope",1},{"subject",9007199254740993LL}},{{"scope",2},{"subject","1"}},{{"scope",4},{"subject",""}},{{"scope",true},{"subject",""}},{{"scope",0},{"subject","127.0.0.1"},{"authority","owner"}}})refuses([&]{web::throttle_update(value);});
    auto preview_values=request(web::operational_page("general.displays_menus",snapshot)).at("values");preview_values["artwork"]=false;
    Json preview{{"values",preview_values},{"menu",0},{"ansi",true},{"level",10},{"sysop",false}};
    PreviewReader artwork;artwork.assets["MAIN.ANS"]="\x1b[2J\x1b[HANSI PREVIEW \x1b[31mRED@BEEP@@PAUSE@\r\n";artwork.assets["MAIN.ASC"]="ASCII PREVIEW\r\n";
    const auto before_preview=app::serialize_operational_configuration(snapshot.saved);const auto built_in=web::menu_preview(preview,snapshot,&artwork);
    check(built_in["cells"].size()==2000&&built_in["columns"]==80&&built_in["rows"]==25&&artwork.reads==0,"built-in preview is fixed viewport and does not read disabled artwork");
    preview["values"]["artwork"]=true;const auto ansi_preview=web::menu_preview(preview,snapshot,&artwork);
    check(preview_text(ansi_preview).find("ANSI PREVIEW")!=std::string::npos&&ansi_preview["detail"].get<std::string>().find("MAIN.ANS")!=std::string::npos,"preview selects matching ANSI through shared selector");
    preview["ansi"]=false;const auto ascii_preview=web::menu_preview(preview,snapshot,&artwork);check(preview_text(ascii_preview).find("ASCII PREVIEW")!=std::string::npos,"plain artwork selected without ANSI request");
    check(app::serialize_operational_configuration(snapshot.saved)==before_preview,"preview never changes saved settings");
    for(unsigned id=0;id<=13;++id){if(id==3||id==4)continue;auto value=preview;value["menu"]=id;check(web::menu_preview(value,snapshot,nullptr)["cells"].size()==2000,"every primary preview falls back without an artwork reader");}
    for(const auto& bad:std::vector<Json>{-1,3,4,14,1.5,"0",true}){auto value=preview;value["menu"]=bad;refuses([&]{web::menu_preview(value,snapshot,&artwork);});}
    for(const auto& bad:std::vector<Json>{0,1000,1.5,"10",true}){auto value=preview;value["level"]=bad;refuses([&]{web::menu_preview(value,snapshot,&artwork);});}
    auto invalid_preview=preview;invalid_preview["path"]="../../outside";refuses([&]{web::menu_preview(invalid_preview,snapshot,&artwork);});
    invalid_preview=preview;invalid_preview["values"]["selected_text"]=7;invalid_preview["values"]["selected_background"]=7;refuses([&]{web::menu_preview(invalid_preview,snapshot,&artwork);});
    invalid_preview=preview;invalid_preview["values"]["logical_nodes"]=128;refuses([&]{web::menu_preview(invalid_preview,snapshot,&artwork);});
    {app::ChatConfiguration commands;commands.revision=9007199254740993LL;commands.rooms.front().name="Untouched lobby";commands.exclude_sysop_statistics=false;
    app::DoorCatalog doors;app::DoorDefinition target;target.id=17;target.name="Registered Test Door";target.enabled=true;target.rlogin.protected_credential={83,69,67,82,69,84};doors.doors.push_back(target);
    const auto command_page=web::door_command_page(commands,doors,0);check(command_page["record"]==0&&command_page["can_save"].get<bool>()&&command_page["fields"].size()==8,"new menu command exposes only eight typed fields");
    check(command_page.dump().find("protected_credential")==std::string::npos&&command_page.dump().find("SECRET")==std::string::npos,"Door selection DTO never includes credentials");
    auto command_request=request(command_page);command_request["record"]=0;command_request["values"]["key"]="K";command_request["values"]["name"]="Run Test Door";command_request["values"]["door"]=17;
    const auto added=web::door_command_update(command_request,commands,doors,false);check(added.commands.size()==1&&added.commands[0].key=='K'&&added.commands[0].door==17&&added.revision==commands.revision,"add command retains expected exact shared revision");
    check(added.rooms[0].name==commands.rooms[0].name&&added.entry==commands.entry&&added.exclude_sysop_statistics==commands.exclude_sysop_statistics,"command edits preserve rooms, entry and Statistics");
    const auto selected_id=web::door_command_identity(added.commands[0]);check(selected_id==11,"stable menu plus key identity independent of vector position");
    auto selected_page=web::door_command_page(added,doors,selected_id);auto edit=request(selected_page);edit["record"]=selected_id;edit["values"]["name"]="Renamed Test Door";
    const auto edited=web::door_command_update(edit,added,doors,false);check(edited.commands.size()==1&&edited.commands[0].name=="Renamed Test Door"&&web::door_command_identity(edited.commands[0])==selected_id,"edit keeps exact selected identity");
    auto duplicate=command_request;refuses([&]{web::door_command_update(duplicate,added,doors,false);});
    for(const auto* bad:{"M","k","KK",""}){auto value=command_request;value["values"]["key"]=bad;refuses([&]{web::door_command_update(value,commands,doors,false);});}
    for(const auto& bad:std::vector<Json>{0,1000,"17",true}){auto value=command_request;value["values"]["door"]=bad;refuses([&]{web::door_command_update(value,commands,doors,false);});}
    auto missing_door=command_request;missing_door["values"]["door"]=18;refuses([&]{web::door_command_update(missing_door,commands,doors,false);});
    auto bad_command=command_request;bad_command["values"]["menu"]=3;refuses([&]{web::door_command_update(bad_command,commands,doors,false);});bad_command=command_request;bad_command["values"]["rooms"]=Json::array();refuses([&]{web::door_command_update(bad_command,commands,doors,false);});
    const auto removed=web::door_command_update({{"revision",selected_page["revision"]},{"record",selected_id}},added,doors,true);check(removed.commands.empty()&&doors.doors.size()==1,"removing menu command does not delete Door definition");
    refuses([&]{web::door_command_update(edit,removed,doors,false);});const auto tombstone=web::door_command_page(removed,doors,selected_id);check(!tombstone["can_save"].get<bool>()&&!tombstone["can_remove"].get<bool>()&&tombstone["record"]==selected_id,"removed identity stays selected read-only without resurrection");
    check(!web::door_command_page(commands,app::DoorCatalog{},0)["can_save"].get<bool>(),"no registered Doors means no command save");}
    {
    sfx::apps::runtime::LinuxScheduledJobSnapshot job;job.available=true;job.paused=true;job.configuration.revision=9007199254740993ULL;job.configuration.backup_destination="/home/test/private-backups";
    job.state.history.push_back({"fixed-run",1,2,app::ScheduledJobResult::partial,"Partial committed work"});
    auto page=web::scheduled_job_page(job,1);auto value=request(page);value["record"]=1;const auto backup=web::backup_schedule_update(value);
    check(backup.request.expected_revision==9007199254740993ULL&&backup.request.backup_destination==job.configuration.backup_destination&&backup.retained==10,"schedule exact revision and private destination retained");
    check(page["job_status"]["history"][0]["result"]=="Partial"&&page["can_run"]==true,"actual partial result remains partial; manual stopped backup available");
    job.paused=false;page=web::scheduled_job_page(job,1);check(page["fields"].back()["readonly"]==true,"running BBS retention field readonly");job.paused=true;job.any_active=true;
    page=web::scheduled_job_page(job,1);check(page["can_run"]==false&&page["fields"].back()["readonly"]==true,"active worker locks manual dispatch and retention");
    for(const auto& bad:std::vector<Json>{-1,24,true,"2",1.5}){auto invalid=value;invalid["values"]["hour"]=bad;refuses([&]{web::backup_schedule_update(invalid);});}
    for(const auto& bad:std::vector<Json>{0,366,true,"10",1.5}){auto invalid=value;invalid["values"]["retained"]=bad;refuses([&]{web::backup_schedule_update(invalid);});}
    auto invalid=value;invalid["values"]["command"]="rm";refuses([&]{web::backup_schedule_update(invalid);});invalid=value;invalid["record"]="1";refuses([&]{web::backup_schedule_update(invalid);});
    invalid=value;invalid["revision"]=9007199254740993ULL;refuses([&]{web::backup_schedule_update(invalid);});
    for(unsigned id:{2U,3U}){job.configuration=app::default_message_scheduler_configuration(std::string(web::scheduled_job_id(id)));job.configuration.revision=7;job.any_active=false;job.paused=true;
        auto message_page=web::scheduled_job_page(job,id);auto change=request(message_page);change["record"]=id;const auto schedule=web::message_schedule_update(change);
        check(schedule.expected_revision==7&&schedule.kind==(id==2?app::MessageMaintenanceKind::retention:app::MessageMaintenanceKind::integrity),"selected message job has independent expected revision");
        check(message_page["can_run"]==false&&message_page["can_save"]==true,"stopped message job can configure without starting BBS");
        if(id==3){change["values"]["weekday"]=7;refuses([&]{web::message_schedule_update(change);});}
        else{change["values"]["weekday"]=0;refuses([&]{web::message_schedule_update(change);});}
    }
    refuses([&]{web::scheduled_job_id(0);});refuses([&]{web::scheduled_job_id(4);});
    Json age{{"kind","unread_age"},{"policy","message"},{"revision","3"},{"operation","abcdef0123456789abcdef0123456789"},{"record",nullptr},{"record_revision","2"},{"value",nullptr}};
    check(!web::message_utility_update("unread_age",age).unread_mail_age_days,"explicit unlimited unread age");age["value"]=65535;check(web::message_utility_update("unread_age",age).unread_mail_age_days==65535,"unread age upper bound");
    for(const auto& bad:std::vector<Json>{0,-1,65536,true,"30",1.5}){age["value"]=bad;refuses([&]{web::message_utility_update("unread_age",age);});}
    }
#include "linux_web_message_catalog_tests.inc"
#include "linux_web_file_catalog_tests.inc"
#include "linux_web_door_catalog_tests.inc"
#include "linux_web_door_bulletin_tests.inc"
    std::cout<<"PASS "<<checks<<" typed browser configuration boundary checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
