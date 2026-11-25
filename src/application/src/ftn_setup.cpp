#include "sfx/application/ftn_setup.hpp"
#include "sfx/application/message.hpp"
#include <algorithm>
#include <set>

namespace sfx::application {
namespace {
std::string fold(std::string_view value){std::string out(value);for(auto& c:out)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');return out;}
FtnSetupResult failure(const char* field,const char* reason){return {0,{{field,reason}}};}
bool level(const std::optional<std::uint16_t>& value){return !value||(*value>=1&&*value<=999);}
}
FtnSetupResult add_ftn_setup_network(FtnConfiguration& draft,FtnNetwork network,
    const std::vector<std::pair<std::int64_t,std::string>>& groups){
    if(!valid_ftn_code(network.code)||!valid_ftn_display_text(network.name,96)||!parse_ftn_address(network.address))return failure("network","name_and_address_required");
    for(const auto& existing:draft.networks)if(fold(existing.code)==fold(network.code)||fold(existing.name)==fold(network.name))return failure("network","already_exists");
    if(!network.message_group){
        const auto name=network.new_group_name.empty()?network.name:network.new_group_name;
        if(!valid_ftn_display_text(name,64)||name.front()==' '||name.back()==' ')return failure("network.message_group","valid_name_required");
        for(const auto& group:groups)if(fold(group.second)==fold(name)){
            if(group.first<=0||network.message_group)return failure("network.message_group","ambiguous");
            network.message_group=group.first;
        }
        network.new_group_name=network.message_group?std::string{}:name;
    }else if(std::none_of(groups.begin(),groups.end(),[&](const auto& g){return g.first==network.message_group;}))return failure("network.message_group","unknown");
    // Scanning is an explicit per-network choice. An absent legacy choice is Off.
    if(!network.msg_netmail_enabled)network.msg_netmail_enabled=false;
    auto next=draft;next.networks.push_back(std::move(network));
    auto issues=validate_ftn_configuration(next);if(!issues.empty())return {0,std::move(issues)};
    draft=std::move(next);return {1,{}};
}
FtnSetupResult add_ftn_setup_echo(FtnConfiguration& draft,std::string_view code,
    std::string_view tag,std::string_view description,std::string_view caller_name){
    const auto network=std::find_if(draft.networks.begin(),draft.networks.end(),[&](const auto& n){return fold(n.code)==fold(code);});
    if(network==draft.networks.end())return failure("echo.network","unknown");
    if(!valid_ftn_echo_tag(tag)||!valid_ftn_display_text(description,512,true))return failure("echo","invalid_tag_or_description");
    const auto normalized=fold(tag);
    for(const auto& e:draft.message_echoes)if(fold(e.network)==fold(code)&&fold(e.tag)==normalized)return {};
    const auto name=caller_name.empty()?tag:caller_name;
    if(!valid_ftn_display_text(name,64)||name.front()==' '||name.back()==' ')return failure("echo.local_name","name_required");
    FtnMessageEcho echo;echo.network=network->code;echo.tag=normalized;echo.description=std::string(description);echo.local_name=std::string(name);
    echo.require_real_name=fold(network->name)=="FIDONET";echo.ansi_colors=false;echo.pipe_colors=false;
    echo.maximum_messages=500;echo.maximum_age_days.reset();
    auto next=draft;next.message_echoes.push_back(std::move(echo));
    auto issues=validate_ftn_configuration(next);if(!issues.empty())return {0,std::move(issues)};
    draft=std::move(next);return {1,{}};
}
FtnSetupResult add_ftn_setup_echoes(FtnConfiguration& draft,std::string_view code,
    const std::vector<FtnEchoListEntry>& entries){
    const auto network=std::find_if(draft.networks.begin(),draft.networks.end(),[&](const auto& n){return fold(n.code)==fold(code);});
    if(network==draft.networks.end())return failure("echo.network","unknown");
    std::set<std::string> existing;for(const auto& e:draft.message_echoes)if(fold(e.network)==fold(code))existing.insert(fold(e.tag));
    auto next=draft;std::size_t changed{};
    for(const auto& entry:entries)if(entry.selected){
        if(!valid_ftn_echo_tag(entry.tag)||!valid_ftn_display_text(entry.description,512,true))return failure("echo","invalid_tag_or_description");
        const auto tag=fold(entry.tag);if(!existing.insert(tag).second)continue;
        if(!valid_ftn_display_text(entry.tag,64)||entry.tag.front()==' '||entry.tag.back()==' ')return failure("echo.local_name","name_required");
        FtnMessageEcho echo;echo.network=network->code;echo.tag=tag;echo.description=entry.description;echo.local_name=entry.tag;
        echo.require_real_name=fold(network->name)=="FIDONET";echo.ansi_colors=false;echo.pipe_colors=false;
        echo.maximum_messages=500;echo.maximum_age_days.reset();next.message_echoes.push_back(std::move(echo));++changed;
    }
    const auto issues=validate_ftn_configuration(next);if(!issues.empty())return {0,issues};
    draft=std::move(next);return {changed,{}};
}
FtnSetupResult apply_ftn_echo_options(FtnConfiguration& draft,std::string_view code,
    const std::vector<std::string>& explicit_targets,const FtnEchoBulkOptions& options){
    if(!options.access&&!options.reading&&!options.posting&&!options.copy_count&&!options.copy_age)return {};
    if(explicit_targets.empty())return {};
    if(!level(options.access)||!level(options.reading)||!level(options.posting))return failure("echo.levels","invalid");
    if(options.copy_count&&options.maximum_messages&&(!*options.maximum_messages||*options.maximum_messages>kMaximumRetainedMessageCount))return failure("echo.retention.count","invalid");
    if(options.copy_age&&options.maximum_age_days&&(!*options.maximum_age_days||*options.maximum_age_days>kMaximumMessageAgeDays))return failure("echo.retention.age","invalid");
    if(std::none_of(draft.networks.begin(),draft.networks.end(),[&](const auto& n){return fold(n.code)==fold(code);}))return failure("echo.network","unknown");
    std::set<std::string> targets;
    for(const auto& tag:explicit_targets)if(!valid_ftn_echo_tag(tag)||!targets.insert(fold(tag)).second)return failure("echo.targets","invalid_or_duplicate");
    auto next=draft;std::size_t changed{};
    for(auto& echo:next.message_echoes)if(fold(echo.network)==fold(code)&&targets.erase(fold(echo.tag))){
        bool permissions=false;
        if(options.access){permissions|=echo.access_level!=*options.access;echo.access_level=*options.access;}
        if(options.reading){permissions|=echo.read_level!=*options.reading;echo.read_level=*options.reading;}
        if(options.posting){permissions|=echo.post_level!=*options.posting;echo.post_level=*options.posting;}
        echo.update_local_permissions|=echo.local_echo&&permissions;
        if(options.copy_count){echo.maximum_messages=options.maximum_messages;echo.update_local_retention|=1;}
        if(options.copy_age){echo.maximum_age_days=options.maximum_age_days;echo.update_local_retention|=2;}
        ++changed;
    }
    if(!targets.empty())return failure("echo.targets","unknown_in_network");
    auto issues=validate_ftn_configuration(next);if(!issues.empty())return {0,std::move(issues)};
    draft=std::move(next);return {changed,{}};
}
std::vector<FtnStoredDirectory> ftn_stored_directories(const FtnConfiguration& c){
    std::vector<FtnStoredDirectory> result;
    const auto key=[](std::string path){for(auto& ch:path){if(ch=='/')ch='\\';else if(ch>='a'&&ch<='z')ch=static_cast<char>(ch-'a'+'A');}while(path.size()>3&&path.back()=='\\')path.pop_back();return path;};
    for(const auto& n:c.networks){
        if(!n.enabled)continue;
        const bool enabled=n.msg_netmail_enabled.value_or(false);
        if(!enabled)continue;
        const auto& directory=n.msg_netmail_directory;
        if(directory.empty())continue;
        const auto found=std::find_if(result.begin(),result.end(),[&](const auto& source){return key(source.directory)==key(directory);});
        if(found==result.end())result.push_back({directory,{n.code}});else found->networks.push_back(n.code);
    }
    return result;
}
} // namespace sfx::application
