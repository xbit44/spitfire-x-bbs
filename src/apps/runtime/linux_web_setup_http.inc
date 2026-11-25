    api_get("/api/setup",[&](const auto& request,auto& response){
        if(remote){json_response(response,{{"detail","Open the launcher on the Linux computer for first-run setup."}},403);return;}
        if(!request.params.empty())throw std::runtime_error("setup_query_invalid");
        const auto state=runtime.status();app::OperationalConfiguration defaults;
        json_response(response,{{"required",!state.bootstrap_complete},{"recovery_required",state.state==app::AlphaRuntimePresentationState::recovery_required},{"diagnostic",state.diagnostic_code},
            {"defaults",{{"bbs_name",defaults.bbs_name},{"time_zone","Etc/UTC"},{"nodes",defaults.logical_nodes},{"bind_address",defaults.telnet.bind_address},{"telnet_port",defaults.telnet.port},{"ssh_enabled",defaults.ssh.enabled},{"ssh_port",defaults.ssh.port},{"strong_passwords",defaults.password_mode==app::PasswordMode::strong}}}});
    });
    api_post("/api/setup",[&](const auto& request,auto& response){
        if(remote){json_response(response,{{"detail","First-run setup is local. Use the launcher on the Linux computer."}},403);return;}
        auto value=parse(request,8192);require_keys(value,{"bbs_name","time_zone","nodes","bind_address","telnet_port","ssh_enabled","ssh_port","strong_passwords","sysop_name","sysop_password","system_password"});
        app::AlphaBootstrapRequest setup;
        struct EraseSetup {app::AlphaBootstrapRequest& setup;Json& value;~EraseSetup(){app::erase_bootstrap_secrets(setup);for(const auto* key:{"sysop_password","system_password"})if(value.contains(key)&&value.at(key).is_string()){auto& secret=value.at(key).template get_ref<std::string&>();sodium_memzero(secret.data(),secret.size());secret.clear();}}} erase{setup,value};
        const auto integer=[&](const char* key,unsigned maximum){const auto& number=value.at(key);if(!number.is_number_integer()||number<Json(1)||number>Json(maximum))throw std::runtime_error("setup_number_invalid");return number.template get<unsigned>();};
        if(!value.at("ssh_enabled").is_boolean()||!value.at("strong_passwords").is_boolean())throw std::runtime_error("setup_boolean_invalid");
        setup.configuration.bbs_name=value.at("bbs_name").template get<std::string>();setup.configuration.bbs_time_zone=value.at("time_zone").template get<std::string>();
        setup.configuration.logical_nodes=static_cast<std::uint16_t>(integer("nodes",128));setup.configuration.telnet.bind_address=value.at("bind_address").template get<std::string>();setup.configuration.ssh.bind_address=setup.configuration.telnet.bind_address;
        setup.configuration.telnet.port=static_cast<std::uint16_t>(integer("telnet_port",65535));setup.configuration.ssh.port=static_cast<std::uint16_t>(integer("ssh_port",65535));setup.configuration.ssh.enabled=value.at("ssh_enabled").template get<bool>();
        setup.configuration.password_mode=value.at("strong_passwords").template get<bool>()?app::PasswordMode::strong:app::PasswordMode::standard;
        setup.sysop_name=value.at("sysop_name").template get<std::string>();setup.sysop_password=value.at("sysop_password").template get<std::string>();setup.system_password=value.at("system_password").template get<std::string>();
        if(setup.sysop_name.size()>128||setup.sysop_password.size()>128||setup.system_password.size()>128||setup.configuration.bbs_name.size()>64||setup.configuration.bbs_time_zone.size()>128||setup.configuration.telnet.bind_address.size()>15)throw std::runtime_error("setup_text_invalid");
        const auto result=runtime.bootstrap(setup);const bool ok=result.code==Code::ok;
        json_response(response,{{"ok",ok},{"diagnostic",result.diagnostic_code},{"detail",ok?"First-run setup is complete. The BBS is stopped. Review settings, then choose Start BBS when ready.":"Setup did not complete. Check the name, passwords, time zone and listener values. For an interrupted setup, use the same Sysop name and passwords. Existing work is retained; no BBS was started."}},ok?200:409);
    });
