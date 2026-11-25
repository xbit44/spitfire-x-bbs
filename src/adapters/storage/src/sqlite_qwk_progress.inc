// Trusted host packet progress only. Never deserialize these receipts/plans or
// accept a caller's claim of transport completion. Runtime proves file identity
// and actual ZMODEM completion first; preparation alone never calls this writer.
namespace qwk_detail {
struct Digest {
    crypto_hash_sha256_state state{};
    explicit Digest(std::string_view domain){
        if(sodium_init()<0 || crypto_hash_sha256_init(&state)!=0)throw StorageFailure(StorageErrorCode::internal,"initialize packet receipt digest",SQLITE_ERROR);
        text(domain);
    }
    void number(std::uint64_t value){
        unsigned char bytes[8]{};for(unsigned n=0;n<8;++n)bytes[7-n]=static_cast<unsigned char>(value>>(n*8));
        if(crypto_hash_sha256_update(&state,bytes,8)!=0)throw StorageFailure(StorageErrorCode::internal,"digest packet receipt number",SQLITE_ERROR);
    }
    void text(std::string_view value){number(value.size());if(crypto_hash_sha256_update(&state,reinterpret_cast<const unsigned char*>(value.data()),value.size())!=0)throw StorageFailure(StorageErrorCode::internal,"digest packet receipt field",SQLITE_ERROR);}
    template<std::size_t N> void bytes(const std::array<std::uint8_t,N>& value){text({reinterpret_cast<const char*>(value.data()),N});}
    std::array<std::uint8_t,32> finish(){std::array<std::uint8_t,32> result{};if(crypto_hash_sha256_final(&state,result.data())!=0)throw StorageFailure(StorageErrorCode::internal,"finish packet receipt digest",SQLITE_ERROR);return result;}
};
void queue_digest(Digest& d,const app::QwkQueuePosition& q){d.number(q.echo.value);d.number(q.preference_revision);d.number(q.initial_highest_opened);d.number(q.after_arrival);d.number(q.unread);d.number(q.to_you);}
std::array<std::uint8_t,32> delivery_digest(const app::QwkDeliveryCommit& r){
    const auto& p=r.initial;Digest d("SFX/QWKDelivery/v1");d.number(p.owner);d.bytes(p.operation.bytes);d.text(p.policy.board_id);d.number(p.policy.maximum_messages);d.number(p.policy.revision);
    d.number(p.settings.revision);const auto& s=p.settings.preferences;d.number(static_cast<unsigned>(s.private_mail));d.number(s.own_public_posts);d.number(static_cast<unsigned>(s.encoding));d.number(s.preserve_colors);d.number(s.word_wrap.value_or(false));d.number(s.indexes);d.number(s.full_headers);
    d.number(p.arrival_fence);d.number(p.catalog_revision);d.number(p.continuation_revision);d.number(p.next_queue);d.bytes(p.selection_fingerprint);
    d.number(p.queues.size());for(const auto& q:p.queues)queue_digest(d,q);
    d.number(r.delivered.size());for(const auto& m:r.delivered){d.number(m.message.value);d.number(m.echo.value);d.number(m.arrival);d.number(m.area_number);}
    d.number(r.final_queues.size());for(const auto& q:r.final_queues)queue_digest(d,q);
    d.number(r.next_queue);d.number(r.more);d.bytes(r.zip_sha256);d.number(r.compressed_bytes);d.number(r.expanded_bytes);return d.finish();
}
bool valid_delivery(const app::MessageActor& a,const app::QwkDeliveryCommit& r){
    const auto& p=r.initial;
    if(!valid_plan(a,p) || !app::valid_qwk_message_count(p.policy.maximum_messages) || r.delivered.empty() || r.delivered.size()>p.policy.maximum_messages ||
       r.final_queues.size()!=p.queues.size() || r.next_queue>=p.queues.size() || !r.compressed_bytes || r.compressed_bytes>app::kQwkCompressedBytes || !r.expanded_bytes || r.expanded_bytes>app::kQwkExpandedBytes)return false;
    auto cursors=p.queues;std::set<std::int64_t> unique;std::size_t last_queue{};
    for(const auto& m:r.delivered){
        if(m.message.value<=0 || m.arrival<=0 || m.arrival>p.arrival_fence || !unique.insert(m.message.value).second)return false;
        const auto at=std::find_if(cursors.begin(),cursors.end(),[&](const auto& q){return q.echo==m.echo;});
        if(at==cursors.end() || m.arrival<=at->after_arrival || (m.echo.value?m.area_number<=0:m.area_number!=0))return false;
        at->after_arrival=m.arrival;last_queue=static_cast<std::size_t>(at-cursors.begin());
    }
    if(r.next_queue!=(last_queue+1)%p.queues.size())return false;
    for(std::size_t n=0;n<cursors.size();++n){const auto& x=cursors[n];const auto& y=r.final_queues[n];
        if(!(x.echo==y.echo) || x.preference_revision!=y.preference_revision || x.initial_highest_opened!=y.initial_highest_opened || x.after_arrival!=y.after_arrival || x.unread!=y.unread || x.to_you!=y.to_you)return false;}
    return true;
}
std::optional<app::QwkDeliveryOutcome> delivery_receipt(sqlite3* db,const app::QwkDeliveryCommit& r,const std::array<std::uint8_t,32>& fingerprint){
    SqliteStatement q(db,"SELECT account_id,request_fingerprint,delivered,more FROM sfx_qwk_delivery_receipts WHERE operation_id=?1","reconcile only own exact packet delivery");q.bind(1,r.initial.operation);if(!q.row())return {};
    const auto stored=q.blob(1);if(q.int64(0)!=r.initial.owner || stored.size()!=32 || sodium_memcmp(stored.data(),fingerprint.data(),32)!=0)message_detail::conflict();
    const auto count=q.int64(2);if(count<1 || count>65535)throw StorageFailure(StorageErrorCode::corrupt,"invalid packet delivery receipt count",SQLITE_CORRUPT);
    return app::QwkDeliveryOutcome{static_cast<std::uint32_t>(count),q.boolean(3),true};
}
void require_continuation_revision(sqlite3* db,const app::QwkPacketPlan& p){
    SqliteStatement q(db,"SELECT revision FROM sfx_qwk_continuation WHERE account_id=?1","compare caller packet continuation generation");q.bind(1,p.owner);
    const auto current=q.row()?q.int64(0):0;if(current!=p.continuation_revision || current==std::numeric_limits<std::int64_t>::max())message_detail::conflict();
}
void store_continuation(sqlite3* db,const app::QwkDeliveryCommit& r){
    const auto& p=r.initial;
    // Keep a compact generation row even at batch completion. Removing it here
    // would let an older simultaneous first packet re-use generation zero and
    // overwrite newer progress. Exhausted cursors rebase locally on next begin.
    SqliteStatement root(db,"INSERT INTO sfx_qwk_continuation VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(account_id) DO UPDATE SET revision=excluded.revision,settings_revision=excluded.settings_revision,selection_fingerprint=excluded.selection_fingerprint,arrival_fence=excluded.arrival_fence,next_queue=excluded.next_queue,private_after_arrival=excluded.private_after_arrival","persist compact finite batch and monotonic generation");
    root.bind(1,p.owner);root.bind(2,p.continuation_revision+1);root.bind(3,p.settings.revision);root.bind(4,p.selection_fingerprint);root.bind(5,p.arrival_fence);root.bind(6,static_cast<std::int64_t>(r.next_queue));root.bind(7,r.final_queues[0].after_arrival);root.step();
    SqliteStatement remove(db,"DELETE FROM sfx_qwk_continuation_echo WHERE account_id=?1","replace only own bounded public cursors");remove.bind(1,p.owner);remove.step();
    for(std::size_t n=1;n<r.final_queues.size();++n){const auto& q=r.final_queues[n];SqliteStatement save(db,"INSERT INTO sfx_qwk_continuation_echo VALUES(?1,?2,?3,?4,?5)","store delivered public queue cursor");
        save.bind(1,p.owner);save.bind(2,q.echo.value);save.bind(3,q.preference_revision);save.bind(4,q.initial_highest_opened);save.bind(5,q.after_arrival);save.step();}
}
int delivered_progress(sqlite3* db,const app::MessageActor& a,const app::QwkDeliveryCommit& r){
    const auto authority=message_detail::actor(db,a);
    for(const auto& item:r.delivered){
        if(r.cancelled && r.cancelled())return SQLITE_INTERRUPT;
        SqliteStatement m(db,"SELECT kind,coalesce(echo_id,0),coalesce(area_number,0),arrival_ordinal,recipient_account_id,author_account_id FROM sfx_local_messages WHERE message_id=?1","recheck captured delivered identity, not caller-supplied names");m.bind(1,item.message.value);
        if(m.row()){
            if(m.int64(1)!=item.echo.value || m.int64(2)!=item.area_number || m.int64(3)!=item.arrival || m.int64(0)!=(item.echo.value?0:1))return SQLITE_ABORT;
            if(!item.echo.value && (m.is_null(4) || m.int64(4)!=a.account))return SQLITE_ABORT;
            if(item.echo.value && !r.initial.settings.preferences.own_public_posts && m.int64(5)==a.account)return SQLITE_ABORT;
        }
        if(item.echo.value){
            message_detail::echo(db,item.echo,authority,true,false);
            const auto p=message_detail::preference(db,a,item.echo);
            const auto captured=std::find_if(r.initial.queues.begin(),r.initial.queues.end(),[&](const auto& q){return q.echo==item.echo;});
            if(captured==r.initial.queues.end() || captured->preference_revision!=p.revision)return SQLITE_ABORT;
            SqliteStatement bound(db,"SELECT next_message_number FROM sfx_local_echoes WHERE echo_id=?1","reject delivery leap beyond allocated public messages");bound.bind(1,item.echo.value);if(!bound.row() || item.area_number>=bound.int64(0))return SQLITE_ABORT;
            SqliteStatement advance(db,"UPDATE sfx_caller_echo_state SET highest_opened=max(highest_opened,?1) WHERE account_id=?2 AND echo_id=?3","advance included public boundary only");advance.bind(1,item.area_number);advance.bind(2,a.account);advance.bind(3,item.echo.value);advance.step();
        }else{
            if(r.initial.settings.preferences.private_mail==app::QwkPrivateMode::none)return SQLITE_ABORT;
        }
        // Public pointers and an addressed message's personal Read state are
        // separate. A confirmed download opens this caller's included copy in
        // either kind, exactly as reading it online does. Never mark another
        // recipient, a removed Inbox copy or any message absent from the packet.
        SqliteStatement eligible(db,"SELECT 1 FROM sfx_message_recipient_state WHERE message_id=?1 AND recipient_account_id=?2 AND inbox_removed=0 AND opened=0","mark only still-present included received copy");eligible.bind(1,item.message.value);eligible.bind(2,a.account);
        if(eligible.row()){
            SqliteStatement sequence(db,"UPDATE sfx_message_counters SET last_read_sequence=last_read_sequence+1 WHERE singleton_id=1 AND last_read_sequence<9223372036854775807 RETURNING last_read_sequence","allocate delivered recipient Read sequence");if(!sequence.row())return SQLITE_FULL;const auto value=sequence.int64(0);sequence.step();
            SqliteStatement read(db,"UPDATE sfx_message_recipient_state SET opened=1,opened_sequence=?3 WHERE message_id=?1 AND recipient_account_id=?2 AND inbox_removed=0 AND opened=0","mark included recipient mail Read without deleting");read.bind(1,item.message.value);read.bind(2,a.account);read.bind(3,value);read.step();
        }
    }
    return SQLITE_OK;
}
bool valid_pointer_request(const app::QwkPointerChange& c,bool preview){
    if(!c.operation.valid() || static_cast<unsigned>(c.scope)>2 || static_cast<unsigned>(c.mode)>2 ||
       (c.scope==app::MessageQueryScope::current_echo ? c.echo.value<=0 || c.group.value!=0 : c.echo.value!=0 || (c.scope==app::MessageQueryScope::current_group?c.group.value<=0:c.group.value!=0)) ||
       (c.mode==app::QwkPointerMode::since_entry_time ? !c.since_utc || !app::valid_utc_seconds(*c.since_utc) : c.since_utc.has_value()))return false;
    if(preview)return c.expected.empty();
    if(c.expected_catalog_revision<1 || c.expected.empty() || c.expected.size()>999)return false;
    std::int64_t last{};for(const auto& p:c.expected){if(p.echo.value<=last || p.preference_revision<1 || p.highest_opened<0 || p.captured_highest<0 || p.desired_highest<0 || p.desired_highest>p.captured_highest)return false;last=p.echo.value;}return true;
}
std::vector<app::QwkPointerState> pointer_targets(sqlite3* db,const app::MessageActor& a,const app::QwkPointerChange& c){
    const auto authority=message_detail::actor(db,a);
    SqliteStatement q(db,"SELECT e.echo_id,e.next_message_number-1 FROM sfx_local_echoes e JOIN sfx_message_groups g USING(group_id) WHERE e.enabled=1 AND g.enabled=1 AND e.access_minimum<=?1 AND e.reading_minimum<=?1 AND g.access_minimum<=?1 AND (?2=0 OR e.echo_id=?2) AND (?3=0 OR e.group_id=?3) ORDER BY e.echo_id LIMIT 1000","capture exactly accessible public reset scope");q.bind(1,authority.level);q.bind(2,c.echo.value);q.bind(3,c.group.value);
    std::vector<app::QwkPointerState> result;
    while(q.row()){
        if(result.size()==999)message_detail::unavailable();
        const app::LocalEchoId echo{q.int64(0)};const auto p=message_detail::read_preference(db,a,echo);const auto highest=q.int64(1);
        auto desired=c.mode==app::QwkPointerMode::all_available?0:highest;
        if(c.mode==app::QwkPointerMode::since_entry_time){SqliteStatement since(db,message_detail::active_message_sql(db,"SELECT min(area_number) FROM sfx_active_local_messages WHERE kind=0 AND echo_id=?1 AND created_utc_seconds>=?2 AND area_number<=?3").c_str(),"rescan by local SFX entry time, never original authored time");since.bind(1,echo.value);since.bind(2,*c.since_utc);since.bind(3,highest);if(!since.row())message_detail::unavailable();if(!since.is_null(0))desired=since.int64(0)-1;}
        result.push_back({echo,p.revision,p.highest_opened,highest,desired});
    }
    if(result.empty())message_detail::unavailable();
    return result;
}
std::array<std::uint8_t,32> pointer_digest(const app::MessageActor& a,const app::QwkPointerChange& c){
    Digest d("SFX/QWKPointer/v1");d.number(a.account);d.bytes(c.operation.bytes);d.number(static_cast<unsigned>(c.scope));d.number(c.echo.value);d.number(c.group.value);d.number(static_cast<unsigned>(c.mode));d.number(c.since_utc.value_or(0));d.number(c.expected_catalog_revision);d.number(c.expected.size());
    for(const auto& p:c.expected){d.number(p.echo.value);d.number(p.preference_revision);d.number(p.highest_opened);d.number(p.captured_highest);d.number(p.desired_highest);}return d.finish();
}
} // namespace qwk_detail

