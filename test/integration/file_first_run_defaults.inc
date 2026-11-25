void first_run_defaults(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    for(const bool customized:{false,true}){
        auto options=settings(root/(customized?"custom":"fresh"));storage::SqliteStorage db(options,paths);
        check(db.start().succeeded(),"fresh default fixture starts with frozen migrations");
        const app::FileConfigurationActor host{true,{}};
        if(customized){
            const auto before=db.file_configuration(host);check(before&&before.value,"read pre-first-run configuration");
            app::FileConfigurationChange edit;edit.operation=id(7100);edit.action=app::FileConfigurationAction::area;
            edit.expected_catalog_revision=before.value->policy.catalog_revision;edit.area=before.value->areas[0];
            edit.area.area.permissions.upload=77;
            check(db.change_file_configuration(host,edit).succeeded(),"explicit customization before first run");
        }
        app::CredentialEnvelope credential;credential.envelope_format_version=1;credential.scheme_identifier="file-defaults-fixture";
        credential.parameter_encoding_version=1;credential.canonical_parameters={1};credential.salt={1,2};credential.verifier={3,4};credential.created_utc_seconds=1792248000;
        app::StageInitialSysopRequest stage;stage.display_name="Defaults Sysop";stage.credential=credential;
        stage.audit={id(7101),id(7102),{},1792248000,app::AuditReasonCode::initial_setup};
        const auto staged=db.stage_initial_sysop(stage);check(staged&&staged.account_id,"stage actual first Sysop");
        const auto policy=db.security_policy();check(policy&&policy.value,"first-run security policy available");
        const app::CompleteFirstRunRequest complete{*staged.account_id,policy.value->revision,policy.value->policy_hash,policy.value->policy_hash,
            {id(7103),id(7104),{},1792248000,app::AuditReasonCode::initial_setup}};
        check(db.complete_first_run(complete).succeeded(),"complete ordinary first-run transaction");
        const auto after=db.file_configuration(host);check(after&&after.value&&after.value->areas.size()==3,"three built-in areas after first run");
        const auto& areas=after.value->areas;
        if(customized){
            check(areas[0].area.permissions.upload==77&&!areas[0].area.permissions.uploads_enabled&&areas[1].area.permissions.upload==1,
                "customized first-run permission set retained rather than partially reset");
        }else{
            const std::uint16_t access[]={10,10,999},upload[]={999,10,10},download[]={10,999,999};
            for(std::size_t i=0;i<3;++i){const auto& a=areas[i].area;
                check(a.permissions.access==access[i]&&a.permissions.upload==upload[i]&&a.permissions.download==download[i]&&a.permissions.uploads_enabled,
                    "exact owner new-install access/upload/download defaults");
                check(app::file_area_uploadable(after.value->groups.front(),a,{999,999},false),"Sysop can upload in every default area");
            }
            check(app::file_area_uploadable(after.value->groups.front(),areas[2].area,{10,999},true)&&
                !app::file_area_visible(after.value->groups.front(),areas[2].area,{10,999}),"level10 private submission stays hidden");
        }
        const auto catalog_revision=after.value->policy.catalog_revision;
        check(!db.complete_first_run(complete).succeeded(),"completed installation cannot replay initial-default update");
        check(db.shutdown().succeeded()&&db.start().succeeded(),"completed installation reopens without resetting defaults");
        const auto reopened=db.file_configuration(host);check(reopened&&reopened.value&&reopened.value->policy.catalog_revision==catalog_revision,
            "routine restart leaves file configuration revision unchanged");
        check(reopened.value->areas[0].area.permissions.upload==(customized?77:999),"routine restart preserves custom/default upload minimum");
        check(db.shutdown().succeeded(),"first-run default fixture closed");
    }
}
