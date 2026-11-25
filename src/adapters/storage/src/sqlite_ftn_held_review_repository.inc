namespace ftn_held_review_detail {
bool text(std::string_view value,std::size_t maximum){return value.size()<=maximum&&std::none_of(value.begin(),value.end(),[](unsigned char c){return c<32||c==127;});}
application::FtnHeldReviewRow row(SqliteStatement& q){
    application::FtnHeldReviewRow out;out.ingress=ftn_work_detail::identifier(q,0);out.received_utc=q.int64(1);out.network=q.text(2);out.link=q.text(3);out.file=q.text(4);out.reason=q.text(5);out.state=static_cast<application::FtnIngressState>(q.int64(6));out.retry_pending=q.boolean(7);out.from=q.text(8);out.tic=q.boolean(9);return out;
}
const std::string tic_exists="EXISTS(SELECT 1 FROM sfx_ftn_tic_rejections t WHERE t.ingress_id=i.receipt_id)";
const std::string reason="coalesce((SELECT nullif(r.reason,'') FROM sfx_ftn_tic_retries r WHERE r.ingress_id=i.receipt_id ORDER BY r.sequence DESC LIMIT 1),(SELECT t.reason FROM sfx_ftn_tic_rejections t WHERE t.ingress_id=i.receipt_id ORDER BY t.sequence DESC LIMIT 1),i.reason)";
const std::string pending="(EXISTS(SELECT 1 FROM sfx_ftn_reprocess_requests r WHERE r.ingress_id=i.receipt_id AND r.finished=0) OR EXISTS(SELECT 1 FROM sfx_ftn_tic_retries r WHERE r.ingress_id=i.receipt_id AND r.finished=0))";
const std::string columns="i.receipt_id,i.created_utc,i.network,i.link,i.wire_name,"+reason+",i.state,"+pending+",coalesce((SELECT nullif(t.claimed_from,'') FROM sfx_ftn_tic_rejections t WHERE t.ingress_id=i.receipt_id ORDER BY t.sequence DESC LIMIT 1),(SELECT h.sender FROM sfx_ftn_held_messages h WHERE h.ingress_id=i.receipt_id ORDER BY h.message_index LIMIT 1),''),"+tic_exists;

}
application::StorageResult<application::FtnHeldReview> SqliteAccountRepository::ftn_held_review(const application::MessageConfigurationActor& a,const application::FtnHeldReviewQuery& q){
    namespace app=application;using Result=app::FtnHeldReview;
    if(!ftn_work_detail::owner(a)||(!q.network.empty()&&!app::valid_ftn_code(q.network))||!ftn_work_detail::reason(q.reason)||!ftn_held_review_detail::text(q.find,140)||q.message_offset>4096||(q.ingress.valid()?(q.after.valid()||!q.network.empty()||!q.reason.empty()||!q.find.empty()):q.message_offset!=0))return read_failure<Result>(invalid("validate bounded private held review"));
    return impl_->readers_.read<Result>("read owner held review without packet content",[a,q](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);Result out;const auto c=ftn_detail::config(db);out.configuration_revision=c.revision;out.mode=c.settings.mode;out.serial_recovery_required=read_ftn_serial_state(db).recovery_required;
        if(!q.ingress.valid()){
            const auto sql=std::string("SELECT ")+ftn_held_review_detail::columns+" FROM sfx_ftn_ingress i WHERE i.receipt_id>?1 AND (i.state=3 OR (i.state IN(1,2) AND "+ftn_held_review_detail::tic_exists+") OR "+ftn_held_review_detail::pending+") AND (?2='' OR i.network=?2 COLLATE NOCASE OR EXISTS(SELECT 1 FROM sfx_ftn_held_messages h WHERE h.ingress_id=i.receipt_id AND h.network=?2 COLLATE NOCASE)) AND (?3='' OR "+ftn_held_review_detail::reason+"=?3) AND (?4='' OR instr(lower(i.wire_name||' '||i.link||' '||"+ftn_held_review_detail::reason+"),lower(?4))>0 OR EXISTS(SELECT 1 FROM sfx_ftn_held_messages h WHERE h.ingress_id=i.receipt_id AND instr(lower(h.sender||' '||h.recipient||' '||h.echo_tag),lower(?4))>0) OR EXISTS(SELECT 1 FROM sfx_ftn_tic_rejections t WHERE t.ingress_id=i.receipt_id AND instr(lower(t.claimed_from||' '||t.echo_tag||' '||t.payload_name),lower(?4))>0)) ORDER BY i.receipt_id LIMIT 33";
            SqliteStatement rows(db,sql.c_str(),"bounded keyset includes malformed holds and pending exact retries");rows.bind(1,q.after);rows.bind(2,q.network);rows.bind(3,q.reason);rows.bind(4,q.find);while(rows.row()){if(out.rows.size()==32){out.more=true;break;}out.rows.push_back(ftn_held_review_detail::row(rows));}return out;
        }
        const auto sql=std::string("SELECT ")+ftn_held_review_detail::columns+" FROM sfx_ftn_ingress i WHERE i.receipt_id=?1 AND (i.state=3 OR EXISTS(SELECT 1 FROM sfx_ftn_held_messages h WHERE h.ingress_id=i.receipt_id) OR EXISTS(SELECT 1 FROM sfx_ftn_reprocess_requests r WHERE r.ingress_id=i.receipt_id) OR "+ftn_held_review_detail::tic_exists+")";
        SqliteStatement row(db,sql.c_str(),"exact reviewed original including subsequent result");row.bind(1,q.ingress);if(!row.row())message_detail::unavailable();out.rows.push_back(ftn_held_review_detail::row(row));if(out.rows.front().tic){
            if(q.message_offset)message_detail::conflict();
            const auto reviewed=ftn_tic_review_detail::review(db,q.ingress);if(!reviewed)message_detail::unavailable();app::FtnHeldTicDetail d;d.review=reviewed->review;d.claimed_from=reviewed->claimed_from;d.echo=reviewed->echo;d.payload=reviewed->payload;d.retry=reviewed->retry;d.forward_plan_present=reviewed->forward_plan.has_value();
            if(reviewed->publication){const auto& p=*reviewed->publication;d.publication=p.request.operation;d.phase=static_cast<std::uint8_t>(p.phase);if(d.claimed_from.empty())d.claimed_from=p.request.from;if(d.echo.empty())d.echo=p.request.tag;if(d.payload.empty())d.payload=p.request.filename;}out.tic=std::move(d);return out;
        }out.retry=ftn_reprocess_detail::latest(db,q.ingress);
        SqliteStatement messages(db,"SELECT h.operation_id,h.message_index,h.network,h.sender,h.recipient,h.origin,h.destination,h.echo_tag,h.reason,h.local_private,EXISTS(SELECT 1 FROM sfx_ftn_toss_progress p WHERE p.ingress_id=h.ingress_id AND p.message_index=h.message_index) FROM sfx_ftn_held_messages h WHERE h.ingress_id=?1 AND h.message_index>=?2 ORDER BY h.message_index LIMIT 33","bounded redacted original message observations");messages.bind(1,q.ingress);messages.bind(2,static_cast<std::int64_t>(q.message_offset));
        while(messages.row()){
            if(out.messages.size()==32){out.more=true;break;}app::FtnHeldReviewMessage m;m.operation=ftn_work_detail::identifier(messages,0);m.message_index=static_cast<std::uint16_t>(messages.int64(1));m.network=messages.text(2);m.from=messages.text(3);m.to=messages.text(4);m.origin=messages.text(5);m.destination=messages.text(6);m.echo=messages.text(7);m.reason=messages.text(8);m.local_private=messages.boolean(9);m.committed=messages.boolean(10);
            const auto resolution_sql=std::string("SELECT ")+ftn_held_detail::resolution_columns+" FROM sfx_ftn_recipient_resolutions WHERE held_message_id=?1 ORDER BY sequence DESC LIMIT 1";SqliteStatement resolution(db,resolution_sql.c_str(),"current separate recipient decision in same read snapshot");resolution.bind(1,m.operation);if(resolution.row())m.resolution=ftn_held_detail::resolution(resolution);out.messages.push_back(std::move(m));
        }return out;
    });
}
application::StorageResult<application::FtnHeldRecipients> SqliteAccountRepository::ftn_held_recipients(const application::MessageConfigurationActor& a,const application::FtnHeldRecipientQuery& q){
    using Result=application::FtnHeldRecipients;if(!ftn_work_detail::owner(a)||q.after<0||!ftn_held_review_detail::text(q.find,140))return read_failure<Result>(invalid("validate bounded active recipient choices"));
    return impl_->readers_.read<Result>("read active local names for explicit recipient review",[a,q](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);SqliteStatement rows(db,"SELECT a.account_id,a.revision,a.display_name FROM sfx_accounts a JOIN sfx_security_levels l USING(security_level) WHERE a.account_id>?1 AND a.account_state='active' AND l.level_state='active' AND a.display_name IS NOT NULL AND (?2='' OR instr(lower(a.display_name),lower(?2))>0) ORDER BY a.account_id LIMIT 33","active identity and revision only; no private account profile");rows.bind(1,q.after);rows.bind(2,q.find);Result out;while(rows.row()){if(out.rows.size()==32){out.more=true;break;}out.rows.push_back({rows.int64(0),rows.int64(1),rows.text(2)});}return out;});
}