application::StorageResult<application::QwkDeliveryOutcome> SqliteAccountRepository::commit_qwk_delivery(const application::MessageActor& a,const application::QwkDeliveryCommit& r){
    if(!qwk_detail::valid_delivery(a,r))return read_failure<application::QwkDeliveryOutcome>(invalid("validate actual packet delivery commit"));
    const auto fingerprint=qwk_detail::delivery_digest(r);const auto now=impl_->message_utc_now_();
    if(!application::valid_utc_seconds(now))return read_failure<application::QwkDeliveryOutcome>(invalid("packet delivery clock"));
    const auto charge=65536U+2U*(r.delivered.size()*sizeof(application::QwkDeliveredItem)+r.final_queues.size()*sizeof(application::QwkQueuePosition)+r.initial.queues.size()*sizeof(application::QwkQueuePosition)+r.initial.catalog.size()*256U);
    return impl_->write_result<application::QwkDeliveryOutcome>("atomic completed packet progress and receipt",charge,[a,r,fingerprint,now](sqlite3* db,auto& output){
        message_detail::actor(db,a,false);if(const auto prior=qwk_detail::delivery_receipt(db,r,fingerprint)){output=*prior;return SQLITE_OK;}
        if(r.cancelled && r.cancelled())return SQLITE_INTERRUPT;
        qwk_detail::check_plan(db,a,r.initial);qwk_detail::require_continuation_revision(db,r.initial);
        const auto code=qwk_detail::delivered_progress(db,a,r);if(code!=SQLITE_OK)return code;
        if(r.cancelled && r.cancelled())return SQLITE_INTERRUPT;
        qwk_detail::store_continuation(db,r);
        SqliteStatement receipt(db,"INSERT INTO sfx_qwk_delivery_receipts VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)","retain immutable content-free successful packet receipt");receipt.bind(1,r.initial.operation);receipt.bind(2,a.account);receipt.bind(3,fingerprint);receipt.bind(4,r.zip_sha256);receipt.bind(5,static_cast<std::int64_t>(r.compressed_bytes));receipt.bind(6,static_cast<std::int64_t>(r.expanded_bytes));receipt.bind(7,static_cast<std::int64_t>(r.delivered.size()));receipt.bind(8,r.more);receipt.bind(9,now);receipt.step();
        if(r.cancelled && r.cancelled())return SQLITE_INTERRUPT;
        output=application::QwkDeliveryOutcome{static_cast<std::uint32_t>(r.delivered.size()),r.more,false};return SQLITE_OK;
    });
}
application::StorageResult<std::optional<application::QwkDeliveryOutcome>> SqliteAccountRepository::qwk_delivery_outcome(const application::MessageActor& a,const application::QwkDeliveryCommit& r){
    if(!qwk_detail::valid_delivery(a,r))return read_failure<std::optional<application::QwkDeliveryOutcome>>(invalid("validate delivery reconciliation"));
    const auto fingerprint=qwk_detail::delivery_digest(r);
    return impl_->readers_.read<std::optional<application::QwkDeliveryOutcome>>("reconcile exact own delivery, never confuse error with absence",[a,r,fingerprint](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::actor(db,a,false);return qwk_detail::delivery_receipt(db,r,fingerprint);});
}
application::StorageResult<application::QwkPointerChange> SqliteAccountRepository::preview_qwk_public_pointers(const application::MessageActor& a,const application::QwkPointerChange& c){
    if(!qwk_detail::valid_pointer_request(c,true))return read_failure<application::QwkPointerChange>(invalid("validate public pointer preview"));
    return impl_->readers_.read<application::QwkPointerChange>("capture public pointer scope and boundary without effects",[a,c](sqlite3* db){message_detail::ReadSnapshot snapshot(db);auto result=c;result.expected_catalog_revision=message_detail::catalog_revision(db);result.expected=qwk_detail::pointer_targets(db,a,c);return result;});
}
application::StorageStatus SqliteAccountRepository::change_qwk_public_pointers(const application::MessageActor& a,const application::QwkPointerChange& c){
    if(!qwk_detail::valid_pointer_request(c,false))return invalid("validate explicit public pointer change");
    const auto fingerprint=qwk_detail::pointer_digest(a,c);const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return invalid("public pointer clock");
    return impl_->write("change only confirmed own public pointers",4096U+c.expected.size()*sizeof(application::QwkPointerState),[a,c,fingerprint,now](sqlite3* db){
        message_detail::actor(db,a,false);
        SqliteStatement previous(db,"SELECT account_id,request_fingerprint FROM sfx_qwk_pointer_receipts WHERE operation_id=?1","reconcile exact public reset without repeating later side effects");previous.bind(1,c.operation);if(previous.row()){const auto hash=previous.blob(1);return previous.int64(0)==a.account && hash.size()==32 && sodium_memcmp(hash.data(),fingerprint.data(),32)==0?SQLITE_OK:SQLITE_ABORT;}
        if(message_detail::catalog_revision(db)!=c.expected_catalog_revision)return SQLITE_ABORT;
        const auto targets=qwk_detail::pointer_targets(db,a,c);if(targets.size()!=c.expected.size())return SQLITE_ABORT;
        for(std::size_t n=0;n<targets.size();++n){const auto& old=c.expected[n];const auto& current=targets[n];
            if(!(old.echo==current.echo) || old.preference_revision!=current.preference_revision || old.highest_opened!=current.highest_opened || old.captured_highest>current.captured_highest || old.preference_revision==std::numeric_limits<std::int64_t>::max())return SQLITE_ABORT;
            // New arrivals after preview are not caught-up implicitly. Recompute
            // the desired cutoff against that same captured boundary at commit.
            auto desired=c.mode==application::QwkPointerMode::all_available?0:old.captured_highest;
            if(c.mode==application::QwkPointerMode::since_entry_time){SqliteStatement since(db,message_detail::active_message_sql(db,"SELECT min(area_number) FROM sfx_active_local_messages WHERE kind=0 AND echo_id=?1 AND created_utc_seconds>=?2 AND area_number<=?3").c_str(),"recheck entry-time boundary after retention");since.bind(1,old.echo.value);since.bind(2,*c.since_utc);since.bind(3,old.captured_highest);if(!since.row())return SQLITE_ERROR;if(!since.is_null(0))desired=since.int64(0)-1;}
            if(desired!=old.desired_highest)return SQLITE_ABORT;
            message_detail::preference(db,a,old.echo);
            SqliteStatement change(db,"UPDATE sfx_caller_echo_state SET highest_opened=?1,preference_revision=preference_revision+1 WHERE account_id=?2 AND echo_id=?3 AND preference_revision=?4 AND highest_opened=?5","CAS explicit public reset, preserving both scan flags");change.bind(1,desired);change.bind(2,a.account);change.bind(3,old.echo.value);change.bind(4,old.preference_revision);change.bind(5,old.highest_opened);change.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        }
        qwk_detail::clear_continuation(db,a.account);
        SqliteStatement receipt(db,"INSERT INTO sfx_qwk_pointer_receipts VALUES(?1,?2,?3,?4)","retain durable reset receipt in same transaction");receipt.bind(1,c.operation);receipt.bind(2,a.account);receipt.bind(3,fingerprint);receipt.bind(4,now);receipt.step();return SQLITE_OK;
    });
}
