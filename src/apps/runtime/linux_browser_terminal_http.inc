    const auto terminal_owner=[cookie_name,remote](const httplib::Request& request){return std::string(remote?"R:":"L:")+cookie(request,cookie_name);};
    api_get("/api/node-terminal",[&,terminal_owner](const auto& request,auto& response){
        const auto view=terminals.view(terminal_owner(request));
        if(!view){json_response(response,{{"active",false}});return;}
        Json cells=Json::array();for(const auto& cell:view->screen.cells)cells.push_back(Json::array({cell.character,cell.attribute}));
        json_response(response,{{"active",true},{"connecting",view->connecting},{"handle",view->handle},{"node",view->identity.node},{"generation",std::to_string(view->identity.generation)},
            {"sequence",std::to_string(view->next_input)},{"columns",80},{"rows",25},{"cursor_column",view->cursor_column},{"cursor_row",view->cursor_row},{"cells",cells}});
    });
    api_post("/api/node-terminal/open",[&,terminal_owner](const auto& request,auto& response){
        const auto value=parse(request,256);require_keys(value,{"node"});const auto& node=value.at("node");
        if(!node.is_number_integer()||node<Json(1)||node>Json(app::kAlphaNodePoolCapacity))throw std::runtime_error("node_invalid");
        const auto id=cookie(request,cookie_name),source=request.remote_addr;
        const auto valid=[&,remote,id,source]{return remote?allowed_remote(source)&&remote_sessions.valid(id,source,runtime.web_authentication_epoch()):sessions.valid(id);};
        const bool opened=terminals.open(terminal_owner(request),node.template get<std::uint16_t>(),valid);
        json_response(response,{{"ok",opened},{"detail",opened?"Connected. Sign in with your caller account.":"This node or your browser terminal is already in use, or logon was refused. Refresh the current terminal before trying again."}},opened?200:409);
    });
    api_post("/api/node-terminal/input",[&,terminal_owner](const auto& request,auto& response){
        auto value=parse(request,2048);require_keys(value,{"handle","sequence","bytes"});const auto& bytes=value.at("bytes");
        if(!bytes.is_array()||bytes.empty()||bytes.size()>256)throw std::runtime_error("terminal_input_invalid");
        std::string input;for(const auto& byte:bytes){if(!byte.is_number_integer()||byte<Json(0)||byte>Json(255))throw std::runtime_error("terminal_input_invalid");input+=static_cast<char>(byte.template get<unsigned>());}
        struct Erase{std::string& input;Json& value;~Erase(){sodium_memzero(input.data(),input.size());value.clear();}}erase{input,value};
        const bool accepted=terminals.input(terminal_owner(request),value.at("handle").template get<std::string>(),number(value.at("sequence").template get<std::string>()),input);
        json_response(response,{{"ok",accepted},{"detail",accepted?"Input sent.":"Input was not confirmed. Refresh the terminal; it will not be sent again automatically."}},accepted?200:409);
    });
    api_post("/api/node-terminal/close",[&,terminal_owner](const auto& request,auto& response){
        const auto value=parse(request,256);require_keys(value,{"handle"});const bool closed=terminals.close(terminal_owner(request),value.at("handle").template get<std::string>());
        json_response(response,{{"ok",closed},{"detail",closed?"Disconnected from this node.":"This terminal already ended or changed. Refresh its current state."}},closed?200:409);
    });
