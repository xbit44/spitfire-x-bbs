namespace {
void migration18(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    for(const char* point:{"before-door-program-migration","before-door-program-migration-commit","after-door-program-migration-commit"}){
        auto options=settings(root/point);auto previous=storage::sfx_storage_migrations();previous.resize(17);
        {storage::SqliteStorage old(options,paths,previous);check(old.start().succeeded()&&old.health().schema_version==17,"exact v17 predecessor");check(old.shutdown().succeeded(),"predecessor closed");}
        const auto file=options.data_directory/"SFX.DB";seed(file);auto door=sample_door(1);door.execution.level=40;door.rlogin.protected_credential={10,20,30};
        const auto legacy=legacy_storage_door(2,door);
        {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);internal::SqliteStatement q(c.get(),"INSERT INTO sfx_door_definitions VALUES(1,?1)","v2 bytes");q.bind(1,legacy);q.step();
            c.exec("INSERT INTO sfx_door_entries VALUES(0,42,0,1,'Keep this placement','Keep this description')","legacy placement");}
        options.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"injected program migration fault"}:app::StorageStatus::success();};
        {storage::SqliteStorage fault(options,paths);check(!fault.start(),"schema18 interruption reported");}
        {internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(c.get(),true),"interruption retains exact schema");check(c.scalar_int64("PRAGMA user_version","atomic program migration")== (std::string_view(point)=="after-door-program-migration-commit"?18:17),"v18 commit boundary atomic");}
        options.bootstrap_boundary_hook={};storage::SqliteStorage db(options,paths);check(db.start().succeeded()&&db.health().schema_version==27,"schema18 retries safely through current schema27");
        const app::MessageConfigurationActor owner{true,{}};auto catalog=db.door_configuration(owner);check(catalog&&catalog.value&&catalog.value->entries.size()==1&&catalog.value->entries[0].number==42,"gapped placement preserved");
        const auto* restored=app::find_door(*catalog.value,1);check(restored&&restored->code=="DOOR1"&&restored->kind==app::DoorExecutionKind::rlogin&&restored->execution.level==40&&restored->rlogin.protected_credential==door.rlogin.protected_credential&&restored->rlogin.protected_pepper==door.rlogin.protected_pepper,"v2 keeps execution, automatic RLogin and secret blobs");
        {internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);internal::SqliteStatement q(c.get(),"SELECT definition FROM sfx_door_definitions WHERE door_id=1","preserved bytes");check(q.row()&&q.blob(0)==legacy,"migration preserves immutable v2 bytes");}
        app::DoorConfigurationChange change;change.operation=id(18001);change.expected_revision=catalog.value->revision;change.action=app::DoorConfigurationAction::save_door;change.door=*restored;
        change.door.kind=app::DoorExecutionKind::local;change.door.program.startup_directory=std::string(1024,'S');change.door.program.command_line=std::string(4096,'C');change.door.program.cleanup_command=std::string(4096,'X');
        change.door.program.type=app::DoorProgramType::native;change.door.program.io=app::DoorIo::socket;change.door.program.drop_file=app::DoorDropFile::door32_sys;change.door.program.drop_location=app::DoorDropLocation::temporary;change.door.allow_multiple=false;change.door.automatic_event=app::DoorAutomaticEvent::logon;change.door.event_only=true;
        // Existing protected credentials can make a legal v3 record exceed the
        // old 10KB SQL bound; they must remain opaque and survive switching kind.
        change.door.rlogin.protected_credential.resize(4096,10);check(app::encode_door_definition(change.door).size()>10000,"fixture exceeds old definition bound");
        const auto wire=app::encode_door_configuration_change(change);const auto request=app::decode_door_configuration_change(wire);check(request.has_value(),"maximum local fields cross bounded IPC codec");
        const auto saved=db.change_door_configuration(owner,*request);check(saved&&saved.value,"v3 local settings persist through authoritative storage");const auto replay=db.change_door_configuration(owner,*request);check(replay&&replay.value&&replay.value->replayed&&replay.value->revision==saved.value->revision,"v3 exact operation replay has no duplicate mutation");
        check(db.shutdown().succeeded()&&db.start().succeeded(),"restart v3 storage");catalog=db.door_configuration(owner);check(catalog&&catalog.value&&app::encode_door_definition(*app::find_door(*catalog.value,1))==app::encode_door_definition(change.door),"all local, hidden and protected fields survive restart");
        check(db.shutdown().succeeded(),"migration fixture stopped");check(scalar(file,"SELECT count(*) FROM pragma_foreign_key_check")==0&&scalar(file,"SELECT count(*) FROM sfx_accounts")==3&&scalar(file,"SELECT count(*) FROM sfx_schema_migrations")==27,"accounts, placement integrity and current schema27 ledger retained");
    }
}
}
