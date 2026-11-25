    api_get("/api/ftn/held",[&](const auto& request,auto& response){
        const auto query=rt::web::web_ftn_held_query(ftn_query(request,{{"ingress",""},{"after",""},{"offset","0"},{"network",""},{"reason",""},{"find",""}}));
        auto out=rt::web::web_ftn_held_reply(runtime.ftn_held(query));
        out["time_zone"]="";const auto current=runtime.configuration();
        if(current){const auto zone=host::LinuxBbsTimeZoneRuleProvider::create(current->active.bbs_time_zone);if(zone)out["time_zone"]=zone->iana_zone();}
        if(!query.ingress.valid()){
            out["networks"]=Json::array();
            const auto saved=rt::web::web_ftn_read([&](const auto& q){return runtime.ftn_configuration(q);},rt::web::web_ftn_token(random_token().substr(0,32)));
            out["networks_available"]=saved.configuration.has_value();
            if(saved.configuration)for(const auto& network:saved.configuration->networks)out["networks"].push_back({{"code",network.code},{"name",network.name}});
        }
        json_response(response,out);
    });
    api_get("/api/ftn/held/recipients",[&](const auto& request,auto& response){
        const auto query=rt::web::web_ftn_held_recipients_query(ftn_query(request,{{"after","0"},{"find",""}}));
        json_response(response,rt::web::web_ftn_held_reply(runtime.ftn_held(query)));
    });
    api_post("/api/ftn/held/token",[&](const auto& request,auto& response){require_keys(parse(request,128),{});json_response(response,{{"operation",random_token().substr(0,32)}});});
    api_post("/api/ftn/held/action",[&](const auto& request,auto& response){
        // The retained exact request can reconcile its durable receipt. Storage
        // owns idempotency, revision/CAS and current policy checks; no UI proof,
        // credential, native path or body is accepted by this narrow adapter.
        const auto action=rt::web::web_ftn_held_action(parse(request,4096));
        json_response(response,rt::web::web_ftn_held_reply(runtime.ftn_held(action)));
    });
