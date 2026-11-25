    api_get(R"(/api/config/message-utility/([a-z_]+))",[&](const auto& request,auto& response){
        const auto kind=request.matches[1].str();if(kind!="line_limit"&&kind!="private_colors"&&kind!="system_sysop"&&kind!="unread_age"){json_response(response,{{"detail","Unknown message setting."}},404);return;}
        const auto current=runtime.message_configuration_snapshot({});if(current.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","Message settings could not be read."}},409);return;}
        const auto& catalog=current.catalog;
        Json value={{"kind",kind},{"policy","message"},{"revision",std::to_string(catalog.revision)},{"operation",random_token().substr(0,32)},{"record",nullptr},{"record_revision","1"},{"value",nullptr},{"can_save",true}};
        if(kind=="line_limit"){
            const auto text=request.get_param_value("record");const auto id=number(text);if(!id||id>999||std::to_string(id)!=text||request.get_param_value_count("record")!=1)throw std::runtime_error("message_level_invalid");
            value["record"]=id;const auto found=std::find_if(catalog.line_limits.begin(),catalog.line_limits.end(),[&](const auto& row){return row.security_level==id;});
            value["can_save"]=found!=catalog.line_limits.end();if(found!=catalog.line_limits.end()){value["value"]=found->maximum_lines;value["record_revision"]=std::to_string(found->revision);}
        }else if(kind=="unread_age"){
            value["value"]=catalog.unread_mail_age_days?Json(*catalog.unread_mail_age_days):Json(nullptr);value["record_revision"]=std::to_string(catalog.unread_retention_revision);
        }else if(kind=="private_colors"){
            value["value"]=catalog.private_pipe_colors;value["record_revision"]=std::to_string(catalog.private_presentation_revision);
        }else{
            app::LocalAccountQuery query;query.name_prefix=request.get_param_value("prefix");if(query.name_prefix.size()>30||query.name_prefix.find('\0')!=std::string::npos||request.get_param_value_count("prefix")>1)throw std::runtime_error("sysop_filter_invalid");
            if(request.has_param("offset")){const auto text=request.get_param_value("offset");const auto offset=number(text);if(offset>1000000||std::to_string(offset)!=text||request.get_param_value_count("offset")!=1)throw std::runtime_error("sysop_offset_invalid");query.offset=static_cast<std::uint32_t>(offset);}
            const auto callers=runtime.account_snapshot(query);if(callers.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","Eligible Sysop callers could not be read."}},409);return;}
            Json records=Json::array();for(const auto& caller:callers.accounts)if(caller.state==app::AccountState::active&&caller.security_level&&*caller.security_level>=callers.policy.sysop_threshold)records.push_back({{"id",std::to_string(caller.account_id)},{"name",caller.display_name.value_or("")+" (#"+std::to_string(caller.account_id)+")"},{"revision",std::to_string(caller.revision)}});
            std::string assigned="No eligible System Sysop assigned.";
            if(catalog.system_sysop){app::LocalAccountQuery exact;exact.account_id=*catalog.system_sysop;const auto account=runtime.account_snapshot(exact);if(account.code==app::LocalAccountResultCode::ok&&account.accounts.size()==1)assigned=account.accounts.front().display_name.value_or("Unavailable caller")+" (#"+std::to_string(*catalog.system_sysop)+")";else assigned="Assigned caller #"+std::to_string(*catalog.system_sysop)+" is unavailable.";}
            value["assignment_revision"]=std::to_string(catalog.assignment_revision);value["assigned"]=assigned;value["assigned_id"]=catalog.system_sysop?Json(std::to_string(*catalog.system_sysop)):Json(nullptr);value["records"]=records;value["prefix"]=query.name_prefix;value["offset"]=query.offset;value["has_more"]=callers.has_more;value["can_save"]=false;
        }
        json_response(response,value);
    });
    api_post(R"(/api/config/message-utility/([a-z_]+))",[&](const auto& request,auto& response){
        app::MessageConfigurationChange change;try{change=rt::web::message_utility_update(request.matches[1].str(),parse(request));}
        catch(const std::exception&){json_response(response,{{"unresolved",false},{"detail","Choose a current eligible caller, an explicit body limit from 1 to 65,535, or the private pipe color setting."}},400);return;}
        const auto result=runtime.change_message_configuration(change);const bool ok=result.code==app::LocalAccountResultCode::ok,unresolved=result.code==app::LocalAccountResultCode::unavailable||result.code==app::LocalAccountResultCode::storage_failure;
        json_response(response,{{"ok",ok},{"unresolved",unresolved},{"replayed",result.result.replayed},{"detail",ok?(result.result.replayed?"Matching message setting confirmed. No duplicate change was made.":"Message setting saved and audited. Stored message bodies remain unchanged."):unresolved?"Save outcome is uncertain. Reconcile the same operation before editing again.":"The settings, selected caller or eligibility changed. Reload and review; your draft is retained."}},ok?200:409);
    });
