namespace maintenance_detail {
namespace app=application;
using Outcome=app::MessageMaintenanceOutcome;
using Reason=app::MessageMaintenanceReason;
struct MaintenanceAbort { bool cancelled{},deadline{}; };

struct SqlBudget {
    sqlite3* db;
    app::MessageMaintenanceStop stop;
    std::chrono::steady_clock::time_point until;
    bool cancelled{}, expired{};
    MaintenanceAbort* abort{};
    SqlBudget(sqlite3* database,app::MessageMaintenanceStop callback,std::uint64_t milliseconds,MaintenanceAbort* outcome=nullptr)
        :db(database),stop(std::move(callback)),until(std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds)),abort(outcome){
        sqlite3_progress_handler(db,1000,[](void* context)->int{
            auto& self=*static_cast<SqlBudget*>(context);
            try{self.cancelled=self.cancelled || (self.stop && self.stop());}catch(...){self.cancelled=true;}
            self.expired=std::chrono::steady_clock::now()>=self.until;
            return self.cancelled || self.expired;
        },this);
    }
    ~SqlBudget(){sqlite3_progress_handler(db,0,nullptr,nullptr);if(abort){abort->cancelled=cancelled;abort->deadline=expired;}}
    void poll(){
        try{cancelled=cancelled || (stop && stop());}catch(...){cancelled=true;}
        expired=std::chrono::steady_clock::now()>=until;
        if(cancelled || expired)throw StorageFailure(StorageErrorCode::busy,cancelled?"message maintenance cancelled":"message maintenance deadline",SQLITE_INTERRUPT);
    }
};
app::MessageMaintenanceRun run(sqlite3* db,app::Identifier128 id,bool required=true){
    SqliteStatement q(db,"SELECT kind,started_utc_seconds,coalesce(finished_utc_seconds,0),result,removed_public,expired_unread,committed_batches,safe_reason FROM sfx_message_maintenance_runs WHERE run_id=?1","read actual message maintenance outcome");
    q.bind(1,id);if(!q.row()){if(required)message_detail::unavailable();return {};}
    app::MessageMaintenanceRun result;result.id=id;
    if(q.int64(0)<0 || q.int64(0)>1 || q.int64(3)<0 || q.int64(3)>5 || q.int64(7)<0 || q.int64(7)>10)
        throw StorageFailure(StorageErrorCode::corrupt,"invalid maintenance outcome",SQLITE_CORRUPT);
    result.kind=static_cast<app::MessageMaintenanceKind>(q.int64(0));result.started_utc=q.int64(1);result.finished_utc=q.int64(2);
    result.outcome=static_cast<Outcome>(q.int64(3));result.removed_public=q.int64(4);result.expired_unread=q.int64(5);
    result.committed_batches=q.int64(6);result.reason=static_cast<Reason>(q.int64(7));
    SqliteStatement echoes(db,"SELECT echo_id,removed,protected_overage FROM sfx_message_maintenance_echo_results WHERE run_id=?1 ORDER BY echo_id LIMIT 1000","read bounded committed per-echo outcomes");echoes.bind(1,id);
    while(echoes.row())result.echoes.push_back({{echoes.int64(0)},echoes.int64(1),echoes.int64(2)});
    if(result.echoes.size()>999)throw StorageFailure(StorageErrorCode::corrupt,"unbounded maintenance area results",SQLITE_CORRUPT);
    return result;
}
void same_owner(sqlite3* db,const app::MessageConfigurationActor& actor,app::Identifier128 id){
    message_detail::config_authority(db,actor);
    SqliteStatement q(db,"SELECT installation_owner,actor_account_id FROM sfx_message_maintenance_runs WHERE run_id=?1","verify stable maintenance operation owner");q.bind(1,id);
    if(!q.row())message_detail::unavailable();
    if(q.boolean(0)!=actor.installation_owner || (actor.installation_owner?!q.is_null(1):q.is_null(1) || !actor.caller || q.int64(1)!=actor.caller->account))message_detail::conflict();
}
void insert_run(sqlite3* db,const app::MessageConfigurationActor& actor,app::Identifier128 id,app::MessageMaintenanceKind kind,std::int64_t now){
    SqliteStatement active(db,"SELECT 1 FROM sfx_message_maintenance_runs WHERE result=0 LIMIT 1","prevent overlapping message maintenance");
    if(active.row())throw StorageFailure(StorageErrorCode::busy,"message maintenance already active",SQLITE_BUSY);
    SqliteStatement q(db,"INSERT INTO sfx_message_maintenance_runs(run_id,kind,installation_owner,actor_account_id,started_utc_seconds,finished_utc_seconds,result,removed_public,expired_unread,committed_batches,safe_reason) VALUES(?1,?2,?3,?4,?5,NULL,0,0,0,0,0)","begin message maintenance receipt");
    q.bind(1,id);q.bind(2,static_cast<std::int64_t>(kind));q.bind(3,actor.installation_owner);
    if(actor.caller)q.bind(4,actor.caller->account);else q.bind_null(4);q.bind(5,now);q.step();
}
void hold(sqlite3* db,std::int64_t scope,app::Identifier128 incident,std::int64_t now,std::int64_t findings){
    SqliteStatement q(db,"INSERT INTO sfx_message_integrity_holds VALUES(?1,?2,?3,?4) ON CONFLICT(scope) DO UPDATE SET incident_id=excluded.incident_id,occurred_utc_seconds=excluded.occurred_utc_seconds,findings=excluded.findings","persist scoped corruption hold without deleting suspect data");
    q.bind(1,scope);q.bind(2,incident);q.bind(3,now);q.bind(4,findings);q.step();
}
bool held(sqlite3* db,std::int64_t scope){
    SqliteStatement q(db,"SELECT 1 FROM sfx_message_integrity_holds WHERE scope=0 OR scope=?1 LIMIT 1","enforce current corruption hold");q.bind(1,scope);return q.row();
}
// Data-only verification, deliberately distinct from caller reading/opening.
// No terminal output, read flags, permissions bypass or private content logging.
bool message_valid(sqlite3* db,std::int64_t id){
    SqliteStatement q(db,"SELECT kind,coalesce(echo_id,0),subject,body,body_lines,arrival_ordinal,created_utc_seconds,encoding,parent_message_id,recipient_account_id,author_account_id FROM sfx_local_messages WHERE message_id=?1","validate maintenance candidate content");q.bind(1,id);
    if(!q.row())return false;
    const auto kind=q.int64(0),echo=q.int64(1),ordinal=q.int64(5);
    if((kind<0 || kind>2) || (kind==0?echo<=0:echo!=0) || ordinal<1 ||
       !app::valid_utc_seconds(q.int64(6)) || q.text(7)!="cp437" ||
       app::validate_message_subject(message_detail::bytes(q,2),true)!=app::MessageValidation::valid)return false;
    if(kind!=0 && message_detail::permanent_flag(db,{id}))return false;
    {SqliteStatement state(db,"SELECT recipient_account_id FROM sfx_message_recipient_state WHERE message_id=?1","check exact recipient-state relationship");state.bind(1,id);
        if(q.is_null(9)){if(state.row())return false;}
        else if(!state.row() || state.int64(0)!=q.int64(9))return false;}
    if(q.is_null(10)){
        if(kind==2)return false;
        SqliteStatement receipt(db,"SELECT 1 FROM sfx_ftn_import_receipts r JOIN sfx_ftn_message_metadata m ON m.import_id=r.import_id AND m.message_id=r.original_message_id WHERE r.original_message_id=?1 AND NOT EXISTS(SELECT 1 FROM sfx_message_commit_receipts WHERE message_id=?1)","check immutable remote import creation identity");receipt.bind(1,id);if(!receipt.row())return false;
    }else{
        SqliteStatement receipt(db,"SELECT 1 FROM sfx_message_commit_receipts WHERE message_id=?1 AND actor_account_id=?2 AND NOT EXISTS(SELECT 1 FROM sfx_ftn_message_metadata WHERE message_id=?1)","check legitimate local posting identity");receipt.bind(1,id);receipt.bind(2,q.int64(10));if(!receipt.row())return false;
        if(kind==2){SqliteStatement target(db,"SELECT 1 FROM sfx_ftn_message_targets t JOIN sfx_message_commit_receipts r USING(operation_id) WHERE t.message_id=?1 AND r.message_id=t.message_id","check bound outgoing FTN target and posting receipt");target.bind(1,id);if(!q.is_null(9)||!target.row())return false;}
    }
    if(!q.is_null(8)){
        SqliteStatement parent(db,"SELECT kind,coalesce(echo_id,0),arrival_ordinal,author_account_id FROM sfx_local_messages WHERE message_id=?1","verify stable independent reply relationship");parent.bind(1,q.int64(8));
        if(!parent.row() || parent.int64(2)>=ordinal || !message_detail::reply_edge_valid(db,{id},{q.int64(8)}))return false;
    }
    SqliteStatement source(db,"SELECT encoding,format,profile_version,source,source_sha256,extent_off,extent_on,charged_extent,visible_off,visible_on FROM sfx_message_sources WHERE message_id=?1","verify inert candidate source");source.bind(1,id);
    if(source.row()){
        if(source.int64(1)<1 || source.int64(1)>2 || source.int64(2)!=1)return false;
        app::MessageSource original{static_cast<app::MessageSourceFormat>(source.int64(1)),1,message_detail::bytes(source,3)};
        const auto derived=app::validate_message_source(original,app::kMaximumMessageLines);
        const auto hash=message_detail::source_digest(original.bytes);const auto stored=source.blob(4);
        return source.text(0)=="cp437" && derived && stored.size()==32 && sodium_memcmp(hash.data(),stored.data(),32)==0 &&
            derived.off.extent==source.int64(5) && derived.on.extent==source.int64(6) && derived.charged_extent==source.int64(7) &&
            message_detail::visible(derived.off)==message_detail::bytes(source,8) && message_detail::visible(derived.on)==message_detail::bytes(source,9) &&
            message_detail::bytes(q,3)==derived.off.text.canonical_lf && q.int64(4)==derived.off.extent;
    }
    const auto body=app::normalize_message_body(message_detail::bytes(q,3),app::kMaximumMessageLines);
    return body && static_cast<std::int64_t>(body.rows.size())==q.int64(4) && app::validate_message_save(message_detail::bytes(q,2),body)==app::MessageValidation::valid;
}
void remove_public(sqlite3* db,std::int64_t message){
    if(message_detail::permanent_flag(db,{message}))message_detail::conflict();
    const auto statement=[&](const char* sql){SqliteStatement q(db,sql,"remove only eligible public message dependencies");q.bind(1,message);q.step();};
    statement("INSERT INTO sfx_message_delete_scope VALUES(?1)");
    statement("DELETE FROM sfx_ftn_message_intents WHERE message_id=?1 AND state=1");
    statement("INSERT INTO sfx_message_purged_receipts SELECT operation_id,actor_account_id,request_fingerprint,message_id FROM sfx_message_commit_receipts WHERE message_id=?1");
    statement("UPDATE sfx_local_messages SET parent_message_id=NULL WHERE parent_message_id=?1");
    statement("DELETE FROM sfx_message_arrival_intents WHERE message_id=?1");
    statement("DELETE FROM sfx_message_recipient_state WHERE message_id=?1");
    statement("DELETE FROM sfx_message_sources WHERE message_id=?1");
    statement("DELETE FROM sfx_ftn_message_metadata WHERE message_id=?1");
    statement("DELETE FROM sfx_ftn_message_targets WHERE message_id=?1");
    statement("DELETE FROM sfx_message_commit_receipts WHERE message_id=?1");
    statement("DELETE FROM sfx_local_messages WHERE message_id=?1 AND kind=0");
    if(sqlite3_changes(db)!=1)message_detail::conflict();
    statement("DELETE FROM sfx_message_delete_scope WHERE message_id=?1");
}
void echo_result(sqlite3* db,app::Identifier128 run,std::int64_t echo,std::int64_t removed,std::int64_t overage){
    SqliteStatement q(db,"INSERT INTO sfx_message_maintenance_echo_results VALUES(?1,?2,?3,?4) ON CONFLICT(run_id,echo_id) DO UPDATE SET removed=removed+excluded.removed,protected_overage=excluded.protected_overage","record actual per-echo committed removal");
    q.bind(1,run);q.bind(2,echo);q.bind(3,removed);q.bind(4,overage);q.step();
}
void complete_area(sqlite3* db,app::Identifier128 id,std::int64_t echo){
    SqliteStatement q(db,"UPDATE sfx_message_maintenance_plan SET completed=1 WHERE run_id=?1 AND echo_id=?2","finish only captured retention area");q.bind(1,id);q.bind(2,echo);q.step();
}
void prune_details(sqlite3* db,app::MessageMaintenanceKind kind){
    // Keep minimal completed-run receipts so old tokens never start a new purge.
    // Bounded detailed history/plan/progress is not another content store.
    for(const auto* table:{"sfx_message_maintenance_plan","sfx_message_maintenance_progress","sfx_message_maintenance_echo_results"}){
        const std::string sql=std::string("DELETE FROM ")+table+" WHERE run_id IN (SELECT run_id FROM sfx_message_maintenance_runs WHERE kind=?1 AND result<>0 ORDER BY started_utc_seconds DESC,run_id DESC LIMIT -1 OFFSET 32)";
        SqliteStatement q(db,sql.c_str(),"retain only bounded completed maintenance details");q.bind(1,static_cast<std::int64_t>(kind));q.step();
    }
}
} // namespace maintenance_detail

