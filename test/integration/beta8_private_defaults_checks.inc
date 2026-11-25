// Exercise the supported first-run transaction, not a synthetic row seed.
void beta8_private_default_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    for(int saved_choice:{-1,0,1}){
        const auto directory=root/(saved_choice<0?"untouched":saved_choice==0?"saved-off":"saved-on");
        const auto settings=options(directory);db::SqliteStorage store(settings,paths);
        check(store.start().succeeded(),"private default fixture creates exact current schema");
        app::StageInitialSysopRequest stage;stage.display_name="Initial Pipe Sysop";
        stage.credential.envelope_format_version=1;stage.credential.scheme_identifier="test-deferred-envelope";
        stage.credential.parameter_encoding_version=1;stage.credential.canonical_parameters={1,2,3};
        stage.credential.salt={0x10,0x11,1};stage.credential.verifier={0x20,0x21,0x22,1};
        stage.credential.pepper_key_identifier="test-key-1";stage.credential.created_utc_seconds=1791278000;
        stage.audit={id(6100),id(6101),{},1791278000,app::AuditReasonCode::initial_setup};
        const auto staged=store.stage_initial_sysop(stage);check(staged.succeeded()&&staged.account_id.has_value(),"real initial Sysop staged for private default");
        if(saved_choice>=0){
            const auto catalog=store.message_configuration({true,{}},{},{});check(bool(catalog),"pre-completion private policy is readable by installation owner");
            app::MessageConfigurationChange change;change.operation=id(6102);change.action=app::MessageConfigurationAction::private_presentation;
            change.expected_configuration_revision=catalog.value->revision;change.expected_private_presentation_revision=catalog.value->private_presentation_revision;change.private_pipe_colors=saved_choice!=0;
            check(bool(store.change_message_configuration({true,{}},change)),"explicit pre-completion private choice is audited even Off to Off");
        }
        // Closing after staging models interrupted setup. Retrying completion
        // must seed only untouched policy, never overwrite a saved choice.
        check(store.shutdown().succeeded()&&store.start().succeeded(),"interrupted first-run private choice survives actual close/reopen");
        const auto policy=store.security_policy();check(bool(policy)&&policy.value.has_value(),"private default reads authoritative security policy");
        app::CompleteFirstRunRequest complete;complete.staged_sysop_account_id=*staged.account_id;
        complete.expected_policy_revision=policy.value->revision;complete.expected_policy_hash=policy.value->policy_hash;complete.published_policy_hash=policy.value->policy_hash;
        complete.audit={id(6103),id(6104),{},1791278001,app::AuditReasonCode::initial_setup};
        auto wrong=complete;wrong.published_policy_hash=std::string(64,'0');check(!store.complete_first_run(wrong),"refused first-run completion cannot seed defaults");
        check(store.complete_first_run(complete).succeeded(),"real first-run completion publishes default atomically");
        auto current=store.message_configuration({true,{}},{},{});check(current&&current.value->private_pipe_colors==(saved_choice!=0),"fresh private pipe On; explicit pre-completion Off or On retained");
        check(current.value->private_presentation_revision==2,"fresh default or explicit saved choice has one authoritative revision advance");
        check(!store.complete_first_run(complete),"completed first-run transaction cannot repeat/reset private policy");
        for(bool chosen:{false,true}){
            current=store.message_configuration({true,{}},{},{});app::MessageConfigurationChange change;change.operation=id(chosen?6106:6105);
            change.action=app::MessageConfigurationAction::private_presentation;change.expected_configuration_revision=current.value->revision;
            change.expected_private_presentation_revision=current.value->private_presentation_revision;change.private_pipe_colors=chosen;
            check(bool(store.change_message_configuration({true,{}},change)),"completed board saves explicit private presentation choice");
            check(store.shutdown().succeeded()&&store.start().succeeded(),"completed board closes/reopens without first-run defaults");
            current=store.message_configuration({true,{}},{},{});check(current&&current.value->private_pipe_colors==chosen,"saved completed-board Off and On both preserved on reopen/update path");
        }
        check(store.shutdown().succeeded(),"private default fixture stopped");
    }
}
