application::StorageResult<application::OtherBoardPage> SqliteAccountRepository::other_boards(std::int64_t after) {
    if(after<0 || after>99999) return read_failure<application::OtherBoardPage>(invalid("validate Other BBS page"));
    if(impl_->other_board_text_)return impl_->other_board_text_->read(after);
    return impl_->readers_.read<application::OtherBoardPage>("read Other BBS page",[after](sqlite3* db) {
        SqliteStatement version(db,"PRAGMA user_version","read catalog storage contract");
        if(version.row() && version.int64(0)>=7)return application::OtherBoardPage{};
        SqliteStatement query(db,"SELECT board_id,revision,name,telnet,ssh FROM sfx_other_boards WHERE removed=0 AND board_id>?1 ORDER BY board_id LIMIT 33","list Other BBS");
        query.bind(1,after);application::OtherBoardPage result;
        while(query.row()) {
            if(result.entries.size()==application::kOtherBoardPageSize) {result.has_more=true;break;}
            application::OtherBoardRecord row{query.int64(0),query.int64(1),{query.text(2),query.text(3),query.text(4)}};
            if(row.id<1 || row.revision<1 || !application::valid_other_board_values(row.values)) throw StorageFailure(StorageErrorCode::corrupt,"invalid Other BBS stored values",SQLITE_CORRUPT);
            result.entries.push_back(std::move(row));
        }
        return result;
    });
}
application::StorageResult<application::OpeningDateRecord> SqliteAccountRepository::opening_date() {
    return impl_->readers_.read<application::OpeningDateRecord>("read BBS opening date",[](sqlite3* db) {
        SqliteStatement query(db,"SELECT revision,initialization_utc,configured_date FROM sfx_bbs_opening_date WHERE singleton_id=1","read opening date");
        if(!query.row()) throw StorageFailure(StorageErrorCode::corrupt,"opening date absent",SQLITE_CORRUPT);
        application::OpeningDateRecord value{query.int64(0),query.int64(1),{}};
        if(!query.is_null(2)) value.configured_date=query.text(2);
        if(value.revision<1 || !application::valid_utc_seconds(value.initialization_utc) ||
            (value.configured_date && !application::valid_iso_civil_date(*value.configured_date))) throw StorageFailure(StorageErrorCode::corrupt,"opening date invalid",SQLITE_CORRUPT);
        return value;
    });
}
StorageStatus SqliteAccountRepository::change_classic_information(const application::ClassicInformationChange& change,const application::ClassicInformationActor& actor) {
    using Action=application::ClassicInformationAction;
    if(!application::valid_classic_information_change(change) || !application::valid_utc_seconds(actor.now_utc) || !actor.event_id.valid() ||
        (actor.installation_owner?(actor.caller!=0 || actor.caller_revision!=0):
            ((change.action!=Action::add_board && change.action!=Action::remove_board) || actor.caller<=0 || actor.caller_revision<=0))) return invalid("validate classic information authority/change");
    auto text_log=std::make_shared<std::optional<application::OtherBoardLogRecord>>();
    const auto text_store=impl_->other_board_text_;
    const auto status=impl_->write("change classic information",2048,[change,actor,text_store,text_log](sqlite3* db) {
        std::string actor_name="Installation owner";
        if(!actor.installation_owner) {
            // Authority is read again while the writer serializes account changes
            // and text publication. A menu snapshot cannot grant this privilege.
            SqliteStatement active(db,"SELECT security_level,display_name FROM sfx_accounts WHERE account_id=?1 AND revision=?2 AND account_state='active'","verify authenticated caller");
            active.bind(1,actor.caller);active.bind(2,actor.caller_revision);
            if(!active.row() || (change.action==Action::remove_board && active.int64(0)!=999)) return SQLITE_ABORT;
            actor_name=active.text(1);
        }
        if(text_store && change.action!=Action::opening_date) {
            const auto changed=text_store->change(change,actor,actor_name);
            if(!changed)return changed.status.code==StorageErrorCode::conflict?SQLITE_ABORT:SQLITE_IOERR;
            *text_log=changed.value;return SQLITE_OK;
        }
        if(change.action!=Action::opening_date) {
            SqliteStatement version(db,"PRAGMA user_version","require text catalog backend");
            if(version.row() && version.int64(0)>=7)return SQLITE_IOERR;
        }
        SqliteStatement capacity(db,"SELECT count(*) FROM sfx_classic_information_audit","bound classic audit");
        if(!capacity.row() || capacity.int64(0)>=1000000) return SQLITE_FULL;
        std::int64_t id=change.id,revision=change.expected_revision+1;
        if(change.action==Action::add_board) {
            SqliteStatement count(db,"SELECT coalesce(max(board_id),0),coalesce(sum(removed=0),0) FROM sfx_other_boards","bound Other BBS");
            if(!count.row() || count.int64(0)>=99999 || count.int64(1)>=static_cast<std::int64_t>(application::kOtherBoardMaximum)) return SQLITE_FULL;
            id=count.int64(0)+1;revision=1;
            SqliteStatement save(db,"INSERT INTO sfx_other_boards VALUES(?1,1,?2,?3,?4,0)","add Other BBS");
            save.bind(1,id);save.bind(2,change.values.name);save.bind(3,change.values.telnet);save.bind(4,change.values.ssh);save.step();
        } else if(change.action==Action::opening_date) {
            SqliteStatement save(db,"UPDATE sfx_bbs_opening_date SET configured_date=?1,revision=revision+1 WHERE singleton_id=1 AND revision=?2","save opening date");
            save.bind(1,change.date);save.bind(2,change.expected_revision);save.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        } else if(change.action==Action::remove_board) {
            SqliteStatement save(db,"UPDATE sfx_other_boards SET removed=1,revision=revision+1 WHERE board_id=?1 AND revision=?2 AND removed=0","remove Other BBS");
            save.bind(1,id);save.bind(2,change.expected_revision);save.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        } else {
            SqliteStatement save(db,"UPDATE sfx_other_boards SET name=?1,telnet=?2,ssh=?3,revision=revision+1 WHERE board_id=?4 AND revision=?5 AND removed=0","edit Other BBS");
            save.bind(1,change.values.name);save.bind(2,change.values.telnet);save.bind(3,change.values.ssh);save.bind(4,id);save.bind(5,change.expected_revision);save.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        }
        SqliteStatement audit(db,"INSERT INTO sfx_classic_information_audit VALUES(?1,?2,?3,?4,?5,?6,?7)","audit classic information change");
        audit.bind(1,actor.event_id);audit.bind(2,static_cast<std::int64_t>(change.action));
        if(id) audit.bind(3,id);else audit.bind_null(3);
        audit.bind(4,revision);if(actor.caller)audit.bind(5,actor.caller);else audit.bind_null(5);
        audit.bind(6,actor.installation_owner);audit.bind(7,actor.now_utc);audit.step();
        return SQLITE_OK;
    });
    if(*text_log && text_store) {
        (void)text_store->publish_log(**text_log);
        // The file is authoritative and already published. A subsequent failure
        // closing the authority transaction cannot truthfully undo that outcome.
        return StorageStatus::success();
    }
    return status;
}
application::StorageResult<application::SystemInformationCounters> SqliteAccountRepository::system_information_counters(std::int64_t begin,std::int64_t end) {
    if(!application::valid_utc_seconds(begin) || !application::valid_utc_seconds(end) || end<=begin) return read_failure<application::SystemInformationCounters>(invalid("validate system information day"));
    return impl_->readers_.read<application::SystemInformationCounters>("read system information counters",[begin,end](sqlite3* db) {
        const std::string filter=chat_statistics_supported(db)?" AND NOT EXISTS(SELECT 1 FROM sfx_chat_usage_exclusions x WHERE x.session_id=sfx_account_sessions.session_id)":"";
        const auto sql="SELECT (SELECT count(*) FROM sfx_account_sessions WHERE 1=1"+filter+"),(SELECT count(*) FROM sfx_account_sessions WHERE started_utc_seconds>=?1 AND started_utc_seconds<?2"+filter+"),(SELECT count(*) FROM sfx_accounts WHERE account_state='active')";
        SqliteStatement query(db,sql.c_str(),"read authoritative system counters");
        query.bind(1,begin);query.bind(2,end);if(!query.row())throw StorageFailure(StorageErrorCode::corrupt,"system counters unavailable",SQLITE_CORRUPT);
        return application::SystemInformationCounters{query.int64(0),query.int64(1),query.int64(2)};
    });
}
