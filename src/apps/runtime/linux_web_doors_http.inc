    api_get("/api/config/doors/catalog",[&](const auto& request,auto& response){
        unsigned menu{};for(const auto& param:request.params){if(param.first!="menu"||request.get_param_value_count("menu")!=1||number(param.second)>999)throw std::runtime_error("door_menu_invalid");menu=static_cast<unsigned>(number(param.second));}
        const auto catalog=runtime.complete_door_catalog();if(!catalog){json_response(response,{{"detail","Door settings could not be read. Check the BBS status, then reload."}},409);return;}
        json_response(response,rt::web::door_catalog_page(*catalog,menu,random_token().substr(0,32)));
    });
    api_get(R"(/api/config/doors/door/(\d+))",[&](const auto& request,auto& response){
        const auto id=number(request.matches[1].str());if(id>999||!request.params.empty())throw std::runtime_error("door_id_invalid");
        const auto catalog=runtime.complete_door_catalog();if(!catalog){json_response(response,{{"detail","Door settings could not be read."}},409);return;}
        json_response(response,rt::web::door_editor_page(*catalog,static_cast<unsigned>(id),random_token().substr(0,32)));
    });
    api_post("/api/config/doors/removal-preview",[&](const auto& request,auto& response){
        const auto value=parse(request);require_keys(value,{"action","id","number"});
        const auto catalog=runtime.complete_door_catalog();if(!catalog){json_response(response,{{"detail","Door settings could not be read."}},409);return;}
        // Parse the same typed identities as the write, but perform no storage mutation.
        auto check=Json{{"revision",std::to_string(catalog->revision)},{"operation",random_token().substr(0,32)},{"action",value.at("action")},{"entry",{{"id",value.at("id")},{"number",value.at("number")}}}};
        const auto change=rt::web::door_catalog_update(check,*catalog,{});(void)change;
        json_response(response,rt::web::door_removal_preview(*catalog,value.at("action").template get<std::string>(),value.at("id").template get<unsigned>(),value.at("number").template get<unsigned>()));
    });
    api_post("/api/config/doors/change",[&](const auto& request,auto& response){
        const auto catalog=runtime.complete_door_catalog();if(!catalog){json_response(response,{{"unresolved",false},{"detail","Door settings could not be read. Your draft is retained."}},409);return;}
        app::DoorConfigurationChange change;
        try{auto value=parse(request,32768);change=rt::web::door_catalog_update(value,*catalog,[&](std::string_view secret)->std::optional<std::vector<std::uint8_t>>{
            const auto key=host::load_portable_board_key(root);if(!key)return {};
            return sfx::platform::portable::seal_portable_credential(key.value->key,key.value->board,{'S','F','X','R'},"RLOGIN",secret);
        });}catch(const std::exception&){json_response(response,{{"unresolved",false},{"detail","The saved Door settings changed, or the requested settings are invalid. Check the command, security levels, menu placement and password choices. Reload before retrying a stale draft."}},400);return;}
        const auto result=change.action==app::DoorConfigurationAction::remove_door?runtime.remove_door_with_placements(change):runtime.change_door_configuration(change);const bool ok=result.code==app::LocalAccountResultCode::ok;
        const bool unresolved=result.code==app::LocalAccountResultCode::unavailable||result.code==app::LocalAccountResultCode::storage_failure;
        json_response(response,{{"ok",ok},{"unresolved",unresolved},{"door_id",change.action==app::DoorConfigurationAction::save_door?change.door.id:0},{"revision",std::to_string(result.result.revision)},
            {"detail",ok?"Door settings saved. Active Doors keep their captured settings. Program files were not changed.":unresolved?"The save outcome is uncertain. Reload and review the saved Door settings before making another change; this request will not be repeated automatically.":"Door settings changed elsewhere or this action was refused. Reload and review your draft."}},ok?200:409);
    });
