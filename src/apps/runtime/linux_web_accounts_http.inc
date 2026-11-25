    const auto account_change=[&](const httplib::Request& request,httplib::Response& response,std::string_view action){
        app::LocalAccountRequest change;struct Wipe{app::LocalAccountRequest& value;~Wipe(){app::erase_local_account_secrets(value);}}wipe{change};
        try{change=action.empty()?rt::web::account_update(parse(request)):rt::web::account_action(action,parse(request));}
        catch(const std::exception&){json_response(response,{{"committed",false},{"detail","Check the caller selection, profile, assigned level and matching password fields. Passwords must be entered again after submitting."}},400);return;}
        const auto selected=change.account_id;const auto name=change.name;const bool creating=change.action==app::LocalAccountAction::create_user;
        const auto result=runtime.administer_account(change);const bool ok=result.code==app::LocalAccountResultCode::ok;
        const char* detail=ok?(creating?"User created. Find and select the saved record to review its assigned level.":"Caller change saved and audited."):result.committed?"A change was committed, but the caller operation did not finish. Review the saved account and Runtime status; do not repeat the action.":result.code==app::LocalAccountResultCode::system_sysop_protected?"This is the designated System Sysop. Assign another eligible active Sysop under Board Identity before deactivating, deleting or demoting this caller.":result.code==app::LocalAccountResultCode::last_sysop_protected?"The last usable Sysop is protected. Nothing was saved.":result.code==app::LocalAccountResultCode::policy_rejected?"The password or account policy refused this change. Review the fields and enter passwords again if needed.":"The caller or policy changed, or the action was refused. Reload and review before trying again.";
        json_response(response,{{"ok",ok},{"committed",result.committed},{"sessions_revoked",result.sessions_revoked},{"record",selected?Json(std::to_string(selected)):Json(nullptr)},{"created_name",creating&&ok?name:""},{"revision",std::to_string(result.revision)},{"detail",detail}},ok?200:409);
    };
    api_post(R"(/api/config/accounts/(deactivate|reactivate|delete|reset_password))",[&](const auto& request,auto& response){account_change(request,response,request.matches[1].str());});
    const auto read_accounts_page=[&](const httplib::Request& request,httplib::Response& response){
        app::LocalAccountQuery query;const auto prefix=request.get_param_value("prefix");query.name_prefix=prefix;
        if(prefix.size()>30||prefix.find('\0')!=std::string::npos||request.get_param_value_count("prefix")>1)throw std::runtime_error("account_filter_invalid");
        if(request.has_param("offset")){const auto text=request.get_param_value("offset");const auto offset=number(text);if(offset>1000000||std::to_string(offset)!=text||request.get_param_value_count("offset")!=1)throw std::runtime_error("account_offset_invalid");query.offset=static_cast<std::uint32_t>(offset);}
        std::optional<app::AccountId> selected;
        if(request.has_param("record")){const auto text=request.get_param_value("record");const auto id=number(text);if(id>static_cast<std::uint64_t>(INT64_MAX)||std::to_string(id)!=text||request.get_param_value_count("record")!=1)throw std::runtime_error("account_id_invalid");selected=static_cast<app::AccountId>(id);}
        if(!selected&&!prefix.empty()&&std::all_of(prefix.begin(),prefix.end(),[](char c){return c>='0'&&c<='9';})){
            const auto id=number(prefix);if(!id||id>static_cast<std::uint64_t>(INT64_MAX)||std::to_string(id)!=prefix||query.offset)throw std::runtime_error("account_id_invalid");query={};query.account_id=static_cast<app::AccountId>(id);selected=query.account_id;
        }
        auto listed=runtime.account_snapshot(query);
        if(listed.code!=app::LocalAccountResultCode::ok&&!query.account_id){json_response(response,{{"detail","Caller accounts could not be read."}},409);return;}
        app::LocalAccountSnapshot detail=listed;
        if(selected&&*selected>0){app::LocalAccountQuery exact;exact.account_id=*selected;detail=runtime.account_snapshot(exact);if(detail.code!=app::LocalAccountResultCode::ok)detail.policy=listed.policy;}
        auto page=rt::web::account_page(detail,selected);Json records=Json::array();
        if(listed.code==app::LocalAccountResultCode::ok)for(const auto& account:listed.accounts)records.push_back({{"id",std::to_string(account.account_id)},{"name",std::to_string(account.account_id)+"  "+account.display_name.value_or("Deleted caller")}});
        if(selected&&*selected>0&&std::none_of(records.begin(),records.end(),[&](const auto& row){return row.at("id")==std::to_string(*selected);}))records.push_back({{"id",std::to_string(*selected)},{"name",std::to_string(*selected)+"  "+(detail.accounts.size()==1?detail.accounts.front().display_name.value_or("Deleted caller"):"Unavailable caller")}});
        page["records"]=records;page["prefix"]=prefix;page["offset"]=query.offset;page["has_more"]=listed.has_more;json_response(response,page);
    };
