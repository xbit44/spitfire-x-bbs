    const auto ftn_list_json=[](const rt::web::WebFtnListSnapshot& value){
        Json rows=Json::array(),issues=Json::array();
        if(value.ready){for(const auto& e:value.list.entries)rows.push_back({{"tag",e.tag},{"description",e.description},{"line",e.line}});for(const auto& e:value.list.issues)issues.push_back({{"line",e.line},{"reason",e.reason}});}
        return Json{{"list",rt::web::web_ftn_token_text(value.token)},{"network",value.network},{"source",value.source},{"active",value.active},{"ready",value.ready},{"detail",value.diagnostic},{"rows",rows},{"issues",issues}};
    };
    api_post("/api/ftn/list-start",[&](const auto& request,auto& response){
        const Json value=parse(request,8192);require_keys(value,{"draft","edit_revision","network","source","kind","encoding"});
        const auto kind=value.at("kind").get<std::string>(),encoding=value.at("encoding").get<std::string>();
        if((kind!="file"&&kind!="url")||(encoding!="utf8"&&encoding!="cp437"))throw std::runtime_error("ftn_list_options_invalid");
        const auto selected_source=value.at("source").get<std::string>();auto source=selected_source;
        if(kind=="file"&&!source.empty()&&std::filesystem::path(source).is_relative())source=(root/std::filesystem::path(source)).string();
        json_response(response,ftn_list_json(ftn_drafts.start_list(ftn_owner(request),rt::web::web_ftn_token(value.at("draft").get<std::string>()),number(value.at("edit_revision").get<std::string>()),value.at("network").get<std::string>(),std::move(source),kind=="url",encoding=="cp437",selected_source)));
    });
    api_get("/api/ftn/list",[&](const auto& request,auto& response){
        const Json query=ftn_query(request,{{"draft",""},{"list",""}});const auto list=query.at("list").get<std::string>();json_response(response,ftn_list_json(ftn_drafts.read_list(ftn_owner(request),rt::web::web_ftn_token(query.at("draft").get<std::string>()),list.empty()?app::FtnTransferToken{}:rt::web::web_ftn_token(list))));
    });
    api_post("/api/ftn/list-cancel",[&](const auto& request,auto& response){
        const Json value=parse(request,512);require_keys(value,{"draft","list"});ftn_drafts.cancel_list(ftn_owner(request),rt::web::web_ftn_token(value.at("draft").get<std::string>()),rt::web::web_ftn_token(value.at("list").get<std::string>()));json_response(response,{{"cancel_requested",true}});
    });
    const auto ftn_import_catalog=[&]{
        rt::web::WebFtnImportCatalog out;app::LocalMessageConfigurationQuery query;
        const auto page=[&](const app::LocalMessageConfigurationQuery& q){const auto value=runtime.message_configuration_snapshot(q);if(value.code!=app::LocalAccountResultCode::ok||(out.revision&&value.catalog.revision!=out.revision))throw std::runtime_error("ftn_import_catalog_changed");out.revision=value.catalog.revision;return value.catalog;};
        for(;;){const auto value=page(query);for(const auto& group:value.groups)out.groups.emplace_back(group.id.value,group.name);if(out.groups.size()>999)throw std::runtime_error("ftn_import_catalog_limit");if(!value.next)break;query.cursor=*value.next;}
        for(const auto& group:out.groups){query={};query.group=app::MessageGroupId{group.first};query.cursor.configuration_revision=out.revision;
            for(;;){const auto value=page(query);for(const auto& echo:value.echoes)out.echoes.emplace_back(echo.id.value,echo.group.value);if(out.echoes.size()>999)throw std::runtime_error("ftn_import_catalog_limit");if(!value.next)break;query.cursor=*value.next;}}
        return out;
    };
    const auto ftn_import=[&](const httplib::Request& request,httplib::Response& response,bool apply){
        const Json value=parse(request,4*1024*1024);require_keys(value,{"draft","edit_revision","list","selected","access","read","post","update_text"});
        if(!value.at("selected").is_array()||value.at("selected").size()>16384||!value.at("update_text").is_boolean())throw std::runtime_error("ftn_import_selection_invalid");
        const auto level=[&](const char* key){const auto& n=value.at(key);if(!n.is_number_integer()||n<1||n>999)throw std::runtime_error("ftn_import_level_invalid");return n.get<std::uint16_t>();};
        const app::FtnEchoImportRequirements requirements{level("access"),level("read"),level("post")};const auto catalog=ftn_import_catalog();
        const auto handle=rt::web::web_ftn_token(value.at("draft").get<std::string>()),list=rt::web::web_ftn_token(value.at("list").get<std::string>());const auto revision=number(value.at("edit_revision").get<std::string>());const auto selected=value.at("selected").get<std::vector<std::string>>();const auto update=value.at("update_text").get<bool>();
        if(apply){json_response(response,rt::web::web_ftn_draft_summary(ftn_drafts.import_apply(ftn_owner(request),handle,revision,list,catalog,selected,requirements,update)));return;}
        const auto plan=ftn_drafts.import_preview(ftn_owner(request),handle,revision,list,catalog,selected,requirements,update);Json issues=Json::array();for(const auto& issue:plan.issues)issues.push_back({{"field",issue.field},{"reason",issue.reason}});
        json_response(response,{{"selected",plan.selected},{"created",plan.created},{"unchanged",plan.unchanged},{"text_updates",plan.text_updates},{"issues",issues}});
    };
    api_post("/api/ftn/import-preview",[&](const auto& request,auto& response){ftn_import(request,response,false);});
    api_post("/api/ftn/import-apply",[&](const auto& request,auto& response){ftn_import(request,response,true);});
