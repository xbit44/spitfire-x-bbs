#include "linux_web_ftn_view.hpp"
#include <algorithm>
#include <charconv>

namespace sfx::apps::runtime::web {
namespace {
namespace app=application;using Json=nlohmann::json;
void require(bool value){if(!value)throw std::runtime_error("ftn_page_invalid_or_changed");}
std::string text(const Json& value,std::size_t maximum){require(value.is_string());const auto out=value.get<std::string>();require(app::valid_ftn_display_text(out,maximum,true));return out;}
std::uint64_t number(const Json& value){const auto s=text(value,20);std::uint64_t n{};const auto result=std::from_chars(s.data(),s.data()+s.size(),n);require(result.ec==std::errc{}&&result.ptr==s.data()+s.size()&&std::to_string(n)==s);return n;}
std::string fold(std::string s){for(auto& c:s)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');return s;}
}
std::string web_ftn_token_text(app::FtnTransferToken token){std::string out;for(auto b:token){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}return out;}
app::FtnTransferToken web_ftn_token(std::string_view s){require(s.size()==32);app::FtnTransferToken out{};const auto digit=[](char c){require((c>='0'&&c<='9')||(c>='a'&&c<='f'));return c<='9'?c-'0':c-'a'+10;};for(std::size_t i=0;i<16;++i)out[i]=static_cast<std::uint8_t>(digit(s[i*2])*16+digit(s[i*2+1]));require(std::any_of(out.begin(),out.end(),[](auto b){return b!=0;}));return out;}
Json web_ftn_draft_summary(const WebFtnDraft& draft){
    Json out={{"draft",web_ftn_token_text(draft.handle)},{"edit_revision",std::to_string(draft.edit_revision)},{"revision",std::to_string(draft.configuration.revision)},
        {"state",draft.state==WebFtnDraftState::editing?"editing":draft.state==WebFtnDraftState::review?"review":"submitted"},{"operation",web_ftn_token_text(draft.operation)}};
    if(draft.result)out["result"]={{"code",static_cast<unsigned>(draft.result->code)},{"revision",std::to_string(draft.result->revision)},{"saved",draft.result->code==app::FtnConfigurationCode::ok}};
    if(draft.review){const auto& r=*draft.review;Json networks=Json::array();for(const auto& n:r.networks)networks.push_back({{"network",n.network},{"name",n.name},{"due_now",n.due_now},{"expire_later",n.expire_later},{"from_forever",n.from_forever},{"earliest",std::to_string(n.earliest)},{"latest",std::to_string(n.latest)}});
        out["review"]={{"changed",r.changed},{"shortened",r.shortened},{"reviewed_utc",std::to_string(r.reviewed_utc)},{"networks",networks}};
    }
    return out;
}
Json web_ftn_draft_page(const WebFtnDraft& draft,const Json& request){
    require(request.is_object()&&request.size()==6);for(const auto* key:{"edit_revision","section","network","record","offset","search"})require(request.contains(key));
    require(number(request.at("edit_revision"))==draft.edit_revision);const auto section=text(request.at("section"),16),network=text(request.at("network"),48),record=text(request.at("record"),128),search=fold(text(request.at("search"),128));const auto offset=number(request.at("offset"));require(offset<=65536);
    const auto& c=draft.configuration;auto out=web_ftn_draft_summary(draft);out["section"]=section;out["network"]=network;out["record"]=record;out["offset"]=offset;out["rows"]=Json::array();out["values"]=nullptr;std::size_t total{};
    const auto add=[&](const std::string& id,const std::string& label){if(fold(id+" "+label).find(search)==std::string::npos)return;if(total>=offset&&total-offset<25)out["rows"].push_back({{"id",id},{"label",label}});++total;};
    const auto net=std::find_if(c.networks.begin(),c.networks.end(),[&](const auto& n){return web_ftn_same_id(n.code,network);});
    if(section=="settings"){require(network.empty()&&record.empty());out["values"]=web_ftn_settings(c.settings);}
    else if(section=="networks"){
        require(network.empty());for(const auto& n:c.networks){add(n.code,n.name+(n.enabled?"":" (disabled)"));if(web_ftn_same_id(n.code,record))out["values"]=web_ftn_network(n);}
        if(record.empty()){app::FtnNetwork n;n.msg_netmail_enabled=false;out["values"]=web_ftn_network(n);}
    }else{
        require(net!=c.networks.end());out["network_name"]=net->name;
        if(section=="links"){
            for(const auto& l:c.links)if(web_ftn_same_id(l.network,network)){add(l.code,l.name+(l.enabled?"":" (disabled)"));if(web_ftn_same_id(l.code,record))out["values"]=web_ftn_link(l);}
            if(record.empty()){app::FtnLink l;l.areafix_enabled=false;out["values"]=web_ftn_link(l);}
        }else if(section=="clone"){
            require(record.empty());for(const auto& e:c.message_echoes)if(web_ftn_same_id(e.network,network))add(e.tag,e.local_name.empty()?e.description:e.local_name);
            out["values"]={{"copy_count",false},{"maximum_messages",500},{"copy_age",false},{"maximum_age_days",nullptr},{"copy_access",false},{"access_level",10},{"copy_read",false},{"read_level",10},{"copy_post",false},{"post_level",10}};
        }else if(section=="echoes"){
            for(const auto& e:c.message_echoes)if(web_ftn_same_id(e.network,network)){add(e.tag,e.description);if(web_ftn_same_id(e.tag,record))out["values"]=web_ftn_echo(e);}
            if(record.empty()){app::FtnMessageEcho e;e.ansi_colors=false;e.require_real_name=fold(net->name)=="FIDONET";out["values"]=web_ftn_echo(e);}
        }else if(section=="files"){
            for(const auto& e:c.file_echoes)if(web_ftn_same_id(e.network,network)){add(e.tag,e.description);if(web_ftn_same_id(e.tag,record))out["values"]=web_ftn_file(e);}
            if(record.empty())out["values"]=web_ftn_file(app::FtnFileEcho{});
        }else if(section=="routes"){
            for(const auto& r:c.routes)if(web_ftn_same_id(r.network,network)){add(r.destination,"via "+r.via_link);if(r.destination==record)out["values"]=web_ftn_route(r);}
            if(record.empty())out["values"]=web_ftn_route(app::FtnRoute{});
            out["default_link"]=net->default_link;
        }else if(section=="permissions"){
            require(std::any_of(c.links.begin(),c.links.end(),[&](const auto& l){return web_ftn_same_id(l.network,network)&&web_ftn_same_id(l.code,record);}));
            const auto echo=[&](const std::string& tag,bool file){const auto before=out["rows"].size();add((file?"F:":"M:")+tag,tag);if(out["rows"].size()==before)return;auto& row=out["rows"].back();row["tag"]=tag;row["file_echo"]=file;row["receive"]=row["send"]=row["manage"]=false;
                const auto p=std::find_if(c.permissions.begin(),c.permissions.end(),[&](const auto& value){return web_ftn_same_id(value.network,network)&&web_ftn_same_id(value.link,record)&&web_ftn_same_id(value.tag,tag)&&value.file_echo==file;});if(p!=c.permissions.end()){row["receive"]=p->receive;row["send"]=p->send;row["manage"]=p->manage;}};
            for(const auto& e:c.message_echoes)if(web_ftn_same_id(e.network,network))echo(e.tag,false);
            for(const auto& e:c.file_echoes)if(web_ftn_same_id(e.network,network))echo(e.tag,true);
        }else throw std::runtime_error("ftn_section_unavailable");
    }
    if(section!="permissions")require(!out["values"].is_null());
    out["total"]=total;out["next"]=offset+out["rows"].size()<total;return out;
}
} // namespace sfx::apps::runtime::web
