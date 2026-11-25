// Included within message_detail. Caller destinations remain typed and carry no
// installation-owner authority. Every network/source/route is rechecked at save.
bool remote_address_equal(std::string_view left,std::string_view right){
    const auto a=app::parse_ftn_address(left),b=app::parse_ftn_address(right);
    return a&&b&&app::same_ftn_address(*a,*b,true);
}
std::int64_t remote_context(sqlite3* db,const app::MessageTarget& target){
    if(!target.remote||target.recipient||target.sysop_assignment_revision||target.remote->name.empty()||target.remote->name.size()>35||app::validate_message_subject(target.remote->name,true)!=app::MessageValidation::valid)unavailable();
    auto c=ftn_detail::config(db);const auto& r=*target.remote;const auto& n=ftn_detail::network(c,r.network);
    const auto source=app::parse_ftn_address(r.source_aka),destination=app::parse_ftn_address(r.address),primary=app::parse_ftn_address(n.address);
    if(!n.enabled||!source||!destination||!primary||!app::ftn_local_address(n,*source)||(!destination->domain.empty()&&!ftn_detail::same(destination->domain,primary->domain)))unavailable();
    if(target.kind==app::MessageKind::private_ftn){if(app::ftn_local_address(n,*destination)||!app::ftn_route_link(c,n.code,*destination))unavailable();}
    else if(target.kind!=app::MessageKind::public_echo||std::none_of(c.message_echoes.begin(),c.message_echoes.end(),[&](const auto& e){return e.local_echo==target.echo.value&&ftn_detail::same(e.network,n.code);}))unavailable();
    return c.revision;
}
bool reply_target_valid(const app::MessageTarget& target,app::AccountId author,const app::MessageHeader& original,bool sysop){
    if(original.remote_author){
        if(!target.remote||target.recipient||!ftn_detail::same(target.remote->network,original.remote_author->network)||!remote_address_equal(target.remote->address,original.remote_author->address)||target.remote->name!=original.remote_author->name)return false;
        return original.target.kind==app::MessageKind::public_echo?target.kind==app::MessageKind::public_echo&&target.echo==original.target.echo:
            original.target.kind==app::MessageKind::private_local&&target.kind==app::MessageKind::private_ftn&&(original.target.recipient==author||sysop);
    }
    if(original.target.kind==app::MessageKind::private_ftn){
        if(!original.target.remote||!target.remote||target.kind!=app::MessageKind::private_ftn||target.recipient)return false;
        const auto& r=*original.target.remote;return (original.author==author||sysop)&&ftn_detail::same(r.network,target.remote->network)&&remote_address_equal(r.address,target.remote->address)&&r.name==target.remote->name;
    }
    return original.author&&target.kind==original.target.kind&&target.echo==original.target.echo&&target.recipient==original.author&&!target.remote;
}
std::string ftn_post_msgid(sqlite3* db,std::string_view aka,const app::Identifier128& operation,std::int64_t now){
    std::uint32_t serial{};
    {SqliteStatement prior(db,"SELECT serial FROM sfx_ftn_serial_allocations WHERE operation_id=?1","reuse only same atomic local post allocation");prior.bind(1,operation);if(prior.row())serial=static_cast<std::uint32_t>(prior.int64(0));}
    if(!serial){
        if(read_ftn_serial_state(db).recovery_required)throw StorageFailure(app::StorageErrorCode::backpressure,"FTN message ID history needs recovery",SQLITE_BUSY);
        // A restored snapshot may not contain the dead host's later posts.
        // Advance to current UTC as well as the retained monotonic floor. This
        // keeps the interoperable eight-hex serial and all known-history guards.
        // Unknown future bursts, wrong clocks and simultaneous clones cannot be
        // proven absent from a self-contained snapshot; no such claim is made.
        if(now<0||now>UINT32_MAX)throw StorageFailure(app::StorageErrorCode::backpressure,"FTN message clock outside serial range",SQLITE_FULL);
        SqliteStatement next(db,"UPDATE sfx_ftn_serial_allocator SET high_water=max(high_water+1,?1) WHERE singleton_id=1 AND recovery_required=0 AND high_water<4294967295 RETURNING high_water","reserve clock-aware nonwrapping local FTN serial in post transaction");next.bind(1,now);
        if(!next.row())throw StorageFailure(app::StorageErrorCode::backpressure,"FTN message serial exhausted or allocator unavailable",SQLITE_FULL);
        serial=static_cast<std::uint32_t>(next.int64(0));next.step();
        SqliteStatement keep(db,"INSERT INTO sfx_ftn_serial_allocations VALUES(?1,?2)","retain allocation through message purge and restart");keep.bind(1,operation);keep.bind(2,static_cast<std::int64_t>(serial));keep.step();
    }
    constexpr char hex[]="0123456789abcdef";std::string text(aka);text+=' ';for(int shift=28;shift>=0;shift-=4)text+=hex[(serial>>shift)&15];return text;
}
std::string stored_ftn_msgid(sqlite3* db,app::MessageId message,std::string_view network){
    SqliteStatement q(db,"SELECT t.msgid FROM sfx_ftn_message_targets t WHERE t.message_id=?1 AND t.network=?2 UNION SELECT f.msgid FROM sfx_ftn_message_metadata f JOIN sfx_ftn_import_receipts i USING(import_id) WHERE f.message_id=?1 AND i.network=?2 UNION SELECT msgid FROM sfx_ftn_message_intents WHERE message_id=?1 AND network=?2","read immutable same-network MSGID");q.bind(1,message.value);q.bind(2,std::string(network));if(!q.row())return {};auto result=bytes(q,0);if(q.row())conflict();return result;
}
std::int64_t prepare_remote_post(sqlite3* db,const app::CommitMessageRequest& request,std::int64_t now){
    if(!request.context.target.remote)return 0;
    SqliteStatement next(db,"SELECT max(coalesce((SELECT seq FROM sqlite_sequence WHERE name='sfx_local_messages'),0),coalesce((SELECT max(message_id) FROM sfx_local_messages),0))","reserve typed remote target and native identity atomically");if(!next.row()||next.int64(0)==INT64_MAX)conflict();const auto id=next.int64(0)+1;
    const auto& r=*request.context.target.remote;const auto authority=actor(db,request.actor);std::string reply;bool override=false;
    if(request.context.parent){const auto original=load(db,request.actor,authority,*request.context.parent,false).header;reply=stored_ftn_msgid(db,*request.context.parent,r.network);override=original.target.kind!=app::MessageKind::public_echo&&original.author!=request.actor.account&&original.target.recipient!=request.actor.account&&authority.level>=authority.threshold;}
    SqliteStatement target(db,"INSERT INTO sfx_ftn_message_targets VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)","bind real local author to immutable remote target without fake local recipient");target.bind(1,id);target.bind(2,request.operation);target.bind(3,r.network);target.bind(4,r.source_aka);target.bind(5,r.address);target.bind(6,std::vector<std::uint8_t>(r.name.begin(),r.name.end()));const auto msgid=ftn_post_msgid(db,r.source_aka,request.operation,now);target.bind(7,std::vector<std::uint8_t>(msgid.begin(),msgid.end()));target.bind(8,std::vector<std::uint8_t>(reply.begin(),reply.end()));target.bind(9,override);target.step();return id;
}
void commit_ftn_intents(sqlite3* db,const app::CommitMessageRequest& request,std::int64_t id,std::int64_t now){
    if(request.context.target.kind==app::MessageKind::private_local)return;
    SqliteStatement version(db,"PRAGMA user_version","keep predecessor local posting semantics");if(!version.row())unavailable();if(version.int64(0)<22)return;
    auto c=ftn_detail::config(db);const auto add=[&](const app::FtnNetwork& network,std::string_view aka,std::string_view tag,std::string_view link){
        const auto msgid=ftn_post_msgid(db,aka,request.operation,now);SqliteStatement intent(db,"INSERT INTO sfx_ftn_message_intents(message_id,network,operation_id,source_aka,echo_tag,msgid,via_link,configuration_revision,state,reason) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,0,'')","commit content/source/receipt and durable FTN scan intent together");intent.bind(1,id);intent.bind(2,network.code);intent.bind(3,request.operation);intent.bind(4,std::string(aka));intent.bind(5,std::string(tag));intent.bind(6,std::vector<std::uint8_t>(msgid.begin(),msgid.end()));intent.bind(7,std::string(link));intent.bind(8,c.revision);intent.step();
    };
    if(request.context.target.kind==app::MessageKind::private_ftn){const auto& r=*request.context.target.remote;const auto& n=ftn_detail::network(c,r.network);const auto address=app::parse_ftn_address(r.address);const auto route=address?app::ftn_route_link(c,n.code,*address):std::nullopt;if(!route)unavailable();add(n,r.source_aka,"",*route);}
    else for(const auto& e:c.message_echoes)if(e.local_echo==request.context.target.echo.value){const auto& n=ftn_detail::network(c,e.network);if(n.enabled){const auto& remote=request.context.target.remote;add(n,remote&&ftn_detail::same(remote->network,n.code)?remote->source_aka:n.address,e.tag,"");}}
}
struct ReplyIdentity {app::MessageHeader header;bool sysop{};std::string reply;};
std::optional<ReplyIdentity> reply_identity(sqlite3* db,app::MessageId id){
    SqliteStatement q(db,message_detail::active_message_sql(db,"SELECT m.kind,coalesce(m.echo_id,0),m.author_account_id,m.recipient_account_id,coalesce(f.from_name,''),i.network,i.origin,t.network,t.source_aka,t.destination,t.recipient_name,coalesce(t.parent_sysop_authorized,0),coalesce(f.reply,t.reply,x'') FROM sfx_active_local_messages m LEFT JOIN sfx_ftn_message_metadata f USING(message_id) LEFT JOIN sfx_ftn_import_receipts i USING(import_id) LEFT JOIN sfx_ftn_message_targets t USING(message_id) WHERE m.message_id=?1").c_str(),"validate typed reply identity without reading private bodies");q.bind(1,id.value);if(!q.row())return {};ReplyIdentity out;auto& h=out.header;h.target.kind=static_cast<app::MessageKind>(q.int64(0));h.target.echo={q.int64(1)};if(!q.is_null(2))h.author=q.int64(2);if(!q.is_null(3))h.target.recipient=q.int64(3);if(!h.author&&!q.is_null(5))h.remote_author=app::MessageHeader::RemoteAuthor{q.text(5),q.text(6),bytes(q,4)};if(!q.is_null(7))h.target.remote=app::MessageRemoteTarget{q.text(7),q.text(8),q.text(9),bytes(q,10)};out.sysop=q.boolean(11);out.reply=bytes(q,12);return out;
}
bool reply_edge_valid(sqlite3* db,app::MessageId child,app::MessageId parent){
    const auto c=reply_identity(db,child),p=reply_identity(db,parent);if(!c||!p)return false;
    if(c->header.author){
        if(!reply_target_valid(c->header.target,*c->header.author,p->header,c->sysop))return false;
        if(c->header.target.remote)return c->reply==stored_ftn_msgid(db,parent,c->header.target.remote->network);
        return true;
    }
    if(!c->header.remote_author||c->reply.empty())return false;
    const auto& remote=*c->header.remote_author;
    if(c->reply!=stored_ftn_msgid(db,parent,remote.network))return false;
    if(c->header.target.kind==app::MessageKind::public_echo&&p->header.target.kind==app::MessageKind::public_echo&&c->header.target.echo==p->header.target.echo)return true;
    return c->header.target.kind==app::MessageKind::private_local&&p->header.target.kind==app::MessageKind::private_ftn&&p->header.author==c->header.target.recipient&&p->header.target.remote&&ftn_detail::same(remote.network,p->header.target.remote->network)&&remote_address_equal(remote.address,p->header.target.remote->address);
}