application::StorageResult<application::MessageMaintenanceRun> SqliteAccountRepository::begin_message_retention(
    const application::MessageConfigurationActor& actor,application::Identifier128 id){
    using Run=application::MessageMaintenanceRun;
    if(!id.valid())return read_failure<Run>(invalid("validate retention operation"));
    const auto now=impl_->message_utc_now_?impl_->message_utc_now_():std::int64_t{};
    if(!application::valid_utc_seconds(now))return read_failure<Run>(invalid("retention clock"));
    return impl_->write_result<Run>("begin bounded message retention",65536,[actor,id,now](sqlite3* db,std::optional<Run>& out){
        maintenance_detail::SqlBudget budget(db,{},application::kMessageRetentionSqlBudgetMillis);budget.poll();
        message_detail::config_authority(db,actor);
        auto previous=maintenance_detail::run(db,id,false);if(previous.id.valid()){
            maintenance_detail::same_owner(db,actor,id);if(previous.kind!=application::MessageMaintenanceKind::retention)message_detail::conflict();
            previous.replayed=true;out=std::move(previous);return SQLITE_OK;
        }
        maintenance_detail::insert_run(db,actor,id,application::MessageMaintenanceKind::retention,now);
        SqliteStatement echoes(db,"SELECT e.echo_id,e.revision,p.maximum_count,p.maximum_age_days FROM sfx_local_echoes e JOIN sfx_echo_retention p USING(echo_id) ORDER BY e.echo_id LIMIT 1000","capture bounded retention policy, not message ID list");
        std::size_t count{};while(echoes.row()){
            budget.poll();
            if(++count>999)throw StorageFailure(StorageErrorCode::corrupt,"too many maintenance echoes",SQLITE_CORRUPT);
            const auto limit=message_detail::retention_value(echoes,2,application::kMaximumRetainedMessageCount),age=message_detail::retention_value(echoes,3,application::kMaximumMessageAgeDays);
            SqliteStatement q(db,"INSERT INTO sfx_message_maintenance_plan VALUES(?1,?2,?3,?4,?5,0)","capture next-run-only echo policy");q.bind(1,id);q.bind(2,echoes.int64(0));q.bind(3,echoes.int64(1));
            if(limit)q.bind(4,static_cast<std::int64_t>(*limit));else q.bind_null(4);if(age)q.bind(5,static_cast<std::int64_t>(*age));else q.bind_null(5);q.step();
        }
        SqliteStatement policy(db,"SELECT maximum_age_days,revision FROM sfx_unread_mail_retention WHERE singleton_id=1","capture unread retention choice");
        if(!policy.row())throw StorageFailure(StorageErrorCode::corrupt,"unread policy missing",SQLITE_CORRUPT);
        const auto age=message_detail::retention_value(policy,0,application::kMaximumMessageAgeDays);
        SqliteStatement q(db,"INSERT INTO sfx_message_maintenance_progress VALUES(?1,(SELECT last_arrival_ordinal FROM sfx_message_counters WHERE singleton_id=1),?2,?3,0,0,0,0,0,0,0)","capture durable retention fence/progress");
        q.bind(1,id);if(age)q.bind(2,static_cast<std::int64_t>(*age));else q.bind_null(2);q.bind(3,policy.int64(1));q.step();
        out=maintenance_detail::run(db,id);return SQLITE_OK;
    });
}

