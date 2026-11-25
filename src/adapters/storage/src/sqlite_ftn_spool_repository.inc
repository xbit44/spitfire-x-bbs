namespace ftn_spool_detail {
namespace app=application;
app::FtnDigest fingerprint(std::int64_t revision,const app::FtnOutboundJob& job){
    file_detail::Fingerprint hash("FTN-forward-spool-v1");hash.number(revision);
    for(const auto& id:{job.id,job.operation})hash.text({reinterpret_cast<const char*>(id.bytes.data()),id.bytes.size()});
    hash.number(job.ordinal);hash.text(job.network);hash.text(job.link);hash.number(static_cast<std::int64_t>(job.kind));hash.number(static_cast<std::int64_t>(job.flavor));hash.text(job.file.name);hash.number(job.file.size);hash.number(job.file.timestamp);hash.text({reinterpret_cast<const char*>(job.digest.data()),job.digest.size()});return hash.value();
}
bool object(const app::FileObjectIdentity& value){const auto decimal=[](const std::string& s){return !s.empty()&&s.size()<=20&&std::all_of(s.begin(),s.end(),[](char c){return c>='0'&&c<='9';});};return decimal(value.volume)&&decimal(value.file_id);}
app::FtnSpoolObject read(sqlite3* db,const app::Identifier128& id){
    SqliteStatement q(db,"SELECT s.job_id,s.publication_id,s.ordinal,s.network,s.link,s.kind,s.flavor,s.wire_name,s.size,s.wire_time,s.content_sha256,0,NULL,NULL,0,'',s.configuration_revision,b.volume,b.native_id,r.job_id,s.fingerprint FROM sfx_ftn_spool_objects s LEFT JOIN sfx_ftn_spool_bindings b USING(job_id) LEFT JOIN sfx_ftn_spool_ready r USING(job_id) WHERE s.job_id=?1","read immutable outbound spool intent and native readiness");q.bind(1,id);if(!q.row())message_detail::unavailable();app::FtnSpoolObject value;value.job=ftn_work_detail::job(q);value.configuration_revision=q.int64(16);if(!q.is_null(17))value.object=app::FileObjectIdentity{q.text(17),q.text(18)};value.ready=!q.is_null(19);if(fingerprint(value.configuration_revision,value.job)!=file_detail::digest(q,20))file_detail::corrupt();return value;
}
void authorize(sqlite3* db,const app::FtnSpoolObject& value,const app::Identifier128& attempt){
    SqliteStatement typed(db,"SELECT publication_id,hatch_id FROM sfx_ftn_spool_owners WHERE operation_id=?1","require exact immutable typed journal owner");typed.bind(1,value.job.operation);if(!typed.row())message_detail::unavailable();
    if(!typed.is_null(1)){if(attempt.valid())message_detail::conflict();ftn_hatch_detail::authorize_spool(db,value);return;}
    if(typed.is_null(0))file_detail::corrupt();
    const auto publication=ftn_file_detail::read(db,value.job.operation);const auto& request=publication.request;
    if(request.configuration_revision!=value.configuration_revision||publication.phase==app::FtnFilePhase::completed)message_detail::conflict();
    ftn_file_detail::current(db,publication,attempt);const auto c=ftn_detail::config(db);
    ftn_forward_plan_detail::authorize_job(db,value.configuration_revision,value.job,attempt);
    if(!ftn_detail::same(value.job.network,request.network)||ftn_detail::same(value.job.link,request.link)||!app::ftn_echo_permission(c,request.network,value.job.link,request.tag,true,false))message_detail::unavailable();
    (void)ftn_work_detail::link(c,request.network,value.job.link,true);
    if(value.job.kind==app::FtnOutboundKind::payload&&(value.job.file.name!=request.filename||value.job.file.size!=request.payload.bytes||value.job.digest!=request.payload.sha256))message_detail::conflict();
}
void ready(sqlite3* db,const app::FtnOutboundJob& job,std::int64_t revision){
    const auto value=read(db,job.id);if(!value.ready||!value.object||fingerprint(revision,job)!=fingerprint(value.configuration_revision,value.job))message_detail::conflict();
}
}
application::StorageResult<application::FtnSpoolObject> SqliteAccountRepository::prepare_ftn_spool(const application::MessageConfigurationActor& a,std::int64_t revision,const application::FtnOutboundJob& job,const application::Identifier128& attempt){
    namespace app=application;using Result=app::FtnSpoolObject;
    if(!ftn_work_detail::owner(a)||revision<1||!ftn_work_detail::valid_job(job)||!job.echo_tag.empty()||job.kind==app::FtnOutboundKind::packet||(job.kind==app::FtnOutboundKind::tic&&job.file.size>65536))return read_failure<Result>(invalid("validate bounded forwarding spool intent"));
    return impl_->write_result<Result>("reserve immutable forwarding object before native creation",8192,[a,revision,job,attempt](sqlite3* db,std::optional<Result>& out){
        message_detail::config_authority(db,a);Result value;value.job=job;value.configuration_revision=revision;ftn_spool_detail::authorize(db,value,attempt);const auto hash=ftn_spool_detail::fingerprint(revision,job);
        SqliteStatement prior(db,"SELECT fingerprint FROM sfx_ftn_spool_objects WHERE job_id=?1","reconcile original generated metadata without replacing changed intent");prior.bind(1,job.id);if(prior.row()){if(file_detail::digest(prior,0)!=hash)message_detail::conflict();out=ftn_spool_detail::read(db,job.id);return SQLITE_OK;}
        SqliteStatement add(db,"INSERT INTO sfx_ftn_spool_objects VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)","bind expected forwarding descriptor and complete content digest");add.bind(1,job.id);add.bind(2,job.operation);add.bind(3,revision);add.bind(4,hash);add.bind(5,job.ordinal);add.bind(6,job.network);add.bind(7,job.link);add.bind(8,static_cast<std::int64_t>(job.kind));add.bind(9,static_cast<std::int64_t>(job.flavor));add.bind(10,job.file.name);add.bind(11,static_cast<std::int64_t>(job.file.size));add.bind(12,static_cast<std::int64_t>(job.file.timestamp));add.bind(13,job.digest);add.step();out=std::move(value);return SQLITE_OK;
    });
}
application::StorageResult<application::FtnSpoolObject> SqliteAccountRepository::bind_ftn_spool(const application::MessageConfigurationActor& a,const application::Identifier128& id,const application::FileObjectIdentity& object,const application::Identifier128& attempt){
    using Result=application::FtnSpoolObject;if(!ftn_work_detail::owner(a)||!id.valid()||!ftn_spool_detail::object(object))return read_failure<Result>(invalid("bind exact new native forwarding object"));
    return impl_->write_result<Result>("retain exact spool object before its first write",4096,[a,id,object,attempt](sqlite3* db,std::optional<Result>& out){
        message_detail::config_authority(db,a);auto value=ftn_spool_detail::read(db,id);ftn_spool_detail::authorize(db,value,attempt);
        if(value.object){if(value.object->volume!=object.volume||value.object->file_id!=object.file_id)message_detail::conflict();out=value;return SQLITE_OK;}
        SqliteStatement bind(db,"INSERT INTO sfx_ftn_spool_bindings VALUES(?1,?2,?3)","bind CREATE_NEW object identity only once");bind.bind(1,id);bind.bind(2,object.volume);bind.bind(3,object.file_id);bind.step();ftn_continuation_detail::spool(db,id,1,attempt);value.object=object;out=std::move(value);return SQLITE_OK;
    });
}
application::StorageResult<application::FtnSpoolObject> SqliteAccountRepository::verify_ftn_spool(const application::MessageConfigurationActor& a,const application::Identifier128& id,const application::FilePayloadIdentity& identity,const application::Identifier128& attempt){
    using Result=application::FtnSpoolObject;if(!ftn_work_detail::owner(a)||!id.valid()||!ftn_spool_detail::object({identity.volume,identity.file_id}))return read_failure<Result>(invalid("verify exact retained forwarding bytes"));
    return impl_->write_result<Result>("mark flushed hashed spool object ready",4096,[a,id,identity,attempt](sqlite3* db,std::optional<Result>& out){
        message_detail::config_authority(db,a);auto value=ftn_spool_detail::read(db,id);ftn_spool_detail::authorize(db,value,attempt);
        if(!value.object||value.object->volume!=identity.volume||value.object->file_id!=identity.file_id||value.job.file.size!=identity.bytes||value.job.digest!=identity.sha256)message_detail::conflict();
        if(!value.ready){SqliteStatement ready(db,"INSERT INTO sfx_ftn_spool_ready VALUES(?1)","record verified full native object only once");ready.bind(1,id);ready.step();ftn_continuation_detail::spool(db,id,2,attempt);value.ready=true;}out=std::move(value);return SQLITE_OK;
    });
}
