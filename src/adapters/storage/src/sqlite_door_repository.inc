namespace door_detail {
namespace app=application;
app::DoorMenuDefinition read_menu(SqliteStatement& q){
    app::DoorMenuDefinition m{static_cast<app::DoorMenuId>(q.int64(0)),q.text(1)};
    unsigned column=2;for(auto* p:{&m.access,&m.execution}){
        const auto mode=q.int64(column++),level=q.int64(column++),sysop=q.int64(column++);
        if(mode<0||mode>2||level<1||level>999||sysop<0||sysop>1)file_detail::corrupt();
        *p={static_cast<app::DoorSecurityMode>(mode),static_cast<std::uint16_t>(level),sysop!=0};
    }
    if(!app::valid_door_text(m.name,64))file_detail::corrupt();
    return m;
}
void save_requirements(sqlite3* db,const app::DoorMenuDefinition& m){
    SqliteStatement q(db,"INSERT INTO sfx_door_menu_requirements VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(menu_id) DO UPDATE SET access_mode=excluded.access_mode,access_level=excluded.access_level,access_sysop=excluded.access_sysop,execution_mode=excluded.execution_mode,execution_level=excluded.execution_level,execution_sysop=excluded.execution_sysop","save category requirements");
    q.bind(1,m.id);int column=2;for(const auto& p:{m.access,m.execution}){q.bind(column++,static_cast<std::int64_t>(p.mode));q.bind(column++,p.level);q.bind(column++,p.sysop_only);}q.step();
}
[[noreturn]] void clearance(){throw StorageFailure(StorageErrorCode::permission_denied,"Door security clearance required",SQLITE_AUTH);}
app::DoorCatalog catalog(sqlite3* db) {
    app::DoorCatalog c;c.menus.clear();
    SqliteStatement p(db,"SELECT revision FROM sfx_door_policy WHERE singleton_id=1","read Door catalog revision");
    if(!p.row())file_detail::corrupt();
    c.revision=p.int64(0);
    SqliteStatement doors(db,"SELECT door_id,definition FROM sfx_door_definitions ORDER BY door_id LIMIT 1000","read bounded Door definitions");
    while(doors.row()){
        const auto d=app::decode_door_definition(doors.blob(1));
        if(!d || d->id!=doors.int64(0))file_detail::corrupt();
        c.doors.push_back(*d);
    }
    SqliteStatement menus(db,"SELECT m.menu_id,m.name,r.access_mode,r.access_level,r.access_sysop,r.execution_mode,r.execution_level,r.execution_sysop FROM sfx_door_menus m LEFT JOIN sfx_door_menu_requirements r USING(menu_id) ORDER BY m.menu_id LIMIT 1001","read bounded Door menus");
    while(menus.row())c.menus.push_back(read_menu(menus));
    SqliteStatement entries(db,"SELECT menu_id,local_number,kind,target_id,title,description FROM sfx_door_entries ORDER BY menu_id,local_number LIMIT 99001","read bounded Door placements");
    while(entries.row())c.entries.push_back({static_cast<app::DoorMenuId>(entries.int64(0)),static_cast<std::uint8_t>(entries.int64(1)),
        static_cast<app::DoorEntryKind>(entries.int64(2)),static_cast<std::uint16_t>(entries.int64(3)),entries.text(4),entries.text(5)});
    if(!app::validate_door_catalog(c).valid())file_detail::corrupt();
    return c;
}
app::DoorAuthority authority(sqlite3* db,const app::MessageActor& a) {
    const auto v=message_detail::actor(db,a);
    return {static_cast<std::uint16_t>(v.level),static_cast<std::uint16_t>(v.threshold)};
}
std::int64_t revision(sqlite3* db) {
    SqliteStatement q(db,"SELECT revision FROM sfx_door_policy WHERE singleton_id=1","read Door menu revision");
    if(!q.row()||q.int64(0)<1)file_detail::corrupt();
    return q.int64(0);
}
app::DoorMenuDefinition menu(sqlite3* db,app::DoorMenuId id) {
    SqliteStatement q(db,"SELECT m.menu_id,m.name,r.access_mode,r.access_level,r.access_sysop,r.execution_mode,r.execution_level,r.execution_sysop FROM sfx_door_menus m LEFT JOIN sfx_door_menu_requirements r USING(menu_id) WHERE m.menu_id=?1","read one Door menu");q.bind(1,id);
    if(!q.row())file_detail::unavailable();
    return read_menu(q);
}
app::DoorDefinition definition(sqlite3* db,app::DoorId id) {
    SqliteStatement q(db,"SELECT definition FROM sfx_door_definitions WHERE door_id=?1","read one referenced Door");q.bind(1,id);
    if(!q.row())file_detail::corrupt();
    const auto value=app::decode_door_definition(q.blob(0));
    if(!value||value->id!=id)file_detail::corrupt();
    return *value;
}
app::DoorMenuEntry placement(SqliteStatement& q) {
    const auto menu_id=q.int64(0),number=q.int64(1),kind=q.int64(2),target=q.int64(3);
    if(menu_id<0||menu_id>999||number<1||number>99||kind<0||kind>1||target<1||target>999)file_detail::corrupt();
    app::DoorMenuEntry e{static_cast<app::DoorMenuId>(menu_id),static_cast<std::uint8_t>(number),
        static_cast<app::DoorEntryKind>(kind),static_cast<std::uint16_t>(target),q.text(4),q.text(5)};
    if(!app::valid_door_text(e.title,64)||!app::valid_door_text(e.description,256,true))file_detail::corrupt();
    return e;
}
std::array<std::uint8_t,32> fingerprint(const app::MessageConfigurationActor& a,const app::DoorConfigurationChange& r){
    file_detail::Fingerprint f("Door configuration/v3");f.actor(a);f.number(r.expected_revision);f.number(static_cast<unsigned>(r.action));f.number(r.previous_number);f.number(r.source_menu);f.number(r.create_only);f.number(r.move_entry);f.number(r.order.size());for(const auto n:r.order)f.number(n);
    switch(r.action){
    case app::DoorConfigurationAction::save_door:{const auto b=app::encode_door_definition(r.door);f.text({reinterpret_cast<const char*>(b.data()),b.size()});break;}
    case app::DoorConfigurationAction::save_menu:f.number(r.menu.id);f.text(r.menu.name);for(const auto& p:{r.menu.access,r.menu.execution}){f.number(static_cast<unsigned>(p.mode));f.number(p.level);f.number(p.sysop_only);}break;
    case app::DoorConfigurationAction::save_entry:f.number(r.entry.menu);f.number(r.entry.number);f.number(static_cast<unsigned>(r.entry.kind));f.number(r.entry.target);f.text(r.entry.title);f.text(r.entry.description);break;
    case app::DoorConfigurationAction::remove_door:f.number(r.door.id);break;
    case app::DoorConfigurationAction::remove_menu:f.number(r.menu.id);break;
    case app::DoorConfigurationAction::remove_entry:f.number(r.entry.menu);f.number(r.entry.number);break;
    case app::DoorConfigurationAction::duplicate_menu:f.number(r.menu.id);f.text(r.menu.name);break;
    }
    return f.value();
}
}
application::StorageResult<application::DoorCatalog> SqliteAccountRepository::door_configuration(const application::MessageConfigurationActor& a){
    return impl_->readers_.read<application::DoorCatalog>("read authorized Door configuration",[a](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);return door_detail::catalog(db);});
}
application::StorageResult<application::DoorCallerMenu> SqliteAccountRepository::door_menu(const application::MessageActor& a,application::DoorMenuId id){
    if(id>999)return read_failure<application::DoorCallerMenu>(invalid("validate Door menu"));
    return impl_->readers_.read<application::DoorCallerMenu>("read authorized Door menu",[a,id](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto authority=door_detail::authority(db,a);
        application::DoorCallerMenu result{door_detail::revision(db),door_detail::menu(db,id),{},false};
        if(!application::meets_door_requirements(result.menu.access,authority))file_detail::unavailable();
        if(!application::meets_door_requirements(result.menu.execution,authority))door_detail::clearance();
        // Caller reads are bounded by this menu's 99 placements, rather than
        // decoding the complete owner catalog and every protected secret blob.
        SqliteStatement q(db,"SELECT menu_id,local_number,kind,target_id,title,description FROM sfx_door_entries WHERE menu_id=?1 ORDER BY local_number LIMIT 100","read bounded current Door menu");q.bind(1,id);
        unsigned count{};while(q.row()){
            if(++count>99)file_detail::corrupt();
            auto e=door_detail::placement(q);
            if(e.kind==application::DoorEntryKind::door){
                const auto definition=door_detail::definition(db,e.target);if(definition.event_only||!application::door_accessible(definition,authority)){result.has_hidden_entries=true;continue;}
            }else if(!application::meets_door_requirements(door_detail::menu(db,e.target).access,authority)){result.has_hidden_entries=true;continue;}
            result.entries.push_back(std::move(e));
        }
        return result;
    });
}
application::StorageResult<application::DoorLaunch> SqliteAccountRepository::door_launch(const application::MessageActor& a,const application::DoorLaunchRequest& r){
    if(r.menu>999 || !r.number || r.number>99 || r.expected_revision<1)return read_failure<application::DoorLaunch>(invalid("validate Door launch"));
    return impl_->readers_.read<application::DoorLaunch>("authorize captured Door launch",[a,r](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto authority=door_detail::authority(db,a);const auto revision=door_detail::revision(db);
        if(revision!=r.expected_revision)file_detail::conflict();
        const auto menu=door_detail::menu(db,r.menu);
        if(!application::meets_door_requirements(menu.access,authority))file_detail::unavailable();
        if(!application::meets_door_requirements(menu.execution,authority))door_detail::clearance();
        SqliteStatement q(db,"SELECT menu_id,local_number,kind,target_id,title,description FROM sfx_door_entries WHERE menu_id=?1 AND local_number=?2","read exact Door launch placement");q.bind(1,r.menu);q.bind(2,static_cast<std::int64_t>(r.number));
        if(!q.row())file_detail::unavailable();
        const auto entry=door_detail::placement(q);
        if(entry.kind!=application::DoorEntryKind::door)file_detail::unavailable();
        const auto door=door_detail::definition(db,entry.target);
        if(door.event_only||!application::door_accessible(door,authority))file_detail::unavailable();
        if(!application::door_executable(door,authority))door_detail::clearance();
        return application::DoorLaunch{revision,door,menu,entry};
    });
}
application::StorageResult<application::DoorEventList> SqliteAccountRepository::door_events(const application::MessageActor& actor,application::DoorAutomaticEvent event){
    if(event!=application::DoorAutomaticEvent::logon&&event!=application::DoorAutomaticEvent::logoff)return read_failure<application::DoorEventList>(invalid("validate Door event"));
    return impl_->readers_.read<application::DoorEventList>("read authorized automatic Doors",[actor,event](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto authority=door_detail::authority(db,actor);application::DoorEventList result{door_detail::revision(db),{}};
        SqliteStatement q(db,"SELECT door_id,definition FROM sfx_door_definitions ORDER BY door_id LIMIT 1000","read bounded event Door candidates");unsigned count{};
        while(q.row()){if(++count>999)file_detail::corrupt();const auto door=application::decode_door_definition(q.blob(1));if(!door||door->id!=q.int64(0))file_detail::corrupt();
            if(door->automatic_event==event&&application::door_executable(*door,authority))result.doors.push_back(door->id);}
        return result;
    });
}
application::StorageResult<application::DoorLaunch> SqliteAccountRepository::door_event_launch(const application::MessageActor& actor,const application::DoorEventLaunchRequest& request){
    if(!request.door||request.door>999||request.expected_revision<1||(request.event!=application::DoorAutomaticEvent::logon&&request.event!=application::DoorAutomaticEvent::logoff))return read_failure<application::DoorLaunch>(invalid("validate automatic Door launch"));
    return impl_->readers_.read<application::DoorLaunch>("authorize automatic Door launch",[actor,request](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto authority=door_detail::authority(db,actor);const auto revision=door_detail::revision(db);if(revision!=request.expected_revision)file_detail::conflict();
        const auto door=door_detail::definition(db,request.door);if(door.automatic_event!=request.event||!application::door_accessible(door,authority))file_detail::unavailable();if(!application::door_executable(door,authority))door_detail::clearance();
        return application::DoorLaunch{revision,door,{0,request.event==application::DoorAutomaticEvent::logon?"Logon Doors":"Logoff Doors"},{}};
    });
}
application::StorageResult<application::DoorChangeOutcome> SqliteAccountRepository::change_door_configuration(const application::MessageConfigurationActor& a,const application::DoorConfigurationChange& r){
    if(!application::valid_door_change(r))return read_failure<application::DoorChangeOutcome>(invalid("validate Door configuration"));
    const auto fingerprint=door_detail::fingerprint(a,r);const auto now=impl_->message_utc_now_();
    if(!application::valid_utc_seconds(now))return read_failure<application::DoorChangeOutcome>(invalid("validate Door configuration clock"));
    return impl_->write_result<application::DoorChangeOutcome>("change authorized Door configuration",16384,[a,r,fingerprint,now](sqlite3* db,auto& out){
        message_detail::config_authority(db,a);
        SqliteStatement receipt(db,"SELECT fingerprint,actor_account_id,revision FROM sfx_door_configuration_receipts WHERE operation_id=?1","read Door operation receipt");receipt.bind(1,r.operation);
        if(receipt.row()){
            const auto bytes=receipt.blob(0);const auto account=a.caller?a.caller->account:0;
            if(bytes.size()!=fingerprint.size() || !std::equal(bytes.begin(),bytes.end(),fingerprint.begin()) || receipt.int64(1)!=account)return SQLITE_ABORT;
            out=application::DoorChangeOutcome{receipt.int64(2),true};return SQLITE_OK;
        }
        auto c=door_detail::catalog(db);if(c.revision!=r.expected_revision)return SQLITE_ABORT;
        const auto revision=c.revision;
        const auto candidate=application::changed_door_catalog(c,r);if(!candidate)return SQLITE_CONSTRAINT;
        using Action=application::DoorConfigurationAction;
        std::uint16_t identity{};
        switch(r.action){
        case Action::save_door:{
            SqliteStatement q(db,"INSERT INTO sfx_door_definitions VALUES(?1,?2) ON CONFLICT(door_id) DO UPDATE SET definition=excluded.definition","save Door definition");
            q.bind(1,r.door.id);q.bind(2,application::encode_door_definition(r.door));q.step();identity=r.door.id;break;}
        case Action::save_menu:{
            SqliteStatement q(db,"INSERT INTO sfx_door_menus VALUES(?1,?2) ON CONFLICT(menu_id) DO UPDATE SET name=excluded.name","save Door menu");
            q.bind(1,r.menu.id);q.bind(2,r.menu.name);q.step();door_detail::save_requirements(db,r.menu);identity=r.menu.id;break;}
        case Action::save_entry:{
            if(r.move_entry||!r.order.empty()){
                SqliteStatement clear(db,"DELETE FROM sfx_door_entries WHERE menu_id=?1 OR (?2=1 AND menu_id=?3)","replace explicitly arranged menu entries atomically");clear.bind(1,r.entry.menu);clear.bind(2,r.move_entry);clear.bind(3,r.source_menu);clear.step();
                for(const auto& e:candidate->entries)if(e.menu==r.entry.menu||(r.move_entry&&e.menu==r.source_menu)){
                    SqliteStatement q(db,"INSERT INTO sfx_door_entries VALUES(?1,?2,?3,?4,?5,?6)","retain complete arranged entry");q.bind(1,e.menu);q.bind(2,static_cast<std::int64_t>(e.number));q.bind(3,static_cast<std::int64_t>(e.kind));q.bind(4,e.target);q.bind(5,e.title);q.bind(6,e.description);q.step();
                }
                identity=r.entry.menu;break;
            }
            const auto& e=r.entry;SqliteStatement q(db,"INSERT INTO sfx_door_entries VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(menu_id,local_number) DO UPDATE SET kind=excluded.kind,target_id=excluded.target_id,title=excluded.title,description=excluded.description","save Door placement");
            q.bind(1,e.menu);q.bind(2,static_cast<std::int64_t>(e.number));q.bind(3,static_cast<std::int64_t>(e.kind));q.bind(4,e.target);q.bind(5,e.title);q.bind(6,e.description);q.step();identity=e.menu;
            if(r.previous_number && r.previous_number!=e.number){SqliteStatement move(db,"DELETE FROM sfx_door_entries WHERE menu_id=?1 AND local_number=?2","move stable Door entry explicitly");move.bind(1,e.menu);move.bind(2,static_cast<std::int64_t>(r.previous_number));move.step();}break;}
        case Action::remove_door:{SqliteStatement q(db,"DELETE FROM sfx_door_definitions WHERE door_id=?1","remove unreferenced Door definition");q.bind(1,r.door.id);q.step();identity=r.door.id;break;}
        case Action::remove_menu:{
            // One transaction and receipt for the exact confirmed revision.
            // Remove every placement first, then definitions and their security
            // rows; a failure at any point rolls back the complete deletion.
            for(const auto& m:c.menus)if(!application::find_door_menu(*candidate,m.id)){
                SqliteStatement q(db,"DELETE FROM sfx_door_entries WHERE menu_id=?1 OR (kind=1 AND target_id=?1)","remove deleted category placements and contents");q.bind(1,m.id);q.step();
            }
            for(const auto& d:c.doors)if(!application::find_door(*candidate,d.id)){
                SqliteStatement entries(db,"DELETE FROM sfx_door_entries WHERE kind=0 AND target_id=?1","remove deleted Door appearances everywhere");entries.bind(1,d.id);entries.step();
                SqliteStatement q(db,"DELETE FROM sfx_door_definitions WHERE door_id=?1","delete contained Door configuration");q.bind(1,d.id);q.step();
            }
            for(const auto& m:c.menus)if(!application::find_door_menu(*candidate,m.id)){
                SqliteStatement q(db,"DELETE FROM sfx_door_menus WHERE menu_id=?1","delete category definition");q.bind(1,m.id);q.step();
            }
            identity=r.menu.id;break;}
        case Action::remove_entry:{SqliteStatement q(db,"DELETE FROM sfx_door_entries WHERE menu_id=?1 AND local_number=?2","remove Door placement");q.bind(1,r.entry.menu);q.bind(2,static_cast<std::int64_t>(r.entry.number));q.step();identity=r.entry.menu;break;}
        case Action::duplicate_menu:{
            SqliteStatement menu(db,"INSERT INTO sfx_door_menus VALUES(?1,?2)","duplicate Menu definition with new permanent ID");menu.bind(1,r.menu.id);menu.bind(2,r.menu.name);menu.step();
            SqliteStatement entries(db,"INSERT INTO sfx_door_entries SELECT ?1,local_number,kind,target_id,title,description FROM sfx_door_entries WHERE menu_id=?2","copy ordered references only; no recursive duplication");entries.bind(1,r.menu.id);entries.bind(2,r.source_menu);entries.step();door_detail::save_requirements(db,*application::find_door_menu(*candidate,r.menu.id));identity=r.menu.id;break;}
        }
        SqliteStatement p(db,"UPDATE sfx_door_policy SET revision=revision+1 WHERE singleton_id=1","advance Door catalog revision");p.step();
        SqliteStatement q(db,"INSERT INTO sfx_door_configuration_receipts VALUES(?1,?2,?3,?4,?5,?6,?7)","audit Door configuration without secrets");
        q.bind(1,r.operation);q.bind(2,std::vector<std::uint8_t>(fingerprint.begin(),fingerprint.end()));q.bind(3,a.caller?a.caller->account:0);
        q.bind(4,revision+1);q.bind(5,static_cast<std::int64_t>(r.action));q.bind(6,identity);q.bind(7,now);q.step();
        // Bounded replay history; old UUIDs cannot bypass the revision CAS.
        SqliteStatement trim(db,"DELETE FROM sfx_door_configuration_receipts WHERE revision<(?1-4096)","bound Door receipts");trim.bind(1,revision+1);trim.step();
        out=application::DoorChangeOutcome{revision+1,false};return SQLITE_OK;
    });
}