application::StorageResult<application::MessageRetentionBatch> SqliteAccountRepository::message_retention_batch(
    const application::MessageConfigurationActor& actor,application::Identifier128 id,std::int64_t expected,application::MessageMaintenanceStop stop){
    using Batch=application::MessageRetentionBatch;using Reason=application::MessageMaintenanceReason;
    if(!id.valid() || expected<1)return read_failure<Batch>(invalid("validate retention batch sequence"));
    auto abort=std::make_shared<maintenance_detail::MaintenanceAbort>();
    auto completed=impl_->write_result<Batch>("bounded public/unread-mail retention batch",32768,[actor,id,expected,abort,stop=std::move(stop)](sqlite3* db,std::optional<Batch>& out){
        maintenance_detail::same_owner(db,actor,id);auto run=maintenance_detail::run(db,id);
        if(run.kind!=application::MessageMaintenanceKind::retention)message_detail::conflict();
        SqliteStatement progress(db,"SELECT arrival_fence,unread_age_days,unread_revision,private_cursor,private_done,last_sequence,last_public,last_private,last_complete,policy_changed FROM sfx_message_maintenance_progress WHERE run_id=?1","read last committed retention batch");progress.bind(1,id);
        if(!progress.row())message_detail::conflict();
        const auto sequence=progress.int64(5);
        if(expected==sequence){out=Batch{run,sequence,progress.int64(6),progress.int64(7),progress.boolean(8),true,progress.boolean(9)};return SQLITE_OK;}
        if(sequence==INT64_MAX || expected!=sequence+1 || run.outcome!=application::MessageMaintenanceOutcome::running)message_detail::conflict();
        maintenance_detail::SqlBudget budget(db,stop,application::kMessageRetentionSqlBudgetMillis,abort.get());budget.poll();
        Batch result;result.sequence=expected;result.policy_changed=progress.boolean(9);
        const auto fence=progress.int64(0);auto private_cursor=progress.int64(3);bool private_done=progress.boolean(4);
        SqliteStatement area(db,"SELECT echo_id,expected_revision,maximum_count,maximum_age_days FROM sfx_message_maintenance_plan WHERE run_id=?1 AND completed=0 ORDER BY echo_id LIMIT 1","select next captured retention area");area.bind(1,id);
        if(area.row()){
            const auto echo=area.int64(0),revision=area.int64(1);
            const auto maximum=message_detail::retention_value(area,2,application::kMaximumRetainedMessageCount),days=message_detail::retention_value(area,3,application::kMaximumMessageAgeDays);
            SqliteStatement current(db,"SELECT revision FROM sfx_local_echoes WHERE echo_id=?1","revalidate current captured echo identity");current.bind(1,echo);
            if(!current.row() || current.int64(0)!=revision){result.policy_changed=true;maintenance_detail::complete_area(db,id,echo);}
            else if(maintenance_detail::held(db,echo)){run.reason=Reason::corruption_hold;maintenance_detail::complete_area(db,id,echo);}
            else{
                SqliteStatement count(db,"SELECT count(*),coalesce(sum(EXISTS(SELECT 1 FROM sfx_message_permanent p WHERE p.message_id=m.message_id AND p.permanent=1) OR EXISTS(SELECT 1 FROM sfx_ftn_message_intents f WHERE f.message_id=m.message_id AND f.state<>1)),0) FROM sfx_local_messages m WHERE m.echo_id=?1 AND m.kind=0","measure retained total including Permanent and outstanding FTN messages");count.bind(1,echo);if(!count.row())message_detail::conflict();
                const auto total=count.int64(0),permanent=count.int64(1),excess=maximum?std::max<std::int64_t>(0,total-*maximum):0;
                const auto overage=maximum?std::max<std::int64_t>(0,permanent-*maximum):0;
                std::map<std::pair<std::int64_t,std::int64_t>,std::int64_t> candidates;
                const auto select=[&](bool age){
                    SqliteStatement q(db,age?
                        "SELECT m.arrival_ordinal,m.message_id FROM sfx_local_messages m WHERE m.kind=0 AND m.echo_id=?1 AND m.arrival_ordinal<=?2 AND m.created_utc_seconds<=?3 AND NOT EXISTS(SELECT 1 FROM sfx_message_permanent p WHERE p.message_id=m.message_id AND p.permanent=1) AND NOT EXISTS(SELECT 1 FROM sfx_ftn_message_intents f WHERE f.message_id=m.message_id AND f.state<>1) ORDER BY m.arrival_ordinal,m.message_id LIMIT ?4":
                        "SELECT m.arrival_ordinal,m.message_id FROM sfx_local_messages m WHERE m.kind=0 AND m.echo_id=?1 AND m.arrival_ordinal<=?2 AND NOT EXISTS(SELECT 1 FROM sfx_message_permanent p WHERE p.message_id=m.message_id AND p.permanent=1) AND NOT EXISTS(SELECT 1 FROM sfx_ftn_message_intents f WHERE f.message_id=m.message_id AND f.state<>1) ORDER BY m.arrival_ordinal,m.message_id LIMIT ?3","select bounded independent oldest/count and full-day candidates");
                    q.bind(1,echo);q.bind(2,fence);
                    if(age){q.bind(3,run.started_utc-static_cast<std::int64_t>(*days)*86400);q.bind(4,static_cast<std::int64_t>(application::kMessageRetentionBatchLimit));}
                    else q.bind(3,std::min<std::int64_t>(excess,static_cast<std::int64_t>(application::kMessageRetentionBatchLimit)));
                    while(q.row())candidates.emplace(std::make_pair(q.int64(0),q.int64(1)),q.int64(1));
                };
                if(excess)select(false);
                if(days)select(true);
                while(candidates.size()>application::kMessageRetentionBatchLimit)candidates.erase(std::prev(candidates.end()));
                bool clean=true;for(const auto& entry:candidates){budget.poll();if(!maintenance_detail::message_valid(db,entry.second)){clean=false;break;}}
                if(!clean){maintenance_detail::hold(db,echo,id,run.started_utc,1);run.reason=Reason::corrupt;maintenance_detail::complete_area(db,id,echo);}
                else{for(const auto& entry:candidates){budget.poll();maintenance_detail::remove_public(db,entry.second);++result.removed_public;}
                    if(candidates.empty())maintenance_detail::complete_area(db,id,echo);}
                maintenance_detail::echo_result(db,id,echo,result.removed_public,overage);
            }
        }else if(!private_done){
            const auto days=message_detail::retention_value(progress,1,application::kMaximumMessageAgeDays);
            SqliteStatement current(db,"SELECT revision FROM sfx_unread_mail_retention WHERE singleton_id=1","revalidate captured unread expiry policy");
            if(!current.row() || current.int64(0)!=progress.int64(2)){result.policy_changed=true;private_done=true;}
            else if(maintenance_detail::held(db,-1)){run.reason=Reason::corruption_hold;private_done=true;}
            else if(!days)private_done=true;
            else{
                SqliteStatement q(db,"SELECT m.message_id FROM sfx_local_messages m JOIN sfx_message_recipient_state r USING(message_id) WHERE m.kind=1 AND r.opened=0 AND r.inbox_removed=0 AND m.created_utc_seconds<=?1 AND m.arrival_ordinal<=?2 AND m.message_id>?3 ORDER BY m.message_id LIMIT ?4","select bounded unread recipient inbox expiry only");
                q.bind(1,run.started_utc-static_cast<std::int64_t>(*days)*86400);q.bind(2,fence);q.bind(3,private_cursor);q.bind(4,static_cast<std::int64_t>(application::kMessageRetentionBatchLimit));std::vector<std::int64_t> messages;while(q.row())messages.push_back(q.int64(0));
                bool clean=true;for(const auto message:messages){budget.poll();if(!maintenance_detail::message_valid(db,message)){clean=false;break;}}
                if(!clean){maintenance_detail::hold(db,-1,id,run.started_utc,1);run.reason=Reason::corrupt;private_done=true;}
                else for(const auto message:messages){budget.poll();SqliteStatement expire(db,"UPDATE sfx_message_recipient_state SET inbox_removed=1 WHERE message_id=?1 AND opened=0 AND inbox_removed=0","expire only still-unread recipient inbox, preserving sender/body");expire.bind(1,message);expire.step();result.expired_unread+=sqlite3_changes(db);private_cursor=message;}
                if(messages.empty())private_done=true;
            }
        }
        SqliteStatement remaining(db,"SELECT count(*) FROM sfx_message_maintenance_plan WHERE run_id=?1 AND completed=0","measure remaining captured areas");remaining.bind(1,id);if(!remaining.row())message_detail::conflict();result.complete=remaining.int64(0)==0 && private_done;
        {SqliteStatement save(db,"UPDATE sfx_message_maintenance_progress SET private_cursor=?2,private_done=?3,last_sequence=?4,last_public=?5,last_private=?6,last_complete=?7,policy_changed=?8 WHERE run_id=?1","commit bounded batch receipt and cursor together");
            save.bind(1,id);save.bind(2,private_cursor);save.bind(3,private_done);save.bind(4,expected);save.bind(5,result.removed_public);save.bind(6,result.expired_unread);save.bind(7,result.complete);save.bind(8,result.policy_changed);save.step();}
        {SqliteStatement save(db,"UPDATE sfx_message_maintenance_runs SET removed_public=removed_public+?2,expired_unread=expired_unread+?3,committed_batches=committed_batches+1,safe_reason=CASE WHEN ?4<>0 THEN ?4 ELSE safe_reason END WHERE run_id=?1","record committed removals, never optimistic totals");
            save.bind(1,id);save.bind(2,result.removed_public);save.bind(3,result.expired_unread);save.bind(4,static_cast<std::int64_t>(run.reason));save.step();}
        result.run=maintenance_detail::run(db,id);out=std::move(result);return SQLITE_OK;
    });
    if(!completed && (abort->cancelled || abort->deadline))completed.status={StorageErrorCode::busy,abort->cancelled?"message_maintenance_cancelled":"message_maintenance_deadline"};
    return completed;
}

