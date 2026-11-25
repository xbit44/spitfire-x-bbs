namespace message_detail {
app::MessageArea current_area(sqlite3* db,const app::MessageActor& caller,app::LocalEchoId id){
    const auto a=actor(db,caller);echo(db,id,a,true,false);
    SqliteStatement q(db,"SELECT g.group_id,g.name,g.display_order,g.revision,g.access_minimum,e.conference_number,e.name,e.display_order,e.revision,e.access_minimum,e.reading_minimum,e.posting_minimum FROM sfx_local_echoes e JOIN sfx_message_groups g ON g.group_id=e.group_id WHERE e.echo_id=?1","load stable selected echo");q.bind(1,id.value);if(!q.row())unavailable();
    app::MessageArea result;result.group={{q.int64(0)},q.text(1),q.int64(2),q.int64(3),static_cast<std::uint16_t>(q.int64(4)),true};
    result.echo={id,result.group.id,visible_conference_number(db,id,a),q.text(6),q.int64(7),q.int64(8),{static_cast<std::uint16_t>(q.int64(9)),static_cast<std::uint16_t>(q.int64(10)),static_cast<std::uint16_t>(q.int64(11))},true};result.configuration_revision=catalog_revision(db);return result;
}
// Addressed-only identity work. No All Callers broadcast, header/body copies,
// hidden totals or notification-driven read updates.
std::string arrival_sql(bool personal){
    std::string sql=" FROM sfx_message_arrival_intents i JOIN sfx_active_local_messages m ON m.message_id=i.message_id JOIN sfx_message_recipient_state r ON r.message_id=m.message_id LEFT JOIN sfx_local_echoes e ON e.echo_id=m.echo_id LEFT JOIN sfx_message_groups g ON g.group_id=e.group_id LEFT JOIN sfx_caller_echo_state p ON p.echo_id=m.echo_id AND p.account_id=?1 WHERE i.recipient_account_id=?1 AND r.opened=0 AND ((m.kind IN (1,2) AND r.inbox_removed=0) OR (m.kind=0 AND e.enabled=1 AND g.enabled=1 AND e.access_minimum<=?2 AND e.reading_minimum<=?2 AND g.access_minimum<=?2";
    if(personal)sql+=" AND coalesce(p.to_you_enabled,1)=1";
    return sql+="))";
}
}
application::StorageResult<std::optional<application::MessageArea>> SqliteAccountRepository::saved_message_area(const application::MessageActor& caller){
    return impl_->readers_.read<std::optional<application::MessageArea>>("restore own message location",[caller](sqlite3* db)->std::optional<application::MessageArea>{
        message_detail::ReadSnapshot snapshot(db);message_detail::actor(db,caller);
        SqliteStatement version(db,"PRAGMA user_version","read location schema");if(!version.row()||version.int64(0)<27)return {};
        SqliteStatement q(db,"SELECT echo_id FROM sfx_caller_message_location WHERE account_id=?1","read own stable conference");q.bind(1,caller.account);if(!q.row())return {};
        try{return message_detail::current_area(db,caller,{q.int64(0)});}catch(const StorageFailure& error){if(error.code()==application::StorageErrorCode::not_found||error.code()==application::StorageErrorCode::permission_denied)return {};throw;}
    });
}
application::StorageStatus SqliteAccountRepository::remember_message_area(const application::MessageActor& caller,application::LocalEchoId id){
    return impl_->write("remember own selected conference",512,[caller,id](sqlite3* db){
        message_detail::current_area(db,caller,id);
        SqliteStatement q(db,"INSERT INTO sfx_caller_message_location VALUES(?1,?2) ON CONFLICT(account_id) DO UPDATE SET echo_id=excluded.echo_id WHERE echo_id<>excluded.echo_id","remember successful stable selection only");q.bind(1,caller.account);q.bind(2,id.value);q.step();return SQLITE_OK;
    });
}

