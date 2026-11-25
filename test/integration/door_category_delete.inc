namespace {
void category_delete_storage_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    auto options=settings(root);storage::SqliteStorage db(options,paths);check(db.start().succeeded(),"category delete fixture starts");check(db.shutdown().succeeded(),"fixture stops for synthetic identities");
    const auto file=options.data_directory/"SFX.DB";seed(file);check(db.start().succeeded(),"category delete synthetic owner starts");
    const app::MessageConfigurationActor owner{true,{}};unsigned sequence=8000;
    const auto save=[&](app::DoorConfigurationChange c){c.operation=id(sequence++);c.expected_revision=db.door_configuration(owner).value->revision;check(db.change_door_configuration(owner,c).succeeded(),"seed category deletion through authorized storage");};
    for(unsigned n=1;n<=4;++n){app::DoorConfigurationChange c;c.create_only=true;c.action=app::DoorConfigurationAction::save_menu;c.menu={static_cast<app::DoorMenuId>(n),"Category "+std::to_string(n)};save(c);
        c={};c.create_only=true;c.door=sample_door(n);c.door.rlogin.protected_credential={1,2,3};save(c);}
    for(const app::DoorMenuEntry& e:std::vector<app::DoorMenuEntry>{{0,7,app::DoorEntryKind::submenu,1,"Delete",""},{1,5,app::DoorEntryKind::submenu,2,"Child",""},
        {1,20,app::DoorEntryKind::door,1,"One",""},{2,22,app::DoorEntryKind::door,2,"Two",""},{0,31,app::DoorEntryKind::submenu,3,"Keep",""},
        {3,9,app::DoorEntryKind::door,1,"Shared Door",""},{3,10,app::DoorEntryKind::submenu,2,"Shared child",""},{3,50,app::DoorEntryKind::door,3,"Unrelated",""}}){
        app::DoorConfigurationChange c;c.create_only=true;c.action=app::DoorConfigurationAction::save_entry;c.entry=e;save(c);
    }
    auto before=*db.door_configuration(owner).value;const auto bytes=app::encode_door_catalog(before);
    app::DoorConfigurationChange remove;remove.operation=id(sequence++);remove.expected_revision=before.revision;remove.action=app::DoorConfigurationAction::remove_menu;remove.menu.id=1;
    check(!db.change_door_configuration({false,app::MessageActor{id(2),2,1,1}},remove),"ordinary caller cannot cascade-delete categories");
    auto invalid=remove;invalid.menu.id=0;check(!db.change_door_configuration(owner,invalid),"storage rejects root deletion");invalid=remove;--invalid.expected_revision;check(!db.change_door_configuration(owner,invalid),"storage rejects stale deletion");
    // A test-only trigger aborts after placement and Door removal statements.
    // It never ships and is removed before restart/schema validation.
    {internal::SqliteConnection sql(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);sql.exec("CREATE TRIGGER fixture_category_delete_abort BEFORE DELETE ON sfx_door_menus WHEN old.menu_id=1 BEGIN SELECT RAISE(ABORT,'fixture cascade interruption'); END","inject late delete failure");}
    check(!db.change_door_configuration(owner,remove),"late category SQL failure reported");
    check(app::encode_door_catalog(*db.door_configuration(owner).value)==bytes,"failed cascade rolls back definitions, credentials, references and revision");
    {internal::SqliteConnection sql(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);sql.exec("DROP TRIGGER fixture_category_delete_abort","remove only owned synthetic fault");}
    const auto expected=app::without_door_category(before,1);check(expected.has_value(),"expected cascade valid");
    const auto result=db.change_door_configuration(owner,remove);check(result&&result.value->revision==before.revision+1&&!result.value->replayed,"one atomic cascade advances one revision");
    auto after=*db.door_configuration(owner).value;auto wanted=*expected;wanted.revision=result.value->revision;check(app::encode_door_catalog(after)==app::encode_door_catalog(wanted),"stored cascade exactly matches confirmed recursive scope");
    const auto replay=db.change_door_configuration(owner,remove);check(replay&&replay.value->replayed&&replay.value->revision==result.value->revision,"lost reply replay does not delete again");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"deleted catalog survives actual database restart");
    check(app::encode_door_catalog(*db.door_configuration(owner).value)==app::encode_door_catalog(wanted),"no deleted configuration reappears after restart");
    check(scalar(file,"SELECT count(*) FROM sfx_door_definitions WHERE door_id IN(1,2)")==0&&scalar(file,"SELECT count(*) FROM sfx_door_menus WHERE menu_id IN(1,2)")==0,"removed definition rows absent");
    check(scalar(file,"SELECT count(*) FROM pragma_foreign_key_check")==0,"cascade leaves no foreign key failures");
    remove.operation=id(sequence++);remove.expected_revision=wanted.revision;remove.menu.id=4;check(db.change_door_configuration(owner,remove).succeeded(),"empty unplaced category really deleted");
    check(app::find_door(*db.door_configuration(owner).value,4)!=nullptr,"unrelated unplaced Door not accidentally swept");check(db.shutdown().succeeded(),"delete fixture stops cleanly");
}
}
