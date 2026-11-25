namespace ftn_detail {
application::FtnConfiguration config(sqlite3*);
application::FtnConfiguration stored_config(sqlite3*);
application::FtnNetwork& network(application::FtnConfiguration&,const std::string&);
bool same(std::string,std::string);
}
namespace message_detail {
namespace app = sfx::application;
[[noreturn]] void unavailable() {
    throw StorageFailure(StorageErrorCode::not_found, "message unavailable", SQLITE_NOTFOUND);
}
[[noreturn]] void conflict() {
    throw StorageFailure(StorageErrorCode::conflict, "refresh message context", SQLITE_ABORT);
}
struct Authority { std::int64_t level{}, threshold{}, revision{}; };
bool can_delete_for_everyone(const Authority& authority) {
    return authority.level==999 || authority.level>=authority.threshold;
}
bool has_list_deletion_schema(sqlite3* db) {
    SqliteStatement version(db,"PRAGMA user_version","read message list deletion schema");
    if (!version.row()) unavailable();
    return version.int64(0)>=31;
}
std::string active_message_sql(sqlite3* db, std::string sql) {
    // Test/upgrade guests may open an approved predecessor. Such databases
    // have no reversible deletions and the original table is the live source.
    if (!has_list_deletion_schema(db)) {
        constexpr std::string_view active="sfx_active_local_messages";
        constexpr std::string_view original="sfx_local_messages";
        for (auto index=sql.find(active);index!=std::string::npos;index=sql.find(active,index+original.size()))
            sql.replace(index,active.size(),original);
    }
    return sql;
}
struct ListDeletion { bool everyone{}, inbox{}; };
ListDeletion list_deletion(sqlite3* db,app::MessageId id) {
    SqliteStatement query(db,"SELECT scope FROM sfx_message_list_deletion WHERE message_id=?1",
        "read durable reversible message deletion");
    query.bind(1,id.value);
    ListDeletion result;
    while (query.row()) {
        if (query.int64(0)==0) result.everyone=true;
        else result.inbox=true;
    }
    return result;
}
bool permanent_flag(sqlite3* db,app::MessageId id){
    SqliteStatement version(db,"PRAGMA user_version","read Permanent schema version");
    if(!version.row() || version.int64(0)<13)return false;
    SqliteStatement q(db,"SELECT permanent FROM sfx_message_permanent WHERE message_id=?1","read public Permanent flag");
    q.bind(1,id.value);if(!q.row())return false;
    const auto value=q.int64(0);if(value!=0 && value!=1)throw StorageFailure(StorageErrorCode::corrupt,"invalid Permanent flag",SQLITE_CORRUPT);
    return value==1;
}
Authority actor(sqlite3* db, const app::MessageActor& value, bool revisions = true) {
    if (!value.session.valid() || value.account <= 0 || value.account_revision <= 0 || value.policy_revision <= 0)
        unavailable();
    SqliteStatement q(db,
        "SELECT a.security_level,p.sysop_threshold,a.revision,p.revision FROM sfx_accounts a "
        "JOIN sfx_account_sessions s ON s.account_id=a.account_id JOIN sfx_security_policy p "
        "JOIN sfx_security_levels l ON l.security_level=a.security_level "
        "WHERE a.account_id=?1 AND s.session_id=?2 AND s.completed_utc_seconds IS NULL "
        "AND a.account_state='active' AND l.level_state='active' "
        "AND p.published_revision=p.revision AND p.published_policy_hash=p.policy_hash",
        "verify message actor");
    q.bind(1, value.account); q.bind(2, value.session);
    if (!q.row()) unavailable();
    if (revisions && (q.int64(2) != value.account_revision || q.int64(3) != value.policy_revision)) conflict();
    return {q.int64(0), q.int64(1), q.int64(2)};
}
struct EchoAuthority { std::int64_t group_revision{}, echo_revision{}; };
EchoAuthority echo(sqlite3* db, app::LocalEchoId id, const Authority& a, bool reading, bool posting) {
    SqliteStatement q(db,
        "SELECT g.revision,e.revision FROM sfx_local_echoes e JOIN sfx_message_groups g ON g.group_id=e.group_id "
        "WHERE e.echo_id=?1 AND g.enabled=1 AND e.enabled=1 AND g.access_minimum<=?2 "
        "AND e.access_minimum<=?2 AND (?3=0 OR e.reading_minimum<=?2) AND (?4=0 OR e.posting_minimum<=?2)",
        "verify message echo authority");
    q.bind(1, id.value); q.bind(2, a.level); q.bind(3, reading); q.bind(4, posting);
    if (!q.row()) unavailable();
    return {q.int64(0), q.int64(1)};
}
std::string bytes(SqliteStatement& q, int column) {
    const auto raw = q.blob(column);
    return {raw.begin(), raw.end()};
}
std::array<std::uint8_t,32> source_digest(std::string_view bytes){
    std::array<std::uint8_t,32> digest{};
    if(sodium_init()<0 || crypto_hash_sha256(digest.data(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size())!=0)
        throw StorageFailure(StorageErrorCode::internal,"digest message source",SQLITE_ERROR);
    return digest;
}
std::pair<std::int64_t,bool> presentation(sqlite3* db,const app::MessageTarget& target){
    SqliteStatement version(db,"PRAGMA user_version","read message presentation schema");
    if(!version.row())unavailable();
    if(version.int64(0)<12)return {0,false};
    SqliteStatement p(db,target.kind==app::MessageKind::public_echo?
        "SELECT revision,renegade_pipe_colors FROM sfx_local_echoes WHERE echo_id=?1":
        "SELECT revision,private_pipe_colors FROM sfx_message_presentation_policy WHERE singleton_id=?1","read current message presentation policy");
    p.bind(1,target.kind==app::MessageKind::public_echo?target.echo.value:1);
    if(!p.row())unavailable();
    return {p.int64(0),p.boolean(1)};
}
std::string visible(const app::MessageCanvas& c){auto text=c.text.canonical_lf;if(!text.empty())text.pop_back();return text;}
bool valid_source_request(const app::CommitMessageRequest& r){
    return !r.original_source || (r.body.empty() && r.context.presentation_revision>0 &&
        static_cast<bool>(app::validate_message_source(*r.original_source,r.context.maximum_lines)));
}
std::uint16_t visible_conference_number(sqlite3* db,app::LocalEchoId id,const Authority& a) {
    SqliteStatement q(db,"SELECT count(*) FROM sfx_local_echoes e JOIN sfx_message_groups g USING(group_id) JOIN sfx_local_echoes target ON target.group_id=e.group_id WHERE target.echo_id=?1 AND g.enabled=1 AND e.enabled=1 AND g.access_minimum<=?2 AND e.access_minimum<=?2 AND e.reading_minimum<=?2 AND (e.display_order,e.echo_id)<=(target.display_order,target.echo_id)","derive caller group conference position");
    q.bind(1,id.value);q.bind(2,a.level);if(!q.row() || q.int64(0)<1 || q.int64(0)>999)unavailable();return static_cast<std::uint16_t>(q.int64(0));
}
app::OpenedMessage load(sqlite3* db, const app::MessageActor& caller, const Authority& a, app::MessageId id,
    bool body=true, bool list_review=false) {
    const std::string sql=
        "SELECT m.message_id,m.kind,m.echo_id,m.area_number,m.author_account_id,m.recipient_account_id,"
        "m.subject,CASE WHEN ?2=1 THEN m.body ELSE NULL END,m.body_lines,m.arrival_ordinal,m.created_utc_seconds,m.parent_message_id,"
        "coalesce(fm.from_name,author.display_name,'Removed caller'),coalesce(recipient.display_name,'Removed caller'),"
        "coalesce(r.opened,0),coalesce(r.inbox_removed,0),coalesce(r.opened_sequence,0),"
        "coalesce(g.display_order,0),coalesce(g.group_id,0),coalesce(e.display_order,0),coalesce(g.name,''),coalesce(e.name,''),coalesce(e.conference_number,0),ir.network,ir.origin,fm.to_name,t.network,t.source_aka,t.destination,t.recipient_name FROM " +
        std::string(list_review ? "sfx_local_messages" : "sfx_active_local_messages") + " m " +
        "LEFT JOIN sfx_accounts author ON author.account_id=m.author_account_id "
        "LEFT JOIN sfx_accounts recipient ON recipient.account_id=m.recipient_account_id "
        "LEFT JOIN sfx_local_echoes e ON e.echo_id=m.echo_id LEFT JOIN sfx_message_groups g ON g.group_id=e.group_id "
        "LEFT JOIN sfx_message_recipient_state r ON r.message_id=m.message_id "
        "LEFT JOIN sfx_ftn_message_metadata fm ON fm.message_id=m.message_id LEFT JOIN sfx_ftn_import_receipts ir ON ir.import_id=fm.import_id AND ir.original_message_id=m.message_id "
        "LEFT JOIN sfx_ftn_message_targets t ON t.message_id=m.message_id WHERE m.message_id=?1";
    SqliteStatement q(db,(list_review ? sql : active_message_sql(db,sql)).c_str(),"read authorized local message");
    q.bind(1, id.value); q.bind(2,body);
    if (!q.row()) unavailable();
    app::OpenedMessage result;
    auto& h = result.header;
    h.identity = {{q.int64(0)}, q.is_null(3) ? 0 : q.int64(3), q.int64(9), q.int64(10)};
    if(q.int64(1)<0||q.int64(1)>2)throw StorageFailure(StorageErrorCode::corrupt,"invalid message kind",SQLITE_CORRUPT);
    h.target.kind = static_cast<app::MessageKind>(q.int64(1));
    h.target.echo = {q.is_null(2) ? 0 : q.int64(2)};
    if(!q.is_null(4))h.author = q.int64(4);
    if (!q.is_null(5)) h.target.recipient = q.int64(5);
    h.recipient_opened = q.boolean(14); h.inbox_removed = q.boolean(15);
    h.recipient_opened_sequence=q.int64(16);
    h.position={false,q.int64(17),q.int64(18),q.int64(19),h.target.echo.value,h.identity.area_number,id.value,h.identity.arrival_ordinal};
    h.group_name=q.text(20);h.echo_name=q.text(21);h.conference_number=static_cast<std::uint16_t>(q.int64(22));
    if (h.target.kind == app::MessageKind::public_echo) {echo(db, h.target.echo, a, true, false);h.conference_number=visible_conference_number(db,h.target.echo,a);}
    else if (h.author != caller.account && a.level < a.threshold &&
        (h.target.recipient != caller.account ||
            (h.inbox_removed && !(list_review && list_deletion(db,id).inbox)))) unavailable();
    h.subject = bytes(q,6); h.from = q.text(12);
    if(app::validate_message_subject(h.subject,true,h.author?app::kMessageSubjectCells:71)!=app::MessageValidation::valid)
        throw StorageFailure(StorageErrorCode::corrupt,"invalid stored message subject",SQLITE_CORRUPT);
    if(!h.author){if(q.is_null(23)||q.is_null(24))throw StorageFailure(StorageErrorCode::corrupt,"remote message identity missing",SQLITE_CORRUPT);h.remote_author=app::MessageHeader::RemoteAuthor{q.text(23),q.text(24),h.from};}
    if(!q.is_null(26))h.target.remote=app::MessageRemoteTarget{q.text(26),q.text(27),q.text(28),q.text(29)};
    if((h.author&&!q.is_null(25))||(!h.author&&h.target.remote)||(h.target.kind==app::MessageKind::private_local&&h.target.remote))throw StorageFailure(StorageErrorCode::corrupt,"mixed local and remote message identity",SQLITE_CORRUPT);
    if(h.target.kind==app::MessageKind::private_ftn&&(!h.author||h.target.recipient||!h.target.remote))throw StorageFailure(StorageErrorCode::corrupt,"invalid outgoing FTN target",SQLITE_CORRUPT);
    h.to = h.target.remote?h.target.remote->name:!q.is_null(25)?q.text(25):h.target.recipient?q.text(13):"All Callers";
    if(h.target.kind==app::MessageKind::private_ftn){
        SqliteStatement status(db,"SELECT i.state,EXISTS(SELECT 1 FROM sfx_ftn_message_jobs j JOIN sfx_ftn_outbound o USING(job_id) WHERE j.message_id=i.message_id AND j.network=i.network AND o.state=4),EXISTS(SELECT 1 FROM sfx_ftn_message_jobs j JOIN sfx_ftn_outbound o USING(job_id) WHERE j.message_id=i.message_id AND j.network=i.network AND o.state=5),EXISTS(SELECT 1 FROM sfx_ftn_message_jobs j WHERE j.message_id=i.message_id AND j.network=i.network),NOT EXISTS(SELECT 1 FROM sfx_ftn_message_jobs j JOIN sfx_ftn_outbound o USING(job_id) WHERE j.message_id=i.message_id AND j.network=i.network AND o.state<>3) FROM sfx_ftn_message_intents i JOIN sfx_ftn_message_targets t ON t.message_id=i.message_id AND t.network=i.network AND t.operation_id=i.operation_id JOIN sfx_message_commit_receipts r ON r.operation_id=t.operation_id AND r.message_id=t.message_id AND r.actor_account_id=?3 WHERE i.message_id=?1 AND i.network=?2","read truthful sender-only FTN link outcome");status.bind(1,id.value);status.bind(2,h.target.remote->network);status.bind(3,*h.author);if(!status.row())throw StorageFailure(StorageErrorCode::corrupt,"outgoing FTN intent missing",SQLITE_CORRUPT);
        h.remote_delivery=status.int64(0)==2||status.boolean(1)?"Held":status.boolean(2)?"Handed to external mailer":status.boolean(3)&&status.boolean(4)?"Sent to link":"Queued";
    }
    if (!q.is_null(11)) h.parent = app::MessageId{q.int64(11)};
    if(h.target.kind==app::MessageKind::public_echo)h.permanent=permanent_flag(db,id);
    const auto policy=presentation(db,h.target);result.pipe_colors=policy.second;
    // The FTN echo flag governs local outgoing posts only, never reading.

    if(body && policy.first){
        SqliteStatement source(db,"SELECT encoding,format,profile_version,source,source_sha256,extent_off,extent_on,charged_extent,visible_off,visible_on FROM sfx_message_sources WHERE message_id=?1","read immutable original message source");source.bind(1,id.value);
        if(source.row()){
            app::MessageSource original{static_cast<app::MessageSourceFormat>(source.int64(1)),static_cast<std::uint8_t>(source.int64(2)),bytes(source,3)};
            const auto derived=app::validate_message_source(original,app::kMaximumMessageLines);const auto hash=source_digest(original.bytes);const auto stored=source.blob(4);
            if(source.text(0)!="cp437" || source.int64(2)!=1 || !derived || stored.size()!=32 || sodium_memcmp(hash.data(),stored.data(),32)!=0 ||
                derived.off.extent!=source.int64(5) || derived.on.extent!=source.int64(6) || derived.charged_extent!=source.int64(7) ||
                visible(derived.off)!=bytes(source,8) || visible(derived.on)!=bytes(source,9) ||
                bytes(q,7)!=derived.off.text.canonical_lf || q.int64(8)!=derived.off.extent)
                throw StorageFailure(StorageErrorCode::corrupt,"invalid immutable message source",SQLITE_CORRUPT);
            result.original_source=std::move(original);result.body=policy.second?derived.on.text:derived.off.text;
        }else result.body=app::normalize_message_body(bytes(q,7),app::kMaximumMessageLines);
    }else if(body)result.body=app::normalize_message_body(bytes(q,7),app::kMaximumMessageLines);
    if (body && !result.original_source && (!result.body || result.body.rows.size() != static_cast<std::size_t>(q.int64(8)) ||
        app::validate_message_save(h.subject.size()==71&&h.remote_author?std::string_view{}:std::string_view(h.subject),result.body) != app::MessageValidation::valid)
        )
        throw StorageFailure(StorageErrorCode::corrupt,"invalid stored local message",SQLITE_CORRUPT);
    if(body && !result.original_source && policy.second){
        const auto rendered=app::message_plain_canvas(result.body,true);
        if(!rendered)throw StorageFailure(StorageErrorCode::corrupt,"invalid plain message rendition",SQLITE_CORRUPT);
        result.plain_rendition_source=result.body;result.body=rendered->text;
    }
    if(body&&h.remote_author){
        // Authorize and validate the immutable stored source above first. A
        // bounded v2 view can recover old plain fallbacks without rewriting
        // any source, identity, receipt, read status or original wire bytes.
        SqliteStatement wire(db,"SELECT wire_text,wire_to FROM sfx_ftn_message_metadata WHERE message_id=?1","derive received ANSI view from retained original");wire.bind(1,id.value);
        if(wire.row()){app::FtnPackedMessage original;original.text=bytes(wire,0);original.to=bytes(wire,1);
            const auto address=app::parse_ftn_address(h.remote_author->address);
            if(auto canvas=app::ftn_received_ansi_canvas(original,policy.second,address?address->domain:std::string{}))result.received_canvas=std::make_shared<const app::MessageCanvas>(std::move(*canvas));
            if(result.received_canvas)result.body=result.received_canvas->text;
        }
    }
    return result;
}
#include "sqlite_message_ftn.inc"
app::ComposeContext begin(sqlite3* db, const app::MessageActor& caller, const app::MessageTarget& target,
    std::optional<app::MessageId> parent) {
    const auto a = actor(db,caller);
    if ((target.kind != app::MessageKind::public_echo && target.kind != app::MessageKind::private_local && target.kind != app::MessageKind::private_ftn) ||
        (target.kind == app::MessageKind::public_echo ? target.echo.value<=0 : target.echo.value!=0 || (target.kind==app::MessageKind::private_local?!target.recipient:target.recipient.has_value()||!target.remote)) ||
        (target.recipient && *target.recipient<=0)) unavailable();
    app::ComposeContext result; result.target=target; result.parent=parent;
    if(target.remote)result.ftn_configuration_revision=remote_context(db,target);
    const auto policy=presentation(db,target);result.presentation_revision=policy.first;result.pipe_colors=policy.second;
    if (target.kind == app::MessageKind::public_echo) {
        const auto access=echo(db,target.echo,a,false,true);
        result.group_revision=access.group_revision; result.echo_revision=access.echo_revision;
    }
    if (target.recipient) {
        SqliteStatement q(db,"SELECT revision FROM sfx_accounts WHERE account_id=?1 AND account_state='active'",
            "verify local recipient");
        q.bind(1,*target.recipient); if (!q.row()) unavailable(); result.recipient_revision=q.int64(0);
    }
    if (target.sysop_assignment_revision) {
        if (target.kind != app::MessageKind::private_local) unavailable();
        SqliteStatement q(db,"SELECT account_id,revision FROM sfx_system_sysop WHERE singleton_id=1", "verify System Sysop assignment");
        if (!q.row() || q.is_null(0) || q.int64(0)!=*target.recipient || q.int64(1)!=*target.sysop_assignment_revision) conflict();
        SqliteStatement eligible(db,"SELECT 1 FROM sfx_accounts WHERE account_id=?1 AND account_state='active' AND security_level>=?2", "verify System Sysop eligibility");
        eligible.bind(1,*target.recipient); eligible.bind(2,a.threshold); if (!eligible.row()) unavailable();
    }
    SqliteStatement limits(db,"SELECT maximum_lines,revision FROM sfx_security_message_limits WHERE security_level=?1",
        "load authoritative message limits");
    limits.bind(1,a.level); if (!limits.row()) unavailable();
    result.maximum_lines=static_cast<std::uint32_t>(limits.int64(0)); result.limits_revision=limits.int64(1);
    if (parent) {
        const auto original=load(db,caller,a,*parent).header;
        if(!reply_target_valid(target,caller.account,original,a.level>=a.threshold))unavailable();
    }
    else if(target.kind==app::MessageKind::public_echo&&target.remote)unavailable();
    if (target.kind == app::MessageKind::public_echo) {
        SqliteStatement version(db,"PRAGMA user_version","check FTN posting policy availability");
        if (!version.row()) unavailable();
        if (version.int64(0) < 22) return result;
        const auto configuration = ftn_detail::stored_config(db);
        const auto needs_real_name = std::any_of(configuration.message_echoes.begin(),
            configuration.message_echoes.end(), [&](const auto& mapped) {
                if (mapped.local_echo != target.echo.value || !mapped.require_real_name) return false;
                return std::any_of(configuration.networks.begin(),configuration.networks.end(),
                    [&](const auto& network){return network.enabled && ftn_detail::same(network.code,mapped.network);});
            });
        if (needs_real_name) {
            SqliteStatement profile(db,"SELECT real_name FROM sfx_account_profiles WHERE account_id=?1",
                "check Real Name for FTN posting");
            profile.bind(1,caller.account);
            const auto name = profile.row() ? app::qwk_utf8_to_cp437(profile.text(0)) : std::nullopt;
            if (!name || name->empty() || name->size() > 35)
                throw StorageFailure(app::StorageErrorCode::permission_denied,app::kEchoRealNameRequired);
        }
    }
    return result;
}
std::array<std::uint8_t,32> fingerprint(const app::CommitMessageRequest& request) {
    // Domain-separated, length-framed stable canonical fields. No private copy
    // is retained outside this call; the durable receipt stores only the hash.
    std::string canonical=request.context.target.remote?"SFX/FTNMessageCommit/v1":request.original_source?"SFX/LocalMessageCommit/v2":"SFX/LocalMessageCommit/v1";
    const auto number=[&](std::int64_t value) { for(int shift=56;shift>=0;shift-=8) canonical.push_back(static_cast<char>(static_cast<std::uint64_t>(value)>>shift)); };
    const auto text=[&](std::string_view value) { number(static_cast<std::int64_t>(value.size())); canonical.append(value); };
    const auto& c=request.context;
    number(request.actor.account); number(static_cast<std::int64_t>(c.target.kind)); number(c.target.echo.value);
    number(c.target.recipient.value_or(0)); number(c.target.sysop_assignment_revision.value_or(0));
    number(c.group_revision); number(c.echo_revision); number(c.recipient_revision); number(c.limits_revision);
    number(c.maximum_lines); number(c.parent ? c.parent->value : 0); text(request.subject);
    if(c.target.remote){const auto& remote=*c.target.remote;number(c.ftn_configuration_revision);text(remote.network);text(remote.source_aka);text(remote.address);text(remote.name);number(request.original_source.has_value());}
    if(request.original_source){
        const auto& source=*request.original_source;const auto derived=app::validate_message_source(source,c.maximum_lines);
        if(!derived)conflict();
        number(c.presentation_revision);number(c.pipe_colors);number(static_cast<unsigned>(source.format));number(source.profile);text(source.bytes);number(derived.charged_extent);
        for(const auto* canvas:{&derived.off,&derived.on}){const auto hash=source_digest(visible(*canvas));text({reinterpret_cast<const char*>(hash.data()),hash.size()});}
    }else text(request.body);
    std::array<std::uint8_t,32> digest{};
    if (sodium_init()<0 || crypto_hash_sha256(digest.data(),reinterpret_cast<const unsigned char*>(canonical.data()),canonical.size())!=0)
        throw StorageFailure(StorageErrorCode::internal,"fingerprint local message",SQLITE_ERROR);
    sodium_memzero(canonical.data(),canonical.size());
    return digest;
}
std::optional<app::CommittedMessage> receipt(sqlite3* db, const app::CommitMessageRequest& request,
    const std::array<std::uint8_t,32>& digest) {
    SqliteStatement purged(db,"SELECT actor_account_id,request_fingerprint FROM sfx_message_purged_receipts WHERE operation_id=?1","reject resurrection of purged message");purged.bind(1,request.operation);
    if(purged.row()) {
        const auto stored=purged.blob(1);
        if(purged.int64(0)!=request.actor.account || stored.size()!=digest.size() || sodium_memcmp(stored.data(),digest.data(),digest.size())!=0)conflict();
        unavailable(); // Failure is never reported as confirmed-not-committed.
    }
    SqliteStatement q(db,"SELECT r.actor_account_id,r.request_fingerprint,m.message_id,coalesce(m.area_number,0),m.arrival_ordinal,m.created_utc_seconds "
        "FROM sfx_message_commit_receipts r JOIN sfx_local_messages m ON m.message_id=r.message_id WHERE r.operation_id=?1", "reconcile local message receipt");
    q.bind(1,request.operation); if (!q.row()) return {};
    const auto stored=q.blob(1);
    if (q.int64(0)!=request.actor.account || stored.size()!=digest.size() || sodium_memcmp(stored.data(),digest.data(),digest.size())!=0) conflict();
    return app::CommittedMessage{{q.int64(2)},q.int64(3),q.int64(4),q.int64(5)};
}
app::MessageScanPreference read_preference(sqlite3* db,const app::MessageActor& caller,app::LocalEchoId id) {
    SqliteStatement q(db,"SELECT highest_opened,preference_revision,unread_enabled,to_you_enabled FROM sfx_caller_echo_state WHERE account_id=?1 AND echo_id=?2", "read caller scan preference");
    q.bind(1,caller.account);q.bind(2,id.value);if(!q.row())return {id,0,1,true,true};
    return {id,q.int64(0),q.int64(1),q.boolean(2),q.boolean(3)};
}
app::MessageScanPreference preference(sqlite3* db, const app::MessageActor& caller, app::LocalEchoId id) {
    echo(db,id,actor(db,caller),true,false);
    SqliteStatement init(db,"INSERT INTO sfx_caller_echo_state VALUES(?1,?2,0,1,1,1) ON CONFLICT(account_id,echo_id) DO NOTHING", "initialize caller echo state");
    init.bind(1,caller.account); init.bind(2,id.value); init.step();
    return read_preference(db,caller,id);
}
// One business insert for online commits and atomic offline batches. The caller
// must validate current context/content and own the existing writer transaction.
int insert_validated_message(sqlite3* db,const app::CommitMessageRequest& request,
    const app::MessageBody& body,const app::ValidatedMessageSource& derived,
    std::int64_t now,const std::array<std::uint8_t,32>& digest,
    std::optional<app::CommittedMessage>& output){
    const auto& old=request.context;std::int64_t area{};
    const auto reserved=prepare_remote_post(db,request,now);
    if(old.target.kind==app::MessageKind::public_echo){
        SqliteStatement q(db,"UPDATE sfx_local_echoes SET next_message_number=next_message_number+1 WHERE echo_id=?1 AND next_message_number<9223372036854775807 RETURNING next_message_number-1","allocate echo message number");
        q.bind(1,old.target.echo.value);if(!q.row())return SQLITE_FULL;area=q.int64(0);q.step();
    }
    SqliteStatement ordinal(db,"UPDATE sfx_message_counters SET last_arrival_ordinal=last_arrival_ordinal+1,total_committed=total_committed+1 WHERE singleton_id=1 AND last_arrival_ordinal<9223372036854775807 AND total_committed<9223372036854775807 RETURNING last_arrival_ordinal","allocate local arrival");
    if(!ordinal.row())return SQLITE_FULL;
    const auto arrival=ordinal.int64(0);ordinal.step();
    SqliteStatement save(db,"INSERT INTO sfx_local_messages(message_id,kind,echo_id,area_number,author_account_id,recipient_account_id,subject,body,encoding,body_lines,arrival_ordinal,created_utc_seconds,parent_message_id) VALUES(?12,?1,?2,?3,?4,?5,?6,?7,'cp437',?8,?9,?10,?11) RETURNING message_id","store local or explicitly addressed FTN message");
    if(reserved)save.bind(12,reserved);else save.bind_null(12);
    save.bind(1,static_cast<std::int64_t>(old.target.kind));
    if(area){save.bind(2,old.target.echo.value);save.bind(3,area);}else{save.bind_null(2);save.bind_null(3);}
    save.bind(4,request.actor.account);bind_optional_account_id(save,5,old.target.recipient);
    save.bind(6,std::vector<std::uint8_t>(request.subject.begin(),request.subject.end()));
    save.bind(7,std::vector<std::uint8_t>(body.canonical_lf.begin(),body.canonical_lf.end()));
    save.bind(8,static_cast<std::int64_t>(body.rows.size()));save.bind(9,arrival);save.bind(10,now);
    if(old.parent)save.bind(11,old.parent->value);else save.bind_null(11);
    if(!save.row())return SQLITE_ERROR;
    const auto id=save.int64(0);save.step();
    if(old.target.recipient){
        SqliteStatement state(db,"INSERT INTO sfx_message_recipient_state VALUES(?1,?2,0,0,0)","create independent addressed state");state.bind(1,id);state.bind(2,*old.target.recipient);state.step();
        SqliteStatement intent(db,"INSERT INTO sfx_message_arrival_intents VALUES(?1,?2)","commit addressed arrival intent");intent.bind(1,id);intent.bind(2,*old.target.recipient);intent.step();
    }
    SqliteStatement count(db,"INSERT INTO sfx_caller_message_statistics VALUES(?1,1) ON CONFLICT(account_id) DO UPDATE SET messages_entered=messages_entered+1","count successful message entry");count.bind(1,request.actor.account);count.step();
    SqliteStatement receipt(db,"INSERT INTO sfx_message_commit_receipts VALUES(?1,?2,?3,?4)","retain message commit receipt");receipt.bind(1,request.operation);receipt.bind(2,request.actor.account);receipt.bind(3,digest);receipt.bind(4,id);receipt.step();
    commit_ftn_intents(db,request,id,now);
    if(request.original_source){
        SqliteStatement scope(db,"INSERT INTO sfx_message_source_write_scope VALUES(?1,?2)","authorize atomic original source insert");scope.bind(1,id);scope.bind(2,request.operation);scope.step();
        SqliteStatement source(db,"INSERT INTO sfx_message_sources VALUES(?1,'cp437',?2,1,?3,?4,?5,?6,?7,?8,?9)","store immutable original source and derived text");const auto& raw=*request.original_source;
        source.bind(1,id);source.bind(2,static_cast<std::int64_t>(raw.format));source.bind(3,std::vector<std::uint8_t>(raw.bytes.begin(),raw.bytes.end()));source.bind(4,source_digest(raw.bytes));
        source.bind(5,derived.off.extent);source.bind(6,derived.on.extent);source.bind(7,derived.charged_extent);
        const auto off=visible(derived.off),on=visible(derived.on);source.bind(8,std::vector<std::uint8_t>(off.begin(),off.end()));source.bind(9,std::vector<std::uint8_t>(on.begin(),on.end()));source.step();
        SqliteStatement done(db,"DELETE FROM sfx_message_source_write_scope WHERE message_id=?1","close atomic source insert scope");done.bind(1,id);done.step();
    }
    output=app::CommittedMessage{{id},area,arrival,now};return SQLITE_OK;
}
// Each read operation spans one bounded consistent snapshot. Never hold this
// across terminal output or input; only owned database work runs inside it.
struct ReadSnapshot {
    sqlite3* db;
    explicit ReadSnapshot(sqlite3* value):db(value) {
        const int code=sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr);
        if(code!=SQLITE_OK) throw StorageFailure(map_sqlite_error(code),"begin message read snapshot",code);
    }
    ~ReadSnapshot(){sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);}
};
int accept_open(sqlite3* db,const app::MessageActor& caller,app::OpenedMessage& message) {
    if(message.header.target.kind==app::MessageKind::public_echo) {
        preference(db,caller,message.header.target.echo);
        SqliteStatement advance(db,"UPDATE sfx_caller_echo_state SET highest_opened=max(highest_opened,?1) WHERE account_id=?2 AND echo_id=?3", "advance ordinary scan monotonically");
        advance.bind(1,message.header.identity.area_number);advance.bind(2,caller.account);advance.bind(3,message.header.target.echo.value);advance.step();
    }
    if(message.header.target.recipient==caller.account && !message.header.recipient_opened) {
        SqliteStatement sequence(db,"UPDATE sfx_message_counters SET last_read_sequence=last_read_sequence+1 WHERE singleton_id=1 AND last_read_sequence<9223372036854775807 RETURNING last_read_sequence", "allocate monotonic recipient open sequence");
        if(!sequence.row())return SQLITE_FULL;
        const auto number=sequence.int64(0);sequence.step();
        SqliteStatement opened(db,"UPDATE sfx_message_recipient_state SET opened=1,opened_sequence=?3 WHERE message_id=?1 AND recipient_account_id=?2 AND opened=0", "mark actual recipient opened");
        opened.bind(1,message.header.identity.id.value);opened.bind(2,caller.account);opened.bind(3,number);opened.step();
        message.header.recipient_opened=true;message.header.recipient_opened_sequence=number;
    }
    return SQLITE_OK;
}
} // namespace message_detail

