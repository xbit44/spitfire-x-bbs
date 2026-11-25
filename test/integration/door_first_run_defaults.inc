namespace {
app::CompleteFirstRunRequest stage_door_defaults(storage::SqliteStorage& db) {
    app::CredentialEnvelope credential;
    credential.envelope_format_version=1;credential.scheme_identifier="door-defaults-fixture";
    credential.parameter_encoding_version=1;credential.canonical_parameters={1};
    credential.salt={1,2};credential.verifier={3,4};credential.created_utc_seconds=1792248000;
    app::StageInitialSysopRequest stage;stage.display_name="Defaults Sysop";stage.credential=credential;
    stage.audit={id(8101),id(8102),{},1792248000,app::AuditReasonCode::initial_setup};
    const auto staged=db.stage_initial_sysop(stage);
    check(staged&&staged.account_id,"stage first-run Sysop through storage authority");
    const auto policy=db.security_policy();check(policy&&policy.value,"read first-run policy");
    return {*staged.account_id,policy.value->revision,policy.value->policy_hash,policy.value->policy_hash,
        {id(8103),id(8104),{},1792248000,app::AuditReasonCode::initial_setup}};
}

void check_starter_door_categories(const app::DoorCatalog& catalog) {
    check(catalog.revision==2&&catalog.menus.size()==4&&catalog.entries.size()==3,
        "first run publishes three categories with one catalog revision");
    check(catalog.doors.empty(),"starter categories create no Door target or credential");
    const auto* root=app::find_door_menu(catalog,0);
    check(root&&root->name=="Door Menu"&&root->access.level==1&&root->execution.level==1,
        "starter defaults preserve root menu settings");
    const char* names[]={"Main","BBS Transporter","Utilities & Tools"};
    for(unsigned n=0;n<3;++n){
        const auto* menu=app::find_door_menu(catalog,static_cast<app::DoorMenuId>(n+1));
        check(menu&&menu->name==names[n],"exact ordered starter category names");
        for(const auto& requirement:{menu->access,menu->execution}){
            check(requirement.mode==app::DoorSecurityMode::minimum&&requirement.level==10&&!requirement.sysop_only,
                "each starter category requires Access10 and Execution10 without Sysop Only");
            check(app::meets_door_requirements(requirement,{10,999})&&
                !app::meets_door_requirements(requirement,{9,999}),"level10 qualifies and level9 does not");
        }
        const auto& entry=catalog.entries[n];
        check(entry.menu==0&&entry.number==n+1&&entry.kind==app::DoorEntryKind::submenu&&
            entry.target==n+1&&entry.title==names[n]&&entry.description.empty(),
            "starter categories occupy root choices1 through3 without extra content");
    }
    check(app::validate_door_catalog(catalog).valid(),"starter catalog satisfies normal model invariants");
}

[[maybe_unused]] void door_first_run_defaults(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths) {
    const app::MessageConfigurationActor owner{true,{}};
    for(const std::string variant:{"fresh","custom-category","custom-root","rollback","legacy16"}){
        auto options=settings(root/variant);auto migrations=storage::sfx_storage_migrations();
        if(variant=="legacy16")migrations.resize(16);
        storage::SqliteStorage db(options,paths,migrations);check(db.start().succeeded(),"new isolated defaults fixture starts");
        if(variant=="custom-category"||variant=="custom-root"){
            const auto initial=db.door_configuration(owner);check(initial&&initial.value,"read untouched pre-setup Door catalog");
            app::DoorConfigurationChange change;change.operation=id(8200);change.expected_revision=initial.value->revision;
            change.action=app::DoorConfigurationAction::save_menu;change.create_only=variant=="custom-category";
            change.menu={static_cast<app::DoorMenuId>(change.create_only?1:0),"Owner category"};
            change.menu.access.level=77;change.menu.execution.level=88;
            check(db.change_door_configuration(owner,change).succeeded(),"customize Door settings before initial completion");
            if(variant=="custom-category"){
                change={};change.operation=id(8201);change.expected_revision=db.door_configuration(owner).value->revision;
                change.action=app::DoorConfigurationAction::save_entry;change.create_only=true;
                change.entry={0,7,app::DoorEntryKind::submenu,1,"Owner placement","Keep this"};
                check(db.change_door_configuration(owner,change).succeeded(),"preserve pre-setup placement and number");
            }
        }
        std::vector<std::uint8_t> custom_before;
        if(variant=="custom-category"||variant=="custom-root")
            custom_before=app::encode_door_catalog(*db.door_configuration(owner).value);
        const auto complete=stage_door_defaults(db);const auto file=options.data_directory/"SFX.DB";
        if(variant=="rollback"){
            {internal::SqliteConnection inject(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
                inject.exec("CREATE TRIGGER defaults_test_abort BEFORE INSERT ON sfx_door_entries "
                    "WHEN NEW.local_number=2 BEGIN SELECT RAISE(ABORT,'injected defaults failure'); END;",
                    "inject failure after the first default placement");}
            check(!db.complete_first_run(complete),"partial starter creation reports transaction failure");
            const auto state=db.first_run_state();check(state&&state.value&&!state.value->complete,
                "failed defaults do not publish completed first run");
            const auto catalog=db.door_configuration(owner);check(catalog&&catalog.value&&catalog.value->revision==1&&
                catalog.value->menus.size()==1&&catalog.value->entries.empty(),"failed creation rolls back all starter categories and placements");
            check(scalar(file,"SELECT count(*) FROM sfx_door_menu_requirements")==1,"failed creation rolls back all starter requirements");
            check(scalar(file,"SELECT access_minimum FROM sfx_file_areas WHERE area_id=1")==1,
                "failed starter creation also rolls back first-run File Area defaults");
            {internal::SqliteConnection inject(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
                inject.exec("DROP TRIGGER defaults_test_abort","remove only owned failure injection");}
        }
        check(db.complete_first_run(complete).succeeded(),"complete ordinary first-run transaction");
        if(variant!="legacy16"){
            const auto after=db.door_configuration(owner);check(after&&after.value,"read completed first-run Door configuration");
            if(custom_before.empty())check_starter_door_categories(*after.value);
            else check(app::encode_door_catalog(*after.value)==custom_before,"all pre-setup Door configuration bytes preserved");
            const auto completed=app::encode_door_catalog(*after.value);
            check(!db.complete_first_run(complete),"completed first run cannot reapply defaults");
            check(db.shutdown().succeeded()&&db.start().succeeded(),"completed board restarts normally");
            check(app::encode_door_catalog(*db.door_configuration(owner).value)==completed,
                "restart and completion replay preserve exact catalog without duplicates");
        }
        check(db.shutdown().succeeded(),"defaults fixture closes normally");
        if(variant=="legacy16"){
            storage::SqliteStorage upgrade(options,paths);check(upgrade.start().succeeded(),"completed old installation upgrades normally");
            const auto catalog=upgrade.door_configuration(owner);check(catalog&&catalog.value&&catalog.value->menus.size()==1&&
                catalog.value->entries.empty()&&catalog.value->revision==1,"upgrade preserves an intentionally empty existing Door Menu");
            check(upgrade.shutdown().succeeded(),"upgraded existing board closes normally");
        }
        check(scalar(file,"SELECT count(*) FROM sfx_accounts")==1,"defaults add no accounts beyond staged Sysop");
        check(scalar(file,"SELECT count(*) FROM pragma_foreign_key_check")==0,"default and preserved catalogs retain foreign-key integrity");
        {internal::SqliteConnection inspect(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
            check(internal::validate_sfx_shipping_database(inspect.get(),true),"defaults retain exact shipping schema and migration history");}
    }
}
}
