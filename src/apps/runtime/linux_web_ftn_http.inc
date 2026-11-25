    const auto ftn_owner=[&](const httplib::Request& request){return cookie_name+'|'+cookie(request,cookie_name);};
    const auto ftn_summary=[&](const auto& draft){
        auto out=rt::web::web_ftn_draft_summary(draft);
        if(draft.review){out["review"]["time_zone"]="";const auto config=runtime.configuration();if(config){const auto zone=host::LinuxBbsTimeZoneRuleProvider::create(config->active.bbs_time_zone);if(zone)out["review"]["time_zone"]=zone->iana_zone();}}
        return out;
    };
    const auto ftn_query=[&](const httplib::Request& request,Json query){for(const auto& item:request.params){if(!query.contains(item.first)||request.get_param_value_count(item.first)!=1)throw std::runtime_error("ftn_query_invalid");query[item.first]=item.second;}return query;};
    api_post("/api/ftn/open",[&](const auto& request,auto& response){require_keys(parse(request,128),{});json_response(response,ftn_summary(ftn_drafts.open(ftn_owner(request))));});
    api_get("/api/ftn/state",[&](const auto& request,auto& response){const auto query=ftn_query(request,{{"draft",""}});json_response(response,ftn_summary(ftn_drafts.read(ftn_owner(request),rt::web::web_ftn_token(query.at("draft").template get<std::string>()))));});
    api_get("/api/ftn/page",[&](const auto& request,auto& response){
        auto query=ftn_query(request,{{"draft",""},{"edit_revision",""},{"section","networks"},{"network",""},{"record",""},{"offset","0"},{"search",""}});
        const auto draft=ftn_drafts.read(ftn_owner(request),rt::web::web_ftn_token(query.at("draft").template get<std::string>()));query.erase("draft");json_response(response,rt::web::web_ftn_draft_page(draft,query));
    });
    api_post("/api/ftn/edit",[&](const auto& request,auto& response){
        auto value=parse(request,1048576);
        struct EraseSecrets {Json& value;~EraseSecrets(){if(!value.is_object()||!value.contains("change")||!value["change"].is_object()||!value["change"].contains("secrets"))return;auto& secrets=value["change"]["secrets"];if(!secrets.is_object())return;for(auto& entry:secrets.items())if(entry.value().is_object()&&entry.value().contains("value")&&entry.value()["value"].is_string()){auto& plain=entry.value()["value"].get_ref<std::string&>();sodium_memzero(plain.data(),plain.size());}}} erase{value};
        require_keys(value,{"draft","edit_revision","change"});const auto handle=rt::web::web_ftn_token(value.at("draft").template get<std::string>());const auto revision=number(value.at("edit_revision").template get<std::string>());
        const auto changed=ftn_drafts.edit(ftn_owner(request),handle,revision,value.at("change"));auto out=ftn_summary(changed);
        const auto& change=value.at("change");const auto action=change.at("action").template get<std::string>();
        if(action=="route_add"||action=="route_save"){const auto address=app::parse_ftn_address(change.at("values").at("destination").template get<std::string>());if(address)out["selected_record"]=app::format_ftn_address(*address);}
        if(action=="echo_add"||action=="echo_save"||action=="file_add"||action=="file_save"){
            const auto network=change.at("network").template get<std::string>(),tag=change.at("values").at("tag").template get<std::string>();
            const auto select=[&](const auto& rows){for(const auto& echo:rows)if(rt::web::web_ftn_same_id(echo.network,network)&&rt::web::web_ftn_same_id(echo.tag,tag))out["selected_record"]=echo.tag;};
            if(action=="echo_add"||action=="echo_save")select(changed.configuration.message_echoes);else select(changed.configuration.file_echoes);
        }
        json_response(response,out);
    });
    api_post("/api/ftn/clone-preview",[&](const auto& request,auto& response){
        const auto value=parse(request,1048576);require_keys(value,{"draft","edit_revision","change"});
        const auto draft=ftn_drafts.read(ftn_owner(request),rt::web::web_ftn_token(value.at("draft").template get<std::string>()));
        if(draft.state!=rt::web::WebFtnDraftState::editing||number(value.at("edit_revision").template get<std::string>())!=draft.edit_revision||value.at("change").at("action")!="echo_clone")throw std::runtime_error("ftn_clone_changed");
        const auto rows=rt::web::web_ftn_clone_preview(value.at("change"),draft.configuration);
        json_response(response,{{"draft",value.at("draft")},{"edit_revision",value.at("edit_revision")},{"rows",rows}});
    });
    api_get("/api/ftn/credentials",[&](const auto& request,auto& response){
        const auto query=ftn_query(request,{{"draft",""},{"edit_revision",""},{"network",""},{"record",""}});const auto draft=ftn_drafts.read(ftn_owner(request),rt::web::web_ftn_token(query.at("draft").template get<std::string>()));
        if(number(query.at("edit_revision").template get<std::string>())!=draft.edit_revision)throw std::runtime_error("ftn_draft_changed");
        const auto network=query.at("network").template get<std::string>(),record=query.at("record").template get<std::string>();const auto link=std::find_if(draft.configuration.links.begin(),draft.configuration.links.end(),[&](const auto& l){return rt::web::web_ftn_same_id(l.network,network)&&rt::web::web_ftn_same_id(l.code,record);});if(link==draft.configuration.links.end())throw std::runtime_error("ftn_connection_removed");
        Json out=Json::object();const std::array<const char*,4> names{{"session","packet","areafix","tic"}};const std::array<const std::vector<std::uint8_t>*,4> envelopes{{&link->credentials.session,&link->credentials.packet,&link->credentials.areafix,&link->credentials.tic}};
        struct EraseOutput {Json& out;~EraseOutput(){for(auto& item:out.items())if(item.value().contains("value")&&item.value()["value"].is_string()){auto& value=item.value()["value"].get_ref<std::string&>();sodium_memzero(value.data(),value.size());}}} erase{out};
        for(unsigned i=0;i<4;++i){auto plain=envelopes[i]->empty()?std::optional<app::RloginSecret>{}:host::unprotect_ftn_secret(root.string(),link->network,link->code,static_cast<host::FtnCredentialKind>(i),*envelopes[i]);out[names[i]]={{"present",!envelopes[i]->empty()},{"readable",envelopes[i]->empty()||plain.has_value()},{"value",plain?Json(std::string(plain->view())):Json("")}};}
        json_response(response,out);
    });
    api_post("/api/ftn/prepare",[&](const auto& request,auto& response){const auto value=parse(request,512);require_keys(value,{"draft","edit_revision"});const auto result=ftn_drafts.prepare(ftn_owner(request),rt::web::web_ftn_token(value.at("draft").template get<std::string>()),number(value.at("edit_revision").template get<std::string>()));json_response(response,ftn_summary(result));});
    api_post("/api/ftn/commit",[&](const auto& request,auto& response){const auto value=parse(request,512);require_keys(value,{"draft","operation"});json_response(response,ftn_summary(ftn_drafts.commit(ftn_owner(request),rt::web::web_ftn_token(value.at("draft").template get<std::string>()),rt::web::web_ftn_token(value.at("operation").template get<std::string>()))));});
    api_post("/api/ftn/result",[&](const auto& request,auto& response){const auto value=parse(request,512);require_keys(value,{"draft"});json_response(response,ftn_summary(ftn_drafts.check_result(ftn_owner(request),rt::web::web_ftn_token(value.at("draft").template get<std::string>()))));});
    api_post("/api/ftn/cancel-review",[&](const auto& request,auto& response){const auto value=parse(request,512);require_keys(value,{"draft"});json_response(response,ftn_summary(ftn_drafts.cancel_review(ftn_owner(request),rt::web::web_ftn_token(value.at("draft").template get<std::string>()))));});
    api_post("/api/ftn/discard",[&](const auto& request,auto& response){const auto value=parse(request,512);require_keys(value,{"draft"});ftn_drafts.discard(ftn_owner(request),rt::web::web_ftn_token(value.at("draft").template get<std::string>()));json_response(response,{{"discarded",true}});});
#include "linux_web_ftn_import_http.inc"
#include "linux_web_ftn_setup_http.inc"
#include "linux_web_ftn_held_http.inc"
