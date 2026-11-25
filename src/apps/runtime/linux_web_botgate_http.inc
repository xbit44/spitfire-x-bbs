// All routes use api_get/api_post: session, CSRF/Origin, LAN allowlist and
// maintenance admission are the same as other SFXConfig controls.
    api_get("/api/config/botgate/rule",[&](const auto& request,auto& response){
        if(request.params.size()!=1||!request.has_param("file"))throw std::runtime_error("botgate_file_required");
        const auto selected=number(request.get_param_value("file"));
        if(selected>=app::kBotGateRuleNames.size())throw std::runtime_error("botgate_file_invalid");
        const auto rule=runtime.botgate_rule_file(static_cast<unsigned>(selected));
        Json files=Json::array();
        constexpr std::array<const char*,6> labels{{"Exempt addresses","Blocked IP addresses","Silent IP blocks","Verified hostnames","Temporary bans","Feed exceptions"}};
        for(unsigned i=0;i<app::kBotGateRuleNames.size();++i)files.push_back({{"file",i},{"name",app::kBotGateRuleNames[i]},{"label",labels[i]},{"readonly",i==4}});
        json_response(response,{{"file",rule.file},{"files",files},{"text",rule.text},{"revision",rule.revision},{"editable",rule.editable},{"available",rule.available},{"detail",rule.detail}},rule.available?200:409);
    });
    api_post("/api/config/botgate/rule/save",[&](const auto& request,auto& response){
        const auto value=parse(request,131072);require_keys(value,{"file","revision","text"});
        if(!value.at("file").is_number_unsigned()&&!value.at("file").is_number_integer())throw std::runtime_error("botgate_file_invalid");
        const auto selected=value.at("file").template get<std::int64_t>();
        if(selected<0||selected>=static_cast<std::int64_t>(app::kBotGateRuleNames.size()))throw std::runtime_error("botgate_file_invalid");
        const auto result=runtime.save_botgate_rule_file(static_cast<unsigned>(selected),value.at("revision").template get<std::string>(),value.at("text").template get<std::string>());
        const bool ok=result.code==Code::ok;
        const auto detail=ok?"Rule file saved. Valid rules reload within about two seconds while the BBS runs; a stopped BBS uses them at its next start.":
            result.diagnostic_code=="botgate_rule_changed"?"This rule file changed elsewhere. Your draft was kept. Reload and review before saving.":
            result.diagnostic_code=="botgate_rule_invalid"?"Rule file not saved. Check the selected editable file, valid rules and 65,535-byte limit.":
            "Rule save could not be confirmed. Keep your draft and read the file again before another save.";
        Json reply={{"ok",ok},{"file",selected},{"diagnostic",result.diagnostic_code},{"detail",detail}};
        if(ok){const auto saved=runtime.botgate_rule_file(static_cast<unsigned>(selected));if(saved.available)reply["revision"]=saved.revision;}
        json_response(response,reply,ok?200:result.diagnostic_code=="botgate_rule_invalid"?400:409);
    });
    api_get("/api/config/botgate/status",[&](const auto&,auto& response){
        const auto gate=runtime.botgate_status();const bool running=runtime.bbs_owned();
        json_response(response,{{"running",running},{"available",running&&gate.available},{"updating",running&&gate.updating},{"entries",gate.entries},
            {"bytes_received",gate.bytes_received},{"last_attempt",gate.last_attempt},{"last_success",gate.last_success},{"result",gate.result},
            {"progress",app::render_botgate_feed_progress(gate)},{"rule_status",gate.rule_status},{"rules_ready",running&&gate.rules_ready}});
    });
    api_post("/api/config/botgate/update",[&](const auto& request,auto& response){
        require_keys(parse(request,128),{});const auto result=runtime.update_botgate_feed();const bool ok=result.code==Code::ok;
        json_response(response,{{"ok",ok},{"diagnostic",result.diagnostic_code},{"detail",ok?"Threat feed update requested. Read its result; a queued request is not a successful update.":
            result.diagnostic_code=="feed_requires_running_bbs"?"Start the BBS before requesting a threat feed update.":"Update not started. Another update may be active, or the worker is unavailable. Read current status before retrying."}},ok?200:409);
    });
