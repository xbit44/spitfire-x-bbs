#include "linux_web_file_catalog.hpp"
#include <charconv>
#include <climits>
namespace sfx::apps::runtime::web {
namespace {
namespace app=application;using Json=nlohmann::json;
void require(bool value){if(!value)throw std::runtime_error("file_catalog_request_invalid");}
void keys(const Json& value,std::initializer_list<const char*> names){require(value.is_object()&&value.size()==names.size());for(const auto* name:names)require(value.contains(name));}
std::int64_t integer(const Json& value,std::int64_t minimum=1){
    require(value.is_string());const auto text=value.get<std::string>();std::int64_t result{};const auto read=std::from_chars(text.data(),text.data()+text.size(),result);
    require(read.ec==std::errc{}&&read.ptr==text.data()+text.size()&&result>=minimum&&result<INT64_MAX&&std::to_string(result)==text);return result;
}
unsigned small(const Json& value,unsigned minimum,unsigned maximum){require(value.is_number_integer()&&value>=Json(minimum)&&value<=Json(maximum));return value.get<unsigned>();}
bool boolean(const Json& value){require(value.is_boolean());return value.get<bool>();}
std::string text(const Json& value,std::size_t maximum){require(value.is_string());const auto result=value.get<std::string>();require(result.size()<=maximum&&result.find('\0')==result.npos);return result;}
app::Identifier128 operation(const Json& value){
    const auto value_text=text(value,32);require(value_text.size()==32);app::Identifier128 result;
    const auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a'+10);throw std::runtime_error("file_operation_invalid");};
    for(std::size_t i=0;i<result.bytes.size();++i)result.bytes[i]=static_cast<std::uint8_t>(digit(value_text[i*2])*16+digit(value_text[i*2+1]));
    require(result.valid());return result;
}
Json group(const app::FileGroup& value){return {{"id",std::to_string(value.id.value)},{"revision",std::to_string(value.revision)},{"name",value.name},{"order",std::to_string(value.order)},{"access",value.access},{"enabled",value.enabled}};}
Json area(const app::FileAreaConfiguration& value){const auto& a=value.area;return {{"id",std::to_string(a.id.value)},{"revision",std::to_string(a.revision)},{"group",std::to_string(a.group.value)},{"number",a.number},{"kind",static_cast<unsigned>(a.kind)},{"name",a.name},{"description",a.description},{"order",std::to_string(a.order)},
    {"access",a.permissions.access},{"upload",a.permissions.upload},{"download",a.permissions.download},{"uploads_enabled",a.permissions.uploads_enabled},{"downloads_sysop_only",a.permissions.downloads_sysop_only},{"enabled",a.enabled},
    {"own_description_edit",a.own_description_edit},{"backup_contents",a.backup_contents},{"ratio_uploads",a.ratio_uploads},{"ratio_downloads",a.ratio_downloads},{"sysop_review",a.sysop_review},
    {"maximum_upload_bytes",a.maximum_upload_bytes?Json(*a.maximum_upload_bytes):Json(nullptr)},{"storage_path",value.storage_path}};}
}
application::LocalFileConfigurationQuery file_catalog_query(const Json& value){
    keys(value,{"group","revision","after_id"});app::LocalFileConfigurationQuery query;
    query.part=value.at("group").is_null()?app::FileConfigurationPart::groups:app::FileConfigurationPart::areas;
    if(!value.at("group").is_null())query.group={integer(value.at("group"))};
    query.catalog_revision=integer(value.at("revision"),0);query.after_id=integer(value.at("after_id"),0);require(app::valid_file_configuration_query(query));return query;
}
Json file_catalog_page(const app::LocalFileConfigurationSnapshot& snapshot,const app::LocalFileConfigurationQuery& query,std::string_view op){
    require(snapshot.code==app::LocalAccountResultCode::ok&&app::valid_file_configuration_snapshot(snapshot));Json groups=Json::array(),areas=Json::array(),deleting=Json::array(),next=nullptr;
    for(const auto& row:snapshot.groups){groups.push_back(group(row));if(row.deletion_operation.valid())deleting.push_back(std::to_string(row.id.value));}
    for(const auto& row:snapshot.areas)areas.push_back(area(row));
    if(snapshot.next_after)next={{"group",query.group.value?Json(std::to_string(query.group.value)):Json(nullptr)},{"revision",std::to_string(snapshot.policy.catalog_revision)},{"after_id",std::to_string(snapshot.next_after)}};
    return {{"revision",std::to_string(snapshot.policy.catalog_revision)},{"operation",op},{"group",query.group.value?Json(std::to_string(query.group.value)):Json(nullptr)},{"groups",groups},{"areas",areas},{"deleting_groups",deleting},{"next",next}};
}
app::FileConfigurationChange file_catalog_update(const Json& value){
    keys(value,{"revision","operation","action","entry"});const auto action=text(value.at("action"),16);const auto& entry=value.at("entry");
    app::FileConfigurationChange change;change.operation=operation(value.at("operation"));change.expected_catalog_revision=integer(value.at("revision"));
    if(action=="group"){
        keys(entry,{"id","revision","name","order","access","enabled"});change.action=app::FileConfigurationAction::group;auto& g=change.group;
        g.id={integer(entry.at("id"),0)};g.revision=integer(entry.at("revision"));g.name=text(entry.at("name"),64);g.order=integer(entry.at("order"),0);g.access=static_cast<std::uint16_t>(small(entry.at("access"),1,999));g.enabled=boolean(entry.at("enabled"));require(g.id.value||g.revision==1);
    }else{
        require(action=="area"||action=="insert_before"||action=="insert_after"||action=="up"||action=="down");
        const bool insert=action=="insert_before"||action=="insert_after";
        if(insert)keys(entry,{"id","revision","group","number","kind","name","description","order","access","upload","download","uploads_enabled","downloads_sysop_only","enabled","own_description_edit","backup_contents","ratio_uploads","ratio_downloads","sysop_review","maximum_upload_bytes","storage_path","anchor","anchor_revision"});
        else keys(entry,{"id","revision","group","number","kind","name","description","order","access","upload","download","uploads_enabled","downloads_sysop_only","enabled","own_description_edit","backup_contents","ratio_uploads","ratio_downloads","sysop_review","maximum_upload_bytes","storage_path"});
        change.action=action=="area"?app::FileConfigurationAction::area:action=="up"?app::FileConfigurationAction::move_area_up:action=="down"?app::FileConfigurationAction::move_area_down:action=="insert_before"?app::FileConfigurationAction::insert_area_before:app::FileConfigurationAction::insert_area_after;
        auto& a=change.area.area;a.id={integer(entry.at("id"),0)};a.revision=integer(entry.at("revision"));a.group={integer(entry.at("group"))};a.number=static_cast<std::uint16_t>(small(entry.at("number"),0,999));a.kind=static_cast<app::FileAreaKind>(small(entry.at("kind"),0,2));a.name=text(entry.at("name"),64);a.description=text(entry.at("description"),256);a.order=integer(entry.at("order"),0);
        a.permissions={static_cast<std::uint16_t>(small(entry.at("access"),1,999)),static_cast<std::uint16_t>(small(entry.at("upload"),1,999)),static_cast<std::uint16_t>(small(entry.at("download"),1,999)),boolean(entry.at("uploads_enabled")),boolean(entry.at("downloads_sysop_only"))};
        a.enabled=boolean(entry.at("enabled"));a.own_description_edit=boolean(entry.at("own_description_edit"));a.backup_contents=boolean(entry.at("backup_contents"));a.ratio_uploads=boolean(entry.at("ratio_uploads"));a.ratio_downloads=boolean(entry.at("ratio_downloads"));a.sysop_review=boolean(entry.at("sysop_review"));
        if(!entry.at("maximum_upload_bytes").is_null())a.maximum_upload_bytes=small(entry.at("maximum_upload_bytes"),1,static_cast<unsigned>(app::kMaximumCallerFileBytes));
        change.area.storage_path=text(entry.at("storage_path"),2048);require(a.id.value||a.revision==1);
        if(insert){change.anchor={integer(entry.at("anchor"))};change.anchor_revision=integer(entry.at("anchor_revision"));}
    }
    require(app::valid_file_configuration_change(change));return change;
}
}