application::StorageResult<application::MessageMaintenanceRun> SqliteAccountRepository::finish_message_retention(
    const application::MessageConfigurationActor& actor,application::Identifier128 id,application::MessageMaintenanceOutcome outcome,application::MessageMaintenanceReason reason){
    using Run=application::MessageMaintenanceRun;using Outcome=application::MessageMaintenanceOutcome;
    if(!id.valid() || outcome==Outcome::running || outcome>Outcome::interrupted || reason>application::MessageMaintenanceReason::runtime_restart)return read_failure<Run>(invalid("validate retention finish"));
    const auto now=impl_->message_utc_now_?impl_->message_utc_now_():std::int64_t{};
    if(!application::valid_utc_seconds(now))return read_failure<Run>(invalid("retention finish clock"));
    return impl_->write_result<Run>("finish actual message retention outcome",4096,[actor,id,outcome,reason,now](sqlite3* db,std::optional<Run>& out){
        maintenance_detail::same_owner(db,actor,id);auto run=maintenance_detail::run(db,id);
        if(run.kind!=application::MessageMaintenanceKind::retention)message_detail::conflict();
        if(run.outcome!=Outcome::running){run.replayed=true;out=std::move(run);return SQLITE_OK;}
        SqliteStatement progress(db,"SELECT last_complete,policy_changed FROM sfx_message_maintenance_progress WHERE run_id=?1","verify actual completed retention work");progress.bind(1,id);if(!progress.row())message_detail::conflict();
        if(outcome==Outcome::success && (!progress.boolean(0) || progress.boolean(1) || run.reason!=application::MessageMaintenanceReason::none))message_detail::conflict();
        const auto actual=(outcome==Outcome::failed || outcome==Outcome::interrupted) && (run.removed_public || run.expired_unread)?Outcome::partial:outcome;
        SqliteStatement q(db,"UPDATE sfx_message_maintenance_runs SET finished_utc_seconds=?2,result=?3,safe_reason=CASE WHEN ?4<>0 THEN ?4 ELSE safe_reason END WHERE run_id=?1","finish committed totals without whole-run rollback claim");
        q.bind(1,id);q.bind(2,std::max(now,run.started_utc));q.bind(3,static_cast<std::int64_t>(actual));q.bind(4,static_cast<std::int64_t>(reason));q.step();
        maintenance_detail::prune_details(db,application::MessageMaintenanceKind::retention);out=maintenance_detail::run(db,id);return SQLITE_OK;
    });
}

