    const auto binkp_owner=[&](const httplib::Request& request){return cookie_name+'|'+cookie(request,cookie_name);};
    const auto binkp_reply=[](const std::optional<rt::web::WebBinkpReview>& review){
        if(!review)return Json{{"found",false}};
        return Json{{"found",true},{"handle",review->handle},{"action",review->request.action==app::FtnRuntimeAction::poll?"poll":"scan"},{"network",review->request.network},{"link",review->request.link},{"submitted",review->submitted},{"completed",review->completed},{"result",review->completed?rt::web::web_binkp_snapshot(review->result):Json(nullptr)}};
    };
    api_get("/api/control/binkp",[&,observation_epoch](const auto& request,auto& response){
        for(const auto& parameter:request.params)if((parameter.first!="view"&&parameter.first!="offset")||request.get_param_value_count(parameter.first)!=1)throw std::runtime_error("binkp_query_invalid");
        app::FtnRuntimeRequest query;const auto view=request.has_param("view")?request.get_param_value("view"):"status";
        if(view=="queues")query.action=app::FtnRuntimeAction::queues;else if(view=="connections")query.action=app::FtnRuntimeAction::connections;else if(view!="status")throw std::runtime_error("binkp_view_invalid");
        const auto offset=request.has_param("offset")?request.get_param_value("offset"):"0";const auto parsed=number(offset);
        if(parsed>1000000||std::to_string(parsed)!=offset||(view=="status"&&parsed))throw std::runtime_error("binkp_offset_invalid");
        query.offset=static_cast<std::uint32_t>(parsed);auto out=rt::web::web_binkp_snapshot(runtime.ftn_runtime(query));const auto state=runtime.status();
        out["epoch"]=observation_epoch(state.runtime_epoch);out["zone"]=state.bbs_time_zone;out["running"]=runtime.bbs_owned();json_response(response,out);
    });
    api_get("/api/control/binkp/action",[&,binkp_owner,binkp_reply](const auto& request,auto& response){if(!request.params.empty())throw std::runtime_error("binkp_query_invalid");json_response(response,binkp_reply(binkp_actions.current(binkp_owner(request))));});
    api_post("/api/control/binkp/prepare",[&,binkp_owner,binkp_reply,observation_epoch](const auto& request,auto& response){
        const auto value=parse(request,512);require_keys(value,{"action","network","link","epoch","revision"});const auto action=value.at("action").template get<std::string>();
        app::FtnRuntimeRequest command;if(action=="poll")command.action=app::FtnRuntimeAction::poll;else if(action=="scan")command.action=app::FtnRuntimeAction::scan;else throw std::runtime_error("binkp_action_invalid");
        command.network=value.at("network").template get<std::string>();command.link=value.at("link").template get<std::string>();if(app::encode_ftn_runtime_request(command).empty())throw std::runtime_error("binkp_target_invalid");
        const auto epoch=runtime.status().runtime_epoch;const auto current=runtime.ftn_runtime({});const auto revision=value.at("revision").template get<std::string>();
        if(value.at("epoch")!=observation_epoch(epoch)||revision!=std::to_string(current.saved_revision)||current.saved_revision<=0||current.code!=app::FtnConfigurationCode::ok||
            current.active_revision!=current.saved_revision||!current.capable||current.paused||current.serial_recovery_required||current.mode==app::FtnMailerMode::disabled||(action=="poll"&&current.mode!=app::FtnMailerMode::native)){json_response(response,{{"detail","FTN status changed or is not ready. Refresh before making a new request."}},409);return;}
        json_response(response,binkp_reply(binkp_actions.prepare(binkp_owner(request),command,epoch,current.saved_revision)));
    });
    api_post("/api/control/binkp/submit",[&,binkp_owner,binkp_reply](const auto& request,auto& response){const auto value=parse(request,256);require_keys(value,{"handle"});json_response(response,binkp_reply(binkp_actions.read(binkp_owner(request),value.at("handle").template get<std::string>(),true)));});
    api_post("/api/control/binkp/cancel",[&,binkp_owner](const auto& request,auto& response){const auto value=parse(request,256);require_keys(value,{"handle"});json_response(response,{{"cancelled",binkp_actions.cancel(binkp_owner(request),value.at("handle").template get<std::string>())}});});
