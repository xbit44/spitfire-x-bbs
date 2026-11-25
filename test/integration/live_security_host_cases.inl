void qualify_live_security_administration(const std::filesystem::path& root) {
    const auto port=available_loopback_port();
    auto setup=request("Live administration QA",port);
    std::int64_t final_policy{};
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code==app::LocalControlResponseCode::setup_required,"live admin fresh Setup-backed host");
        require(host.bootstrap(setup).code==app::LocalControlResponseCode::ok,"live admin bootstrap");
        const auto original=host.configuration_snapshot();
        require(original.has_value(),"live configuration snapshot");
        auto levels=host.security_snapshot(0);
        require(levels.code==app::LocalAccountResultCode::ok && levels.levels.size()==2 && levels.policy.new_user_level==10 && levels.policy.sysop_threshold==999,
            "Runtime exposes existing definitions/defaults without shutdown");
        app::LocalSecurityChange change{levels.policy.revision,app::LocalSecurityAction::upsert_level,20,0,10,999,"Member",{}};
        auto result=host.change_security(change);
        require(result.code==app::LocalAccountResultCode::ok && result.committed,"Runtime publishes new level while listening");
        require(host.status().telnet_active,"catalog save keeps listener operational");
        const auto saved=host.configuration_snapshot();
        require(saved && saved->active.security_policy_revision==2 && saved->saved.security_policy_revision==2 &&
            saved->active.configuration_revision==saved->saved.configuration_revision && saved->active.security_policy_hash==saved->saved.security_policy_hash,
            "running and saved policy bindings match after live publication");
        require(host.change_security(change).code==app::LocalAccountResultCode::conflict,"stale live catalog save refuses");
        app::LocalAccountRequest user;user.action=app::LocalAccountAction::create_user;user.expected_revision=1;
        user.name="Live Editor User";user.password=user.confirmation="Fixture-user-123";
        require(host.administer_account(user).code==app::LocalAccountResultCode::ok && user.password.empty(),"live New User and secret erasure");
        const auto list=host.account_snapshot({0,"Live Editor",0});
        require(list.code==app::LocalAccountResultCode::ok && list.accounts.size()==1,"live account list loads");
        const auto id=list.accounts[0].account_id;
        auto detail=host.account_snapshot({id,"",0});
        user={};user.action=app::LocalAccountAction::save_user;user.account_id=id;user.expected_revision=detail.accounts[0].revision;
        user.expected_policy_revision=detail.policy.revision;user.name="Live Editor User";user.security_level=20;user.profile.location="Saved live";
        require(host.administer_account(user).code==app::LocalAccountResultCode::ok,"live full record Save");
        require(host.account_snapshot({id,"",0}).accounts[0].security_level==20,"live assigned level distinct from defaults");
        require(host.administer_account(user).code==app::LocalAccountResultCode::conflict,"concurrent account revision protection via Runtime");
        // An unrelated staged configuration must not be applied by a catalog Save.
        auto pending=*host.configuration_snapshot();
        auto candidate=pending.saved;candidate.password_mode=app::PasswordMode::strong;
        ++candidate.configuration_revision;
        const auto staged=host.update_configuration({pending.saved.configuration_revision,candidate});
        require(staged.code==app::LocalControlResponseCode::ok,"stage independent password policy");
        const auto before=host.configuration_snapshot();
        change={2,app::LocalSecurityAction::set_defaults,0,0,20,999,{},{}};
        require(host.change_security(change).code==app::LocalAccountResultCode::ok,"save defaults while unrelated setting is staged");
        const auto after=host.configuration_snapshot();
        require(before && after && after->active.password_mode==before->active.password_mode &&
            after->saved.password_mode==before->saved.password_mode && after->active.security_policy_revision==3 && after->saved.security_policy_revision==3,
            "live security binding preserves unrelated active versus saved settings");
        const auto preview=host.account_snapshot({id,"",0});
        const auto definition=app::security_reassignment_definition({{id,preview.accounts[0].revision}});
        auto hash=win::installation::sha256({definition.begin(),definition.end()});
        std::transform(hash.begin(),hash.end(),hash.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        change={3,app::LocalSecurityAction::retire_level,20,10,10,999,{},{},hash};
        require(host.change_security(change).code==app::LocalAccountResultCode::ok,"live removal atomically reassigns reviewed users/default");
        require(host.account_snapshot({id,"",0}).accounts[0].security_level==10 && host.security_snapshot(0).policy.new_user_level==10,
            "removed level leaves no dangling current references");
        final_policy=host.security_snapshot(0).policy.revision;
        require(host.shutdown().code==app::LocalControlResponseCode::ok,"live administration orderly shutdown");
    }
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code==app::LocalControlResponseCode::ok,"published live security survives exact protected restart");
        require(host.security_snapshot(0).policy.revision==final_policy,"restarted policy revision exact");
        require(host.shutdown().code==app::LocalControlResponseCode::ok,"live administration restarted host stopped");
    }
    std::cout<<"PASS live Runtime account/catalog/default/removal publication, optimistic edits, pending-setting isolation and protected restart\n";
}
