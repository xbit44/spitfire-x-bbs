namespace ftn_scan_detail {
namespace app=application;
[[noreturn]] void unavailable(const char* why){throw StorageFailure(StorageErrorCode::not_found,why,SQLITE_NOTFOUND);}
std::string rendition(sqlite3* db,std::int64_t message,const app::FtnMessageEcho& echo,const std::string& original){
    if(!echo.ansi_colors)return original; // Immutable legacy wire policy.
    SqliteStatement source(db,"SELECT format,profile_version,source FROM sfx_message_sources WHERE message_id=?1","derive approved outgoing ANSI rendition from immutable source");source.bind(1,message);if(!source.row())return original;
    app::MessageSource input;input.format=static_cast<app::MessageSourceFormat>(source.int64(0));input.profile=static_cast<std::uint8_t>(source.int64(1));input.bytes=message_detail::bytes(source,2);
    // Pipe codes are a separate format. When ANSI is disabled, keep their
    // literal source form for peers and suppress only ANSI attributes.
    const auto canvas=app::message_source_rendition(input,*echo.ansi_colors&&echo.pipe_colors,*echo.ansi_colors);
    if(!canvas)message_detail::unavailable();
    if(!*echo.ansi_colors)return canvas->text.canonical_lf;
    std::string out;for(std::size_t row=0;row<canvas->extent;row+=100){const auto part=app::message_canvas_page(*canvas,row,100,true);if(!part)message_detail::unavailable();for(const auto& op:part->operations){if(op.kind==app::TerminalDisplayOperationKind::logical_newline)out+='\n';else out+=op.bytes;}if(out.size()>app::kFtnMessageMaximumBytes)message_detail::unavailable();}return out;
}
app::FtnScanIntent read(sqlite3* db,const app::FtnConfiguration& config,std::int64_t message,std::string_view network){
    SqliteStatement q(db,message_detail::active_message_sql(db,"SELECT i.message_id,i.operation_id,i.network,i.source_aka,i.echo_tag,i.via_link,i.msgid,coalesce(t.reply,x''),coalesce(t.destination,''),a.display_name,coalesce(p.real_name,''),coalesce(t.recipient_name,cast('All' AS BLOB)),m.subject,m.body,m.created_utc_seconds,m.kind,m.echo_id,m.parent_message_id FROM sfx_ftn_message_intents i JOIN sfx_active_local_messages m USING(message_id) JOIN sfx_message_commit_receipts r ON r.message_id=m.message_id AND r.operation_id=i.operation_id AND r.actor_account_id=m.author_account_id JOIN sfx_accounts a ON a.account_id=m.author_account_id LEFT JOIN sfx_account_profiles p ON p.account_id=a.account_id LEFT JOIN sfx_ftn_message_targets t ON t.message_id=m.message_id AND t.network=i.network WHERE i.message_id=?1 AND i.network=?2").c_str(),"read legitimate committed local author and immutable FTN target");q.bind(1,message);q.bind(2,std::string(network));if(!q.row())unavailable("ftn_scan_commit_source_missing");
    app::FtnScanIntent out;out.message={q.int64(0)};out.operation=ftn_work_detail::identifier(q,1);out.network=q.text(2);out.source_aka=q.text(3);out.tag=q.text(4);out.via_link=q.text(5);out.msgid=message_detail::bytes(q,6);out.reply=message_detail::bytes(q,7);out.destination=q.text(8);if(out.reply.empty()&&!q.is_null(17))out.reply=message_detail::stored_ftn_msgid(db,{q.int64(17)},out.network);
    std::string author=q.text(9);bool real_name_required{};
    if(!out.tag.empty()) {const auto echo=std::find_if(config.message_echoes.begin(),config.message_echoes.end(),[&](const auto& e){return ftn_detail::same(e.network,out.network)&&ftn_detail::same(e.tag,out.tag)&&e.local_echo==q.int64(16);});if(echo==config.message_echoes.end()||q.int64(15)!=0)unavailable("ftn_scan_echo_mapping_changed");if(echo->require_real_name){real_name_required=true;author=q.text(10);}}
    else if(q.int64(15)!=2||out.destination.empty())message_detail::unavailable();
    bool published{};
    out.to=message_detail::bytes(q,11);out.subject=message_detail::bytes(q,12);out.body=message_detail::bytes(q,13);out.created=q.int64(14);out.origin_line=ftn_work_detail::network(config,out.network).origin_line;
    {SqliteStatement frozen(db,"SELECT author_name,origin_line,reply FROM sfx_ftn_local_wire_sources WHERE message_id=?1 AND network=?2","retain published wire author Origin and REPLY despite later profile edits");frozen.bind(1,out.message.value);frozen.bind(2,out.network);if(frozen.row()){published=true;out.from=message_detail::bytes(frozen,0);out.origin_line=frozen.text(1);out.reply=message_detail::bytes(frozen,2);}else{const auto cp437=app::qwk_utf8_to_cp437(author);if(!cp437||cp437->empty()||cp437->size()>35)unavailable(real_name_required?"ftn_scan_real_name_required":"ftn_scan_author_not_representable");out.from=*cp437;}}
    SqliteStatement schema(db,"PRAGMA user_version","read wire rendition version");if(!schema.row())message_detail::unavailable();
    if(published&&schema.int64(0)>=25){SqliteStatement body(db,"SELECT body FROM sfx_ftn_wire_bodies WHERE message_id=?1 AND network=?2","reuse exact published color rendition");body.bind(1,out.message.value);body.bind(2,out.network);if(body.row())out.body=message_detail::bytes(body,0);}
    else if(!published&&!out.tag.empty()){const auto echo=std::find_if(config.message_echoes.begin(),config.message_echoes.end(),[&](const auto& e){return ftn_detail::same(e.network,out.network)&&ftn_detail::same(e.tag,out.tag);});if(echo==config.message_echoes.end())message_detail::unavailable();out.body=rendition(db,out.message.value,*echo,out.body);}
    if(out.to.empty()||out.to.size()>35||out.subject.size()>71||out.body.size()>app::kFtnMessageMaximumBytes)message_detail::unavailable();
    std::string material="SFX/FTNScan/v1";for(const auto* field:{&out.network,&out.source_aka,&out.tag,&out.via_link,&out.msgid,&out.reply,&out.destination,&out.from,&out.to,&out.subject,&out.body,&out.origin_line}){material+=';';material+=std::to_string(field->size());material+=':';material+=*field;}material+=';'+std::to_string(out.created);out.fingerprint=message_detail::source_digest(material);return out;
}
void freeze(sqlite3* db,const app::FtnScanIntent& intent){SqliteStatement q(db,"INSERT INTO sfx_ftn_local_wire_sources VALUES(?1,?2,?3,?4,?5) ON CONFLICT(message_id,network) DO NOTHING","freeze local wire identity with first committed publication");q.bind(1,intent.message.value);q.bind(2,intent.network);q.bind(3,std::vector<std::uint8_t>(intent.from.begin(),intent.from.end()));q.bind(4,intent.origin_line);q.bind(5,std::vector<std::uint8_t>(intent.reply.begin(),intent.reply.end()));q.step();SqliteStatement schema(db,"PRAGMA user_version","read frozen body publication version");if(!schema.row()||schema.int64(0)<25)return;SqliteStatement body(db,"INSERT INTO sfx_ftn_wire_bodies VALUES(?1,?2,?3) ON CONFLICT(message_id,network) DO NOTHING","freeze exact outgoing ANSI policy at first publication");body.bind(1,intent.message.value);body.bind(2,intent.network);body.bind(3,std::vector<std::uint8_t>(intent.body.begin(),intent.body.end()));body.step();}
}
application::StorageResult<std::vector<application::FtnScanIntent>> SqliteAccountRepository::pending_ftn_scan(const application::MessageConfigurationActor& a,std::string_view network,std::int64_t after,std::size_t maximum){
    using Result=std::vector<application::FtnScanIntent>;if(!ftn_work_detail::owner(a)||!application::valid_ftn_code(network)||after<0||!maximum||maximum>16)return read_failure<Result>(invalid("validate bounded owner scan page"));
    return impl_->readers_.read<Result>("read pending local FTN scan intents",[a,n=std::string(network),after,maximum](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);const auto config=ftn_detail::config(db);SqliteStatement q(db,message_detail::active_message_sql(db,"SELECT i.message_id FROM sfx_ftn_message_intents i JOIN sfx_active_local_messages m USING(message_id) WHERE i.network=?1 AND i.state=0 AND i.message_id>?2 ORDER BY i.message_id LIMIT ?3").c_str(),"page unscanned committed local messages");q.bind(1,n);q.bind(2,after);q.bind(3,static_cast<std::int64_t>(maximum));Result out;while(q.row()){try{out.push_back(ftn_scan_detail::read(db,config,q.int64(0),n));}catch(const StorageFailure& error){if(error.code()!=application::StorageErrorCode::not_found&&error.code()!=application::StorageErrorCode::invalid_argument)throw;application::FtnScanIntent item;item.message={q.int64(0)};item.network=n;item.reason=error.operation().rfind("ftn_scan_",0)==0?error.operation():"ftn_scan_source_unavailable";out.push_back(std::move(item));}}return out;});
}
application::StorageStatus SqliteAccountRepository::commit_ftn_scan(const application::MessageConfigurationActor& a,std::int64_t revision,const application::FtnScanIntent& intent,const std::vector<application::FtnOutboundJob>& jobs){
    namespace app=application;if(!ftn_work_detail::owner(a)||revision<1||intent.message.value<1||!intent.operation.valid()||!app::valid_ftn_code(intent.network)||jobs.empty()||jobs.size()>4096||!std::all_of(jobs.begin(),jobs.end(),ftn_work_detail::valid_job))return invalid("validate atomic local FTN scan publication");const auto now=impl_->message_utc_now_?impl_->message_utc_now_():0;
    return impl_->write_result<bool>("commit exact local FTN scan and immutable artifacts",65536+intent.body.size()*2+jobs.size()*1024,[a,revision,intent,jobs,now](sqlite3* db,std::optional<bool>& out){message_detail::config_authority(db,a);const auto c=ftn_detail::config(db);if(c.revision!=revision||c.settings.mode==app::FtnMailerMode::disabled)message_detail::conflict();const auto& network=ftn_work_detail::network(c,intent.network);const auto source=app::parse_ftn_address(intent.source_aka);if(!source||!app::ftn_local_address(network,*source))message_detail::unavailable();
        const auto current=ftn_scan_detail::read(db,c,intent.message.value,intent.network);if(current.operation.bytes!=intent.operation.bytes||!ftn_work_detail::same(current.fingerprint,intent.fingerprint))message_detail::conflict();
        SqliteStatement state(db,"SELECT state FROM sfx_ftn_message_intents WHERE message_id=?1 AND network=?2","reconcile only pending or scanned intent");state.bind(1,intent.message.value);state.bind(2,intent.network);if(!state.row()||state.int64(0)==2)message_detail::conflict();const bool replay=state.int64(0)==1;
        std::set<std::string> links;for(const auto& job:jobs){if(job.operation.bytes!=intent.operation.bytes||job.network!=intent.network||job.kind!=app::FtnOutboundKind::packet||!links.insert(job.link).second)message_detail::conflict();if(!intent.tag.empty()){if(!app::ftn_echo_permission(c,network.code,job.link,intent.tag,false,false))message_detail::unavailable();}else{const auto destination=app::parse_ftn_address(intent.destination);const auto route=destination?app::ftn_route_link(c,network.code,*destination):std::nullopt;if(!route||*route!=job.link||job.link!=intent.via_link)message_detail::unavailable();}}
        if(replay){SqliteStatement existing(db,"SELECT job_id FROM sfx_ftn_message_jobs WHERE message_id=?1 AND network=?2","reconcile complete prior scan set");existing.bind(1,intent.message.value);existing.bind(2,intent.network);std::size_t count{};while(existing.row()){const auto id=ftn_work_detail::identifier(existing,0);if(std::none_of(jobs.begin(),jobs.end(),[&](const auto& job){return job.id.bytes==id.bytes;}))message_detail::conflict();++count;}if(count!=jobs.size())message_detail::conflict();}
        auto tagged=jobs;for(auto& job:tagged){if(!job.echo_tag.empty()&&job.echo_tag!=intent.tag)message_detail::conflict();job.echo_tag=intent.tag;}ftn_work_detail::queue(db,c,tagged,now);ftn_scan_detail::freeze(db,current);if(!replay){for(const auto& job:jobs){SqliteStatement bind(db,"INSERT INTO sfx_ftn_message_jobs VALUES(?1,?2,?3)","bind sender history to actual immutable outbound artifact");bind.bind(1,intent.message.value);bind.bind(2,intent.network);bind.bind(3,job.id);bind.step();}SqliteStatement finish(db,"UPDATE sfx_ftn_message_intents SET state=1,reason='' WHERE message_id=?1 AND network=?2 AND state=0","finish scan only with all jobs committed");finish.bind(1,intent.message.value);finish.bind(2,intent.network);finish.step();}out=true;return SQLITE_OK;}).status;
}
application::StorageResult<application::FtnSerialState> SqliteAccountRepository::ftn_serial_state(const application::MessageConfigurationActor& a){
    if(!ftn_work_detail::owner(a))return read_failure<application::FtnSerialState>(invalid("validate FTN serial owner"));
    return impl_->readers_.read<application::FtnSerialState>("read FTN serial recovery state",[a](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);return read_ftn_serial_state(db);});
}
application::StorageResult<application::FtnHistoryRecoveryQueue> SqliteAccountRepository::ftn_history_recovery_queue(const application::MessageConfigurationActor& a){
    using Result=application::FtnHistoryRecoveryQueue;
    if(!ftn_work_detail::owner(a))return read_failure<Result>(invalid("validate stopped FTN recovery queue owner"));
    return impl_->readers_.read<Result>("read required FTN recovery payload bindings",[a](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);Result out;
        const auto outgoing=std::string("SELECT ")+ftn_work_detail::job_columns+" FROM sfx_ftn_outbound WHERE state IN(0,1,2) ORDER BY job_id LIMIT 65537";
        SqliteStatement jobs(db,outgoing.c_str(),"read pending outbound recovery bindings");
        while(jobs.row())out.outbound.push_back(ftn_work_detail::job(jobs));
        const auto incoming=std::string("SELECT ")+ftn_work_detail::ingress_columns+" FROM sfx_ftn_ingress WHERE state IN(0,1,2) ORDER BY receipt_id LIMIT 65537";
        SqliteStatement receipts(db,incoming.c_str(),"read pending inbound recovery bindings");
        while(receipts.row())out.ingress.push_back(ftn_work_detail::ingress(receipts));
        if(out.outbound.size()>65536||out.ingress.size()>65536)
            throw StorageFailure(application::StorageErrorCode::backpressure,"FTN recovery queue exceeds bounded review",SQLITE_TOOBIG);
        return out;
    });
}
application::StorageStatus SqliteAccountRepository::preserve_ftn_serial_history(const application::MessageConfigurationActor& a,const std::optional<application::FtnSerialState>& prior,bool restored_predecessor){
    if(!ftn_work_detail::owner(a))return invalid("validate host FTN restore authority");
    return impl_->write_result<bool>("preserve FTN serial floor before restore publication",4096,[a,prior,restored_predecessor](sqlite3* db,std::optional<bool>& out){
        message_detail::config_authority(db,a);const auto current=read_ftn_serial_state(db);
        // Only the exclusive native restore caller supplies this classification,
        // after validating the predecessor schema before opening/migrating it.
        if(restored_predecessor){SqliteStatement count(db,"SELECT count(*) FROM sfx_ftn_serial_allocations","verify newly initialized predecessor history");if(current.high_water||!count.row()||count.int64(0))message_detail::conflict();}
        const bool valid_prior=prior&&(prior->predecessor?(!prior->high_water&&!prior->lineage.valid()):prior->lineage.valid());
        const auto floor=valid_prior?std::max(current.high_water,prior->high_water):current.high_water;
        const bool held=current.recovery_required;
        if(floor==UINT32_MAX)throw StorageFailure(application::StorageErrorCode::backpressure,"FTN message serial exhausted; restored history preserved",SQLITE_FULL);
        SqliteStatement update(db,"UPDATE sfx_ftn_serial_allocator SET high_water=?1,recovery_required=?2 WHERE singleton_id=1","raise matching history floor or persist unresolved-history hold");update.bind(1,static_cast<std::int64_t>(floor));update.bind(2,held);update.step();const auto saved=read_ftn_serial_state(db);if(saved.high_water!=floor||saved.recovery_required!=held||saved.lineage.bytes!=current.lineage.bytes)message_detail::conflict();out=true;return SQLITE_OK;}).status;
}
application::StorageResult<application::FtnSerialRecoveryReceipt> SqliteAccountRepository::recover_ftn_serial_history(
    const application::MessageConfigurationActor& a,const application::FtnSerialRecoveryRequest& request){
    namespace app=application;
    if(!ftn_work_detail::owner(a)||!request.operation.valid())
        return read_failure<app::FtnSerialRecoveryReceipt>(invalid("validate exclusive FTN history recovery authority"));
    const auto now=impl_->message_utc_now_?impl_->message_utc_now_():-1;
    return impl_->write_result<app::FtnSerialRecoveryReceipt>("recover only validated FTN serial history",4096,
        [a,request,now](sqlite3* db,std::optional<app::FtnSerialRecoveryReceipt>& out){
        message_detail::config_authority(db,a);
        if(!validate_sfx_shipping_database(db,true))throw StorageFailure(app::StorageErrorCode::corrupt,"FTN recovery database verification failed",SQLITE_CORRUPT);
        const auto current=read_ftn_serial_state(db);
        SqliteStatement prior_receipt(db,"SELECT operation_id,lineage,generation,recovered_utc,old_floor,new_floor,prior_known,held_outbound,held_ingress FROM sfx_ftn_serial_recoveries WHERE operation_id=?1","reconcile committed FTN recovery before retry");
        prior_receipt.bind(1,request.operation);
        if(prior_receipt.row()){
            app::FtnSerialRecoveryReceipt saved;
            saved.operation=ftn_work_detail::identifier(prior_receipt,0);saved.lineage=ftn_work_detail::identifier(prior_receipt,1);
            saved.generation=prior_receipt.int64(2);saved.utc=prior_receipt.int64(3);
            saved.old_floor=static_cast<std::uint32_t>(prior_receipt.int64(4));saved.new_floor=static_cast<std::uint32_t>(prior_receipt.int64(5));saved.prior_known=prior_receipt.boolean(6);saved.held_outbound=static_cast<std::uint64_t>(prior_receipt.int64(7));saved.held_ingress=static_cast<std::uint64_t>(prior_receipt.int64(8));
            if(current.recovery_required||current.lineage.bytes!=saved.lineage.bytes||current.recovery_generation!=saved.generation||current.high_water<saved.new_floor)message_detail::conflict();
            out=saved;return SQLITE_OK;
        }
        const auto& expected=request.expected;
        if(!current.recovery_required||expected.predecessor||!expected.recovery_required||
           current.high_water!=expected.high_water||current.lineage.bytes!=expected.lineage.bytes||current.recovery_generation!=expected.recovery_generation)
            message_detail::conflict();
        if(now<0||now>UINT32_MAX||current.recovery_generation==INT64_MAX)
            throw StorageFailure(app::StorageErrorCode::backpressure,"FTN recovery clock or generation outside range",SQLITE_FULL);
        const auto& prior=request.prior;
        const bool known=prior&&(prior->predecessor?(!prior->high_water&&!prior->lineage.valid()):prior->lineage.valid());
        const auto floor=known?std::max(current.high_water,prior->high_water):current.high_water;
        if(floor==UINT32_MAX)throw StorageFailure(app::StorageErrorCode::backpressure,"FTN message serial exhausted; history preserved",SQLITE_FULL);
        app::FtnSerialRecoveryReceipt receipt{request.operation,current.lineage,current.recovery_generation+1,now,current.high_water,floor,known};
        SqliteStatement holds(db,"SELECT (SELECT count(*) FROM sfx_ftn_outbound WHERE state=4),(SELECT count(*) FROM sfx_ftn_ingress WHERE state=3)","retain separate held-item counts with recovery receipt");
        if(!holds.row()){message_detail::conflict();}
        receipt.held_outbound=static_cast<std::uint64_t>(holds.int64(0));receipt.held_ingress=static_cast<std::uint64_t>(holds.int64(1));
        SqliteStatement insert(db,"INSERT INTO sfx_ftn_serial_recoveries VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)","append exact immutable FTN history recovery receipt");
        insert.bind(1,receipt.operation);insert.bind(2,receipt.generation);insert.bind(3,receipt.lineage);insert.bind(4,static_cast<std::int64_t>(receipt.old_floor));insert.bind(5,static_cast<std::int64_t>(receipt.new_floor));insert.bind(6,receipt.utc);insert.bind(7,receipt.prior_known);insert.bind(8,static_cast<std::int64_t>(receipt.held_outbound));insert.bind(9,static_cast<std::int64_t>(receipt.held_ingress));insert.step();
        SqliteStatement update(db,"UPDATE sfx_ftn_serial_allocator SET high_water=?1,recovery_generation=?2,recovery_required=0 WHERE singleton_id=1 AND recovery_required=1","publish receipted history-only recovery without changing lineage or mail");
        update.bind(1,static_cast<std::int64_t>(floor));update.bind(2,receipt.generation);update.step();
        const auto saved=read_ftn_serial_state(db);
        if(saved.recovery_required||saved.high_water!=floor||saved.lineage.bytes!=current.lineage.bytes||saved.recovery_generation!=receipt.generation)message_detail::conflict();
        out=receipt;return SQLITE_OK;
    });
}
application::StorageResult<std::optional<application::FtnSerialRecoveryReceipt>> SqliteAccountRepository::last_ftn_serial_recovery(const application::MessageConfigurationActor& a){
    using Result=std::optional<application::FtnSerialRecoveryReceipt>;
    if(!ftn_work_detail::owner(a))return read_failure<Result>(invalid("validate FTN recovery report authority"));
    return impl_->readers_.read<Result>("read committed FTN history recovery report",[a](sqlite3* db)->Result{
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);
        const auto state=read_ftn_serial_state(db);if(state.recovery_required)message_detail::conflict();if(!state.recovery_generation)return {};
        SqliteStatement q(db,"SELECT operation_id,lineage,generation,recovered_utc,old_floor,new_floor,prior_known,held_outbound,held_ingress FROM sfx_ftn_serial_recoveries WHERE generation=?1","read current immutable recovery generation");q.bind(1,state.recovery_generation);
        if(!q.row())throw StorageFailure(application::StorageErrorCode::corrupt,"FTN recovery receipt missing",SQLITE_CORRUPT);
        application::FtnSerialRecoveryReceipt saved;
        saved.operation=ftn_work_detail::identifier(q,0);saved.lineage=ftn_work_detail::identifier(q,1);saved.generation=q.int64(2);saved.utc=q.int64(3);
        saved.old_floor=static_cast<std::uint32_t>(q.int64(4));saved.new_floor=static_cast<std::uint32_t>(q.int64(5));saved.prior_known=q.boolean(6);saved.held_outbound=static_cast<std::uint64_t>(q.int64(7));saved.held_ingress=static_cast<std::uint64_t>(q.int64(8));
        if(saved.lineage.bytes!=state.lineage.bytes||saved.new_floor>state.high_water)throw StorageFailure(application::StorageErrorCode::corrupt,"FTN recovery receipt conflicts with allocator",SQLITE_CORRUPT);
        return saved;
    });
}
