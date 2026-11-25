// Trusted sealed-source port only. Uploaded archives/paths/claims of completion
// never enter this writer; Runtime first validates the entire hostile envelope.
// Multiple bounded passes avoid a packet-sized writer capture or partial import.
namespace qwk_reply_detail {
namespace app=application;
bool valid_info(const app::QwkReplySourceInfo& i){
    return app::valid_qwk_board_id(i.board_id)&&app::canonical_qwk_board_id(i.board_id)==i.board_id&&
        i.records>0&&i.records<=app::kQwkExpandedBytes/256&&i.compressed_bytes>0&&i.compressed_bytes<=app::kQwkCompressedBytes&&
        i.expanded_bytes>=128&&i.expanded_bytes<=app::kQwkExpandedBytes;
}
[[noreturn]] void invalid_record(){throw StorageFailure(StorageErrorCode::invalid_argument,"invalid or unavailable reply record; nothing imported",SQLITE_MISUSE);}
void source_ready(app::IQwkReplySource& s){
    if(s.cancelled())throw StorageFailure(StorageErrorCode::shutting_down,"reply import cancelled",SQLITE_INTERRUPT);
    if(!s.verify_identity())throw StorageFailure(StorageErrorCode::io,"sealed reply source identity unavailable",SQLITE_IOERR);
}
void rewind(app::IQwkReplySource& s){source_ready(s);if(!s.rewind())throw StorageFailure(StorageErrorCode::io,"rewind sealed reply source",SQLITE_IOERR);}
struct LogicalDigest {
    crypto_hash_sha256_state state{};
    LogicalDigest(std::string_view board){if(sodium_init()<0||crypto_hash_sha256_init(&state)!=0)invalid_record();raw("SFX-QWK-REP-1;");field(board);}
    void raw(std::string_view bytes){if(crypto_hash_sha256_update(&state,reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size())!=0)invalid_record();}
    void field(std::string_view bytes){raw(std::to_string(bytes.size()));raw(":");raw(bytes);raw(";");}
    void record(const app::QwkReplyRecord& r){const auto& m=r.message;field(std::to_string(m.conference));field(std::to_string(m.reference));field(m.private_mail?"private":"public");
        for(const auto* v:{&m.from,&m.to,&m.subject,&m.body,&m.date,&m.time,&m.message_id,&m.in_reply_to,&m.when_written})field(*v);}
    std::array<std::uint8_t,32> finish(){std::array<std::uint8_t,32> result{};if(crypto_hash_sha256_final(&state,result.data())!=0)invalid_record();return result;}
};
bool field(std::string_view v,std::size_t maximum,bool empty=false){
    if(v.size()>maximum||(!empty&&v.empty()))return false;
    return std::all_of(v.begin(),v.end(),[](unsigned char c){return c>=32&&c!=127;});
}
bool blank(std::string_view v){return std::all_of(v.begin(),v.end(),[](char c){return c==' '||c=='\t'||c=='\n'||c=='\r';});}
void structure(const app::QwkReplyRecord& r){
    const auto& m=r.message;
    const bool extended_control=r.command&&m.from.empty()&&m.date.empty()&&m.time.empty();
    if(!field(m.from,255,extended_control)||!field(m.to,255)||!field(m.subject,app::kMessageSubjectCells,true)||
        !field(m.message_id,1024,true)||!field(m.in_reply_to,1024,true)||!field(m.when_written,64,true)||
        m.body.size()>app::kMessageBodyBytes||(!extended_control&&(m.date.size()!=8||m.time.size()!=5||!field(m.date,8)||!field(m.time,5))))invalid_record();
    const auto control=message_detail::recipient_key(m.to)=="sfxqwk";
    if(control!=r.command.has_value())invalid_record();
    if(r.command){
        const auto subject=message_detail::recipient_key(m.subject);
        if((*r.command!=app::QwkScanCommand::add&&*r.command!=app::QwkScanCommand::drop)||
            subject!=(*r.command==app::QwkScanCommand::add?"add":"drop")||!m.conference||m.private_mail||m.reference||
            !m.message_id.empty()||!m.in_reply_to.empty()||!blank(m.body)||(extended_control&&!m.when_written.empty()))invalid_record();
    }else if(!m.private_mail&&!m.conference)invalid_record();
}
template<class Function> void pass(app::IQwkReplySource& source,const app::QwkReplySourceInfo& info,Function fn){
    rewind(source);LogicalDigest digest(info.board_id);std::uint64_t count{},charged{};
    for(;;){
        source_ready(source);auto next=source.next();if(!next)throw StorageFailure(StorageErrorCode::io,"read sealed reply source",SQLITE_IOERR);
        if(!next.value->has_value())break;
        if(++count>info.records)invalid_record();
        const auto& r=next.value->value();structure(r);
        for(const auto* v:{&r.message.from,&r.message.to,&r.message.subject,&r.message.body,&r.message.date,&r.message.time,&r.message.message_id,&r.message.in_reply_to,&r.message.when_written}){
            if(v->size()>info.expanded_bytes-charged)invalid_record();
            charged+=v->size();}
        digest.record(r);fn(r,count);
    }
    source_ready(source);const auto actual=digest.finish();
    if(count!=info.records||sodium_memcmp(actual.data(),info.logical_sha256.data(),actual.size())!=0)invalid_record();
}
std::optional<app::QwkImportOutcome> receipt(sqlite3* db,const app::MessageActor& a,const app::QwkReplySourceInfo& info){
    SqliteStatement q(db,"SELECT board_id,record_count,messages,commands,missing_parents FROM sfx_qwk_rep_receipts WHERE account_id=?1 AND logical_fingerprint=?2","reconcile own accepted logical REP");q.bind(1,a.account);q.bind(2,info.logical_sha256);
    if(!q.row())return {};
    if(q.text(0)!=info.board_id||q.int64(1)!=static_cast<std::int64_t>(info.records))message_detail::conflict();
    const auto messages=q.int64(2),commands=q.int64(3),missing=q.int64(4);
    if(messages<0||commands<0||missing<0||missing>messages||messages+commands!=q.int64(1))throw StorageFailure(StorageErrorCode::corrupt,"invalid durable REP receipt",SQLITE_CORRUPT);
    return app::QwkImportOutcome{static_cast<std::uint32_t>(messages),static_cast<std::uint32_t>(commands),static_cast<std::uint32_t>(missing),true};
}
app::LocalEchoId echo(sqlite3* db,std::uint16_t conference,const message_detail::Authority& a,bool posting){
    SqliteStatement q(db,"SELECT echo_id FROM sfx_qwk_conference_mapping WHERE conference=?1","resolve permanent QWK conference identity");q.bind(1,conference);if(!q.row())invalid_record();
    app::LocalEchoId result{q.int64(0)};message_detail::echo(db,result,a,true,posting);return result;
}
void author(sqlite3* db,const app::MessageActor& a,const app::QwkRecord& m){
    SqliteStatement q(db,"SELECT display_name FROM sfx_accounts WHERE account_id=?1","compare authenticated author, not packet privilege");q.bind(1,a.account);if(!q.row())message_detail::unavailable();const auto name=q.text(0);
    const auto key=message_detail::recipient_key(m.from);
    if(key!=message_detail::recipient_key(name)&&(name.size()<=25||key!=message_detail::recipient_key(name.substr(0,25))))invalid_record();
}
std::optional<app::AccountId> recipient(sqlite3* db,const app::QwkRecord& m){
    const auto key=app::account_name_comparison_key(m.to);
    if(!m.private_mail&&(message_detail::recipient_key(m.to)=="all"||message_detail::recipient_key(m.to)=="all callers"))return {};
    if(!key)invalid_record();
    SqliteStatement q(db,"SELECT a.account_id FROM sfx_accounts a JOIN sfx_security_levels l ON l.security_level=a.security_level WHERE a.name_key=?1 AND a.account_state='active' AND l.level_state='active'","resolve exact current recipient; never guess truncated names");q.bind(1,*key);if(!q.row())invalid_record();return q.int64(0);
}
std::optional<std::uint32_t> reply_reference(const app::QwkRecord& m,std::string_view board){
    std::uint32_t reference=m.reference;
    if(!m.in_reply_to.empty()){
        const auto suffix="."+std::string(board)+"@sfx.invalid>";
        const auto end=m.in_reply_to.find(suffix);std::uint32_t extended{};
        if(end!=m.in_reply_to.npos&&m.in_reply_to.size()>suffix.size()+1&&m.in_reply_to.front()=='<'&&end+suffix.size()==m.in_reply_to.size()){
            for(std::size_t n=1;n<end;++n){const auto digit=static_cast<unsigned char>(m.in_reply_to[n])-'0';if(digit>9||extended>(app::kQwkMaximumReference-digit)/10){extended=0;break;}extended=extended*10+digit;}
        }
        if(extended&&app::qwk_message_identifier(board,extended)==m.in_reply_to){if(reference&&reference!=extended)invalid_record();reference=extended;}
        else return {};
    }
    return reference;
}
std::optional<app::MessageId> parent(sqlite3* db,const app::MessageActor& a,const message_detail::Authority& authority,
    const app::QwkRecord& m,const app::MessageTarget& target,std::string_view board,bool& missing){
    const auto resolved=reply_reference(m,board);if(!resolved){missing=true;return {};}
    const auto reference=*resolved;
    if(!reference)return {};
    SqliteStatement q(db,message_detail::active_message_sql(db,"SELECT m.message_id,m.kind,coalesce(m.echo_id,0),m.author_account_id,m.recipient_account_id,coalesce(r.inbox_removed,0) FROM sfx_qwk_message_reference x JOIN sfx_active_local_messages m ON m.message_id=x.message_id LEFT JOIN sfx_message_recipient_state r ON r.message_id=m.message_id WHERE x.reference=?1 AND (EXISTS(SELECT 1 FROM sfx_message_commit_receipts c WHERE c.operation_id=x.creation_operation AND c.message_id=m.message_id) OR EXISTS(SELECT 1 FROM sfx_ftn_import_receipts i JOIN sfx_ftn_message_metadata f USING(import_id) WHERE i.import_id=x.creation_operation AND i.original_message_id=m.message_id AND f.message_id=m.message_id AND m.author_account_id IS NULL))").c_str(),"resolve immutable local or remote reply creation identity");q.bind(1,static_cast<std::int64_t>(reference));
    if(!q.row()){missing=true;return {};}
    if(target.kind==app::MessageKind::private_ftn){
        const auto original=message_detail::load(db,a,authority,{q.int64(0)},false).header;
        if(!message_detail::reply_target_valid(target,a.account,original,false))invalid_record();
        return original.identity.id;
    }
    if(q.int64(1)==2 || q.is_null(3))throw StorageFailure(StorageErrorCode::invalid_argument,"Use online Reply for a remote FTN destination",SQLITE_MISMATCH);
    const bool private_message=q.int64(1)==1;
    if(private_message&&(q.is_null(4)||q.int64(4)!=a.account||q.boolean(5))){missing=true;return {};}
    if(!private_message){try{message_detail::echo(db,{q.int64(2)},authority,true,false);}catch(const StorageFailure& e){if(e.code()==StorageErrorCode::not_found){missing=true;return {};}throw;}}
    if(private_message!=(target.kind==app::MessageKind::private_local)||q.int64(2)!=target.echo.value||target.recipient!=q.int64(3))invalid_record();
    return app::MessageId{q.int64(0)};
}
app::MessageTarget netmail_target(sqlite3* db,const app::MessageActor& a,const message_detail::Authority& authority,const app::QwkRecord& m,std::string_view board){
    const auto conference=qwk_detail::netmail_conference(db);
    if(!conference||m.conference!=*conference||!m.private_mail||m.subject.size()<2||m.subject[0]!='@')invalid_record();
    const auto destination=app::parse_ftn_address(std::string_view(m.subject).substr(1));if(!destination)invalid_record();
    std::optional<std::string> original_network;
    const auto reference=reply_reference(m,board);
    if(!m.in_reply_to.empty()&&!reference)invalid_record();
    if(reference&&*reference){
        SqliteStatement ref(db,"SELECT f.message_id,i.network FROM sfx_qwk_message_reference r JOIN sfx_ftn_message_metadata f ON f.message_id=r.message_id AND f.import_id=r.creation_operation JOIN sfx_ftn_import_receipts i ON i.import_id=f.import_id AND i.original_message_id=f.message_id WHERE r.reference=?1","scope owned immutable NetMail original from either supported reply link");ref.bind(1,static_cast<std::int64_t>(*reference));
        if(ref.row()){
            const auto original=message_detail::load(db,a,authority,{ref.int64(0)},false).header;
            if(original.target.kind!=app::MessageKind::private_local||original.target.recipient!=a.account||original.inbox_removed||!original.remote_author)invalid_record();
            original_network=ref.text(1);
        }
    }
    const auto config=ftn_detail::config(db);const app::FtnNetwork* chosen{};
    std::string source_aka;unsigned best_rank=3;
    for(const auto& n:config.networks){const auto primary=app::parse_ftn_address(n.address);
        if(!n.enabled||!primary||app::ftn_local_address(n,*destination)||(!destination->domain.empty()&&!ftn_detail::same(destination->domain,primary->domain))||!app::ftn_route_link(config,n.code,*destination))continue;
        // A reply is allowed to use its original network; the permanent
        // reference is checked again for recipient ownership in parent().
        if(original_network&&!ftn_detail::same(*original_network,n.code))continue;
        // Preserve serialized network order, then primary/additional AKA order.
        // Rank globally so a later zone+net match beats an earlier zone match.
        const auto consider=[&](const std::string& text){
            const auto local=app::parse_ftn_address(text);if(!local)return;
            if(!destination->domain.empty()&&!local->domain.empty()&&!ftn_detail::same(destination->domain,local->domain))return;
            const unsigned rank=local->zone!=destination->zone?2U:local->net!=destination->net?1U:0U;
            if(rank<best_rank){chosen=&n;source_aka=text;best_rank=rank;}
        };
        consider(n.address);for(const auto& aka:n.additional_addresses)consider(aka);
    }
    if(!chosen)throw StorageFailure(StorageErrorCode::invalid_argument,"No configured FTN route for reply destination",SQLITE_MISMATCH);
    app::MessageTarget target;target.kind=app::MessageKind::private_ftn;
    target.remote=app::MessageRemoteTarget{chosen->code,source_aka,app::format_ftn_address(*destination),m.to};return target;
}
app::QwkRecord netmail_text(const app::QwkRecord& original){
    auto m=original;std::string real_subject;std::set<std::string> seen;std::size_t at{};
    // QWKE header kludges are interpreted only for explicitly addressed private
    // NetMail, never for ordinary local/public body text. Author is rechecked.
    for(;;){const auto end=m.body.find_first_of("\r\n",at);if(end==m.body.npos)break;
        auto next=end+1;if(m.body[end]=='\r'&&next<m.body.size()&&m.body[next]=='\n')++next;
        const auto line=m.body.substr(at,end-at);
        if(line.empty()){at=next;break;}const auto colon=line.find(": ");if(colon==line.npos)break;const auto key=line.substr(0,colon);
        if(key!="From"&&key!="To"&&key!="Subject")break;
        if(!seen.insert(key).second)invalid_record();
        const auto value=line.substr(colon+2);
        if(key=="From")m.from=value;else if(key=="To")m.to=value;else real_subject=value;at=next;
    }
    m.body.erase(0,at);m.subject=real_subject;return m;
}
struct Prepared {app::CommitMessageRequest request;app::MessageBody body;app::ValidatedMessageSource source;bool missing{};};
Prepared prepare(sqlite3* db,const app::MessageActor& a,const app::QwkReplyRecord& r,std::string_view board){
    const auto authority=message_detail::actor(db,a);author(db,a,r.message);Prepared p;auto& request=p.request;request.actor=a;
    const bool netmail=r.message.private_mail&&r.message.conference;
    const auto text=netmail?netmail_text(r.message):r.message;author(db,a,text);
    if(netmail){auto addressed=r.message;addressed.to=text.to;request.context.target=netmail_target(db,a,authority,addressed,board);}
    else{request.context.target.kind=r.message.private_mail?app::MessageKind::private_local:app::MessageKind::public_echo;
        if(!r.message.private_mail)request.context.target.echo=echo(db,r.message.conference,authority,true);
        request.context.target.recipient=recipient(db,r.message);}
    if(netmail&&(!field(text.from,255)||!field(text.to,35)||!field(text.subject,app::kMessageSubjectCells,true)))invalid_record();
    const auto link=parent(db,a,authority,r.message,request.context.target,board,p.missing);
    request.context=message_detail::begin(db,a,request.context.target,link);request.subject=text.subject;
    if(app::validate_message_subject(request.subject,true)!=app::MessageValidation::valid)invalid_record();
    if(text.body.find('\x1b')!=std::string::npos){
        request.original_source=app::MessageSource{app::MessageSourceFormat::ansi_canvas,1,text.body};
        p.source=app::validate_message_source(*request.original_source,request.context.maximum_lines);if(!p.source)invalid_record();p.body=p.source.off.text;
    }else{request.body=text.body;p.body=app::normalize_message_body(request.body,request.context.maximum_lines);if(app::validate_message_save(request.subject,p.body)!=app::MessageValidation::valid)invalid_record();}
    return p;
}
void command(sqlite3* db,const app::MessageActor& a,const app::QwkReplyRecord& r,bool apply){
    const auto authority=message_detail::actor(db,a);if(!r.message.from.empty())author(db,a,r.message);const auto target=echo(db,r.message.conference,authority,false);
    if(!apply)return;
    const auto old=message_detail::preference(db,a,target);const bool enabled=*r.command==app::QwkScanCommand::add;
    if(old.unread==enabled&&old.to_you==enabled)return;
    SqliteStatement q(db,"UPDATE sfx_caller_echo_state SET unread_enabled=?1,to_you_enabled=?1,preference_revision=preference_revision+1 WHERE account_id=?2 AND echo_id=?3 AND preference_revision=?4 AND preference_revision<9223372036854775807","apply plain ADD/DROP to caller shared scan columns");q.bind(1,enabled);q.bind(2,a.account);q.bind(3,target.value);q.bind(4,old.revision);q.step();if(sqlite3_changes(db)!=1)message_detail::conflict();
    qwk_detail::clear_continuation(db,a.account);
}
} // namespace qwk_reply_detail
application::StorageResult<application::QwkPolicy> SqliteAccountRepository::qwk_caller_policy(const application::MessageActor& a){
    return impl_->readers_.read<application::QwkPolicy>("read ordinary caller packet policy",[a](sqlite3* db){message_detail::ReadSnapshot tx(db);message_detail::actor(db,a);return qwk_detail::policy(db);});
}
application::StorageResult<std::optional<application::QwkImportOutcome>> SqliteAccountRepository::qwk_import_outcome(const application::MessageActor& a,const application::QwkReplySourceInfo& info){
    if(!qwk_reply_detail::valid_info(info))return read_failure<std::optional<application::QwkImportOutcome>>(invalid("validate REP outcome identity"));
    return impl_->readers_.read<std::optional<application::QwkImportOutcome>>("reconcile own exact REP outcome",[a,info](sqlite3* db){message_detail::ReadSnapshot tx(db);message_detail::actor(db,a,false);return qwk_reply_detail::receipt(db,a,info);});
}
application::StorageResult<application::QwkImportOutcome> SqliteAccountRepository::import_qwk_reply(const application::MessageActor& a,std::shared_ptr<application::IQwkReplySource> source){
    if(!source)return read_failure<application::QwkImportOutcome>(invalid("missing sealed REP source"));
    const auto info=source->info();
    if(!qwk_reply_detail::valid_info(info))return read_failure<application::QwkImportOutcome>(invalid("validate sealed REP bounds"));
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return read_failure<application::QwkImportOutcome>(invalid("validate REP entry clock"));
    // Reserve a bounded single-record working allowance in the EXISTING8MiB
    // queue. This capture owns a sealed streaming handle, never packet bodies.
    auto refusal=std::make_shared<application::StorageStatus>();
    auto imported=impl_->write_result<application::QwkImportOutcome>("atomic authorized REP import",7U*1024U*1024U,[a,info,source=std::move(source),now,refusal](sqlite3* db,auto& output){*refusal=application::StorageStatus::success();try{
        message_detail::actor(db,a);qwk_reply_detail::pass(*source,info,[](const auto&,auto){});
        if(const auto accepted=qwk_reply_detail::receipt(db,a,info)){output=*accepted;return SQLITE_OK;}
        if(qwk_detail::policy(db).board_id!=info.board_id)return SQLITE_ABORT;
        // Current targets/recipients/content of EVERY record before any write.
        qwk_reply_detail::pass(*source,info,[&](const auto& r,auto){if(r.command)qwk_reply_detail::command(db,a,r,false);else(void)qwk_reply_detail::prepare(db,a,r,info.board_id);});
        application::QwkImportOutcome result;int code=SQLITE_OK;
        qwk_reply_detail::pass(*source,info,[&](const auto& r,auto){
            if(code!=SQLITE_OK)return;
            if(r.command){qwk_reply_detail::command(db,a,r,true);++result.commands;return;}
            auto p=qwk_reply_detail::prepare(db,a,r,info.board_id);randombytes_buf(p.request.operation.bytes.data(),p.request.operation.bytes.size());
            const auto digest=message_detail::fingerprint(p.request);std::optional<application::CommittedMessage> inserted;
            code=message_detail::insert_validated_message(db,p.request,p.body,p.source,now,digest,inserted);
            if(code==SQLITE_OK){++result.messages;if(p.missing)++result.missing_parents;}
        });
        if(code!=SQLITE_OK)return code;
        SqliteStatement receipt(db,"INSERT INTO sfx_qwk_rep_receipts VALUES(?1,?2,?3,?4,?5,?6,?7,?8)","commit REP messages/commands/counters/intents/acceptance atomically");
        receipt.bind(1,a.account);receipt.bind(2,info.logical_sha256);receipt.bind(3,info.board_id);receipt.bind(4,static_cast<std::int64_t>(info.records));receipt.bind(5,static_cast<std::int64_t>(result.messages));receipt.bind(6,static_cast<std::int64_t>(result.commands));receipt.bind(7,static_cast<std::int64_t>(result.missing_parents));receipt.bind(8,now);receipt.step();
        qwk_reply_detail::source_ready(*source);output=result;return SQLITE_OK;
        }catch(const StorageFailure& failure){
            *refusal={failure.code(),failure.operation()};
            return failure.sqlite_code()==SQLITE_OK?SQLITE_ERROR:failure.sqlite_code();
        }
    });
    // The future has completed; the owned status capture cannot outlive or
    // borrow a caller Context. Non-OK callback result still rolls back atomically.
    if(!imported&&!refusal->succeeded())imported.status=*refusal;
    return imported;
}
