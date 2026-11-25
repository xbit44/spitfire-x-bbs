#include "sfx/application/ftn_echo_import.hpp"
#include <algorithm>
namespace sfx::application {
namespace {std::string folded(std::string s){for(auto& ch:s)if(ch>='a'&&ch<='z')ch=static_cast<char>(ch-'a'+'A');return s;}}
FtnEchoImportDestination resolve_ftn_echo_import_destination(const FtnConfiguration& c,std::string_view code,
    const std::vector<std::pair<std::int64_t,std::string>>& groups,
    const std::vector<std::pair<std::int64_t,std::int64_t>>& local_echo_groups){
    FtnEchoImportDestination out;const auto fail=[&](const char* reason){out.issues.push_back({"import.group",reason});return out;};
    const auto n=std::find_if(c.networks.begin(),c.networks.end(),[&](const auto& v){return v.code==code;});
    if(n==c.networks.end())return fail("unknown_network");
    out.group=n->message_group;
    for(const auto& echo:c.message_echoes)if(echo.network==code&&echo.store_locally&&echo.local_echo){
        const auto found=std::find_if(local_echo_groups.begin(),local_echo_groups.end(),[&](const auto& v){return v.first==echo.local_echo;});
        if(found==local_echo_groups.end()||found->second<=0)return fail("missing_mapping");
        if(out.group&&out.group!=found->second)return fail("mapped_groups_differ");
        out.group=found->second;
    }
    if(out.group){const auto found=std::find_if(groups.begin(),groups.end(),[&](const auto& v){return v.first==out.group;});if(found==groups.end())return fail("missing_group");out.name=found->second;return out;}
    out.name=n->new_group_name.empty()?n->name:n->new_group_name;
    if(!valid_ftn_display_text(out.name,64)||out.name.front()==' '||out.name.back()==' ')return fail("invalid_group_name");
    for(const auto& group:groups)if(folded(group.second)==folded(out.name)){
        if(out.group||group.first<=0)return fail("ambiguous_group");
        out.group=group.first;
    }
    return out;
}
void link_new_ftn_echoes_to_hub(const FtnConfiguration& before,FtnConfiguration& after,std::string_view code){
    const auto network=std::find_if(after.networks.begin(),after.networks.end(),[&](const auto& n){return n.code==code;});if(network==after.networks.end())return;
        const FtnLink* hub=nullptr;
        for(const auto& link:after.links)if(link.network==code&&link.code==network->default_link)hub=&link;
        if(!hub){for(const auto& link:after.links)if(link.network==code&&link.enabled){if(hub){hub=nullptr;break;}hub=&link;}}
        if(hub)for(const auto& echo:after.message_echoes)if(echo.network==code){
            const bool existing=std::any_of(before.message_echoes.begin(),before.message_echoes.end(),[&](const auto& e){return e.network==code&&folded(e.tag)==folded(echo.tag)&&(!e.store_locally||e.local_echo);});
            const bool permission=std::any_of(after.permissions.begin(),after.permissions.end(),[&](const auto& v){return v.network==code&&v.link==hub->code&&folded(v.tag)==folded(echo.tag)&&!v.file_echo;});
            if(!existing&&!permission)after.permissions.push_back({std::string(code),hub->code,echo.tag,false,true,true,false});
        }
}
FtnEchoImportPlan plan_ftn_echo_import(const FtnConfiguration& before,std::string_view code,const FtnEchoList& list,const std::vector<FtnEchoImportRequirements>& requirements,std::size_t existing_count,std::int64_t group,std::string new_group,bool update){
    FtnEchoImportPlan out;out.configuration=before;
    const auto network=std::find_if(out.configuration.networks.begin(),out.configuration.networks.end(),[&](const auto& n){return n.code==code;});
    if(network==out.configuration.networks.end()||!list.complete||requirements.size()!=list.entries.size()||group<0||existing_count>999){out.issues.push_back({"import","invalid_source_or_catalog"});return out;}
    std::size_t pending{},appended{};for(const auto& e:before.message_echoes)if(e.store_locally&&!e.local_echo)++pending;
    for(std::size_t i=0;i<list.entries.size();++i){const auto& item=list.entries[i];if(!item.selected)continue;++out.selected;
        const auto found=std::find_if(out.configuration.message_echoes.begin(),out.configuration.message_echoes.end(),[&](const auto& e){return e.network==code&&folded(e.tag)==folded(item.tag);});
        if(found!=out.configuration.message_echoes.end()&&found->store_locally&&!found->local_echo){
            // These are not saved conferences. A failed/reviewed draft may be
            // retried with a corrected destination or security requirements.
            const auto& r=requirements[i];found->description=item.description;found->local_name=item.tag;
            found->access_level=r.access;found->read_level=r.read;found->post_level=r.post;++out.created;
        }else if(found!=out.configuration.message_echoes.end()){
            if(update){found->description=item.description;if(found->store_locally){if(item.tag.size()>64){out.issues.push_back({"import.local_name","name_too_long"});continue;}found->local_name=item.tag;found->update_local_name=found->local_echo!=0;}++out.text_updates;}else ++out.unchanged;
        }else{const auto& r=requirements[i];FtnMessageEcho echo;echo.network=std::string(code);echo.tag=item.tag;echo.description=item.description;echo.local_name=item.tag;echo.access_level=r.access;echo.read_level=r.read;echo.post_level=r.post;echo.ansi_colors=false;echo.require_real_name=folded(network->name)=="FIDONET";out.configuration.message_echoes.push_back(std::move(echo));++out.created;++appended;}
    }
    if(out.created){network->message_group=group;network->new_group_name=std::move(new_group);
        link_new_ftn_echoes_to_hub(before,out.configuration,code);
    }
    if(appended>999-existing_count||pending>999-existing_count-appended)out.issues.push_back({"import.capacity","selected_conferences_exceed_capacity"});
    auto issues=validate_ftn_configuration(out.configuration);out.issues.insert(out.issues.end(),issues.begin(),issues.end());return out;
}
}
