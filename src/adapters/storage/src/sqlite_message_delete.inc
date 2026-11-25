application::StorageResult<bool> SqliteAccountRepository::message_delete_authority(const application::MessageActor& caller){
    return impl_->readers_.read<bool>("read current message delete authority",[caller](sqlite3* db){
        return message_detail::can_delete_for_everyone(message_detail::actor(db,caller));
    });
}

StorageStatus SqliteAccountRepository::set_message_list_deleted(const application::MessageActor& caller,
    const application::MessageQueryContext& context,application::MessageId id,bool expected,bool deleted,
    application::MessageListDeleteScope scope) {
    using Scope=application::MessageListDeleteScope;
    if (id.value<=0 || expected==deleted || (scope!=Scope::inbox && scope!=Scope::everyone) ||
        !message_detail::valid_query_context(caller,context))
        return invalid("validate reversible message deletion");
    return impl_->write("change reversible message deletion",1024,[caller,context,id,expected,deleted,scope](sqlite3* db) {
        if (!message_detail::has_list_deletion_schema(db)) return SQLITE_NOTFOUND;
        const auto authority=message_detail::actor(db,caller);
        message_detail::check_query_context(db,context);
        auto message=message_detail::load(db,caller,authority,id,!context.options.search.empty(),true);
        auto& header=message.header;
        message_detail::list_deletion_status(db,caller,authority,context.options,header);
        if (!header.list_delete_allowed) return SQLITE_AUTH;
        if (header.list_delete_scope!=scope || header.list_deleted!=expected) return SQLITE_ABORT;
        // Restoring a caller's reversible Inbox removal must match the same
        // query that listed it. Keep the stored Read and Inbox states intact
        // until after all authorization and expected-state checks.
        if (header.inbox_removed && header.target.recipient==caller.account &&
            message_detail::inbox_list(context.options) && message_detail::list_deletion(db,id).inbox)
            header.inbox_removed=false;
        if (!message_detail::in_query(db,caller,context,message,
            context.options.mode==application::MessageQueryMode::regular_new,false)) return SQLITE_NOTFOUND;

        SqliteStatement marker(db,deleted ?
            "INSERT INTO sfx_message_list_deletion(message_id,scope) VALUES(?1,?2)" :
            "DELETE FROM sfx_message_list_deletion WHERE message_id=?1 AND scope=?2",
            "persist exact list deletion state");
        marker.bind(1,id.value);
        marker.bind(2,scope==Scope::inbox);
        marker.step();
        if (sqlite3_changes(db)!=1) return SQLITE_ABORT;
        if (scope==Scope::inbox) {
            SqliteStatement inbox(db,"UPDATE sfx_message_recipient_state SET inbox_removed=?1 "
                "WHERE message_id=?2 AND recipient_account_id=?3 AND inbox_removed=?4",
                "apply private Inbox deletion or restoration");
            inbox.bind(1,deleted);
            inbox.bind(2,id.value);
            inbox.bind(3,caller.account);
            inbox.bind(4,expected);
            inbox.step();
            if (sqlite3_changes(db)!=1) return SQLITE_ABORT;
        }
        return SQLITE_OK;
    });
}
application::StorageResult<bool> SqliteAccountRepository::delete_message(const application::MessageActor& caller,
    application::Identifier128 operation,application::MessageId message){
    if(!operation.valid() || message.value<=0)return read_failure<bool>(invalid("validate message delete target"));
    const auto now=impl_->message_utc_now_?impl_->message_utc_now_():std::int64_t{};
    if(!application::valid_utc_seconds(now))return read_failure<bool>(invalid("message delete clock"));
    return impl_->write_result<bool>("permanently delete individual message",4096,[caller,operation,message,now](sqlite3* db,std::optional<bool>& out){
        const auto a=message_detail::actor(db,caller);
        if(a.level!=999 && a.level<a.threshold)message_detail::unavailable();
        {SqliteStatement q(db,"SELECT actor_account_id,message_id FROM sfx_message_delete_receipts WHERE operation_id=?1","reconcile same deletion");
            q.bind(1,operation);if(q.row()){
                if(q.int64(0)!=caller.account || q.int64(1)!=message.value)message_detail::conflict();
                out=true;return SQLITE_OK;
            }}
        {SqliteStatement q(db,"SELECT 1 FROM sfx_local_messages WHERE message_id=?1","verify stable existing deletion target");
            q.bind(1,message.value);if(!q.row())message_detail::unavailable();}
        if(message_detail::permanent_flag(db,message))throw StorageFailure(StorageErrorCode::permission_denied,"public Permanent message protected",SQLITE_AUTH);
        const auto purge=[&](const char* sql){SqliteStatement q(db,sql,"purge selected message only");q.bind(1,message.value);q.step();};
        // Scope and deletion receipt are committed atomically. Parent links are
        // detached, not cascade-deleted. Cumulative counts/read pointers stay put.
        purge("INSERT INTO sfx_message_delete_scope VALUES(?1)");
        purge("DELETE FROM sfx_ftn_message_intents WHERE message_id=?1 AND state=1");
        purge("INSERT INTO sfx_message_purged_receipts SELECT operation_id,actor_account_id,request_fingerprint,message_id FROM sfx_message_commit_receipts WHERE message_id=?1");
        purge("UPDATE sfx_local_messages SET parent_message_id=NULL WHERE parent_message_id=?1");
        purge("DELETE FROM sfx_message_arrival_intents WHERE message_id=?1");
        purge("DELETE FROM sfx_message_recipient_state WHERE message_id=?1");
        purge("DELETE FROM sfx_message_sources WHERE message_id=?1");
        purge("DELETE FROM sfx_ftn_message_metadata WHERE message_id=?1");
        purge("DELETE FROM sfx_ftn_message_targets WHERE message_id=?1");
        purge("DELETE FROM sfx_message_commit_receipts WHERE message_id=?1");
        purge("DELETE FROM sfx_local_messages WHERE message_id=?1");
        purge("DELETE FROM sfx_message_delete_scope WHERE message_id=?1");
        {SqliteStatement audit(db,"INSERT INTO sfx_message_delete_receipts VALUES(?1,?2,?3,?4)","audit content-free individual deletion");
            audit.bind(1,operation);audit.bind(2,caller.account);audit.bind(3,message.value);audit.bind(4,now);audit.step();}
        out=false;return SQLITE_OK;
    });
}

