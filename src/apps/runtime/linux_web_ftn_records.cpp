#include "linux_web_ftn_records.hpp"
#include "sfx/application/ftn_echo_import.hpp"
#include "sfx/application/ftn_setup.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <set>

namespace sfx::apps::runtime::web {
bool web_ftn_same_id(std::string_view a,std::string_view b) noexcept {
    if(a.size()!=b.size())return false;
    const auto fold=[](char c){return c>='a'&&c<='z'?static_cast<char>(c-'a'+'A'):c;};
    for(std::size_t i=0;i<a.size();++i)if(fold(a[i])!=fold(b[i]))return false;
    return true;
}
namespace {
namespace app=application;using Json=nlohmann::json;
void require(bool value,const char* why="ftn_record_invalid"){if(!value)throw std::runtime_error(why);}
void keys(const Json& value,std::initializer_list<const char*> names){require(value.is_object()&&value.size()==names.size());for(const auto* name:names)require(value.contains(name));}
std::string text(const Json& value,std::size_t maximum,bool blank=false){require(value.is_string());auto out=value.get<std::string>();require(app::valid_ftn_display_text(out,maximum,blank));return out;}
unsigned integer(const Json& value,unsigned minimum,unsigned maximum){require(value.is_number_integer()&&value>=Json(minimum)&&value<=Json(maximum));return value.get<unsigned>();}
bool flag(const Json& value){require(value.is_boolean());return value.get<bool>();}
std::int64_t identity(const Json& value,bool zero=false){
    require(value.is_string());const auto s=value.get<std::string>();std::int64_t out{};const auto read=std::from_chars(s.data(),s.data()+s.size(),out);
    require(read.ec==std::errc{}&&read.ptr==s.data()+s.size()&&out>=(zero?0:1)&&out<INT64_MAX&&std::to_string(out)==s);return out;
}
std::vector<std::string> aliases(const Json& value){require(value.is_array()&&value.size()<=32);std::vector<std::string> out;for(const auto& item:value)out.push_back(text(item,96));return out;}
std::string code(const Json& value,bool blank=false){const auto out=text(value,48,blank);require((blank&&out.empty())||app::valid_ftn_code(out));return out;}
template<class Records>auto find_network(Records& rows,const std::string& id){return std::find_if(rows.begin(),rows.end(),[&](const auto& row){return web_ftn_same_id(row.code,id);});}
template<class Records,class Predicate>void erase_if(Records& rows,Predicate predicate){rows.erase(std::remove_if(rows.begin(),rows.end(),predicate),rows.end());}
std::string tag(const Json& value){auto out=text(value,128);require(app::valid_ftn_echo_tag(out));for(auto& ch:out)if(ch>='a'&&ch<='z')ch=static_cast<char>(ch-'a'+'A');return out;}
std::optional<std::uint32_t> limit(const Json& value,unsigned maximum){std::optional<std::uint32_t> out; if(!value.is_null())out.emplace(integer(value,1,maximum));return out;}
}
Json web_ftn_settings(const app::FtnSettings& s){return {{"mode",static_cast<unsigned>(s.mode)},{"external_executable",s.external_executable},{"listener_enabled",s.listener_enabled},{"bind_address",s.bind_address},{"port",s.port},{"allow_direct_local_netmail",s.allow_direct_local_netmail},{"tls_listener_enabled",s.tls_listener_enabled},{"tls_port",s.tls_port},{"tls_certificate",s.tls_certificate},{"tls_private_key",s.tls_private_key}};}
Json web_ftn_network(const app::FtnNetwork& n){return {{"name",n.name},{"description",n.description},{"address",n.address},{"additional_addresses",n.additional_addresses},{"origin_line",n.origin_line},{"echo_list_url",n.echo_list_url},{"echo_list_source",static_cast<unsigned>(n.echo_list_source)},{"echo_list_file",n.echo_list_file},{"nodelist_file",n.nodelist_file},{"enabled",n.enabled},{"keep_processed",n.keep_processed},{"retention_days",n.retention_days},{"message_group",std::to_string(n.message_group)},{"new_group_name",n.new_group_name},{"msg_netmail_enabled",n.msg_netmail_enabled.value_or(false)},{"msg_netmail_directory",n.msg_netmail_directory},{"default_link",n.default_link}};}
Json web_ftn_link(const app::FtnLink& l){return {{"name",l.name},{"address",l.address},{"additional_addresses",l.additional_addresses},{"host",l.host},{"use",static_cast<unsigned>(l.use)},{"port",l.port},{"poll_minutes",l.poll_minutes},{"enabled",l.enabled},{"tls",l.tls},{"allow_legacy_plain_auth",l.allow_legacy_plain_auth},{"forward_missing_echoes",l.forward_missing_echoes},{"areafix",l.areafix_enabled?(*l.areafix_enabled?1:2):0},{"tls_server_name",l.tls_server_name},{"tls_ca_file",l.tls_ca_file},{"forward_via_link",l.forward_via_link},{"contact_on_mail",l.contact_on_mail}};}
Json web_ftn_echo(const app::FtnMessageEcho& e){return {{"tag",e.tag},{"description",e.description},{"local_echo",std::to_string(e.local_echo)},{"store_locally",e.store_locally},{"require_real_name",e.require_real_name},{"access_level",e.access_level},{"read_level",e.read_level},{"post_level",e.post_level},{"local_name",e.local_name},{"update_local_name",e.update_local_name},{"pipe_colors",e.pipe_colors},{"update_local_permissions",e.update_local_permissions},{"maximum_messages",e.maximum_messages?Json(*e.maximum_messages):Json(nullptr)},{"maximum_age_days",e.maximum_age_days?Json(*e.maximum_age_days):Json(nullptr)},{"update_local_retention",e.update_local_retention},{"ansi_colors",e.ansi_colors?Json(*e.ansi_colors):Json(nullptr)}};}
Json web_ftn_file(const app::FtnFileEcho& e){return {{"tag",e.tag},{"description",e.description},{"file_area",std::to_string(e.file_area)}};}
Json web_ftn_route(const app::FtnRoute& r){return {{"destination",r.destination},{"via_link",r.via_link},{"flavor",static_cast<unsigned>(r.flavor)}};}
Json web_ftn_network_list(const app::FtnConfiguration& c){
    Json networks=Json::array();for(const auto& n:c.networks){const auto count=[&](const auto& rows){return std::count_if(rows.begin(),rows.end(),[&](const auto& item){return web_ftn_same_id(item.network,n.code);});};
        networks.push_back({{"id",n.code},{"name",n.name},{"enabled",n.enabled},{"address",n.address},{"connections",count(c.links)},{"echoes",count(c.message_echoes)},{"files",count(c.file_echoes)}});}
    return {{"revision",std::to_string(c.revision)},{"message_catalog_revision",std::to_string(c.message_catalog_revision)},{"file_catalog_revision",std::to_string(c.file_catalog_revision)},{"settings",web_ftn_settings(c.settings)},{"networks",networks}};
}
app::FtnConfiguration web_ftn_record_update(const Json& request,const app::FtnConfiguration& current,const FtnSeal& seal){
    keys(request,{"revision","action","network","record","values","secrets"});require(identity(request.at("revision"))==current.revision,"ftn_revision_changed");
    const auto action=text(request.at("action"),32),network=code(request.at("network"),true),record=text(request.at("record"),128,true);const auto& v=request.at("values");const auto& secrets=request.at("secrets");
    auto next=current;
    if(action=="settings"){
        require(network.empty()&&record.empty());keys(secrets,{});keys(v,{"mode","external_executable","listener_enabled","bind_address","port","allow_direct_local_netmail","tls_listener_enabled","tls_port","tls_certificate","tls_private_key"});
        auto& s=next.settings;s.mode=static_cast<app::FtnMailerMode>(integer(v.at("mode"),0,2));s.external_executable=text(v.at("external_executable"),1024,true);
        const bool native=s.mode==app::FtnMailerMode::native;
        s.listener_enabled=flag(v.at("listener_enabled"));s.bind_address=text(v.at("bind_address"),253,!native);s.allow_direct_local_netmail=flag(v.at("allow_direct_local_netmail"));s.tls_listener_enabled=flag(v.at("tls_listener_enabled"));
        if(native&&s.listener_enabled)s.port=static_cast<std::uint16_t>(integer(v.at("port"),1,65535));
        if(native&&s.tls_listener_enabled){s.tls_port=static_cast<std::uint16_t>(integer(v.at("tls_port"),1,65535));s.tls_certificate=text(v.at("tls_certificate"),1024);s.tls_private_key=text(v.at("tls_private_key"),1024);}
        // Legacy shared .MSG policy is preserved internally and never edited.
        // The accepted UI uses each network's explicit .MSG choice only.
    }else if(action=="network_save"||action=="network_add"||action=="network_setup_add"||action=="network_setup_save"){
        require(!network.empty()&&record.empty());keys(secrets,{});keys(v,{"name","description","address","additional_addresses","origin_line","echo_list_url","echo_list_source","echo_list_file","nodelist_file","enabled","keep_processed","retention_days","message_group","new_group_name","msg_netmail_enabled","msg_netmail_directory","default_link"});
        auto found=find_network(next.networks,network);
        if(action=="network_add"||action=="network_setup_add"){require(found==next.networks.end(),"ftn_record_exists");app::FtnNetwork n;n.code=network;next.networks.push_back(std::move(n));found=std::prev(next.networks.end());}
        else require(found!=next.networks.end(),"ftn_record_removed");
        auto& n=*found;n.name=text(v.at("name"),96);n.description=text(v.at("description"),256,true);n.address=text(v.at("address"),96,true);n.additional_addresses=aliases(v.at("additional_addresses"));n.origin_line=text(v.at("origin_line"),128,true);
        n.echo_list_url=text(v.at("echo_list_url"),2048,true);n.echo_list_source=static_cast<app::FtnEchoListSource>(integer(v.at("echo_list_source"),0,1));n.echo_list_file=text(v.at("echo_list_file"),1024,true);n.nodelist_file=text(v.at("nodelist_file"),1024,true);
        n.enabled=flag(v.at("enabled"));n.keep_processed=flag(v.at("keep_processed"));n.retention_days=static_cast<std::uint16_t>(integer(v.at("retention_days"),0,65535));n.message_group=identity(v.at("message_group"),true);n.new_group_name=text(v.at("new_group_name"),64,true);
        n.msg_netmail_enabled=flag(v.at("msg_netmail_enabled"));n.msg_netmail_directory=text(v.at("msg_netmail_directory"),1024,true);n.default_link=code(v.at("default_link"),true);
        if(action=="network_setup_add"||action=="network_setup_save")require(app::parse_ftn_address(n.address).has_value()&&app::valid_ftn_display_text(n.name,64)&&n.name.front()!=' '&&n.name.back()!=' '&&(n.message_group||(!n.new_group_name.empty()&&n.new_group_name.front()!=' '&&n.new_group_name.back()!=' ')),"ftn_setup_network_fields_invalid");
    }else if(action=="link_save"||action=="link_add"){
        require(app::valid_ftn_code(record));require(!network.empty()&&find_network(next.networks,network)!=next.networks.end(),"ftn_network_removed");
        keys(v,{"name","address","additional_addresses","host","use","port","poll_minutes","enabled","tls","allow_legacy_plain_auth","forward_missing_echoes","areafix","tls_server_name","tls_ca_file","forward_via_link","contact_on_mail"});
        keys(secrets,{"session","packet","areafix","tic"});auto found=std::find_if(next.links.begin(),next.links.end(),[&](const auto& l){return web_ftn_same_id(l.network,network)&&web_ftn_same_id(l.code,record);});
        if(action=="link_add"){require(found==next.links.end(),"ftn_record_exists");app::FtnLink l;l.network=network;l.code=record;next.links.push_back(std::move(l));found=std::prev(next.links.end());}
        else require(found!=next.links.end(),"ftn_record_removed");
        auto& l=*found;l.name=text(v.at("name"),96);l.address=text(v.at("address"),96,true);l.additional_addresses=aliases(v.at("additional_addresses"));l.host=text(v.at("host"),253,true);l.use=static_cast<app::FtnConnectionUse>(integer(v.at("use"),0,2));l.port=static_cast<std::uint16_t>(integer(v.at("port"),0,65535));l.poll_minutes=static_cast<std::uint16_t>(integer(v.at("poll_minutes"),0,65535));l.enabled=flag(v.at("enabled"));l.tls=flag(v.at("tls"));l.allow_legacy_plain_auth=flag(v.at("allow_legacy_plain_auth"));l.forward_missing_echoes=flag(v.at("forward_missing_echoes"));
        const auto areafix=integer(v.at("areafix"),0,2);l.areafix_enabled=areafix?std::optional<bool>{areafix==1}:std::optional<bool>{};l.tls_server_name=text(v.at("tls_server_name"),253,true);l.tls_ca_file=text(v.at("tls_ca_file"),1024,true);l.forward_via_link=code(v.at("forward_via_link"),true);l.contact_on_mail=flag(v.at("contact_on_mail"));
        const std::array<const char*,4> names{{"session","packet","areafix","tic"}};const std::array<std::vector<std::uint8_t>*,4> fields{{&l.credentials.session,&l.credentials.packet,&l.credentials.areafix,&l.credentials.tic}};
        for(unsigned i=0;i<4;++i){const auto& secret=secrets.at(names[i]);keys(secret,{"action","value"});const auto mode=text(secret.at("action"),8);require(secret.at("value").is_string());const auto& plain=secret.at("value").get_ref<const std::string&>();require(app::valid_ftn_display_text(plain,i==1?8:128,true));
            if(mode=="keep"){require(plain.empty());continue;}if(mode=="clear"){require(plain.empty());fields[i]->clear();continue;}
            require(mode=="replace"&&!plain.empty());for(unsigned char ch:plain)require(ch>=32&&ch<=126,"ftn_password_ascii_required");require(static_cast<bool>(seal),"ftn_password_protection_unavailable");
            const auto protected_value=seal(l.network,l.code,i,plain);require(protected_value&& !protected_value->empty(),"ftn_password_protection_failed");*fields[i]=*protected_value;
        }
    }
#include "linux_web_ftn_catalog_edits.inc"
    else throw std::runtime_error("ftn_action_invalid");
    // Staged edits may leave another wizard item unfinished. This function
    // validates the complete resulting catalog before a publishable candidate
    // leaves the adapter; Save and Finish Later must explicitly disable it.
    require(app::validate_ftn_configuration(next).empty(),"ftn_configuration_invalid");return next;
}
Json web_ftn_clone_preview(const Json& request,const app::FtnConfiguration& current){
    require(request.at("action")=="echo_clone");const auto candidate=web_ftn_record_update(request,current,{});Json rows=Json::array();
    const auto network=request.at("network").get<std::string>();
    for(const auto& name:request.at("values").at("selected")){const auto target=name.get<std::string>();for(const auto& echo:candidate.message_echoes)if(web_ftn_same_id(echo.network,network)&&web_ftn_same_id(echo.tag,target))rows.push_back(web_ftn_echo(echo));}
    require(rows.size()==request.at("values").at("selected").size(),"ftn_clone_preview_incomplete");return rows;
}
} // namespace sfx::apps::runtime::web
