namespace qwk_detail {
bool same_settings(const app::QwkSavedPreferences& a,const app::QwkSavedPreferences& b){
    const auto& x=a.preferences;const auto& y=b.preferences;
    return a.revision==b.revision && x.private_mail==y.private_mail && x.own_public_posts==y.own_public_posts && x.encoding==y.encoding && x.preserve_colors==y.preserve_colors && x.word_wrap==y.word_wrap && x.indexes==y.indexes && x.full_headers==y.full_headers;
}
app::QwkPacketPlan current_plan(sqlite3* db,const app::MessageActor& a,app::Identifier128 operation){
    const auto authority=message_detail::actor(db,a);app::QwkPacketPlan plan;plan.owner=a.account;plan.operation=operation;plan.policy=policy(db);plan.netmail_conference=netmail_conference(db);
    const auto saved=saved_preferences(db,a.account);if(!saved || !app::valid_qwk_preferences(saved->preferences))message_detail::conflict();plan.settings=*saved;
    plan.catalog_revision=message_detail::catalog_revision(db);plan.queues.push_back({});
    SqliteStatement fence(db,"SELECT last_arrival_ordinal FROM sfx_message_counters WHERE singleton_id=1","capture finite QWK arrival boundary");if(!fence.row())message_detail::unavailable();plan.arrival_fence=fence.int64(0);
    SqliteStatement areas(db,"SELECT e.echo_id,q.conference,g.name,e.name FROM sfx_local_echoes e JOIN sfx_message_groups g USING(group_id) JOIN sfx_qwk_conference_mapping q USING(echo_id) WHERE e.enabled=1 AND g.enabled=1 AND e.access_minimum<=?1 AND e.reading_minimum<=?1 AND g.access_minimum<=?1 ORDER BY g.display_order,g.group_id,e.display_order,e.echo_id LIMIT 1000","capture only accessible packet conference metadata");areas.bind(1,authority.level);
    std::string fingerprint="SFX/QWKSelection/v1;"+std::to_string(a.account)+';'+std::to_string(plan.catalog_revision)+';'+std::to_string(plan.settings.revision)+';'+std::to_string(a.account_revision)+';'+std::to_string(a.policy_revision)+';';
    while(areas.row()){
        if(plan.catalog.size()==999)message_detail::unavailable();
        const app::LocalEchoId echo{areas.int64(0)};const auto number=areas.int64(1);
        if(number<1 || number>65535)throw StorageFailure(StorageErrorCode::corrupt,"invalid stable QWK conference",SQLITE_CORRUPT);
        plan.catalog.push_back({echo,static_cast<std::uint16_t>(number),areas.text(2),areas.text(3)});
        const auto preference=message_detail::read_preference(db,a,echo);
        fingerprint+=std::to_string(echo.value)+':'+std::to_string(preference.revision)+':'+std::to_string(preference.unread)+':'+std::to_string(preference.to_you)+';';
        if(preference.unread || preference.to_you)plan.queues.push_back({echo,preference.revision,preference.highest_opened,0,preference.unread,preference.to_you});
    }
    fingerprint+="netmail:"+std::to_string(plan.netmail_conference.value_or(0))+";";
    plan.selection_fingerprint=message_detail::source_digest(fingerprint);return plan;
}
bool valid_plan(const app::MessageActor& a,const app::QwkPacketPlan& p){
    if(p.owner!=a.account || !p.operation.valid() || !app::valid_qwk_preferences(p.settings.preferences) || p.settings.revision<1 || p.policy.revision<1 || p.catalog_revision<1 ||
        p.arrival_fence<0 || p.continuation_revision<0 || p.queues.empty() || p.queues.size()>1000 || p.catalog.size()>999 || p.next_queue>=p.queues.size() || p.queues.front().echo.value!=0)return false;
    std::set<std::int64_t> unique;
    for(const auto& queue:p.queues)if(queue.echo.value<0 || queue.preference_revision<1 || queue.initial_highest_opened<0 || queue.after_arrival<0 || queue.after_arrival>p.arrival_fence || !unique.insert(queue.echo.value).second)return false;
    return true;
}
void check_plan(sqlite3* db,const app::MessageActor& a,const app::QwkPacketPlan& p){
    const auto current=current_plan(db,a,p.operation);
    if(current.netmail_conference!=p.netmail_conference || current.policy.revision!=p.policy.revision || current.policy.board_id!=p.policy.board_id || current.arrival_fence<p.arrival_fence ||
        !same_settings(current.settings,p.settings) || current.queues.size()!=p.queues.size() || sodium_memcmp(current.selection_fingerprint.data(),p.selection_fingerprint.data(),32)!=0)message_detail::conflict();
    for(std::size_t n=0;n<p.queues.size();++n){const auto& before=p.queues[n];const auto& now=current.queues[n];
        if(!(before.echo==now.echo) || before.preference_revision!=now.preference_revision || before.unread!=now.unread || before.to_you!=now.to_you || before.initial_highest_opened>now.initial_highest_opened)message_detail::conflict();}
}
std::optional<app::MessageId> next_id(sqlite3* db,const app::MessageActor& a,const app::QwkPacketPlan& p,std::size_t index){
    const auto& queue=p.queues[index];
    if(queue.echo.value==0){
        if(p.settings.preferences.private_mail==app::QwkPrivateMode::none)return {};
        SqliteStatement q(db,message_detail::active_message_sql(db,"SELECT m.message_id FROM sfx_active_local_messages m JOIN sfx_message_recipient_state r ON r.message_id=m.message_id WHERE m.kind=1 AND m.recipient_account_id=?1 AND r.recipient_account_id=?1 AND r.inbox_removed=0 AND (?4=1 OR r.opened=0) AND m.arrival_ordinal>?2 AND m.arrival_ordinal<=?3 ORDER BY m.arrival_ordinal,m.message_id LIMIT 1").c_str(),"peek only caller received private queue");
        q.bind(1,a.account);q.bind(2,queue.after_arrival);q.bind(3,p.arrival_fence);q.bind(4,p.settings.preferences.private_mail==app::QwkPrivateMode::all_received);
        return q.row()?std::optional<app::MessageId>{{q.int64(0)}}:std::optional<app::MessageId>{};
    }
    const auto current=message_detail::read_preference(db,a,queue.echo);
    // Prove emptiness with the existing echo index before the ordered global
    // arrival seek. Empty/exhausted areas otherwise rescan unrelated arrivals
    // on every fair turn. This exact predicate uses the same fresh snapshot,
    // online read progress and finite fence; no empty result is cached.
    SqliteStatement eligible(db,message_detail::active_message_sql(db,"SELECT 1 FROM sfx_active_local_messages m LEFT JOIN sfx_message_recipient_state r ON r.message_id=m.message_id WHERE m.kind=0 AND m.echo_id=?1 AND m.arrival_ordinal>?2 AND m.arrival_ordinal<=?3 AND (?4=1 OR m.author_account_id IS NULL OR m.author_account_id<>?5) AND ((?6=1 AND m.area_number>?7) OR (?8=1 AND m.recipient_account_id=?5 AND r.opened=0)) LIMIT 1").c_str(),"check current finite public queue eligibility using area index");
    eligible.bind(1,queue.echo.value);eligible.bind(2,queue.after_arrival);eligible.bind(3,p.arrival_fence);eligible.bind(4,p.settings.preferences.own_public_posts);eligible.bind(5,a.account);eligible.bind(6,queue.unread);eligible.bind(7,std::max(queue.initial_highest_opened,current.highest_opened));eligible.bind(8,queue.to_you);
    if(!eligible.row())return {};
    // echo_id is a STRICT INTEGER (or NULL). Adding zero keeps its exact
    // filtering semantics, but prevents the OR optimizer choosing echo/number
    // scans plus a full sort for EVERY next record. The existing UNIQUE arrival
    // index instead seeks the finite ordered range. No new index/migration,
    // authority shortcut, changed fence, or skipped eligible record is involved.
    SqliteStatement q(db,message_detail::active_message_sql(db,"SELECT m.message_id FROM sfx_active_local_messages m LEFT JOIN sfx_message_recipient_state r ON r.message_id=m.message_id WHERE m.kind=0 AND (m.echo_id+0)=?1 AND m.arrival_ordinal>?2 AND m.arrival_ordinal<=?3 AND (?4=1 OR m.author_account_id IS NULL OR m.author_account_id<>?5) AND ((?6=1 AND m.area_number>?7) OR (?8=1 AND m.recipient_account_id=?5 AND r.opened=0)) ORDER BY m.arrival_ordinal,m.message_id LIMIT 1").c_str(),"peek public queue using shared scan flags and current online progress");
    q.bind(1,queue.echo.value);q.bind(2,queue.after_arrival);q.bind(3,p.arrival_fence);q.bind(4,p.settings.preferences.own_public_posts);q.bind(5,a.account);q.bind(6,queue.unread);q.bind(7,std::max(queue.initial_highest_opened,current.highest_opened));q.bind(8,queue.to_you);
    return q.row()?std::optional<app::MessageId>{{q.int64(0)}}:std::optional<app::MessageId>{};
}
} // namespace qwk_detail
application::StorageResult<application::QwkPacketPlan> SqliteAccountRepository::begin_qwk_packet(const application::MessageActor& a,application::Identifier128 operation){
    if(!operation.valid())return read_failure<application::QwkPacketPlan>(invalid("validate packet preparation operation"));
    return impl_->readers_.read<application::QwkPacketPlan>("capture/reconcile finite packet plan without Read/progress writes",[a,operation](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);auto plan=qwk_detail::current_plan(db,a,operation);
        SqliteStatement previous(db,"SELECT revision,settings_revision,selection_fingerprint,arrival_fence,next_queue,private_after_arrival FROM sfx_qwk_continuation WHERE account_id=?1","read compact caller continuation");previous.bind(1,a.account);
        if(!previous.row())return plan;
        plan.continuation_revision=previous.int64(0);const auto current=plan;const auto fingerprint=previous.blob(2);
        if(previous.int64(1)!=plan.settings.revision || fingerprint.size()!=32 || sodium_memcmp(fingerprint.data(),plan.selection_fingerprint.data(),32)!=0 || previous.int64(3)>plan.arrival_fence || previous.int64(4)>=static_cast<std::int64_t>(plan.queues.size()))return plan;
        plan.arrival_fence=previous.int64(3);plan.next_queue=static_cast<std::uint16_t>(previous.int64(4));plan.queues[0].after_arrival=previous.int64(5);
        for(std::size_t n=1;n<plan.queues.size();++n){auto& queue=plan.queues[n];SqliteStatement q(db,"SELECT scan_revision,initial_highest_opened,after_arrival FROM sfx_qwk_continuation_echo WHERE account_id=?1 AND echo_id=?2","read bounded public continuation cursor");q.bind(1,a.account);q.bind(2,queue.echo.value);
            if(!q.row() || q.int64(0)!=queue.preference_revision || q.int64(1)>queue.initial_highest_opened){auto reset=current;reset.continuation_revision=plan.continuation_revision;return reset;}queue.initial_highest_opened=q.int64(1);queue.after_arrival=q.int64(2);}
        if(!qwk_detail::valid_plan(a,plan))throw StorageFailure(StorageErrorCode::corrupt,"invalid durable packet continuation",SQLITE_CORRUPT);
        // Retention or concurrent online reading can exhaust an old captured
        // batch. Rebase locally so later arrivals cannot be stranded behind it.
        // No continuation/progress write occurs until an actual delivery commit.
        for(std::size_t n=0;n<plan.queues.size();++n)if(qwk_detail::next_id(db,a,plan,n))return plan;
        auto reset=current;reset.continuation_revision=plan.continuation_revision;
        // Keep completed public delivery cursors across unchanged batches. A
        // public addressed item intentionally remains recipient-Unread, so
        // discarding this cursor would export it forever through To-you alone.
        // Explicit selection/pointer changes invalidate the fingerprint above
        // and intentionally start a new selection. All-received private mail
        // may start a new complete mailbox batch only after its old one ended.
        for(std::size_t n=1;n<reset.queues.size();++n)reset.queues[n].after_arrival=plan.queues[n].after_arrival;
        return reset;
    });
}
application::StorageResult<std::optional<application::QwkMessageCandidate>> SqliteAccountRepository::next_qwk_candidate(const application::MessageActor& a,const application::QwkPacketPlan& plan,std::size_t index){
    if(!qwk_detail::valid_plan(a,plan)||index>=plan.queues.size())return read_failure<std::optional<application::QwkMessageCandidate>>(invalid("validate packet candidate cursor"));
    return impl_->readers_.read<std::optional<application::QwkMessageCandidate>>("peek currently authorized packet identity without body",[a,plan,index](sqlite3* db)->std::optional<application::QwkMessageCandidate>{
        message_detail::ReadSnapshot snapshot(db);qwk_detail::check_plan(db,a,plan);
        const auto next=qwk_detail::next_id(db,a,plan,index);if(!next)return {};
        SqliteStatement arrival(db,message_detail::active_message_sql(db,"SELECT arrival_ordinal FROM sfx_active_local_messages WHERE message_id=?1").c_str(),"read selected immutable packet arrival");arrival.bind(1,next->value);
        if(!arrival.row()||arrival.int64(0)<=0||arrival.int64(0)>plan.arrival_fence)message_detail::unavailable();
        return application::QwkMessageCandidate{*next,arrival.int64(0)};
    });
}
application::StorageResult<std::optional<application::OpenedMessage>> SqliteAccountRepository::next_qwk_message(const application::MessageActor& a,const application::QwkPacketPlan& plan,std::size_t index){
    if(!qwk_detail::valid_plan(a,plan) || index>=plan.queues.size())return read_failure<std::optional<application::OpenedMessage>>(invalid("validate packet queue cursor"));
    return impl_->readers_.read<std::optional<application::OpenedMessage>>("load currently authorized packet message without Read/progress",[a,plan,index](sqlite3* db)->std::optional<application::OpenedMessage>{
        message_detail::ReadSnapshot snapshot(db);qwk_detail::check_plan(db,a,plan);const auto next=qwk_detail::next_id(db,a,plan,index);if(!next)return {};
        auto result=message_detail::load(db,a,message_detail::actor(db,a),*next);
        if(result.header.target.kind==application::MessageKind::private_local && (result.header.target.recipient!=a.account || result.header.inbox_removed))message_detail::unavailable();
        return result;
    });
}
application::StorageResult<application::QwkQueueSelection<application::QwkMessageCandidate>> SqliteAccountRepository::select_qwk_candidate(const application::MessageActor& a,const application::QwkPacketPlan& plan,std::size_t index,std::size_t maximum){
    using Selection=application::QwkQueueSelection<application::QwkMessageCandidate>;
    if(!qwk_detail::valid_plan(a,plan)||index>=plan.queues.size()||maximum==0||maximum>application::kQwkSelectionQueuesMaximum||maximum>plan.queues.size())return read_failure<Selection>(invalid("validate bounded packet candidate selection"));
    return impl_->readers_.read<Selection>("select finite fair packet identity in one fresh authorized snapshot",[a,plan,index,maximum](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);qwk_detail::check_plan(db,a,plan);Selection result;
        for(std::size_t n=0;n<maximum;++n){
            result.queue=(index+n)%plan.queues.size();++result.scanned;
            const auto next=qwk_detail::next_id(db,a,plan,result.queue);if(!next)continue;
            SqliteStatement arrival(db,message_detail::active_message_sql(db,"SELECT arrival_ordinal FROM sfx_active_local_messages WHERE message_id=?1").c_str(),"read selected immutable packet arrival");arrival.bind(1,next->value);
            if(!arrival.row()||arrival.int64(0)<=0||arrival.int64(0)>plan.arrival_fence)message_detail::unavailable();
            result.selected=application::QwkMessageCandidate{*next,arrival.int64(0)};break;
        }
        return result;
    });
}
application::StorageResult<application::QwkQueueSelection<application::OpenedMessage>> SqliteAccountRepository::select_qwk_message(const application::MessageActor& a,const application::QwkPacketPlan& plan,std::size_t index,std::size_t maximum){
    using Selection=application::QwkQueueSelection<application::OpenedMessage>;
    if(!qwk_detail::valid_plan(a,plan)||index>=plan.queues.size()||maximum==0||maximum>application::kQwkSelectionQueuesMaximum||maximum>plan.queues.size())return read_failure<Selection>(invalid("validate bounded packet message selection"));
    return impl_->readers_.read<Selection>("load one finite fair packet message in a fresh authorized snapshot",[a,plan,index,maximum](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);qwk_detail::check_plan(db,a,plan);Selection result;
        for(std::size_t n=0;n<maximum;++n){
            result.queue=(index+n)%plan.queues.size();++result.scanned;
            const auto next=qwk_detail::next_id(db,a,plan,result.queue);if(!next)continue;
            auto opened=message_detail::load(db,a,message_detail::actor(db,a),*next);
            if(opened.header.target.kind==application::MessageKind::private_local&&(opened.header.target.recipient!=a.account||opened.header.inbox_removed))message_detail::unavailable();
            result.selected=std::move(opened);break;
        }
        return result;
    });
}
application::StorageResult<bool> SqliteAccountRepository::validate_qwk_packet_authority(const application::MessageActor& a,const application::QwkPacketPlan& plan){
    if(!qwk_detail::valid_plan(a,plan))return read_failure<bool>(invalid("validate immutable download selection"));
    return impl_->readers_.read<bool>("recheck current prepared download authority without writes",[a,plan](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);qwk_detail::check_plan(db,a,plan);return true;
    });
}
