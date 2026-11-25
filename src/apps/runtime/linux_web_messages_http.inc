    api_get("/api/config/messages/catalog",[&](const auto& request,auto& response){
        Json query={{"group",nullptr},{"revision","0"},{"after_order","-1"},{"after_id","0"}};
        for(const auto& item:request.params){if(!query.contains(item.first)||request.get_param_value_count(item.first)!=1)throw std::runtime_error("message_query_invalid");query[item.first]=item.second;}
        const auto selected=rt::web::message_catalog_query(query);const auto snapshot=runtime.message_configuration_snapshot(selected);
        if(snapshot.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","The message catalog changed or could not be read. Reload the first page; your draft is retained."}},409);return;}
        json_response(response,rt::web::message_catalog_page(snapshot,selected,random_token().substr(0,32)));
    });
    api_post("/api/config/messages/change",[&](const auto& request,auto& response){
        app::MessageConfigurationChange change;
        try{change=rt::web::message_catalog_update(parse(request,131072));}
        catch(const std::exception&){json_response(response,{{"unresolved",false},{"detail","Check the name, selected area, security levels and limits. Your draft is retained."}},400);return;}
        const auto result=runtime.change_message_configuration(change);const bool ok=result.code==app::LocalAccountResultCode::ok,unresolved=result.code==app::LocalAccountResultCode::unavailable||result.code==app::LocalAccountResultCode::storage_failure;
        json_response(response,{{"ok",ok},{"unresolved",unresolved},{"replayed",result.result.replayed},{"id",std::to_string(result.result.entity_id)},{"revision",std::to_string(result.result.configuration_revision)},
            {"detail",ok?(result.result.replayed?"The same message change was confirmed. It was not repeated.":"Message settings saved. Stored message bodies were preserved; retention limits apply at the next run."):unresolved?"Save outcome is uncertain. Reconcile this same change before editing again.":"The catalog, selected area or permissions changed. No matching save was confirmed. Reload and review your draft."}},ok?200:409);
    });