application::StorageResult<application::FtnQueuePage> SqliteAccountRepository::sysop_ftn_queues(const application::MessageActor& actor,std::string_view network,std::uint32_t offset){
    using Result=application::FtnQueuePage;if(!application::valid_ftn_code(network)||offset>1000000)return read_failure<Result>(invalid("validate bounded Sysop network queues"));
    return impl_->readers_.read<Result>("read selected network queues for authenticated Sysop",[actor,network=std::string(network),offset](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,{false,actor});const auto config=ftn_detail::config(db);
        if(std::none_of(config.networks.begin(),config.networks.end(),[&](const auto& n){return n.code==network&&n.enabled;}))message_detail::unavailable();
        Result out;SqliteStatement rows(db,"SELECT receipt_id,0,network,link,wire_name,reason,state,declared_size FROM sfx_ftn_ingress WHERE state<>4 AND network=?1 COLLATE NOCASE UNION ALL SELECT job_id,1,network,link,wire_name,reason,state,size FROM sfx_ftn_outbound WHERE state NOT IN(3,5) AND network=?1 COLLATE NOCASE ORDER BY 2,1 LIMIT 65 OFFSET ?2","bounded network-only metadata");rows.bind(1,network);rows.bind(2,static_cast<std::int64_t>(offset));
        while(rows.row()){if(out.rows.size()==64){out.more=true;break;}application::FtnQueueRow row;row.id=ftn_work_detail::identifier(rows,0);row.outbound=rows.boolean(1);row.network=rows.text(2);row.link=rows.text(3);row.file=rows.text(4);row.reason=rows.text(5);row.state=static_cast<std::uint8_t>(rows.int64(6));row.bytes=rows.int64(7);out.rows.push_back(std::move(row));}return out;
    });
}
application::StorageResult<application::FtnHeldReview> SqliteAccountRepository::sysop_ftn_held(const application::MessageActor& actor,std::string_view network,application::Identifier128 after){
    using Result=application::FtnHeldReview;if(!application::valid_ftn_code(network))return read_failure<Result>(invalid("validate bounded Sysop held view"));
    return impl_->readers_.read<Result>("read selected network held originals without mutation",[actor,network=std::string(network),after](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,{false,actor});const auto config=ftn_detail::config(db);
        if(std::none_of(config.networks.begin(),config.networks.end(),[&](const auto& n){return n.code==network&&n.enabled;}))message_detail::unavailable();
        Result out;out.configuration_revision=config.revision;out.mode=config.settings.mode;
        const auto sql=std::string("SELECT ")+ftn_held_review_detail::columns+" FROM sfx_ftn_ingress i WHERE i.receipt_id>?1 AND i.network=?2 COLLATE NOCASE AND (i.state=3 OR (i.state IN(1,2) AND "+ftn_held_review_detail::tic_exists+") OR "+ftn_held_review_detail::pending+") ORDER BY i.receipt_id LIMIT 33";
        SqliteStatement rows(db,sql.c_str(),"bounded held metadata without packet bodies");rows.bind(1,after);rows.bind(2,network);
        while(rows.row()){if(out.rows.size()==32){out.more=true;break;}out.rows.push_back(ftn_held_review_detail::row(rows));}return out;
    });
}