application::StorageResult<bool> SqliteAccountRepository::message_permanent_state(const application::MessageActor& caller,application::MessageId message){
    if(message.value<=0)return read_failure<bool>(invalid("validate Permanent target"));
    return impl_->readers_.read<bool>("read visible public Permanent status",[caller,message](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto a=message_detail::actor(db,caller);
        const auto target=message_detail::load(db,caller,a,message,false).header;
        if(target.target.kind!=application::MessageKind::public_echo)message_detail::unavailable();
        return target.permanent;
    });
}
application::StorageResult<bool> SqliteAccountRepository::set_message_permanent(const application::MessageActor& caller,
    application::Identifier128 operation,application::MessageId message,bool expected,bool permanent){
    if(!operation.valid() || message.value<=0 || expected==permanent)return read_failure<bool>(invalid("validate Permanent change"));
    const auto now=impl_->message_utc_now_?impl_->message_utc_now_():std::int64_t{};
    if(!application::valid_utc_seconds(now))return read_failure<bool>(invalid("Permanent clock"));
    return impl_->write_result<bool>("change public Permanent flag",4096,[caller,operation,message,expected,permanent,now](sqlite3* db,std::optional<bool>& out){
        const auto a=message_detail::actor(db,caller);if(a.level<a.threshold)message_detail::unavailable();
        {SqliteStatement q(db,"SELECT actor_account_id,message_id,expected_permanent,new_permanent FROM sfx_message_permanent_receipts WHERE operation_id=?1","reconcile same Permanent change");
            q.bind(1,operation);if(q.row()){
                if(q.int64(0)!=caller.account || q.int64(1)!=message.value || q.boolean(2)!=expected || q.boolean(3)!=permanent)message_detail::conflict();
                out=true;return SQLITE_OK;
            }}
        const auto target=message_detail::load(db,caller,a,message,false).header;
        if(target.target.kind!=application::MessageKind::public_echo)message_detail::unavailable();
        if(target.permanent!=expected)message_detail::conflict();
        {SqliteStatement q(db,"INSERT INTO sfx_message_permanent VALUES(?1,?2,1) ON CONFLICT(message_id) DO UPDATE SET permanent=excluded.permanent,revision=revision+1","set explicitly authorized Permanent flag");
            q.bind(1,message.value);q.bind(2,permanent);q.step();}
        {SqliteStatement q(db,"INSERT INTO sfx_message_permanent_receipts VALUES(?1,?2,?3,?4,?5,?6)","audit content-free Permanent action");
            q.bind(1,operation);q.bind(2,caller.account);q.bind(3,message.value);q.bind(4,expected);q.bind(5,permanent);q.bind(6,now);q.step();}
        out=false;return SQLITE_OK;
    });
}