application::StorageResult<application::MessageArea> SqliteAccountRepository::message_area(const application::MessageActor& caller,application::LocalEchoId id){
    return impl_->readers_.read<application::MessageArea>("refresh stable selected echo",[caller,id](sqlite3* db){message_detail::ReadSnapshot snapshot(db);return message_detail::current_area(db,caller,id);});
}
application::StorageResult<application::MessageCallerStatistics> SqliteAccountRepository::message_caller_statistics(const application::MessageActor& caller,std::optional<application::LocalEchoId> selected){
    return impl_->readers_.read<application::MessageCallerStatistics>("read caller-owned message counters",[caller,selected](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto a=message_detail::actor(db,caller);application::MessageCallerStatistics result;
        SqliteStatement q(db,message_detail::active_message_sql(db,"SELECT coalesce((SELECT messages_entered FROM sfx_caller_message_statistics WHERE account_id=?1),0),(SELECT count(*) FROM sfx_active_local_messages m JOIN sfx_message_recipient_state r ON r.message_id=m.message_id WHERE m.kind=1 AND m.recipient_account_id=?1 AND r.inbox_removed=0),(SELECT count(*) FROM sfx_active_local_messages m JOIN sfx_message_recipient_state r ON r.message_id=m.message_id WHERE m.kind=1 AND m.recipient_account_id=?1 AND r.inbox_removed=0 AND r.opened=0),last_arrival_ordinal FROM sfx_message_counters WHERE singleton_id=1").c_str(),"read authoritative own counters");q.bind(1,caller.account);if(!q.row())message_detail::unavailable();
        result.messages_entered=q.int64(0);result.mail_total=q.int64(1);result.mail_unread=q.int64(2);result.last_arrival=q.int64(3);
        if(selected){
            const auto area=message_detail::current_area(db,caller,*selected);
            if(area.echo.permissions.reading<=a.level){
                const auto pref=message_detail::read_preference(db,caller,*selected);
                SqliteStatement counts(db,message_detail::active_message_sql(db,"SELECT count(*),coalesce(sum((?3=1 AND m.area_number>?4) OR (?5=1 AND m.recipient_account_id=?2 AND r.opened=0)),0) FROM sfx_active_local_messages m LEFT JOIN sfx_message_recipient_state r ON r.message_id=m.message_id WHERE m.kind=0 AND m.echo_id=?1").c_str(),"count selected readable echo");counts.bind(1,selected->value);counts.bind(2,caller.account);counts.bind(3,pref.unread);counts.bind(4,pref.highest_opened);counts.bind(5,pref.to_you);if(!counts.row())message_detail::unavailable();result.echo_count=counts.int64(0);result.new_messages=counts.int64(1);
            }
        }
        return result;
    });
}
application::StorageResult<application::MessageArrivalPage> SqliteAccountRepository::message_arrival_page(const application::MessageActor& caller,std::int64_t after,std::size_t limit){
    if(after<0 || !limit || limit>application::kMessagePageMaximum)return read_failure<application::MessageArrivalPage>(invalid("validate arrival continuation"));
    return impl_->readers_.read<application::MessageArrivalPage>("dispatch bounded own message arrivals",[caller,after,limit](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto a=message_detail::actor(db,caller);
        const auto sql="SELECT m.message_id,m.arrival_ordinal"+message_detail::arrival_sql(false)+" AND m.arrival_ordinal>?3 AND m.arrival_ordinal<=?5 ORDER BY m.arrival_ordinal LIMIT ?4";
        SqliteStatement bounds(db,"SELECT last_arrival_ordinal FROM sfx_message_counters WHERE singleton_id=1","capture arrival dispatch fence");if(!bounds.row())message_detail::unavailable();application::MessageArrivalPage result;result.fence=bounds.int64(0);
        SqliteStatement q(db,message_detail::active_message_sql(db,sql).c_str(),"read addressed arrival identities");q.bind(1,caller.account);q.bind(2,a.level);q.bind(3,after);q.bind(4,static_cast<std::int64_t>(limit+1));q.bind(5,result.fence);
        while(q.row()){if(result.arrivals.size()==limit){result.next_after=result.arrivals.back().ordinal;break;}result.arrivals.push_back({{q.int64(0)},caller.account,q.int64(1)});}
        return result;
    });
}
application::StorageResult<bool> SqliteAccountRepository::message_arrival_eligible(const application::MessageActor& caller,application::MessageId id){
    return impl_->readers_.read<bool>("recheck own arrival eligibility",[caller,id](sqlite3* db){message_detail::ReadSnapshot snapshot(db);const auto a=message_detail::actor(db,caller);const auto sql="SELECT 1"+message_detail::arrival_sql(false)+" AND m.message_id=?3 LIMIT 1";SqliteStatement q(db,message_detail::active_message_sql(db,sql).c_str(),"recheck direct unread recipient");q.bind(1,caller.account);q.bind(2,a.level);q.bind(3,id.value);return q.row();});
}
application::StorageResult<std::optional<std::string>> SqliteAccountRepository::message_arrival_sender(const application::MessageActor& caller,application::MessageId id){
    return impl_->readers_.read<std::optional<std::string>>("read current own arrival sender",[caller,id](sqlite3* db)->std::optional<std::string>{
        message_detail::ReadSnapshot snapshot(db);const auto a=message_detail::actor(db,caller);
        const auto sql="SELECT coalesce((SELECT fm.from_name FROM sfx_ftn_message_metadata fm WHERE fm.message_id=m.message_id),(SELECT author.display_name FROM sfx_accounts author WHERE author.account_id=m.author_account_id),'Removed caller')"+message_detail::arrival_sql(false)+" AND m.message_id=?3 LIMIT 1";
        SqliteStatement q(db,message_detail::active_message_sql(db,sql).c_str(),"recheck recipient before sender notification");q.bind(1,caller.account);q.bind(2,a.level);q.bind(3,id.value);if(!q.row())return {};return q.text(0);
    });
}
application::StorageResult<bool> SqliteAccountRepository::has_addressed_unread(const application::MessageActor& caller,bool personal){
    return impl_->readers_.read<bool>("check qualifying addressed unread witness",[caller,personal](sqlite3* db){message_detail::ReadSnapshot snapshot(db);const auto a=message_detail::actor(db,caller);const auto sql="SELECT 1"+message_detail::arrival_sql(personal)+" LIMIT 1";SqliteStatement q(db,message_detail::active_message_sql(db,sql).c_str(),"check sanitized addressed notice");q.bind(1,caller.account);q.bind(2,a.level);return q.row();});
}
application::StorageResult<application::MessageLoginScanSummary> SqliteAccountRepository::login_message_scan_summary(const application::MessageActor& caller,bool include_authored_echoes){
    return impl_->readers_.read<application::MessageLoginScanSummary>("capture authorized personal scan summary",[caller,include_authored_echoes](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto a=message_detail::actor(db,caller);
        application::MessageLoginScanSummary result;
        result.query.options.mode=application::MessageQueryMode::personal;
        result.query.options.scope=application::MessageQueryScope::all_accessible;
        result.query.options.unread_only=true;result.query.owner=caller.account;
        SqliteStatement fence(db,"SELECT last_arrival_ordinal,last_read_sequence FROM sfx_message_counters WHERE singleton_id=1","capture login fences");
        if(!fence.row())message_detail::unavailable();
        result.query.arrival_fence=fence.int64(0);result.query.recipient_read_fence=fence.int64(1);result.query.configuration_revision=message_detail::catalog_revision(db);
        // Summary candidates: readable To-you-enabled public areas plus own
        // received inbox and own sent mail. A self-addressed item counts once.
        // All Callers posts contribute only to review totals, never login opens.
        SqliteStatement q(db,message_detail::active_message_sql(db,"SELECT count(*),coalesce(sum(m.recipient_account_id=?1 AND coalesce(r.inbox_removed,0)=0 AND (m.kind<>0 OR coalesce(p.to_you_enabled,1)=1) AND r.opened=0),0),coalesce(sum(m.recipient_account_id=?1 AND coalesce(r.inbox_removed,0)=0 AND (m.kind<>0 OR coalesce(p.to_you_enabled,1)=1) AND r.opened=1),0),coalesce(sum(m.author_account_id=?1),0) FROM sfx_active_local_messages m LEFT JOIN sfx_message_recipient_state r ON r.message_id=m.message_id LEFT JOIN sfx_local_echoes e ON e.echo_id=m.echo_id LEFT JOIN sfx_message_groups g ON g.group_id=e.group_id LEFT JOIN sfx_caller_echo_state p ON p.echo_id=m.echo_id AND p.account_id=?1 WHERE m.arrival_ordinal<=?3 AND ((m.kind IN(1,2) AND (m.author_account_id=?1 OR (m.kind=1 AND m.recipient_account_id=?1 AND r.inbox_removed=0))) OR (m.kind=0 AND e.enabled=1 AND g.enabled=1 AND e.access_minimum<=?2 AND e.reading_minimum<=?2 AND g.access_minimum<=?2 AND (coalesce(p.to_you_enabled,1)=1 OR (?4=1 AND m.author_account_id=?1))))").c_str(),"count caller-visible scan candidates");
        q.bind(1,caller.account);q.bind(2,a.level);q.bind(3,result.query.arrival_fence);q.bind(4,static_cast<std::int64_t>(include_authored_echoes));if(!q.row())message_detail::unavailable();
        result.scanned=q.int64(0);result.unread_to_you=q.int64(1);result.read_to_you=q.int64(2);result.from_you=q.int64(3);return result;
    });
}
