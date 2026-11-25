    const auto caller_text=[](const Json& value){
        const auto text=value.get<std::string>();if(text.size()>2048)throw std::runtime_error("caller_text_too_long");
        const auto converted=app::qwk_utf8_to_cp437(text);
        if(!converted||converted->empty()||converted->size()>512||app::chat_safe_text(*converted)!=*converted)throw std::runtime_error("caller_text_invalid");
        return *converted;
    };
    const auto caller_number=[](const Json& value){const auto text=value.get<std::string>();const auto result=number(text);if(std::to_string(result)!=text)throw std::runtime_error("caller_number_invalid");return result;};
    const auto notice_json=[](const std::optional<rt::web::WebNoticeReview>& value){
        if(!value)return Json{{"found",false}};
        Json target=nullptr;
        if(value->request.target){const auto& id=*value->request.target;std::array<char,33> epoch{};sodium_bin2hex(epoch.data(),epoch.size(),id.runtime_epoch.bytes.data(),id.runtime_epoch.bytes.size());target={{"node",id.node},{"epoch",epoch.data()},{"generation",std::to_string(id.generation)}};}
        return Json{{"found",true},{"handle",value->handle},{"target",target},{"room",value->request.room},{"text",app::qwk_cp437_to_utf8(value->request.text)},
            {"override_dnd",value->request.override_dnd},{"submitted",value->submitted},{"completed",value->completed},{"code",static_cast<unsigned>(value->result)},
            {"detail",value->completed?(value->result==app::ChatResult::ok?"Notice queued for safe delivery.":std::string(app::chat_result_text(value->result))):value->submitted?"The send result is unknown. Check this result; the notice will not be sent again automatically.":"Review the destination and notice before Send."}};
    };
    const auto operator_json=[](const std::optional<rt::web::WebOperatorView>& value){
        if(!value)return Json{{"present",false}};
        const auto& view=*value;const auto& snapshot=view.snapshot;Json lines=Json::array();
        for(const auto& line:snapshot.lines)lines.push_back({{"sequence",std::to_string(line.sequence)},{"text",app::qwk_cp437_to_utf8(line.text)}});
        return Json{{"present",true},{"handle",view.handle},{"code",static_cast<unsigned>(snapshot.code)},{"detail",std::string(app::chat_result_text(snapshot.code))},
            {"pending",snapshot.pending},{"active",snapshot.active},{"paused",snapshot.paused},{"name",app::qwk_cp437_to_utf8(snapshot.name)},{"next_send",std::to_string(view.next_send)},{"lines",lines}};
    };
    api_get("/api/control/callers",[&](const auto&,auto& response){
        const auto chat=runtime.chat_operator({});const auto state=runtime.status();const auto configuration=runtime.chat_configuration();Json pages=Json::array(),rooms=Json::array();
        for(const auto& page:chat.pages){const auto node=std::find_if(state.nodes.begin(),state.nodes.end(),[&](const auto& row){return row.number==page.caller.node&&row.account_id==page.caller.account&&row.caller_connected;});if(node==state.nodes.end())continue;
            std::array<char,33> epoch{};sodium_bin2hex(epoch.data(),epoch.size(),node->screen_owner.runtime_epoch.bytes.data(),node->screen_owner.runtime_epoch.bytes.size());
            pages.push_back({{"page",std::to_string(page.request)},{"name",app::qwk_cp437_to_utf8(page.name)},{"account",std::to_string(page.caller.account)},
                {"target",{{"node",node->number},{"epoch",epoch.data()},{"generation",std::to_string(node->assignment_sequence)}}}});
        }
        if(configuration.configuration)for(const auto& room:configuration.configuration->rooms)if(room.enabled)rooms.push_back({{"id",room.id},{"name",room.name}});
        json_response(response,{{"ready",chat.code==app::ChatResult::ok},{"available",chat.available},{"pages",pages},{"rooms",rooms}});
    });
    api_post("/api/control/caller-account",[&,control_identity,caller_number](const auto& request,auto& response){
        const auto value=parse(request,512);require_keys(value,{"target","account"});require_keys(value.at("target"),{"node","epoch","generation"});
        const auto account=caller_number(value.at("account"));const auto node=runtime.view_node(control_identity(value.at("target")));
        if(!node||!account||account>static_cast<std::uint64_t>(INT64_MAX)||node->account_id!=static_cast<app::AccountId>(account)){json_response(response,{{"detail","The selected caller changed or has not signed in. Select the current caller again."}},409);return;}
        json_response(response,{{"account",std::to_string(node->account_id)},{"name",node->caller_handle}});
    });
    api_post("/api/control/available",[&](const auto& request,auto& response){
        const auto value=parse(request,128);require_keys(value,{"available"});if(!value.at("available").is_boolean())throw std::runtime_error("availability_invalid");
        app::ChatOperatorRequest command;command.action=app::ChatOperatorAction::availability;command.available=value.at("available").template get<bool>();const auto result=runtime.chat_operator(command);
        json_response(response,{{"ok",result.code==app::ChatResult::ok},{"available",result.available}},result.code==app::ChatResult::ok?200:409);
    });
    api_get("/api/control/notice",[&,terminal_owner,notice_json](const auto& request,auto& response){json_response(response,notice_json(caller_actions.current_notice(terminal_owner(request))));});
    api_post("/api/control/notice/prepare",[&,terminal_owner,notice_json,control_identity,caller_text](const auto& request,auto& response){
        const auto value=parse(request,4096);require_keys(value,{"target","room","text","override_dnd"});app::ChatNoticeRequest notice;
        if(!value.at("target").is_null()){require_keys(value.at("target"),{"node","epoch","generation"});notice.target=control_identity(value.at("target"));}
        const auto& room=value.at("room");if(!room.is_number_integer()||room<Json(0)||room>Json(UINT16_MAX)||!value.at("override_dnd").is_boolean())throw std::runtime_error("notice_scope_invalid");
        notice.room=room.template get<app::ChatRoomId>();notice.text=caller_text(value.at("text"));notice.override_dnd=value.at("override_dnd").template get<bool>();
        const auto review=caller_actions.prepare_notice(terminal_owner(request),notice);json_response(response,notice_json(review),review?200:409);
    });
    api_post("/api/control/notice/send",[&,terminal_owner,notice_json](const auto& request,auto& response){
        const auto value=parse(request,256);require_keys(value,{"handle"});const auto result=caller_actions.notice(terminal_owner(request),value.at("handle").template get<std::string>(),true);
        json_response(response,notice_json(result),result?200:409);
    });
    api_post("/api/control/notice/cancel",[&,terminal_owner](const auto& request,auto& response){const auto value=parse(request,256);require_keys(value,{"handle"});const bool cancelled=caller_actions.cancel_notice(terminal_owner(request),value.at("handle").template get<std::string>());json_response(response,{{"ok",cancelled}},cancelled?200:409);});
    api_post("/api/control/chat/start",[&,terminal_owner,control_identity,caller_number,operator_json](const auto& request,auto& response){
        const auto value=parse(request,512);require_keys(value,{"target","page","override_dnd"});require_keys(value.at("target"),{"node","epoch","generation"});
        if(!value.at("override_dnd").is_boolean())throw std::runtime_error("chat_override_invalid");
        app::ChatOperatorRequest command;command.target=control_identity(value.at("target"));command.page=caller_number(value.at("page"));command.action=command.page?app::ChatOperatorAction::answer:app::ChatOperatorAction::start;command.override_dnd=value.at("override_dnd").template get<bool>();
        const auto view=caller_actions.start(terminal_owner(request),command);json_response(response,operator_json(view),view?200:409);
    });
    api_get("/api/control/chat",[&,terminal_owner,operator_json](const auto& request,auto& response){
        if(request.get_param_value_count("ack")>1)throw std::runtime_error("chat_ack_invalid");
        const auto text=request.has_param("ack")?request.get_param_value("ack"):"0";const auto ack=number(text);
        if(std::to_string(ack)!=text)throw std::runtime_error("chat_ack_invalid");
        json_response(response,operator_json(caller_actions.poll(terminal_owner(request),ack)));
    });
    api_post("/api/control/chat/send",[&,terminal_owner,caller_number,caller_text](const auto& request,auto& response){
        const auto value=parse(request,4096);require_keys(value,{"handle","sequence","text"});const auto result=caller_actions.send(terminal_owner(request),value.at("handle").template get<std::string>(),caller_number(value.at("sequence")),caller_text(value.at("text")));
        json_response(response,{{"ok",result==app::ChatResult::ok},{"code",static_cast<unsigned>(result)},{"detail",std::string(app::chat_result_text(result))}},result==app::ChatResult::ok?200:409);
    });
    api_post("/api/control/chat/end",[&,terminal_owner](const auto& request,auto& response){const auto value=parse(request,256);require_keys(value,{"handle"});const bool ended=caller_actions.end(terminal_owner(request),value.at("handle").template get<std::string>());json_response(response,{{"ok",ended}},ended?200:409);});
