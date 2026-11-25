#include "linux_web_messages.hpp"
#include <charconv>
#include <climits>
#include <set>
namespace sfx::apps::runtime::web {
namespace {
namespace app=application;using Json=nlohmann::json;
void require(bool value){if(!value)throw std::runtime_error("message_catalog_request_invalid");}
void keys(const Json& value,std::initializer_list<const char*> names){require(value.is_object()&&value.size()==names.size());for(const auto* name:names)require(value.contains(name));}
std::int64_t integer(const Json& value,std::int64_t minimum=1){
    require(value.is_string());const auto text=value.get<std::string>();std::int64_t result{};const auto read=std::from_chars(text.data(),text.data()+text.size(),result);
    require(read.ec==std::errc{}&&read.ptr==text.data()+text.size()&&result>=minimum&&result<INT64_MAX&&std::to_string(result)==text);return result;
}
unsigned small(const Json& value,unsigned minimum,unsigned maximum){require(value.is_number_integer()&&value>=Json(minimum)&&value<=Json(maximum));return value.get<unsigned>();}
bool boolean(const Json& value){require(value.is_boolean());return value.get<bool>();}
std::string name(const Json& value){require(value.is_string());const auto text=value.get<std::string>();require(!text.empty()&&text.size()<=64);return text;}
app::Identifier128 operation(const Json& value){
    require(value.is_string());const auto text=value.get<std::string>();require(text.size()==32);app::Identifier128 result;
    const auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a'+10);throw std::runtime_error("message_operation_invalid");};
    for(std::size_t i=0;i<result.bytes.size();++i)result.bytes[i]=static_cast<std::uint8_t>(digit(text[i*2])*16+digit(text[i*2+1]));
    require(result.valid());return result;
}
std::optional<std::uint32_t> limit(const Json& value,unsigned maximum){std::optional<std::uint32_t> result=std::nullopt;if(!value.is_null())result.emplace(small(value,1,maximum));return result;}
Json group(const app::MessageGroup& value){return {{"id",std::to_string(value.id.value)},{"revision",std::to_string(value.revision)},{"name",value.name},{"order",std::to_string(value.order)},{"access",value.access},{"enabled",value.enabled}};}
Json echo(const app::LocalEcho& value){return {{"id",std::to_string(value.id.value)},{"revision",std::to_string(value.revision)},{"group",std::to_string(value.group.value)},{"number",value.conference_number},{"name",value.name},{"order",std::to_string(value.order)},
    {"access",value.permissions.access},{"reading",value.permissions.reading},{"posting",value.permissions.posting},{"enabled",value.enabled},{"pipe_colors",value.renegade_pipe_colors},
    {"maximum_count",value.retention.maximum_count?Json(*value.retention.maximum_count):Json(nullptr)},{"maximum_age",value.retention.maximum_age_days?Json(*value.retention.maximum_age_days):Json(nullptr)}};}
}
app::LocalMessageConfigurationQuery message_catalog_query(const Json& value){
    keys(value,{"group","revision","after_order","after_id"});app::LocalMessageConfigurationQuery query;
    if(!value.at("group").is_null())query.group=app::MessageGroupId{integer(value.at("group"))};
    query.cursor.configuration_revision=integer(value.at("revision"),0);query.cursor.after_order=integer(value.at("after_order"),-1);query.cursor.after_id=integer(value.at("after_id"),0);query.cursor.page_size=64;
    require(app::valid_message_catalog_cursor(query.cursor));return query;
}
Json message_catalog_page(const app::LocalMessageConfigurationSnapshot& snapshot,const app::LocalMessageConfigurationQuery& query,std::string_view op){
    require(snapshot.code==app::LocalAccountResultCode::ok&&snapshot.catalog.revision>0);Json groups=Json::array(),echoes=Json::array(),next=nullptr;
    for(const auto& row:snapshot.catalog.groups)groups.push_back(group(row));
    for(const auto& row:snapshot.catalog.echoes)echoes.push_back(echo(row));
    if(snapshot.catalog.next){const auto& cursor=*snapshot.catalog.next;next={{"group",query.group?Json(std::to_string(query.group->value)):Json(nullptr)},{"revision",std::to_string(cursor.configuration_revision)},{"after_order",std::to_string(cursor.after_order)},{"after_id",std::to_string(cursor.after_id)}};}
    return {{"revision",std::to_string(snapshot.catalog.revision)},{"operation",op},{"group",query.group?Json(std::to_string(query.group->value)):Json(nullptr)},{"groups",groups},{"echoes",echoes},{"next",next}};
}
app::MessageConfigurationChange message_catalog_update(const Json& value){
    keys(value,{"revision","operation","action","entry"});require(value.at("action").is_string());const auto action=value.at("action").get<std::string>();const auto& entry=value.at("entry");
    app::MessageConfigurationChange change;change.expected_configuration_revision=integer(value.at("revision"));change.operation=operation(value.at("operation"));
    if(action=="group"){
        keys(entry,{"id","revision","name","order","access","enabled"});change.action=app::MessageConfigurationAction::group;
        change.group={{integer(entry.at("id"),0)},name(entry.at("name")),integer(entry.at("order"),0),integer(entry.at("revision"),0),static_cast<std::uint16_t>(small(entry.at("access"),1,999)),boolean(entry.at("enabled"))};
        require((change.group.id.value==0)==(change.group.revision==0));
    }else if(action=="echo"||action=="insert"){
        if(action=="echo")keys(entry,{"id","revision","group","number","name","order","access","reading","posting","enabled","pipe_colors","maximum_count","maximum_age"});
        else keys(entry,{"id","revision","group","number","name","order","access","reading","posting","enabled","pipe_colors","maximum_count","maximum_age","before","before_revision"});
        change.action=action=="echo"?app::MessageConfigurationAction::echo:app::MessageConfigurationAction::insert_echo;auto& row=change.echo;
        row.id={integer(entry.at("id"),0)};row.revision=integer(entry.at("revision"),0);row.group={integer(entry.at("group"))};row.conference_number=static_cast<std::uint16_t>(small(entry.at("number"),0,999));row.name=name(entry.at("name"));row.order=integer(entry.at("order"),0);
        require((row.id.value==0)==(row.revision==0));row.permissions={static_cast<std::uint16_t>(small(entry.at("access"),1,999)),static_cast<std::uint16_t>(small(entry.at("reading"),1,999)),static_cast<std::uint16_t>(small(entry.at("posting"),1,999))};row.enabled=boolean(entry.at("enabled"));row.renegade_pipe_colors=boolean(entry.at("pipe_colors"));
        row.retention.maximum_count=limit(entry.at("maximum_count"),app::kMaximumRetainedMessageCount);row.retention.maximum_age_days=limit(entry.at("maximum_age"),app::kMaximumMessageAgeDays);change.presentation_mask=1;change.retention_mask=3;
        if(action=="insert"){change.insert_before={integer(entry.at("before"))};change.insert_before_revision=integer(entry.at("before_revision"));}
    }else if(action=="up"||action=="down"){
        keys(entry,{"id","revision"});change.action=action=="up"?app::MessageConfigurationAction::move_echo_up:app::MessageConfigurationAction::move_echo_down;change.echo.id={integer(entry.at("id"))};change.echo.revision=integer(entry.at("revision"));
    }else if(action=="clone"){
        keys(entry,{"source","source_revision","targets","permission_mask","retention_mask"});change.action=app::MessageConfigurationAction::clone_permissions;
        change.clone_source={integer(entry.at("source"))};change.source_revision=integer(entry.at("source_revision"));change.permission_mask=static_cast<std::uint8_t>(small(entry.at("permission_mask"),0,7));change.retention_mask=static_cast<std::uint8_t>(small(entry.at("retention_mask"),0,3));
        const auto& targets=entry.at("targets");require(targets.is_array()&&targets.size()<=999);for(const auto& target:targets){keys(target,{"id","revision"});change.clone_targets.push_back({{integer(target.at("id"))},integer(target.at("revision"))});}
    }else require(false); // Deletion requires the separate held offline workflow.
    require(app::valid_message_configuration_change(change));return change;
}
}