application::StorageResult<application::MessageCommitOutcome> SqliteAccountRepository::message_commit_outcome(const application::CommitMessageRequest& request) {
    if(!request.operation.valid())return read_failure<application::MessageCommitOutcome>(invalid("validate commit outcome request"));
    if(request.body.size()>application::kMessageBodyBytes || request.subject.size()>application::kMessageSubjectCells || !message_detail::valid_source_request(request))
        return read_failure<application::MessageCommitOutcome>(invalid("validate commit outcome bounds"));
    const auto digest=message_detail::fingerprint(request);
    return impl_->readers_.read<application::MessageCommitOutcome>("reconcile explicit commit outcome",[request,digest](sqlite3* db){
        message_detail::ReadSnapshot tx(db);message_detail::actor(db,request.actor,false);
        return application::MessageCommitOutcome{message_detail::receipt(db,request,digest)};
    });
}

application::StorageResult<std::vector<application::MessageGroup>> SqliteAccountRepository::message_groups(const application::MessageActor& caller) {
    return impl_->readers_.read<std::vector<application::MessageGroup>>("list message groups",[caller](sqlite3* db) {
        message_detail::ReadSnapshot snapshot(db); const auto a=message_detail::actor(db,caller);
        SqliteStatement q(db,"SELECT group_id,name,display_order,revision,access_minimum FROM sfx_message_groups WHERE enabled=1 AND access_minimum<=?1 ORDER BY display_order,group_id LIMIT 999", "list eligible message groups");
        q.bind(1,a.level); std::vector<application::MessageGroup> rows;
        while(q.row())rows.push_back({{q.int64(0)},q.text(1),q.int64(2),q.int64(3),static_cast<std::uint16_t>(q.int64(4)),true});
        return rows;
    });
}
application::StorageResult<std::vector<application::LocalEcho>> SqliteAccountRepository::local_echoes(const application::MessageActor& caller,application::MessageGroupId group) {
    return impl_->readers_.read<std::vector<application::LocalEcho>>("list local echoes",[caller,group](sqlite3* db) {
        message_detail::ReadSnapshot snapshot(db); const auto a=message_detail::actor(db,caller);
        SqliteStatement q(db,"SELECT e.echo_id,e.conference_number,e.name,e.display_order,e.revision,e.access_minimum,e.reading_minimum,e.posting_minimum FROM sfx_local_echoes e JOIN sfx_message_groups g ON g.group_id=e.group_id WHERE g.group_id=?1 AND g.enabled=1 AND e.enabled=1 AND g.access_minimum<=?2 AND e.access_minimum<=?2 AND e.reading_minimum<=?2 ORDER BY e.display_order,e.echo_id LIMIT 999", "list eligible local echoes");
        q.bind(1,group.value);q.bind(2,a.level);std::vector<application::LocalEcho> rows;
        while(q.row())rows.push_back({{q.int64(0)},group,static_cast<std::uint16_t>(q.int64(1)),q.text(2),q.int64(3),q.int64(4),{static_cast<std::uint16_t>(q.int64(5)),static_cast<std::uint16_t>(q.int64(6)),static_cast<std::uint16_t>(q.int64(7))},true});
        for(auto& e:rows)e.conference_number=message_detail::visible_conference_number(db,e.id,a);
        return rows;
    });
}
application::StorageResult<application::ComposeContext> SqliteAccountRepository::begin_message(const application::MessageActor& caller,const application::MessageTarget& target,std::optional<application::MessageId> parent) {
    return impl_->readers_.read<application::ComposeContext>("begin local message",[caller,target,parent](sqlite3* db) {
        message_detail::ReadSnapshot snapshot(db); return message_detail::begin(db,caller,target,parent);
    });
}
application::StorageResult<application::CommittedMessage> SqliteAccountRepository::commit_message(const application::CommitMessageRequest& request) {
    if(!message_detail::valid_source_request(request))return read_failure<application::CommittedMessage>(invalid("validate original message source"));
    const auto derived=request.original_source?application::validate_message_source(*request.original_source,request.context.maximum_lines):application::ValidatedMessageSource{};
    const auto body=request.original_source?derived.off.text:application::normalize_message_body(request.body,request.context.maximum_lines);
    if (!request.operation.valid() || application::validate_message_subject(request.subject,true)!=application::MessageValidation::valid ||
        (!request.original_source && application::validate_message_save(request.subject,body)!=application::MessageValidation::valid))
        return read_failure<application::CommittedMessage>(invalid("validate local message commit"));
    const auto now=impl_->message_utc_now_();
    if(!application::valid_utc_seconds(now))return read_failure<application::CommittedMessage>(invalid("validate message clock"));
    const auto digest=message_detail::fingerprint(request);
    return impl_->write_result<application::CommittedMessage>("commit local message",body.canonical_lf.size()+4096+(request.original_source?request.original_source->bytes.size()+162000:0),[request,body,derived,now,digest](sqlite3* db,auto& output) {
        message_detail::actor(db,request.actor,false);
        if (const auto previous=message_detail::receipt(db,request,digest)) {output=*previous;return SQLITE_OK;}
        const auto current=message_detail::begin(db,request.actor,request.context.target,request.context.parent);
        const auto& old=request.context;
        if (current.group_revision!=old.group_revision || current.echo_revision!=old.echo_revision || current.recipient_revision!=old.recipient_revision || current.limits_revision!=old.limits_revision || current.maximum_lines!=old.maximum_lines || current.ftn_configuration_revision!=old.ftn_configuration_revision) return SQLITE_ABORT;
        if(request.original_source && (current.presentation_revision!=old.presentation_revision || current.pipe_colors!=old.pipe_colors))return SQLITE_ABORT;
        return message_detail::insert_validated_message(db,request,body,derived,now,digest,output);
    });
}
application::StorageResult<application::CommittedMessage> SqliteAccountRepository::reconcile_message(const application::CommitMessageRequest& request) {
    if(!request.operation.valid() || request.subject.size()>application::kMessageSubjectCells || request.body.size()>application::kMessageBodyBytes || !message_detail::valid_source_request(request))
        return read_failure<application::CommittedMessage>(invalid("validate reconciliation request"));
    const auto digest=message_detail::fingerprint(request);
    return impl_->readers_.read<application::CommittedMessage>("reconcile local message",[request,digest](sqlite3* db) {
        message_detail::ReadSnapshot snapshot(db);message_detail::actor(db,request.actor,false);
        const auto result=message_detail::receipt(db,request,digest);if(!result)message_detail::unavailable();return *result;
    });
}
application::StorageResult<application::OpenedMessage> SqliteAccountRepository::open_message(const application::MessageActor& caller,application::MessageId id) {
    return impl_->write_result<application::OpenedMessage>("open local message",4096,[caller,id](sqlite3* db,auto& output) {
        const auto a=message_detail::actor(db,caller);auto message=message_detail::load(db,caller,a,id);
        const auto code=message_detail::accept_open(db,caller,message);if(code!=SQLITE_OK)return code;
        output=std::move(message);return SQLITE_OK;
    });
}
application::StorageResult<application::OpenedMessage> SqliteAccountRepository::quote_source(const application::MessageActor& caller,application::MessageId id) {
    return impl_->readers_.read<application::OpenedMessage>("get authorized quote data",[caller,id](sqlite3* db) {
        message_detail::ReadSnapshot snapshot(db);return message_detail::load(db,caller,message_detail::actor(db,caller),id);
    });
}
application::StorageResult<application::MessageScanPreference> SqliteAccountRepository::message_scan_preference(const application::MessageActor& caller,application::LocalEchoId id) {
    return impl_->write_result<application::MessageScanPreference>("initialize/read message scan state",512,[caller,id](sqlite3* db,auto& output) {
        output=message_detail::preference(db,caller,id);return SQLITE_OK;
    });
}
application::StorageResult<application::MessageScanPreference> SqliteAccountRepository::set_message_scan_preference(const application::MessageActor& caller,application::LocalEchoId id,std::int64_t revision,application::MessageScanColumn column,bool enabled) {
    if(revision<=0 || (column!=application::MessageScanColumn::unread && column!=application::MessageScanColumn::to_you))return read_failure<application::MessageScanPreference>(invalid("validate scan preference change"));
    return impl_->write_result<application::MessageScanPreference>("change one scan column",512,[caller,id,revision,column,enabled](sqlite3* db,auto& output) {
        message_detail::preference(db,caller,id);
        const char* sql=column==application::MessageScanColumn::unread ?
            "UPDATE sfx_caller_echo_state SET unread_enabled=?1,preference_revision=preference_revision+1 WHERE account_id=?2 AND echo_id=?3 AND preference_revision=?4" :
            "UPDATE sfx_caller_echo_state SET to_you_enabled=?1,preference_revision=preference_revision+1 WHERE account_id=?2 AND echo_id=?3 AND preference_revision=?4";
        SqliteStatement change(db,sql,"CAS one scan preference");change.bind(1,enabled);change.bind(2,caller.account);change.bind(3,id.value);change.bind(4,revision);change.step();
        if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        output=message_detail::preference(db,caller,id);return SQLITE_OK;
    });
}
StorageStatus SqliteAccountRepository::remove_private_inbox_item(const application::MessageActor& caller,application::MessageId id) {
    return impl_->write("remove own private inbox item",512,[caller,id](sqlite3* db) {
        message_detail::actor(db,caller);
        SqliteStatement q(db,"SELECT 1 FROM sfx_local_messages WHERE message_id=?1 AND kind=1 AND recipient_account_id=?2", "verify intended private recipient");
        q.bind(1,id.value);q.bind(2,caller.account);if(!q.row())return SQLITE_NOTFOUND;
        SqliteStatement remove(db,"UPDATE sfx_message_recipient_state SET inbox_removed=1 WHERE message_id=?1 AND recipient_account_id=?2", "remove private inbox visibility only");
        remove.bind(1,id.value);remove.bind(2,caller.account);remove.step();return SQLITE_OK;
    });
}
