    // Owner-key operations are reserved for the native desktop entry. Local
    // browser cookies and LAN authentication never reveal the one-use port.
    api_post("/api/desktop-node/reserve",[&](const auto& request,auto& response){
        if(remote){json_response(response,{{"error","desktop_local_only"}},403);return;}
        auto value=parse(request,256);require_keys(value,{"owner","node"});auto& owner=value.at("owner").template get_ref<std::string&>();
        const bool valid=rt::web::browser_secret_equal(owner,sessions.owner_secret());sodium_memzero(owner.data(),owner.size());
        if(!valid){json_response(response,{{"error","native_desktop_required"}},401);return;}
        const auto& node=value.at("node");if(!node.is_number_integer()||node<Json(1)||node>Json(app::kAlphaNodePoolCapacity))throw std::runtime_error("node_invalid");
        const auto started=runtime.reserve_local_node(node.template get<std::uint16_t>());
        if(started.response.code!=Code::ok){json_response(response,{{"error","node_not_ready"}},409);return;}
        std::array<char,33> epoch{};sodium_bin2hex(epoch.data(),epoch.size(),started.reservation.runtime_epoch.bytes.data(),16);
        json_response(response,{{"epoch",epoch.data()},{"node",started.reservation.node},{"generation",std::to_string(started.reservation.generation)},{"port",started.port},{"timeout_milliseconds",started.timeout_milliseconds}});
    });
    api_post("/api/desktop-node/cancel",[&](const auto& request,auto& response){
        if(remote){json_response(response,{{"error","desktop_local_only"}},403);return;}
        auto value=parse(request,512);require_keys(value,{"owner","epoch","node","generation"});auto& owner=value.at("owner").template get_ref<std::string&>();
        const bool valid=rt::web::browser_secret_equal(owner,sessions.owner_secret());sodium_memzero(owner.data(),owner.size());
        if(!valid){json_response(response,{{"error","native_desktop_required"}},401);return;}
        const auto& node=value.at("node");if(!node.is_number_integer()||node<Json(1)||node>Json(app::kAlphaNodePoolCapacity))throw std::runtime_error("node_invalid");
        const auto generation=number(value.at("generation").template get<std::string>());if(!generation)throw std::runtime_error("generation_invalid");
        app::NodeAssignmentIdentity identity;identity.node=node.template get<std::uint16_t>();identity.generation=generation;
        const auto epoch=value.at("epoch").template get<std::string>();
        if(epoch.size()!=32||epoch.find_first_not_of("0123456789abcdef")!=std::string::npos||sodium_hex2bin(identity.runtime_epoch.bytes.data(),16,epoch.data(),epoch.size(),nullptr,nullptr,nullptr)!=0||!identity.valid())throw std::runtime_error("epoch_invalid");
        const bool cancelled=runtime.cancel_local_node(identity);json_response(response,{{"cancelled",cancelled}});
    });
