    api_post("/api/ftn/setup-ids",[&](const auto& request,auto& response){
        const auto value=parse(request,512);require_keys(value,{"draft","edit_revision"});
        const auto draft=ftn_drafts.read(ftn_owner(request),rt::web::web_ftn_token(value.at("draft").template get<std::string>()));
        if(draft.state!=rt::web::WebFtnDraftState::editing||draft.edit_revision!=number(value.at("edit_revision").template get<std::string>()))throw std::runtime_error("ftn_setup_draft_changed");
        std::string network,hub;do{network="N"+random_token().substr(0,16);}while(std::any_of(draft.configuration.networks.begin(),draft.configuration.networks.end(),[&](const auto& n){return rt::web::web_ftn_same_id(n.code,network);}));hub="H"+random_token().substr(0,16);
        json_response(response,{{"network",network},{"hub",hub}});
    });
    api_post("/api/ftn/setup-step",[&](const auto& request,auto& response){
        auto value=parse(request,120000);
        struct Erase {Json& value;~Erase(){if(!value.is_object()||!value.contains("changes")||!value["changes"].is_array())return;for(auto& change:value["changes"]){if(!change.is_object()||!change.contains("secrets")||!change["secrets"].is_object())continue;for(auto& entry:change["secrets"].items())if(entry.value().is_object()&&entry.value().contains("value")&&entry.value()["value"].is_string()){auto& plain=entry.value()["value"].get_ref<std::string&>();sodium_memzero(plain.data(),plain.size());}}}}erase{value};
        require_keys(value,{"draft","edit_revision","changes"});
        json_response(response,rt::web::web_ftn_draft_summary(ftn_drafts.edit_batch(ftn_owner(request),rt::web::web_ftn_token(value.at("draft").template get<std::string>()),number(value.at("edit_revision").template get<std::string>()),value.at("changes"))));
    });
