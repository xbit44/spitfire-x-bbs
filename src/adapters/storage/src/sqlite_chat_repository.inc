namespace chat_detail {
application::ChatConfiguration config(sqlite3* db){
    SqliteStatement q(db,"SELECT revision,definition FROM sfx_chat_configuration WHERE singleton_id=1","read Chat configuration");
    if(!q.row())file_detail::corrupt();
    const auto bytes=q.blob(1);
    auto c=bytes.empty()?std::optional<application::ChatConfiguration>(application::ChatConfiguration{}):application::decode_chat_configuration(bytes);
    if(!c||c->revision!=q.int64(0))file_detail::corrupt();
    return *c;
}
application::ChatPreferences preferences(sqlite3* db,application::AccountId id){
    SqliteStatement q(db,"SELECT revision,dnd,ghost,notices FROM sfx_chat_preferences WHERE account_id=?1","read Chat preferences");q.bind(1,id);
    if(!q.row())return {};
    return {q.int64(0),q.int64(1)!=0,q.int64(2)!=0,q.int64(3)!=0};
}
}
application::StorageResult<application::ChatConfiguration> SqliteAccountRepository::chat_configuration(const application::MessageConfigurationActor& a){
    return impl_->readers_.read<application::ChatConfiguration>("read authorized Chat configuration",[a](sqlite3* db){message_detail::ReadSnapshot snapshot(db);if(a.installation_owner)message_detail::config_authority(db,a);else if(a.caller)message_detail::actor(db,*a.caller);else file_detail::unavailable();return chat_detail::config(db);});
}
application::StorageResult<application::ChatConfiguration> SqliteAccountRepository::change_chat_configuration(const application::MessageConfigurationActor& a,const application::ChatConfiguration& c){
    if(!application::valid_chat_configuration(c))return read_failure<application::ChatConfiguration>(invalid("validate Chat settings"));
    return impl_->write_result<application::ChatConfiguration>("change authorized Chat configuration",32768,[a,c](sqlite3* db,auto& out){
        message_detail::config_authority(db,a);const auto old=chat_detail::config(db);if(old.revision!=c.revision)return SQLITE_ABORT;
        if(old.entry!=c.entry)return SQLITE_CONSTRAINT;
        for(const auto& cmd:c.commands){SqliteStatement d(db,"SELECT count(*) FROM sfx_door_definitions WHERE door_id=?1","validate menu Door target");d.bind(1,cmd.door);if(!d.row()||d.int64(0)!=1)return SQLITE_CONSTRAINT;}
        auto next=c;++next.revision;SqliteStatement q(db,"UPDATE sfx_chat_configuration SET revision=?1,definition=?2 WHERE singleton_id=1","save Chat settings");q.bind(1,next.revision);q.bind(2,application::encode_chat_configuration(next));q.step();out=next;return SQLITE_OK;
    });
}
application::StorageResult<application::ChatPreferences> SqliteAccountRepository::chat_preferences(const application::MessageActor& a){return impl_->readers_.read<application::ChatPreferences>("read own Chat preferences",[a](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::actor(db,a);return chat_detail::preferences(db,a.account);});}
application::StorageResult<application::PrivateChatAllowance> SqliteAccountRepository::private_chat_allowance(const application::MessageActor& a,std::string_view date){
    if(!application::valid_iso_civil_date(date))return read_failure<application::PrivateChatAllowance>(invalid("validate private Chat day"));
    return impl_->readers_.read<application::PrivateChatAllowance>("read own private Chat allowance",[a,date=std::string(date)](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto actor=message_detail::actor(db,a);application::PrivateChatAllowance result;
        SqliteStatement limits(db,"SELECT minutes_per_chat,minutes_per_day FROM sfx_security_chat_limits WHERE security_level=?1","read private Chat limits");limits.bind(1,actor.level);
        if(limits.row()){
            if(!limits.is_null(0))result.minutes_per_chat=static_cast<std::uint32_t>(limits.int64(0));
            if(!limits.is_null(1)){
                SqliteStatement used(db,"SELECT coalesce(sum(c.seconds),0) FROM sfx_private_chat_seconds c JOIN sfx_account_sessions s USING(session_id) WHERE s.account_id=?1 AND c.local_date=?2","read daily private Chat seconds");used.bind(1,a.account);used.bind(2,date);if(!used.row())file_detail::corrupt();
                result.seconds_remaining_today=std::max<std::int64_t>(0,limits.int64(1)*60-used.int64(0));
            }
        }return result;
    });
}
application::StorageStatus SqliteAccountRepository::persist_private_chat_usage(application::Identifier128 session,std::int64_t seconds,const std::vector<std::pair<std::string,std::int64_t>>& days){
    if(!session.valid()||!valid_daily_seconds(days,seconds)||(seconds&&!days.size()))return invalid("validate private Chat cumulative seconds");
    return impl_->write("persist private Chat elapsed seconds",384+days.size()*64,[session,seconds,days](sqlite3* db){
        SqliteStatement caller(db,"SELECT completed_utc_seconds FROM sfx_account_sessions WHERE session_id=?1","verify private Chat session");caller.bind(1,session);if(!caller.row())return SQLITE_NOTFOUND;
        SqliteStatement previous(db,"SELECT local_date,seconds FROM sfx_private_chat_seconds WHERE session_id=?1","read private Chat cumulative usage");previous.bind(1,session);std::int64_t total{};
        while(previous.row()){
            const auto found=std::find_if(days.begin(),days.end(),[&](const auto& day){return day.first==previous.text(0);});
            if(found==days.end()||found->second<previous.int64(1))return SQLITE_ABORT;
            if(previous.int64(1)>INT64_MAX-total)return SQLITE_CORRUPT;
            total+=previous.int64(1);
        }
        if(seconds<total)return SQLITE_ABORT;
        if(!caller.is_null(0))return seconds==total?SQLITE_OK:SQLITE_ABORT;
        for(const auto& day:days){SqliteStatement save(db,"INSERT INTO sfx_private_chat_seconds VALUES(?1,?2,?3) ON CONFLICT(session_id,local_date) DO UPDATE SET seconds=excluded.seconds","persist private Chat day seconds");save.bind(1,session);save.bind(2,day.first);save.bind(3,day.second);save.step();}return SQLITE_OK;
    });
}
application::StorageResult<application::ChatPreferences> SqliteAccountRepository::save_chat_preferences(const application::MessageActor& a,const application::ChatPreferences& p){
    if(p.revision<0||p.revision==INT64_MAX)return read_failure<application::ChatPreferences>(invalid("validate Chat preference revision"));
    return impl_->write_result<application::ChatPreferences>("save own Chat preferences",512,[a,p](sqlite3* db,auto& out){
        const auto actor=message_detail::actor(db,a);const auto old=chat_detail::preferences(db,a.account);if(old.revision!=p.revision)return SQLITE_ABORT;
        if(p.ghost!=old.ghost&&actor.level<actor.threshold)return SQLITE_AUTH;
        auto next=p;++next.revision;SqliteStatement q(db,"INSERT INTO sfx_chat_preferences VALUES(?1,?2,?3,?4,?5) ON CONFLICT(account_id) DO UPDATE SET revision=excluded.revision,dnd=excluded.dnd,ghost=excluded.ghost,notices=excluded.notices","save own Chat preferences");q.bind(1,a.account);q.bind(2,next.revision);q.bind(3,next.dnd);q.bind(4,next.ghost);q.bind(5,next.notices);q.step();out=next;return SQLITE_OK;
    });
}
application::StorageResult<application::MenuDoorCommands> SqliteAccountRepository::menu_door_commands(const application::MessageActor& a,application::MenuId menu){
    return impl_->readers_.read<application::MenuDoorCommands>("read authorized menu Door commands",[a,menu](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto authority=door_detail::authority(db,a);
        const auto config=chat_detail::config(db);const auto catalog=door_detail::catalog(db);
        application::MenuDoorCommands result{config.revision,catalog.revision,{}};
        for(const auto& command:config.commands){
            if(command.menu!=menu||!command.enabled||!application::available_door_command_key(menu,command.key)||!application::meets_door_requirements(command.access,authority))continue;
            const auto door=application::find_door(catalog,command.door);
            if(door&&!door->event_only&&application::door_accessible(*door,authority))result.commands.push_back({command.key,command.name});
        }
        return result;
    });
}
application::StorageResult<application::DoorLaunch> SqliteAccountRepository::menu_door_launch(const application::MessageActor& a,const application::MenuDoorSelection& selection){
    if(!application::available_door_command_key(selection.menu,selection.key)||selection.revision<1||selection.door_revision<1)return read_failure<application::DoorLaunch>(invalid("validate menu Door selection"));
    return impl_->readers_.read<application::DoorLaunch>("authorize exact menu Door launch",[a,selection](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto authority=door_detail::authority(db,a);
        const auto config=chat_detail::config(db);const auto revision=door_detail::revision(db);
        if(config.revision!=selection.revision||revision!=selection.door_revision)file_detail::conflict();
        for(const auto& command:config.commands)if(command.menu==selection.menu&&command.key==selection.key){
            if(!command.enabled||!application::meets_door_requirements(command.access,authority))file_detail::unavailable();
            const auto door=door_detail::definition(db,command.door);
            if(door.event_only||!application::door_accessible(door,authority))file_detail::unavailable();
            if(!application::door_executable(door,authority))door_detail::clearance();
            return application::DoorLaunch{revision,door,{0,std::string(application::menu_definition(selection.menu).title)},{}};
        }
        file_detail::unavailable();return application::DoorLaunch{};
    });
}
