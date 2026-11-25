    const auto control_identity=[](const Json& value){
        app::NodeAssignmentIdentity identity;const auto& node=value.at("node");const auto epoch=value.at("epoch").get<std::string>();
        if(!node.is_number_integer()||node<Json(1)||node>Json(app::kAlphaNodePoolCapacity)||epoch.size()!=32||epoch.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("node_identity_invalid");
        std::size_t length{};
        if(sodium_hex2bin(identity.runtime_epoch.bytes.data(),identity.runtime_epoch.bytes.size(),epoch.data(),epoch.size(),nullptr,&length,nullptr)!=0||length!=identity.runtime_epoch.bytes.size()||!identity.runtime_epoch.valid())throw std::runtime_error("node_epoch_invalid");
        identity.node=node.get<std::uint16_t>();const auto generation=value.at("generation").get<std::string>();identity.generation=number(generation);
        if(std::to_string(identity.generation)!=generation)throw std::runtime_error("node_generation_invalid");
        return identity;
    };
    api_post("/api/control/restart",[&](const auto& request,auto& response){
        const auto value=parse(request,256);require_keys(value,{"run","confirmation"});
        const auto ticket=value.at("confirmation").template get<std::string>();
        const auto result=runtime.restart_bbs(number(value.at("run").template get<std::string>()),ticket.empty()?std::nullopt:std::optional<std::uint64_t>(number(ticket)));
        json_response(response,{{"ok",result.response.code==Code::ok},{"callers",result.callers},{"confirmation",result.confirmation?std::to_string(result.confirmation):""},{"diagnostic",result.response.diagnostic_code}},result.confirmation||result.response.code==Code::ok?200:409);
    });
    api_post("/api/control/drain",[&](const auto& request,auto& response){
        const auto value=parse(request,256);require_keys(value,{"run","seconds"});const auto& seconds=value.at("seconds");
        if(!seconds.is_number_integer()||(seconds!=Json(0)&&seconds!=Json(60)&&seconds!=Json(300)&&seconds!=Json(900)))throw std::runtime_error("drain_choice_invalid");
        const auto timeout=seconds.template get<std::uint32_t>();
        const auto result=runtime.command_lifecycle({timeout?app::AlphaLifecycleCommandKind::begin_drain:app::AlphaLifecycleCommandKind::cancel_drain,timeout},number(value.at("run").template get<std::string>()));
        json_response(response,{{"ok",result.code==Code::ok},{"diagnostic",result.diagnostic_code}},result.code==Code::ok?200:409);
    });
    api_post("/api/control/node",[&,control_identity](const auto& request,auto& response){
        const auto value=parse(request,512);require_keys(value,{"node","epoch","generation","action","door_launch"});const auto identity=control_identity(value);
        const auto action=value.at("action").template get<std::string>();const auto launch=number(value.at("door_launch").template get<std::string>());
        app::AlphaNodeCommandRequest command;command.node_number=identity.node;
        if(action=="enable")command.command=app::AlphaNodeCommandKind::enable;
        else if(action=="disable")command.command=app::AlphaNodeCommandKind::disable;
        else if(action=="disconnect")command.command=app::AlphaNodeCommandKind::disconnect;
        else if(action=="terminate")command.command=app::AlphaNodeCommandKind::terminate_door;
        else throw std::runtime_error("node_action_invalid");
        command.expected_door_launch=launch;
        if(action=="disconnect"||action=="terminate"){command.expected_epoch=identity.runtime_epoch;command.expected_generation=identity.generation;}
        const auto result=runtime.command_node(command,identity);
        const auto detail=result.code==Code::ok?(action=="terminate"?"The selected Door ended.":action=="disconnect"?"Disconnect requested for the selected caller.":action=="disable"?"Node disabled. This setting is saved for the next start.":"Node enabled. This setting is saved for the next start."):"The selected node or caller changed, or the action could not be confirmed. Refresh before choosing another action.";
        json_response(response,{{"ok",result.code==Code::ok},{"diagnostic",result.diagnostic_code},{"detail",detail}},result.code==Code::ok?200:409);
    });
    api_post("/api/control/view",[&,control_identity](const auto& request,auto& response){
        const auto value=parse(request,256);require_keys(value,{"node","epoch","generation"});const auto view=runtime.view_node(control_identity(value));
        if(!view){json_response(response,{{"active",false},{"detail","This caller ended or changed. Select a current caller to view."}});return;}
        Json cells=Json::array();for(const auto& cell:view->screen.cells)cells.push_back(Json::array({cell.character,cell.attribute}));
        json_response(response,{{"active",true},{"node",view->number},{"caller",view->caller_handle},{"activity",view->activity},{"seconds",view->connected_seconds},{"columns",80},{"rows",25},{"cells",cells}});
    });