application::StorageResult<application::MessageMaintenanceRun> SqliteAccountRepository::message_maintenance_run(
    const application::MessageConfigurationActor& actor,application::Identifier128 id){
    if(!id.valid())return read_failure<application::MessageMaintenanceRun>(invalid("validate maintenance outcome identity"));
    return impl_->readers_.read<application::MessageMaintenanceRun>("read content-free maintenance outcome",[actor,id](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,actor);return maintenance_detail::run(db,id);
    });
}

application::StorageResult<std::vector<application::MessageMaintenanceRun>> SqliteAccountRepository::message_maintenance_history(
    const application::MessageConfigurationActor& actor,application::MessageMaintenanceKind kind){
    using Runs=std::vector<application::MessageMaintenanceRun>;
    if(kind>application::MessageMaintenanceKind::integrity)return read_failure<Runs>(invalid("validate maintenance history kind"));
    return impl_->readers_.read<Runs>("read bounded content-free maintenance history",[actor,kind](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,actor);
        SqliteStatement q(db,"SELECT run_id FROM sfx_message_maintenance_runs WHERE kind=?1 ORDER BY started_utc_seconds DESC,run_id DESC LIMIT 32","read latest bounded maintenance identities");q.bind(1,static_cast<std::int64_t>(kind));Runs result;
        while(q.row()){
            const auto bytes=q.blob(0);if(bytes.size()!=16)throw StorageFailure(StorageErrorCode::corrupt,"invalid maintenance identity",SQLITE_CORRUPT);
            application::Identifier128 id;std::copy(bytes.begin(),bytes.end(),id.bytes.begin());result.push_back(maintenance_detail::run(db,id));
        }return result;
    });
}

