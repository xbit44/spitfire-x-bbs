    api_get("/api/config/doors/bulletins",[&](const auto& request,auto& response){
        if(!request.params.empty()){
            throw std::runtime_error("door_bulletin_query_invalid");
        }
        const auto catalog=runtime.complete_door_bulletin_catalog();const auto doors=runtime.complete_door_catalog();
        if(!catalog||!doors){json_response(response,{{"detail","Door Bulletins could not be read. Check BBS management, then reload."}},409);return;}
        json_response(response,rt::web::door_bulletin_catalog_page(*catalog,*doors,random_token().substr(0,32)));
    });
    api_post("/api/config/doors/bulletins/change",[&](const auto& request,auto& response){
        app::DoorBulletinChange change;try{change=rt::web::door_bulletin_update(parse(request));}catch(...){json_response(response,{{"unresolved",false},{"detail","Check the permanent number (1-999), short title and source. Use a relative ASCII path (240 characters total, 128 per name), an existing Door and explicit ASCII/ANSI format. Display sources use DBnnn.ANS/.ASC."}},400);return;}
        const auto saved=runtime.change_door_bulletin(change);const bool ok=saved.code==app::LocalAccountResultCode::ok;const bool unresolved=saved.code==app::LocalAccountResultCode::unavailable||saved.code==app::LocalAccountResultCode::storage_failure;
        json_response(response,{{"ok",ok},{"unresolved",unresolved},{"revision",std::to_string(saved.result.revision)},{"detail",ok?(change.remove?"Definition removed. Source files were not changed.":"Door Bulletin settings saved. Use Validate / Refresh to check the source."):unresolved?"Save outcome is uncertain. Your draft is kept; reload and review saved settings before another change.":"Settings changed elsewhere or the action was refused. Your draft is kept; reload before saving again."}},ok?200:409);
    });
    api_post("/api/config/doors/bulletins/preview",[&](const auto& request,auto& response){
        const auto value=parse(request);require_keys(value,{"id","ansi","revision"});const auto& id=value.at("id");
        if(!(id.is_number_integer()&&id>=Json(1)&&id<=Json(999)&&value.at("ansi").is_boolean()&&value.at("revision").is_string())){
            throw std::runtime_error("door_bulletin_preview_invalid");
        }
        const auto encoded=value.at("revision").template get<std::string>();std::int64_t revision{};const auto result=std::from_chars(encoded.data(),encoded.data()+encoded.size(),revision);
        if(!(result.ec==std::errc{}&&result.ptr==encoded.data()+encoded.size()&&revision>0&&std::to_string(revision)==encoded)){
            throw std::runtime_error("door_bulletin_preview_revision_invalid");
        }
        const auto preview=runtime.preview_door_bulletin({id.template get<std::uint16_t>()},value.at("ansi").template get<bool>(),revision);json_response(response,rt::web::door_bulletin_preview_page(preview));
    });
