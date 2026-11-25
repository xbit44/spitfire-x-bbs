#include "linux_web_operational.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "../config_ui/routes.hpp"
#include "../product_version.h"
#include "sfx/application/menu_session.hpp"
#include "sfx/application/control_panel.hpp"
#include <algorithm>
#include <charconv>
#include <climits>
#include <set>
#include <sstream>
namespace sfx::apps::runtime::web {
namespace {
namespace app=application;using Json=nlohmann::json;
const std::set<std::string_view> supported{"general.board_identity","general.regional_time","general.displays_menus","system.nodes","users.new_user_registration","users.caller_password_policy","connections.telnet_ssh","connections.caller_idle_limits","security.botgate","security.connection_limits","tools.logging"};
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
std::string ascii(std::wstring_view value){std::string out;for(const auto c:value){require(c>=0&&c<128,"route_label_invalid");out+=static_cast<char>(c);}return out;}
std::uint64_t revision(const Json& value){require(value.is_string(),"revision_required");const auto text=value.get<std::string>();std::uint64_t out{};const auto read=std::from_chars(text.data(),text.data()+text.size(),out);require(read.ec==std::errc{}&&read.ptr==text.data()+text.size()&&out&&out<UINT64_MAX&&std::to_string(out)==text,"revision_invalid");return out;}
struct Fields {
    Json items=Json::array();const Json* input{};std::set<std::string> seen;
    const Json* field(const std::string& key,const std::string& label,const std::string& type,const Json& value,const std::string& help={}){
        require(seen.insert(key).second,"duplicate_page_field");items.push_back({{"key",key},{"label",label},{"type",type},{"value",value},{"help",help}});
        if(!input)return nullptr;
        require(input->is_object()&&input->contains(key),"page_fields_invalid");return &input->at(key);
    }
    void text(const std::string& key,const std::string& label,std::string& value,std::size_t maximum,const std::string& help={},bool multiline=false){
        const auto supplied=field(key,label,multiline?"multiline":"text",value,help);items.back()["maximum"]=maximum;
        if(supplied){require(supplied->is_string(),"text_required");const auto next=supplied->get<std::string>();require(next.size()<=maximum&&next.find('\0')==next.npos,"text_too_long_or_invalid");value=next;}
    }
    void boolean(const std::string& key,const std::string& label,bool& value,const std::string& help={}){const auto supplied=field(key,label,"checkbox",value,help);if(supplied){require(supplied->is_boolean(),"yes_no_required");value=supplied->get<bool>();}}
    template<class T>void integer(const std::string& key,const std::string& label,T& value,unsigned minimum,unsigned maximum,const std::string& help={}){
        const auto supplied=field(key,label,"number",value,help);items.back()["minimum"]=minimum;items.back()["maximum"]=maximum;
        if(supplied){require(supplied->is_number_integer(),"whole_number_required");require(*supplied>=Json(minimum)&&*supplied<=Json(maximum),"number_out_of_range");value=static_cast<T>(supplied->template get<unsigned>());}
    }
    void color(const std::string& key,const std::string& label,std::uint8_t& value,unsigned minimum,unsigned maximum){
        integer(key,label,value,minimum,maximum);items.back()["type"]="select";items.back()["choices"]=Json::array();
        const std::array<const char*,16> names{{"Black","Blue","Green","Cyan","Red","Magenta","Brown","Light gray","Dark gray","Bright blue","Bright green","Bright cyan","Bright red","Bright magenta","Yellow","White"}};
        for(unsigned i=minimum;i<=maximum;++i)items.back()["choices"].push_back({{"value",i},{"label",names[i]}});
    }
    void finish(){if(input)require(input->size()==seen.size(),"unexpected_page_field");}
};
Json fields(std::string_view route,app::OperationalConfiguration& config,const Json* input=nullptr){
    Fields f;f.input=input;
    if(route=="general.board_identity")f.text("bbs_name","BBS name",config.bbs_name,app::kBbsNameMaximum);
    else if(route=="general.regional_time"){
        const auto rules=platform::linux_host::LinuxBbsTimeZoneRuleProvider::create(config.bbs_time_zone);require(bool(rules),"time_zone_unavailable");auto zone=rules->iana_zone();
        f.text("time_zone","BBS time zone",zone,128,"Use a Linux time zone such as America/Los_Angeles or Etc/UTC.");
        if(input){const auto replacement=platform::linux_host::LinuxBbsTimeZoneRuleProvider::create(zone);require(bool(replacement),"time_zone_invalid");config.bbs_time_zone=platform::linux_host::LinuxBbsTimeZoneRuleProvider::windows_key_for_zone(replacement->iana_zone()).value_or(replacement->configuration().canonical_windows_registry_key);}
    }else if(route=="general.displays_menus"){
        f.color("text_color","Default text color",config.menus.text_color,1,15);f.color("input_color","Input color",config.menus.input_color,1,15);f.boolean("lightbars","Use lightbar menus",config.menus.lightbars_enabled);
        f.color("selected_text","Selected item text",config.menus.selected_foreground,0,15);f.color("selected_background","Selected item background",config.menus.selected_background,0,7);f.boolean("compact_footer","Use compact menu footer",config.menus.compact_footer);
        f.boolean("legacy_message_pipe_colors","Use legacy message pipe colors",config.menus.legacy_message_pipe_colors,
            "When pipe colors are enabled, use the older 32-code mode with colors carried between lines.");
    }else if(route=="system.nodes")f.integer("logical_nodes","Number of nodes",config.logical_nodes,1,128,"Changing the node count requires a BBS restart.");
    else if(route=="users.new_user_registration")f.boolean("allow_new_users","Allow new callers to register",config.allow_new_users);
    else if(route=="users.caller_password_policy"){
        bool strong=config.password_mode==app::PasswordMode::strong;f.boolean("strong_passwords","Require strong caller passwords",strong,"Changing this choice requires a BBS restart.");if(input)config.password_mode=strong?app::PasswordMode::strong:app::PasswordMode::standard;
    }else if(route=="connections.telnet_ssh"){
        f.boolean("telnet_enabled","Enable Telnet",config.telnet.enabled);f.text("telnet_address","Telnet listen address",config.telnet.bind_address,15,"0.0.0.0 listens on this computer's IPv4 addresses.");f.integer("telnet_port","Telnet port",config.telnet.port,1,65535);
        f.boolean("ssh_enabled","Enable SSH",config.ssh.enabled);f.text("ssh_address","SSH listen address",config.ssh.bind_address,15);f.integer("ssh_port","SSH port",config.ssh.port,1,65535);f.boolean("hostname_lookup","Look up caller hostnames",config.hostname_lookup,"Hostnames are display hints. Numeric addresses remain authoritative for access rules.");
    }else if(route=="connections.caller_idle_limits"){
        f.integer("caller_minutes","Caller idle limit (minutes)",config.caller_afk_minutes,2,1440);f.integer("login_seconds","Login idle limit (seconds)",config.login_afk_seconds,30,600);
    }else if(route=="security.botgate"){
        f.boolean("enabled","Enable BBS Security",config.botgate.enabled);f.boolean("telnet_challenge","Challenge Telnet callers",config.telnet.bot_gate_enabled);f.boolean("new_ssh_challenge","Challenge new SSH registrations",config.botgate.ssh_enabled);f.integer("challenge_seconds","Challenge time (seconds)",config.botgate.timeout_seconds,5,120);
    }else if(route=="security.connection_limits"){
        f.integer("source_limit","Connections per source address",config.maximum_connections_per_source,1,128,"Connection-policy changes require a BBS restart.");
        auto exceptions=app::serialize_source_connection_limits(config.source_connection_exceptions);if(exceptions=="-")exceptions.clear();std::replace(exceptions.begin(),exceptions.end(),',','\n');
        f.text("source_exceptions","Source address exceptions",exceptions,8192,"One address and limit per line, for example 192.168.1.12|8.",true);
        if(input){std::istringstream lines(exceptions);std::string line;std::vector<std::string> rows;while(std::getline(lines,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty())continue;require(line.find(',')==line.npos&&line!="-"&&rows.size()<app::kMaximumSourceConnectionExceptions,"source_exception_invalid");rows.push_back(line);}std::sort(rows.begin(),rows.end());std::string encoded;for(const auto& row:rows){if(!encoded.empty())encoded+=',';encoded+=row;}const auto parsed=app::parse_source_connection_limits(encoded.empty()?"-":encoded);require(bool(parsed),"source_exception_invalid");config.source_connection_exceptions=*parsed;}
    }else if(route=="tools.logging"){
        const std::array<std::pair<const char*,app::LogRetention*>,3> logs{{{"Runtime",&config.logging.runtime},{"Callers",&config.logging.callers},{"Attention",&config.logging.attention}}};
        for(const auto& item:logs){const std::string name=std::string_view(item.first)=="Runtime"?"sfxruntime.log":std::string_view(item.first)=="Callers"?"callers.log":"heysysop.log";f.integer(std::string(item.first)+"_size",name+" size (MiB)",item.second->size_mib,1,100);f.integer(std::string(item.first)+"_archives",name+" retained archives",item.second->archives,1,100);}
    }else throw std::runtime_error("page_not_available");
    f.finish();return f.items;
}
}
bool operational_route(std::string_view route){return supported.count(route)!=0;}
Json configuration_routes(){Json result=Json::array();for(const auto& route:sfx::config_ui::routes){const auto id=ascii(route.id);result.push_back({{"id",id},{"category",ascii(route.category)},{"title",ascii(route.title)},{"available",operational_route(id)||id=="ftn.networks"||id=="connections.external_client"||id=="system.runtime_behavior"||id=="doors.manager"||id=="files.groups_areas"||id=="messages.local_echoes"||id=="messages.clone_permissions"||id=="general.system_information"||id=="chat.rooms"||id=="files.transfer_policy"||id=="security.clamav"||id=="files.count_rules"||id=="files.caller_exemptions"||id=="users.security_levels"||id=="users.accounts"||id=="system.system_password"||id=="security.throttle_recovery"||id=="tools.scheduled_jobs"||id=="tools.bbs_health"||id=="tools.backup_restore"}});}return result;}
Json operational_page(std::string_view route,const app::AlphaConfigurationSnapshot& snapshot){
    require(operational_route(route),"page_not_available");auto saved=snapshot.saved,active=snapshot.active;const auto rows=fields(route,saved);const auto current=fields(route,active);const auto routes=configuration_routes();
    const auto title=std::find_if(routes.begin(),routes.end(),[&](const auto& value){return value.at("id")==route;});require(title!=routes.end(),"page_not_available");
    return {{"route",route},{"title",title->at("title")},{"category",title->at("category")},{"revision",std::to_string(snapshot.saved.configuration_revision)},{"active_revision",std::to_string(snapshot.active.configuration_revision)},{"activation",snapshot.detail},{"fields",rows},{"active_fields",current}};
}
app::AlphaConfigurationUpdateRequest operational_update(std::string_view route,const Json& request,const app::AlphaConfigurationSnapshot& snapshot){
    require(request.is_object()&&request.size()==2&&request.contains("revision")&&request.contains("values"),"page_request_invalid");app::AlphaConfigurationUpdateRequest update;update.expected_revision=revision(request.at("revision"));update.configuration=snapshot.saved;update.configuration.configuration_revision=update.expected_revision+1;(void)fields(route,update.configuration,&request.at("values"));return update;
}
app::QwkPolicyChange qwk_update(const Json& request){
    require(request.is_object()&&request.size()==4&&request.contains("revision")&&request.contains("operation")&&request.contains("board_id")&&request.contains("maximum_messages"),"qwk_request_invalid");
    app::QwkPolicyChange change;const auto expected=revision(request.at("revision"));require(expected<static_cast<std::uint64_t>(INT64_MAX),"revision_invalid");change.expected_revision=static_cast<std::int64_t>(expected);
    require(request.at("operation").is_string(),"operation_invalid");const auto operation=request.at("operation").get<std::string>();require(operation.size()==32,"operation_invalid");
    const auto nibble=[](char c)->unsigned{if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a'+10);throw std::runtime_error("operation_invalid");};
    for(std::size_t i=0;i<change.operation.bytes.size();++i)change.operation.bytes[i]=static_cast<std::uint8_t>(nibble(operation[i*2])*16+nibble(operation[i*2+1]));
    require(request.at("board_id").is_string(),"qwk_id_invalid");change.board_id=request.at("board_id").get<std::string>();const auto& count=request.at("maximum_messages");require(count.is_number_integer()&&count>=Json(1)&&count<=Json(app::kQwkMaximumMessageCount),"qwk_count_invalid");change.maximum_messages=count.get<std::uint32_t>();
    require(app::valid_qwk_policy_change(change),"qwk_policy_invalid");return change;
}
Json opening_date_page(const app::LocalClassicInformationSnapshot& snapshot,std::string_view time_zone,const app::IClock& clock){
    require(snapshot.code==app::LocalAccountResultCode::ok&&snapshot.opening.revision>0,"opening_date_unavailable");
    std::string date;
    if(snapshot.opening.configured_date)date=*snapshot.opening.configured_date;
    else{
        const auto rules=platform::linux_host::LinuxBbsTimeZoneRuleProvider::create(time_zone);require(bool(rules),"time_zone_unavailable");
        const app::BbsLocalTimeAdapters local(rules->configuration(),*rules,clock);const auto label=local.format_utc_seconds(snapshot.opening.initialization_utc);
        require(label&&label->size()>=10,"opening_date_unavailable");date=label->substr(0,10);
    }
    Fields fields;fields.text("opening_date","This BBS began on (YYYY-MM-DD)",date,10,
        "The initial date comes from this board's retained installation date in the BBS time zone. Enter a historical date if needed. Saved dates survive updates and repairs.");
    return {{"route","general.system_information"},{"title","System Information"},{"category","General"},{"revision",std::to_string(snapshot.opening.revision)},
        {"activation","Opening date saves separately from statistics."},{"fields",fields.items},{"active_fields",fields.items}};
}
app::ClassicInformationChange opening_date_update(const Json& request){
    require(request.is_object()&&request.size()==2&&request.contains("revision")&&request.contains("values"),"page_request_invalid");
    const auto expected=revision(request.at("revision"));require(expected<static_cast<std::uint64_t>(INT64_MAX),"revision_invalid");
    const auto& values=request.at("values");require(values.is_object()&&values.size()==1&&values.contains("opening_date")&&values.at("opening_date").is_string(),"page_fields_invalid");
    app::ClassicInformationChange change;change.action=app::ClassicInformationAction::opening_date;change.expected_revision=static_cast<std::int64_t>(expected);change.date=values.at("opening_date").get<std::string>();
    require(app::valid_classic_information_change(change),"opening_date_invalid");return change;
}
Json statistics_page(const app::ChatConfiguration& configuration){
    require(configuration.revision>0,"statistics_unavailable");return {{"revision",std::to_string(configuration.revision)},{"exclude_sysop_statistics",configuration.exclude_sysop_statistics}};
}
app::ChatConfiguration statistics_update(const Json& request,const app::ChatConfiguration& current){
    require(request.is_object()&&request.size()==2&&request.contains("revision")&&request.contains("exclude_sysop_statistics")&&request.at("exclude_sysop_statistics").is_boolean(),"statistics_request_invalid");
    const auto expected=revision(request.at("revision"));require(expected<static_cast<std::uint64_t>(INT64_MAX),"revision_invalid");
    auto next=current;next.revision=static_cast<std::int64_t>(expected);next.exclude_sysop_statistics=request.at("exclude_sysop_statistics").get<bool>();return next;
}
namespace {
Json room_fields(app::ChatRoom& room,const Json* input=nullptr){
    Fields fields;fields.input=input;fields.text("name","Room name",room.name,64);fields.boolean("enabled","Enable this room",room.enabled);
    auto mode=static_cast<unsigned>(room.access.mode);fields.integer("security_mode","Security level rule",mode,0,2);room.access.mode=static_cast<app::DoorSecurityMode>(mode);
    fields.items.back()["type"]="select";fields.items.back()["choices"]=Json::array({{{"value",0},{"label","Minimum level"}},{{"value",1},{"label","Maximum level"}},{{"value",2},{"label","Exact level"}}});
    fields.integer("security_level","Security level",room.access.level,1,999);fields.boolean("sysop_only","Sysop only",room.access.sysop_only);fields.finish();return fields.items;
}
}
Json chat_room_page(const app::ChatConfiguration& current,std::optional<std::uint16_t> requested){
    require(app::valid_chat_configuration(current),"chat_configuration_unavailable");const auto selected=requested.value_or(current.entry);app::ChatRoom room;bool missing=false;
    if(selected){const auto found=std::find_if(current.rooms.begin(),current.rooms.end(),[&](const auto& value){return value.id==selected;});if(found==current.rooms.end()){missing=true;room.id=selected;room.name.clear();}else room=*found;}else room.name.clear();
    auto fields=room_fields(room);if(selected==current.entry)fields[1]["readonly"]=true;
    Json records=Json::array();for(const auto& value:current.rooms)records.push_back({{"id",value.id},{"name",value.name+(value.id==current.entry?" (entry room)":"")+(value.enabled?"":" - disabled")}});
    if(missing)records.push_back({{"id",selected},{"name","Removed room (draft retained)"}});
    return {{"route","chat.rooms"},{"title",selected?"Chat Rooms":"Add Chat Room"},{"category","Chat"},{"revision",std::to_string(current.revision)},
        {"activation",missing?"This room was removed. Review your draft, then discard it to select another room. Saving cannot recreate the removed room.":selected?"The room must be empty when you save. The entry room cannot be disabled or deleted.":"New room: enabled, minimum security level 10."},
        {"record",selected},{"records",records},{"can_remove",!missing&&selected&&selected!=current.entry},{"fields",fields},{"active_fields",fields}};
}
app::ChatConfiguration chat_room_update(const Json& request,const app::ChatConfiguration& current,bool remove){
    require(request.is_object()&&request.size()==(remove?2U:3U)&&request.contains("revision")&&request.contains("record")&&(remove||request.contains("values")),"room_request_invalid");
    const auto expected=revision(request.at("revision"));require(expected<static_cast<std::uint64_t>(INT64_MAX),"revision_invalid");const auto& id=request.at("record");
    require(id.is_number_integer()&&id>=Json(0)&&id<=Json(65535),"room_id_invalid");const auto selected=id.get<std::uint16_t>();auto next=current;next.revision=static_cast<std::int64_t>(expected);
    const auto found=std::find_if(next.rooms.begin(),next.rooms.end(),[&](const auto& value){return value.id==selected;});
    if(remove){require(selected&&selected!=next.entry&&found!=next.rooms.end(),"room_delete_refused");next.rooms.erase(found);}
    else{
        app::ChatRoom room;if(selected){require(found!=next.rooms.end(),"room_not_found");room=*found;}else{
            require(next.rooms.size()<128,"room_limit_reached");std::uint16_t maximum{};for(const auto& value:next.rooms)maximum=std::max(maximum,value.id);require(maximum<65535,"room_id_exhausted");room.id=static_cast<std::uint16_t>(maximum+1);}
        (void)room_fields(room,&request.at("values"));if(selected)*found=room;else next.rooms.push_back(room);
    }
    require(app::valid_chat_configuration(next),"room_configuration_invalid");return next;
}
namespace {
std::uint64_t bounded_number(const Json& value,std::uint64_t minimum,std::uint64_t maximum){require(value.is_number_integer()&&value>=Json(minimum)&&value<=Json(maximum),"number_out_of_range");return value.get<std::uint64_t>();}
app::Identifier128 operation_id(const Json& value){
    require(value.is_string(),"operation_invalid");const auto text=value.get<std::string>();require(text.size()==32,"operation_invalid");app::Identifier128 result;
    const auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a'+10);throw std::runtime_error("operation_invalid");};
    for(std::size_t i=0;i<result.bytes.size();++i)result.bytes[i]=static_cast<std::uint8_t>(digit(text[i*2])*16+digit(text[i*2+1]));
    require(result.valid(),"operation_invalid");return result;
}
Json policy_json(const app::FilePolicy& policy){
    return {{"revision",std::to_string(policy.revision)},{"catalog_revision",std::to_string(policy.catalog_revision)},{"maximum_upload_bytes",policy.maximum_upload_bytes},
        {"lightbar_default",policy.lightbar_default},{"scan_uploads",policy.scan_uploads},{"daily_downloads",policy.counts.daily_downloads?Json(*policy.counts.daily_downloads):Json(nullptr)},
        {"ratio_downloads",policy.counts.ratio?Json(policy.counts.ratio->downloads):Json(nullptr)},{"ratio_uploads",policy.counts.ratio?Json(policy.counts.ratio->uploads):Json(nullptr)}};
}
app::FilePolicy policy_from_json(const Json& value){
    const std::array<const char*,8> keys{{"revision","catalog_revision","maximum_upload_bytes","lightbar_default","scan_uploads","daily_downloads","ratio_downloads","ratio_uploads"}};
    require(value.is_object()&&value.size()==keys.size(),"policy_baseline_invalid");for(const auto* key:keys)require(value.contains(key),"policy_baseline_invalid");app::FilePolicy policy;
    const auto rev=revision(value.at("revision")),catalog=revision(value.at("catalog_revision"));require(rev<static_cast<std::uint64_t>(INT64_MAX)&&catalog<static_cast<std::uint64_t>(INT64_MAX),"revision_invalid");policy.revision=static_cast<std::int64_t>(rev);policy.catalog_revision=static_cast<std::int64_t>(catalog);
    policy.maximum_upload_bytes=bounded_number(value.at("maximum_upload_bytes"),1,app::kMaximumCallerFileBytes);
    require(value.at("lightbar_default").is_boolean()&&value.at("scan_uploads").is_boolean(),"policy_baseline_invalid");policy.lightbar_default=value.at("lightbar_default").get<bool>();policy.scan_uploads=value.at("scan_uploads").get<bool>();
    if(!value.at("daily_downloads").is_null())policy.counts.daily_downloads=static_cast<std::uint32_t>(bounded_number(value.at("daily_downloads"),1,1000000));
    if(!value.at("ratio_downloads").is_null()||!value.at("ratio_uploads").is_null())policy.counts.ratio=app::FileRatio{static_cast<std::uint32_t>(bounded_number(value.at("ratio_downloads"),1,1000000)),static_cast<std::uint32_t>(bounded_number(value.at("ratio_uploads"),1,1000000))};
    require(app::valid_file_policy(policy),"policy_baseline_invalid");return policy;
}
void count_fields(Fields& fields,app::FileCountRule& counts){
    bool daily=counts.daily_downloads.has_value(),ratio=counts.ratio.has_value();fields.boolean("daily_enabled","Limit successful downloads per BBS-local day",daily);fields.boolean("ratio_enabled","Enable file-count ratio",ratio);
    const auto count=[&](const char* key,const char* label,const char* enabled_by,const Json& initial,bool enabled){const auto value=fields.field(key,label,"optional_number",initial);fields.items.back()["minimum"]=1;fields.items.back()["maximum"]=1000000;fields.items.back()["enabled_by"]=enabled_by;
        if(!fields.input)return std::uint32_t{};
        require(value->is_null()||(value->is_number_integer()&&*value>=Json(1)&&*value<=Json(1000000)),"count_invalid");
        if(!enabled)return std::uint32_t{};
        require(!value->is_null(),"enabled_count_required");return static_cast<std::uint32_t>(bounded_number(*value,1,1000000));};
    const auto daily_count=count("daily_files","Daily files","daily_enabled",counts.daily_downloads?Json(*counts.daily_downloads):Json(nullptr),daily);
    const auto down=count("ratio_downloads","Downloads","ratio_enabled",counts.ratio?Json(counts.ratio->downloads):Json(nullptr),ratio);
    const auto up=count("ratio_uploads","Per uploads","ratio_enabled",counts.ratio?Json(counts.ratio->uploads):Json(nullptr),ratio);
    if(fields.input){counts.daily_downloads=daily?std::optional<std::uint32_t>(daily_count):std::nullopt;counts.ratio=ratio?std::optional<app::FileRatio>(app::FileRatio{down,up}):std::nullopt;}
}
Json transfer_fields(app::FilePolicy& policy,const Json* input=nullptr){
    Fields fields;fields.input=input;fields.integer("maximum_upload_bytes","Maximum caller upload bytes",policy.maximum_upload_bytes,1,static_cast<unsigned>(app::kMaximumCallerFileBytes),"1 to 1,073,741,824 bytes. An area can set its own limit.");
    fields.boolean("lightbar_default","Light-bar selection default for new callers",policy.lightbar_default,"Existing saved caller choices stay unchanged.");
    count_fields(fields,policy.counts);
    fields.finish();return fields.items;
}
}
Json transfer_policy_page(const app::FilePolicy& policy,std::string_view operation){
    require(app::valid_file_policy(policy),"file_policy_unavailable");(void)operation_id(Json(operation));auto copy=policy;const auto fields=transfer_fields(copy);
    return {{"route","files.transfer_policy"},{"title","Transfer Policy"},{"category","Files"},{"revision",std::to_string(policy.catalog_revision)},
        {"operation",operation},{"baseline",policy_json(policy)},{"activation","Count limits are off until enabled with explicit values. Caller-upload scanning is saved separately in ClamAV."},{"fields",fields},{"active_fields",fields}};
}
Json clamav_policy_page(const app::FilePolicy& policy,std::string_view operation){
    auto page=transfer_policy_page(policy,operation);Fields fields;bool scan=policy.scan_uploads;
    fields.boolean("scan_uploads","Scan caller uploads",scan,"Scanning Off does not stop daily definition updates. When scanning is On, unavailable or incomplete scans hold new uploads privately. Sysop imports are not automatically scanned.");
    page["route"]="security.clamav";page["title"]="clamav";page["category"]="Security";page["activation"]="Scanning saves separately from transfer limits. Updates never change this choice.";page["fields"]=fields.items;page["active_fields"]=fields.items;return page;
}
app::FileConfigurationChange transfer_policy_update(const Json& request,const app::FilePolicy& current,bool scanning){
    require(request.is_object()&&request.size()==4&&request.contains("revision")&&request.contains("operation")&&request.contains("baseline")&&request.contains("values"),"file_policy_request_invalid");
    app::FileConfigurationChange change;change.action=app::FileConfigurationAction::policy;change.operation=operation_id(request.at("operation"));change.policy=policy_from_json(request.at("baseline"));
    const auto expected=revision(request.at("revision"));require(expected<static_cast<std::uint64_t>(INT64_MAX)&&expected==static_cast<std::uint64_t>(change.policy.catalog_revision),"revision_invalid");change.expected_catalog_revision=static_cast<std::int64_t>(expected);
    // A fresh write must bind the entire original policy. An older exact
    // candidate can only reconcile an existing receipt or fail storage CAS.
    require(change.expected_catalog_revision<=current.catalog_revision,"file_policy_revision_changed");
    if(change.expected_catalog_revision==current.catalog_revision)require(policy_json(change.policy)==policy_json(current),"file_policy_baseline_changed");
    if(scanning){Fields fields;fields.input=&request.at("values");fields.boolean("scan_uploads","Scan caller uploads",change.policy.scan_uploads);fields.finish();}
    else (void)transfer_fields(change.policy,&request.at("values"));
    require(app::valid_file_configuration_change(change),"file_policy_invalid");return change;
}

namespace {
Json rule_fields(app::FileSecurityRule& rule,const Json* input=nullptr){
    Fields fields;fields.input=input;fields.boolean("inherit","Inherit global file-count policy",rule.inherit,"Use the count limits in Files > Transfer Policy. Turn inheritance off to set explicit limits or exemptions for this level.");
    fields.boolean("daily_exempt","Exempt from daily file count",rule.daily_exempt);fields.boolean("ratio_exempt","Exempt from file-count ratio",rule.ratio_exempt);
    count_fields(fields,rule.counts);for(std::size_t i=1;i<fields.items.size();++i)fields.items[i]["disabled_by"]="inherit";
    if(input&&rule.inherit){rule.daily_exempt=false;rule.ratio_exempt=false;rule.counts={};}
    fields.finish();return fields.items;
}
}
Json file_count_rule_page(const app::FilePolicy& policy,const std::vector<app::FileSecurityRule>& rules,const std::vector<app::SecurityLevelRecord>& levels,std::optional<std::uint16_t> requested,std::string_view operation){
    require(app::valid_file_policy(policy)&&levels.size()<=999&&rules.size()<=999,"count_rules_unavailable");(void)operation_id(Json(operation));Json records=Json::array();
    for(const auto& level:levels)if(level.state==app::SecurityLevelState::active)records.push_back({{"id",level.level},{"name",std::to_string(level.level)+"  "+level.label}});
    require(!records.empty(),"security_levels_unavailable");const auto selected=requested.value_or(records.front().at("id").get<std::uint16_t>());require(selected>=1&&selected<=999,"security_level_invalid");
    const bool missing=std::none_of(records.begin(),records.end(),[&](const auto& record){return record.at("id")==selected;});
    app::FileSecurityRule rule;rule.level=selected;const auto existing=std::find_if(rules.begin(),rules.end(),[&](const auto& value){return value.level==selected;});if(existing!=rules.end())rule=*existing;
    if(missing)records.push_back({{"id",selected},{"name","Unavailable level "+std::to_string(selected)+" (draft retained)"}});
    const auto fields=rule_fields(rule);return {{"route","files.count_rules"},{"title","Security-level Count Rules"},{"category","Files"},{"revision",std::to_string(policy.catalog_revision)},
        {"operation",operation},{"record",selected},{"record_revision",std::to_string(rule.revision)},{"records",records},{"can_save",!missing},{"record_label","Security level"},
        {"activation",missing?"This security level is no longer active. Discard the draft to select another level.":"Level definitions stay in Users & Access. Explicit Off removes that count limit for this level. Area and private-file permissions always apply."},{"fields",fields},{"active_fields",fields}};
}
app::FileConfigurationChange file_count_rule_update(const Json& request){
    const std::array<const char*,6> keys{{"revision","operation","record","record_revision","values" ,"kind"}};
    require(request.is_object()&&request.size()==keys.size(),"count_rule_request_invalid");for(const auto* key:keys)require(request.contains(key),"count_rule_request_invalid");require(request.at("kind")=="security_rule","count_rule_request_invalid");
    app::FileConfigurationChange change;change.action=app::FileConfigurationAction::security_rule;change.operation=operation_id(request.at("operation"));
    const auto catalog=revision(request.at("revision")),record=revision(request.at("record_revision"));require(catalog<static_cast<std::uint64_t>(INT64_MAX)&&record<static_cast<std::uint64_t>(INT64_MAX),"revision_invalid");
    change.expected_catalog_revision=static_cast<std::int64_t>(catalog);change.security.revision=static_cast<std::int64_t>(record);change.security.level=static_cast<std::uint16_t>(bounded_number(request.at("record"),1,999));
    (void)rule_fields(change.security,&request.at("values"));require(app::valid_file_configuration_change(change),"count_rule_invalid");return change;
}
Json caller_exemption_page(const app::FilePolicy& policy,const std::optional<app::FileCallerExemption>& current,std::optional<app::AccountId> selected,std::string_view name,bool available,std::string_view operation){
    require(app::valid_file_policy(policy)&&(!selected||*selected>0),"caller_exemption_unavailable");(void)operation_id(Json(operation));
    require(!current||(selected&&current->account==*selected&&current->revision>0),"caller_exemption_identity_invalid");
    Fields fields;bool daily=current&&current->daily_exempt,ratio=current&&current->ratio_exempt;
    fields.boolean("daily_exempt","Exempt from daily file count",daily);fields.boolean("ratio_exempt","Exempt from file-count ratio",ratio);
    const bool usable=available&&current.has_value();if(!usable)for(auto& field:fields.items)field["readonly"]=true;
    return {{"route","files.caller_exemptions"},{"title","Caller Exemptions"},{"category","Files"},{"revision",std::to_string(policy.catalog_revision)},
        {"operation",operation},{"record",selected?Json(std::to_string(*selected)):Json(nullptr)},{"record_revision",std::to_string(current?current->revision:1)},
        {"record_name",name},{"can_save",usable},{"record_label","Caller"},{"fields",fields.items},{"active_fields",fields.items},
        {"activation",!selected?"Find and select a caller. Exemptions affect file counts only; they do not grant area access or change transfer statistics.":!usable?"This caller is no longer available. Review the draft, then discard it to choose a current caller.":"Exemptions affect file counts only. Area and private-file permissions still apply; transfer statistics stay unchanged."}};
}
app::FileConfigurationChange caller_exemption_update(const Json& request){
    const std::array<const char*,6> keys{{"revision","operation","record","record_revision","values","kind"}};
    require(request.is_object()&&request.size()==keys.size(),"exemption_request_invalid");for(const auto* key:keys)require(request.contains(key),"exemption_request_invalid");require(request.at("kind")=="caller_exemption","exemption_request_invalid");
    app::FileConfigurationChange change;change.action=app::FileConfigurationAction::caller_exemption;change.operation=operation_id(request.at("operation"));
    const auto catalog=revision(request.at("revision")),record=revision(request.at("record_revision")),account=revision(request.at("record"));
    require(catalog<static_cast<std::uint64_t>(INT64_MAX)&&record<static_cast<std::uint64_t>(INT64_MAX)&&account<=static_cast<std::uint64_t>(INT64_MAX),"revision_invalid");
    change.expected_catalog_revision=static_cast<std::int64_t>(catalog);change.exemption.revision=static_cast<std::int64_t>(record);change.exemption.account=static_cast<app::AccountId>(account);
    Fields fields;fields.input=&request.at("values");fields.boolean("daily_exempt","Exempt from daily file count",change.exemption.daily_exempt);fields.boolean("ratio_exempt","Exempt from file-count ratio",change.exemption.ratio_exempt);fields.finish();
    require(app::valid_file_configuration_change(change),"caller_exemption_invalid");return change;
}
#include "linux_web_menu_preview.inc"
#include "linux_web_jobs.inc"
#include "linux_web_door_commands.inc"
#include "linux_web_security.inc"
#include "linux_web_accounts.inc"
}
