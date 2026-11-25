namespace ftn_external_detail {
namespace app=application;
using Batch=std::vector<app::FtnExternalPublication>;
Batch batch(sqlite3* db,const app::Identifier128& claim){
    const auto sql=std::string("SELECT ")+ftn_work_detail::job_columns+
        " FROM sfx_ftn_outbound WHERE job_id IN(SELECT job_id FROM sfx_ftn_external_publications WHERE claim_id=?1) ORDER BY CASE kind WHEN 2 THEN 0 WHEN 0 THEN 1 ELSE 2 END,ordinal,job_id LIMIT 65";
    SqliteStatement q(db,sql.c_str(),"read one complete bounded external publication");q.bind(1,claim);Batch result;
    while(q.row()){
        if(result.size()==64)message_detail::conflict();
        app::FtnExternalPublication row;row.job=ftn_work_detail::job(q);
        SqliteStatement p(db,"SELECT busy_path,flow_path,copy_path,state FROM sfx_ftn_external_publications WHERE job_id=?1 AND claim_id=?2","read frozen relative publication paths");p.bind(1,row.job.id);p.bind(2,claim);if(!p.row())message_detail::conflict();
        row.busy_path=p.text(0);row.flow_path=p.text(1);row.copy_path=p.text(2);row.state=static_cast<std::uint8_t>(p.int64(3));result.push_back(std::move(row));
    }return result;
}
}
application::StorageResult<std::vector<application::FtnExternalPublication>> SqliteAccountRepository::prepare_ftn_external_handoff(const application::MessageConfigurationActor& a,std::int64_t revision,const application::Identifier128& claim){
    namespace app=application;using Result=ftn_external_detail::Batch;
    if(!ftn_work_detail::owner(a)||revision<1||!claim.valid())return read_failure<Result>(invalid("validate external publication owner"));
    const auto now=impl_->message_utc_now_?impl_->message_utc_now_():0;
    return impl_->write_result<Result>("prepare durable external mailer handoff",131072,[a,revision,claim,now](sqlite3* db,std::optional<Result>& out){
        message_detail::config_authority(db,a);const auto c=ftn_detail::config(db);
        if(c.revision!=revision||c.settings.mode!=app::FtnMailerMode::external_binkd)message_detail::conflict();
        auto prior=ftn_external_detail::batch(db,claim);if(!prior.empty()){out=std::move(prior);return SQLITE_OK;}
        const auto sql=std::string("SELECT ")+ftn_work_detail::job_columns+" FROM sfx_ftn_outbound WHERE claim_id=?1 AND state=1 ORDER BY ordinal,job_id LIMIT 65";
        SqliteStatement q(db,sql.c_str(),"freeze exact claimed outgoing batch before filesystem work");q.bind(1,claim);std::vector<app::FtnOutboundJob> jobs;while(q.row())jobs.push_back(ftn_work_detail::job(q));
        if(jobs.empty()||jobs.size()>64)message_detail::unavailable();
        std::string busy;
        for(const auto& job:jobs){
            const auto& l=ftn_work_detail::link(c,job.network,job.link,true);const auto& n=ftn_work_detail::network(c,l.network);
            const auto paths=app::ftn_external_outbound_paths(n,l,job);if(!paths)message_detail::unavailable();
            if(busy.empty())busy=paths->busy;else if(busy!=paths->busy)message_detail::conflict();
            SqliteStatement p(db,"INSERT INTO sfx_ftn_external_publications VALUES(?1,?2,?3,?4,?5,0,?6)","retain original job and separate disposable destination");p.bind(1,job.id);p.bind(2,claim);p.bind(3,paths->busy);p.bind(4,paths->flow);p.bind(5,paths->copy);p.bind(6,now);p.step();
        }
        out=ftn_external_detail::batch(db,claim);return SQLITE_OK;
    });
}
application::StorageResult<std::vector<application::FtnExternalPublication>> SqliteAccountRepository::pending_ftn_external_handoff(const application::MessageConfigurationActor& a,const application::Identifier128& after){
    using Result=ftn_external_detail::Batch;if(!ftn_work_detail::owner(a))return read_failure<Result>(invalid("validate external recovery owner"));
    return impl_->readers_.read<Result>("read one unresolved external publication",[a,after](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);
        SqliteStatement q(db,"SELECT claim_id FROM sfx_ftn_external_publications p WHERE state IN(0,1) AND (?1=0 OR claim_id>?2) AND NOT EXISTS(SELECT 1 FROM sfx_ftn_external_releases r WHERE r.claim_id=p.claim_id) ORDER BY claim_id LIMIT 1","recover prepared publication or committed lock release without splitting a batch");q.bind(1,after.valid());q.bind(2,after);
        return q.row()?ftn_external_detail::batch(db,q.identifier(0)):Result{};
    });
}
application::StorageStatus SqliteAccountRepository::release_ftn_external_handoff(const application::MessageConfigurationActor& a,const application::Identifier128& claim){
    if(!ftn_work_detail::owner(a)||!claim.valid())return invalid("validate external release owner");
    const auto now=impl_->message_utc_now_?impl_->message_utc_now_():0;
    return impl_->write_result<bool>("record release after committed external handoff",4096,[a,claim,now](sqlite3* db,std::optional<bool>& out){
        message_detail::config_authority(db,a);const auto batch=ftn_external_detail::batch(db,claim);
        if(batch.empty()||std::any_of(batch.begin(),batch.end(),[](const auto& row){return row.state!=1||row.job.state!=application::FtnOutboundState::external_handoff;}))message_detail::conflict();
        SqliteStatement q(db,"INSERT INTO sfx_ftn_external_releases VALUES(?1,?2) ON CONFLICT(claim_id) DO NOTHING","release recorded separately from publication commit");q.bind(1,claim);q.bind(2,now);q.step();out=true;return SQLITE_OK;
    }).status;
}
application::StorageStatus SqliteAccountRepository::finish_ftn_external_handoff(const application::MessageConfigurationActor& a,const application::Identifier128& claim,bool published){
    namespace app=application;if(!ftn_work_detail::owner(a)||!claim.valid())return invalid("validate exact external publication completion");const auto now=impl_->message_utc_now_?impl_->message_utc_now_():0;
    return impl_->write_result<bool>("commit external handoff without claiming delivery",131072,[a,claim,published,now](sqlite3* db,std::optional<bool>& out){
        message_detail::config_authority(db,a);const auto batch=ftn_external_detail::batch(db,claim);if(batch.empty())message_detail::unavailable();
        for(const auto& row:batch){
            const auto desired=published?1:2;if(row.state==desired)continue;
            if(row.state||row.job.state!=app::FtnOutboundState::claimed||row.job.claim!=claim)message_detail::conflict();
            SqliteStatement intent(db,"UPDATE sfx_ftn_external_publications SET state=?2 WHERE job_id=?1 AND state=0","resolve exact prepared publication");intent.bind(1,row.job.id);intent.bind(2,static_cast<std::int64_t>(desired));intent.step();
            SqliteStatement job(db,"UPDATE sfx_ftn_outbound SET state=?2,reason=?3,updated_utc=?4 WHERE job_id=?1 AND state=1 AND claim_id=?5","handoff completes SFX responsibility, never remote ACK");job.bind(1,row.job.id);job.bind(2,std::int64_t{published?5:4});job.bind(3,std::string_view(published?"handed_to_binkd":"external_publication_uncertain"));job.bind(4,now);job.bind(5,claim);job.step();if(sqlite3_changes(db)!=1)message_detail::conflict();
            if(published){SqliteStatement provider(db,"UPDATE sfx_ftn_provider_requests SET state=1,reason='external_handoff_only' WHERE state=0 AND operation_id IN(SELECT operation_id FROM sfx_ftn_control_intents WHERE job_id=?1)","provider acceptance still requires its reply");provider.bind(1,row.job.id);provider.step();}
        }out=true;return SQLITE_OK;
    }).status;
}