application::StorageResult<std::vector<application::MessageIntegrityFinding>> SqliteAccountRepository::message_integrity_holds(
    const application::MessageConfigurationActor& actor){
    using Findings=std::vector<application::MessageIntegrityFinding>;
    return impl_->readers_.read<Findings>("read current scoped corruption holds",[actor](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,actor);
        SqliteStatement q(db,"SELECT scope,findings FROM sfx_message_integrity_holds ORDER BY scope LIMIT 1002","read bounded hold scope only");Findings result;
        while(q.row())result.push_back({q.int64(0),q.int64(1)});
        if(result.size()>1001)throw StorageFailure(StorageErrorCode::corrupt,"invalid hold scope count",SQLITE_CORRUPT);
        return result;
    });
}

application::StorageResult<application::MessageIntegrityResult> SqliteAccountRepository::check_message_integrity(
    const application::MessageConfigurationActor& actor,application::Identifier128 id,application::MessageMaintenanceStop stop){
    using Result=application::MessageIntegrityResult;using Run=application::MessageMaintenanceRun;
    using Outcome=application::MessageMaintenanceOutcome;using Reason=application::MessageMaintenanceReason;
    if(!id.valid())return read_failure<Result>(invalid("validate current integrity check identity"));
    const auto now=impl_->message_utc_now_?impl_->message_utc_now_():std::int64_t{};
    if(!application::valid_utc_seconds(now))return read_failure<Result>(invalid("integrity clock"));
    const auto begun=impl_->write_result<Run>("begin current integrity check",4096,[actor,id,now](sqlite3* db,std::optional<Run>& out){
        message_detail::config_authority(db,actor);auto prior=maintenance_detail::run(db,id,false);
        if(prior.id.valid()){
            maintenance_detail::same_owner(db,actor,id);if(prior.kind!=application::MessageMaintenanceKind::integrity)message_detail::conflict();prior.replayed=true;out=std::move(prior);return SQLITE_OK;
        }
        maintenance_detail::insert_run(db,actor,id,application::MessageMaintenanceKind::integrity,now);out=maintenance_detail::run(db,id);return SQLITE_OK;
    });
    if(!begun || !begun.value)return {begun.status,{}};
    // Reconciliation is not a fresh current check. Never clear a new/restored
    // store's hold using the stored success of an old operation.
    if(begun.value->replayed)return {StorageStatus::success(),Result{*begun.value,false,false,false,{}}};
    struct Check {
        std::map<std::int64_t,std::int64_t> findings;
        std::map<std::int64_t,std::vector<std::uint8_t>> previous_holds;
    };
    auto abort=std::make_shared<maintenance_detail::MaintenanceAbort>();
    const auto checked=impl_->maintenance_read<Check>("bounded complete current SQLite/message integrity check",[actor,stop,abort](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,actor);
        maintenance_detail::SqlBudget budget(db,stop,application::kMessageIntegritySqlBudgetMillis,abort.get());budget.poll();Check result;
        {SqliteStatement holds(db,"SELECT scope,incident_id FROM sfx_message_integrity_holds ORDER BY scope LIMIT 1002","capture current hold incidents without bodies");
            while(holds.row())result.previous_holds.emplace(holds.int64(0),holds.blob(1));
            if(result.previous_holds.size()>1001)result.findings[0]++;}
        {SqliteStatement q(db,"PRAGMA integrity_check(100)","required SQLite structural integrity check");
            bool returned=false;while(q.row()){returned=true;if(q.text(0)!="ok")result.findings[0]++;}if(!returned)result.findings[0]++;}
        budget.poll();
        // Exact schema/ledger/FKs are shared-store contracts. Corrupt original
        // message bytes/relationships are separately attributable by stable area.
        if(!validate_sfx_shipping_database(db,false))result.findings[0]++;
        budget.poll();
        if(result.findings.find(0)==result.findings.end()){
            {SqliteStatement counters(db,"SELECT last_arrival_ordinal,total_committed,last_read_sequence,(SELECT coalesce(max(arrival_ordinal),0) FROM sfx_local_messages),(SELECT count(*) FROM sfx_local_messages),(SELECT coalesce(max(opened_sequence),0) FROM sfx_message_recipient_state) FROM sfx_message_counters WHERE singleton_id=1","check monotonic shared message/recipient counters");
                if(!counters.row() || counters.int64(0)<counters.int64(3) || counters.int64(1)<counters.int64(4) || counters.int64(2)<counters.int64(5))result.findings[0]++;}
            {SqliteStatement echoes(db,"SELECT e.echo_id FROM sfx_local_echoes e WHERE e.next_message_number<=(SELECT coalesce(max(m.area_number),0) FROM sfx_local_messages m WHERE m.echo_id=e.echo_id AND m.kind=0) ORDER BY e.echo_id LIMIT 1000","check per-echo stable number allocation");
                while(echoes.row()){budget.poll();result.findings[echoes.int64(0)]++;}}
            SqliteStatement messages(db,"SELECT message_id,kind,coalesce(echo_id,0) FROM sfx_local_messages ORDER BY message_id","stream existing messages in current check snapshot");
            while(messages.row()){
                budget.poll();const auto kind=messages.int64(1),echo=messages.int64(2);
                if(!maintenance_detail::message_valid(db,messages.int64(0)))result.findings[kind==0 && echo>0?echo:kind==1?-1:0]++;
            }
        }
        budget.poll();return result;
    });
    const auto finish=impl_->message_utc_now_?impl_->message_utc_now_():now;
    if(!application::valid_utc_seconds(finish))return read_failure<Result>(invalid("integrity finish clock"));
    const bool cancelled=abort->cancelled || [&]{try{return stop && stop();}catch(...){return true;}}();
    return impl_->write_result<Result>("publish actual integrity outcome and hold transition",4096,[actor,id,checked,finish,cancelled,abort](sqlite3* db,std::optional<Result>& out){
        maintenance_detail::same_owner(db,actor,id);auto run=maintenance_detail::run(db,id);
        if(run.outcome!=Outcome::running)message_detail::conflict();
        Result result;result.complete=bool(checked) && checked.value.has_value() && !cancelled;
        result.clean=result.complete && checked.value->findings.empty();
        Reason reason=cancelled?Reason::cancelled:abort->deadline?Reason::deadline:result.complete?Reason::none:checked.status.code==StorageErrorCode::corrupt?Reason::corrupt:Reason::incomplete;
        if(result.complete && !result.clean){
            for(const auto& [scope,count]:checked.value->findings){maintenance_detail::hold(db,scope,id,finish,count);result.findings.push_back({scope,count});}reason=Reason::corrupt;
        }else if(result.clean){
            std::map<std::int64_t,std::vector<std::uint8_t>> current;
            SqliteStatement holds(db,"SELECT scope,incident_id FROM sfx_message_integrity_holds ORDER BY scope LIMIT 1002","revalidate affected hold generation before clean resumption");
            while(holds.row())current.emplace(holds.int64(0),holds.blob(1));
            if(current!=checked.value->previous_holds){result.clean=false;result.complete=false;reason=Reason::stale;}
            else{result.cleared_hold=!current.empty();SqliteStatement clear(db,"DELETE FROM sfx_message_integrity_holds","clear only current fresh fully checked corruption holds");clear.step();}
        }else if(checked.status.code==StorageErrorCode::corrupt){
            maintenance_detail::hold(db,0,id,finish,1);result.findings.push_back({0,1});
        }
        const auto outcome=result.complete?(result.clean?Outcome::success:Outcome::failed):cancelled?Outcome::interrupted:Outcome::failed;
        SqliteStatement save(db,"UPDATE sfx_message_maintenance_runs SET finished_utc_seconds=?2,result=?3,safe_reason=?4 WHERE run_id=?1","report complete/failed/incomplete integrity honestly");
        save.bind(1,id);save.bind(2,std::max(finish,run.started_utc));save.bind(3,static_cast<std::int64_t>(outcome));save.bind(4,static_cast<std::int64_t>(reason));save.step();
        maintenance_detail::prune_details(db,application::MessageMaintenanceKind::integrity);
        result.run=maintenance_detail::run(db,id);out=std::move(result);return SQLITE_OK;
    });
}

application::StorageStatus SqliteAccountRepository::recover_interrupted_message_maintenance(const application::MessageConfigurationActor& actor){
    if(!actor.installation_owner || actor.caller)return {StorageErrorCode::permission_denied,"host-only maintenance startup reconciliation"};
    const auto now=impl_->message_utc_now_?impl_->message_utc_now_():std::int64_t{};
    if(!application::valid_utc_seconds(now))return invalid("maintenance recovery clock");
    return impl_->write("reconcile interrupted message maintenance without rerun",4096,[actor,now](sqlite3* db){
        message_detail::config_authority(db,actor);
        SqliteStatement q(db,"UPDATE sfx_message_maintenance_runs SET finished_utc_seconds=?1,result=CASE WHEN removed_public>0 OR expired_unread>0 THEN 2 ELSE 5 END,safe_reason=10 WHERE result=0","record actual prior committed counts; never resume destructive batches automatically");q.bind(1,now);q.step();return SQLITE_OK;
    });
}
