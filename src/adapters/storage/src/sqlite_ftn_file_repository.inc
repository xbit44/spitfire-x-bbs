namespace ftn_continuation_detail {
application::FtnFileRequest effective(sqlite3*,const application::FtnFilePublication&,const application::Identifier128&);
std::optional<application::FtnTicReview> begin(sqlite3*,const application::FtnFileRequest&,const application::Identifier128&);
void activate(sqlite3*,const application::FtnFilePublication&,const application::FtnFileRequest&,const application::Identifier128&,const std::optional<application::FtnTicReview>&,std::int64_t);
void step(sqlite3*,const application::Identifier128&,application::FtnFilePhase,const application::Identifier128&);
void spool(sqlite3*,const application::Identifier128&,std::int64_t,const application::Identifier128&);
void commit(sqlite3*,const application::Identifier128&,const application::Identifier128&);
}
namespace ftn_file_detail {
namespace app=application;
using Phase=app::FtnFilePhase;
[[noreturn]] void reject(const char* why){throw StorageFailure(app::StorageErrorCode::conflict,why,SQLITE_ABORT);}
bool object(const app::FilePayloadIdentity& value){return !value.volume.empty()&&value.volume.size()<=128&&!value.file_id.empty()&&value.file_id.size()<=128;}
bool same_object(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b){return a.volume==b.volume&&a.file_id==b.file_id;}
bool same_object(const app::FilePayloadIdentity& a,const app::FileObjectIdentity& b){return a.volume==b.volume&&a.file_id==b.file_id;}
app::FtnDigest fingerprint(const app::FtnFileRequest& r){
    file_detail::Fingerprint f("FTN-TIC-publication-v1");
    for(const auto& id:{r.operation,r.tic_receipt,r.payload_receipt})f.text({reinterpret_cast<const char*>(id.bytes.data()),id.bytes.size()});
    f.number(r.configuration_revision);f.number(r.area_revision);
    for(const auto& text:{r.network,r.link,r.tag,r.filename,r.replaces,r.origin,r.from,r.descriptions.short_text,r.descriptions.long_text})f.text(text);
    f.payload(r.tic);f.payload(r.payload);f.number(r.crc);f.number(r.password_verified);f.number(r.authenticated_same_link);return f.value();
}
bool valid(const app::FtnFileRequest& r){
    return r.operation.valid()&&r.tic_receipt.valid()&&r.payload_receipt.valid()&&r.tic_receipt.bytes!=r.payload_receipt.bytes&&r.configuration_revision>0&&r.area_revision>0&&
        app::valid_ftn_code(r.network)&&app::valid_ftn_code(r.link)&&app::valid_ftn_echo_tag(r.tag)&&app::valid_file_name(r.filename)&&
        r.replaces.size()<=128&&r.replaces.find_first_of("/\\:<>\"\r\n\0",0,9)==std::string::npos&&app::parse_ftn_address(r.origin)&&app::parse_ftn_address(r.from)&&
        app::valid_file_descriptions(r.descriptions)&&file_detail::payload_valid(r.tic)&&r.tic.bytes>0&&r.tic.bytes<=65536&&file_detail::payload_valid(r.payload)&&r.payload.bytes<=app::kFtnTransferMaximumBytes&&!same_object(r.tic,r.payload);
}
// Deliberately matches only the selected incoming basename. Never enumerate
// filesystem/catalog matches or infer authority over another matching file.
bool replaces(std::string_view pattern,std::string_view name){
    const auto lower=[](char c){return c>='A'&&c<='Z'?static_cast<char>(c+32):c;};
    std::size_t p{},n{},star=std::string::npos,retry{};
    while(n<name.size()){if(p<pattern.size()&&(pattern[p]=='?'||lower(pattern[p])==lower(name[n]))){++p;++n;}else if(p<pattern.size()&&pattern[p]=='*'){star=p++;retry=n;}else if(star!=std::string::npos){p=star+1;n=++retry;}else return false;}
    while(p<pattern.size()&&pattern[p]=='*')++p;
    return p==pattern.size();
}
app::FtnIngressReceipt custody(sqlite3* db,app::Identifier128 id,const app::FilePayloadIdentity& identity){
    const auto sql=std::string("SELECT ")+ftn_work_detail::ingress_columns+" FROM sfx_ftn_ingress WHERE receipt_id=?1";SqliteStatement q(db,sql.c_str(),"require complete retained TIC publication custody");q.bind(1,id);if(!q.row())message_detail::unavailable();const auto r=ftn_work_detail::ingress(q);
    if((r.state!=app::FtnIngressState::received&&r.state!=app::FtnIngressState::processing)||r.file.size!=identity.bytes||r.received!=identity.bytes||r.digest!=identity.sha256)message_detail::conflict();
    SqliteStatement native(db,"SELECT volume,file_id,checkpoint_bytes,prefix_sha256 FROM sfx_ftn_ingress_native WHERE receipt_id=?1","bind publication to exact durable native checkpoint");native.bind(1,id);
    if(!native.row()||native.text(0)!=identity.volume||native.text(1)!=identity.file_id||native.int64(2)!=static_cast<std::int64_t>(identity.bytes)||file_detail::digest(native,3)!=identity.sha256)message_detail::conflict();
    return r;
}
app::FileAreaConfiguration authorize(sqlite3* db,const app::FtnFileRequest& r){
    if(app::ftn_message_wire_name(r.filename))message_detail::conflict();
    ftn_dispatch_detail::own(db,r.tic_receipt,1,r.operation);
    ftn_dispatch_detail::own(db,r.payload_receipt,2,r.operation);
    const auto c=ftn_detail::config(db);if(c.revision!=r.configuration_revision||c.settings.mode==app::FtnMailerMode::disabled)message_detail::conflict();
    const auto from=app::parse_ftn_address(r.from);const auto peer=from?app::resolve_ftn_peer(c,{*from},r.network,r.link):std::nullopt;
    if(!peer||!app::ftn_echo_permission(c,r.network,r.link,r.tag,true,true))message_detail::unavailable();
    const auto tic=custody(db,r.tic_receipt,r.tic),payload=custody(db,r.payload_receipt,r.payload);
    const auto& link=ftn_work_detail::link(c,r.network,r.link,tic.outbound,tic.proof!=app::FtnIngressProof::external_packet);
    if(!ftn_detail::same(tic.network,payload.network)||!ftn_detail::same(tic.link,payload.link)||!ftn_detail::same(payload.file.name,r.filename))message_detail::conflict();
    if(!tic.network.empty()&&(!ftn_detail::same(tic.network,r.network)||!ftn_detail::same(tic.link,r.link)))message_detail::unavailable();
    const bool verified=!tic.network.empty()&&app::ftn_verified_receipt(tic);
    if(r.authenticated_same_link!=verified||(!link.credentials.tic.empty()?!r.password_verified:!verified))message_detail::unavailable();
    const auto mapping=std::find_if(c.file_echoes.begin(),c.file_echoes.end(),[&](const auto& e){return ftn_detail::same(e.network,r.network)&&ftn_detail::same(e.tag,r.tag);});
    if(mapping==c.file_echoes.end()||mapping->file_area<=0)message_detail::unavailable();
    const auto area=file_detail::area(db,{mapping->file_area});const auto group=file_detail::group(db,area.area.group);
    if(area.area.revision!=r.area_revision)message_detail::conflict();
    if(!area.area.enabled||!group.enabled||area.area.kind!=app::FileAreaKind::ordinary)reject("ftn_tic_area_not_authorized");
    if(file_detail::reupload_blocked(db,r.payload))reject("ftn_tic_reupload_blocked");
    const auto policy=file_detail::policy(db);if(r.payload.bytes>area.area.maximum_upload_bytes.value_or(policy.maximum_upload_bytes))reject("ftn_tic_size_policy_denied");return area;
}
app::FtnFilePublication read(sqlite3* db,app::Identifier128 id){
    SqliteStatement q(db,"SELECT operation_id,tic_receipt,payload_receipt,configuration_revision,area_id,area_revision,storage_path,network,link,echo_tag,filename,replaces_pattern,origin_address,from_address,short_description,long_description,tic_bytes,tic_sha256,tic_volume,tic_native_id,payload_bytes,payload_sha256,payload_volume,payload_native_id,payload_crc,password_verified,authenticated_same_link,displaced_file,displaced_revision,displaced_bytes,displaced_sha256,displaced_volume,displaced_native_id,stage_name,recovery_key,phase,result_file,reason,fingerprint,created_utc FROM sfx_ftn_file_publications WHERE operation_id=?1","read exact durable FTN file publication");q.bind(1,id);if(!q.row())message_detail::unavailable();
    app::FtnFilePublication p;auto& r=p.request;r.operation=ftn_work_detail::identifier(q,0);r.tic_receipt=ftn_work_detail::identifier(q,1);r.payload_receipt=ftn_work_detail::identifier(q,2);r.configuration_revision=q.int64(3);p.area=file_detail::area(db,{q.int64(4)});r.area_revision=q.int64(5);p.area.area.revision=r.area_revision;p.area.storage_path=q.text(6);
    r.network=q.text(7);r.link=q.text(8);r.tag=q.text(9);r.filename=q.text(10);r.replaces=q.text(11);r.origin=q.text(12);r.from=q.text(13);r.descriptions={q.text(14),q.text(15)};
    r.tic={static_cast<std::uint64_t>(q.int64(16)),file_detail::digest(q,17),q.text(18),q.text(19)};r.payload={static_cast<std::uint64_t>(q.int64(20)),file_detail::digest(q,21),q.text(22),q.text(23)};r.crc=static_cast<std::uint32_t>(q.int64(24));r.password_verified=q.boolean(25);r.authenticated_same_link=q.boolean(26);
    if(!q.is_null(27)){p.displaced=file_detail::managed(db,{q.int64(27)});p.displaced->file.revision=q.int64(28);p.displaced->payload={static_cast<std::uint64_t>(q.int64(29)),file_detail::digest(q,30),q.text(31),q.text(32)};}
    p.stage_name=q.text(33);p.recovery_key=q.text(34);p.phase=static_cast<Phase>(q.int64(35));p.result={q.is_null(36)?0:q.int64(36)};p.reason=q.text(37);p.created=q.int64(39);if(!valid(r)||fingerprint(r)!=file_detail::digest(q,38))file_detail::corrupt();
    SqliteStatement steps(db,"SELECT phase,byte_count,sha256,volume,native_id FROM sfx_ftn_file_steps WHERE operation_id=?1 ORDER BY phase","read immutable object and outcome journal");steps.bind(1,id);while(steps.row()){app::FilePayloadIdentity value{static_cast<std::uint64_t>(steps.int64(1)),file_detail::digest(steps,2),steps.text(3),steps.text(4)};switch(static_cast<Phase>(steps.int64(0))){case Phase::stage_bound:p.stage_object=app::FileObjectIdentity{value.volume,value.file_id};break;case Phase::staged:p.staged=value;break;case Phase::recovery_bound:p.recovery_object=app::FileObjectIdentity{value.volume,value.file_id};break;case Phase::displaced:p.recovered=value;break;case Phase::placed:p.placed=value;break;default:file_detail::corrupt();}}
    return p;
}
void current(sqlite3* db,const app::FtnFilePublication& p,const app::Identifier128& attempt={}){
    const auto fresh=ftn_continuation_detail::effective(db,p,attempt);const auto area=authorize(db,fresh);if(!(area.area.id==p.area.area.id)||area.storage_path!=p.area.storage_path)message_detail::conflict();
    if(p.displaced){const auto now=file_detail::entry(db,p.displaced->file.id);if(now.file.revision!=p.displaced->file.revision||now.file.state!=app::FileCatalogState::available||!file_detail::same_payload(now.payload,p.displaced->payload))message_detail::conflict();
        SqliteStatement active(db,"SELECT 1 FROM sfx_file_transfers WHERE file_id=?1 AND state=0 LIMIT 1","retain active download pin during TIC replacement");active.bind(1,p.displaced->file.id.value);if(active.row())message_detail::conflict();}
    if(file_detail::name_reserved(db,p.area.area.id,p.request.filename,p.request.operation)||file_detail::name_reserved(db,p.area.area.id,p.stage_name,p.request.operation))message_detail::conflict();
}
}
namespace ftn_forward_plan_detail {
void authorize_job(sqlite3*,std::int64_t,const application::FtnOutboundJob&,const application::Identifier128&);
void commit_jobs(sqlite3*,const application::FtnFilePublication&,const std::vector<application::FtnOutboundJob>&,const application::Identifier128&);
}
#include "sqlite_ftn_hatch_detail.inc"
#include "sqlite_ftn_spool_repository.inc"
#include "sqlite_ftn_hatch_repository.inc"
#include "sqlite_ftn_forward_plan_repository.inc"
application::StorageResult<application::FtnFileForwardPlan> SqliteAccountRepository::ftn_file_forward_plan(const application::MessageConfigurationActor& a,const application::Identifier128& id){
    using Result=application::FtnFileForwardPlan;if(!ftn_work_detail::owner(a)||!id.valid())return read_failure<Result>(invalid("validate original forwarding plan lookup"));return impl_->readers_.read<Result>("read immutable complete forwarding manifest",[a,id](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);return ftn_forward_plan_detail::read(db,id);});
}
application::StorageResult<application::FtnFilePublication> SqliteAccountRepository::prepare_ftn_file(const application::MessageConfigurationActor& a,const application::FtnFileRequest& r,const std::optional<application::FtnFileForwardPlan>& plan,const application::Identifier128& attempt){
    namespace app=application;using Result=app::FtnFilePublication;
    if(!ftn_work_detail::owner(a)||!ftn_file_detail::valid(r))return read_failure<Result>(invalid("validate trusted TIC publication request"));
    const auto hash=ftn_file_detail::fingerprint(r);const auto clock_now=impl_->message_utc_now_();const auto bytes=plan?app::encode_ftn_file_forward_plan(*plan):std::vector<std::uint8_t>{};if(plan&&bytes.empty())return read_failure<Result>(invalid("validate complete password-free forwarding manifest"));const auto now=plan?plan->created:clock_now;
    const auto rejection=std::make_shared<std::string>();auto written=impl_->write_result<Result>("reserve exact TIC publication and displacement",32768+bytes.size(),[a,r,hash,now,clock_now,plan,bytes,attempt,rejection](sqlite3* db,std::optional<Result>& out){try{
        message_detail::config_authority(db,a);const auto reviewed=ftn_continuation_detail::begin(db,r,attempt);{SqliteStatement prior(db,"SELECT fingerprint FROM sfx_ftn_file_publications WHERE operation_id=?1","replay original TIC publication intent");prior.bind(1,r.operation);if(prior.row()){auto saved=ftn_file_detail::read(db,r.operation);if(!attempt.valid()&&file_detail::digest(prior,0)!=hash)message_detail::conflict();if(plan&&app::encode_ftn_file_forward_plan(ftn_forward_plan_detail::read(db,r.operation))!=bytes)message_detail::conflict();if(attempt.valid())ftn_continuation_detail::activate(db,saved,r,attempt,reviewed,clock_now);out=saved;return SQLITE_OK;}}
        if(attempt.valid()&&!plan)message_detail::conflict();
        if(plan&&(now<1||clock_now<1||(now>clock_now?now-clock_now:clock_now-now)>300))message_detail::conflict();
        const auto area=ftn_file_detail::authorize(db,r);const auto recovery=file_detail::stage_key(r.operation),stage=".sfxftn-"+recovery+".tmp";
        if(file_detail::name_reserved(db,area.area.id,r.filename)||file_detail::name_reserved(db,area.area.id,stage))message_detail::conflict();
        std::optional<app::ManagedFile> displaced;SqliteStatement same(db,"SELECT file_id FROM sfx_file_catalog WHERE area_id=?1 AND filename_key=?2 AND catalog_state<>1","select only incoming basename for replacement");same.bind(1,area.area.id.value);same.bind(2,file_detail::key(r.filename));
        if(same.row()){displaced=file_detail::managed(db,{same.int64(0)});if(same.row()||displaced->file.state!=app::FileCatalogState::available||displaced->operation_pending||!displaced->native_basename.empty())message_detail::conflict();if(r.replaces.empty()||!ftn_file_detail::replaces(r.replaces,r.filename))ftn_file_detail::reject("ftn_tic_replaces_required");SqliteStatement active(db,"SELECT 1 FROM sfx_file_transfers WHERE file_id=?1 AND state=0 LIMIT 1","do not displace a pinned download");active.bind(1,displaced->file.id.value);if(active.row())message_detail::conflict();}
        SqliteStatement staged(db,"SELECT 1 FROM sfx_file_catalog WHERE area_id=?1 AND filename_key=?2 AND catalog_state<>1","reject tracked staging-name collision");staged.bind(1,area.area.id.value);staged.bind(2,file_detail::key(stage));if(staged.row())message_detail::conflict();
        SqliteStatement q(db,"INSERT INTO sfx_ftn_file_publications VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,?33,?34,?35,?36,?37,0,NULL,'',NULL,?38)","journal secret-free request before creating any native object");
        q.bind(1,r.operation);q.bind(2,hash);q.bind(3,r.tic_receipt);q.bind(4,r.payload_receipt);q.bind(5,r.configuration_revision);q.bind(6,area.area.id.value);q.bind(7,r.area_revision);q.bind(8,area.storage_path);q.bind(9,r.network);q.bind(10,r.link);q.bind(11,r.tag);q.bind(12,r.filename);q.bind(13,file_detail::key(r.filename));q.bind(14,r.replaces);q.bind(15,r.origin);q.bind(16,r.from);q.bind(17,r.descriptions.short_text);q.bind(18,r.descriptions.long_text);
        q.bind(19,static_cast<std::int64_t>(r.tic.bytes));q.bind(20,r.tic.sha256);q.bind(21,r.tic.volume);q.bind(22,r.tic.file_id);q.bind(23,static_cast<std::int64_t>(r.payload.bytes));q.bind(24,r.payload.sha256);q.bind(25,r.payload.volume);q.bind(26,r.payload.file_id);q.bind(27,static_cast<std::int64_t>(r.crc));q.bind(28,r.password_verified);q.bind(29,r.authenticated_same_link);
        if(displaced)q.bind(30,displaced->file.id.value);else q.bind_null(30);q.bind(31,displaced?displaced->file.revision:0);q.bind(32,static_cast<std::int64_t>(displaced?displaced->payload.bytes:0));q.bind(33,displaced?displaced->payload.sha256:app::FtnDigest{});q.bind(34,displaced?displaced->payload.volume:std::string{});q.bind(35,displaced?displaced->payload.file_id:std::string{});q.bind(36,stage);q.bind(37,recovery);q.bind(38,now);q.step();SqliteStatement spool_owner(db,"INSERT INTO sfx_ftn_spool_owners VALUES(?1,?1,NULL)","bind incoming publication as typed spool owner");spool_owner.bind(1,r.operation);spool_owner.step();if(plan)ftn_forward_plan_detail::insert(db,r,*plan,bytes);out=ftn_file_detail::read(db,r.operation);if(attempt.valid())ftn_continuation_detail::activate(db,*out,r,attempt,reviewed,clock_now);return SQLITE_OK;
    }catch(const StorageFailure& failure){for(const auto* known:{"ftn_tic_area_not_authorized","ftn_tic_reupload_blocked","ftn_tic_size_policy_denied","ftn_tic_replaces_required"})if(failure.operation()==known)*rejection=known;throw;}});
    if(!written&&!rejection->empty()&&written.status.code==app::StorageErrorCode::conflict)written.status.operation=*rejection;
    return written;
}
application::StorageResult<application::FtnFilePublication> SqliteAccountRepository::ftn_file_publication(const application::MessageConfigurationActor& a,const application::Identifier128& id){
    if(!ftn_work_detail::owner(a)||!id.valid())return read_failure<application::FtnFilePublication>(invalid("validate FTN publication lookup"));
    return impl_->readers_.read<application::FtnFilePublication>("read retained FTN publication",[a,id](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);return ftn_file_detail::read(db,id);});
}
application::StorageResult<std::vector<application::Identifier128>> SqliteAccountRepository::pending_ftn_files(const application::MessageConfigurationActor& a,const application::Identifier128& after,std::size_t maximum){
    using Result=std::vector<application::Identifier128>;if(!ftn_work_detail::owner(a)||!maximum||maximum>100)return read_failure<Result>(invalid("bound pending FTN publications"));
    return impl_->readers_.read<Result>("page durable FTN publication recovery",[a,after,maximum](sqlite3* db){message_detail::config_authority(db,a);SqliteStatement q(db,"SELECT operation_id FROM sfx_ftn_file_publications WHERE phase<>6 AND operation_id>?1 ORDER BY operation_id LIMIT ?2","bounded keyset publication recovery");q.bind(1,after);q.bind(2,static_cast<std::int64_t>(maximum));Result out;while(q.row())out.push_back(ftn_work_detail::identifier(q,0));return out;});
}
application::StorageResult<application::FtnFilePublication> SqliteAccountRepository::advance_ftn_file(const application::MessageConfigurationActor& a,const application::Identifier128& id,application::FtnFilePhase phase,const application::FilePayloadIdentity& native,const application::Identifier128& attempt){
    namespace app=application;using Result=app::FtnFilePublication;using Phase=app::FtnFilePhase;
    if(!ftn_work_detail::owner(a)||!id.valid()||phase<Phase::stage_bound||phase>Phase::placed||!ftn_file_detail::object(native)||native.bytes>app::kFtnTransferMaximumBytes)return read_failure<Result>(invalid("validate native FTN publication checkpoint"));
    return impl_->write_result<Result>("bind verified native TIC publication step",8192,[a,id,phase,native,attempt](sqlite3* db,std::optional<Result>& out){
        message_detail::config_authority(db,a);auto p=ftn_file_detail::read(db,id);
        SqliteStatement prior(db,"SELECT byte_count,sha256,volume,native_id FROM sfx_ftn_file_steps WHERE operation_id=?1 AND phase=?2","reconcile exact object checkpoint");prior.bind(1,id);prior.bind(2,static_cast<std::int64_t>(phase));if(prior.row()){const app::FilePayloadIdentity saved{static_cast<std::uint64_t>(prior.int64(0)),file_detail::digest(prior,1),prior.text(2),prior.text(3)};if(!file_detail::same_payload(saved,native))message_detail::conflict();out=p;return SQLITE_OK;}
        ftn_file_detail::current(db,p,attempt);const bool object_only=phase==Phase::stage_bound||phase==Phase::recovery_bound;
        if(object_only){if(native.bytes||native.sha256!=app::FtnDigest{})message_detail::conflict();}else if(!file_detail::payload_valid(native))message_detail::conflict();
        if(phase==Phase::stage_bound){if(p.phase!=Phase::reserved||ftn_file_detail::same_object(native,p.request.payload)||ftn_file_detail::same_object(native,p.request.tic)||(p.displaced&&ftn_file_detail::same_object(native,p.displaced->payload)))message_detail::conflict();}
        else if(phase==Phase::staged){if(p.phase!=Phase::stage_bound||!p.stage_object||!ftn_file_detail::same_object(native,*p.stage_object)||native.bytes!=p.request.payload.bytes||native.sha256!=p.request.payload.sha256)message_detail::conflict();}
        else if(phase==Phase::recovery_bound){if(p.phase!=Phase::staged||!p.displaced||!p.stage_object||ftn_file_detail::same_object(native,*p.stage_object)||ftn_file_detail::same_object(native,p.request.payload)||ftn_file_detail::same_object(native,p.request.tic))message_detail::conflict();}
        else if(phase==Phase::displaced){if(p.phase!=Phase::recovery_bound||!p.displaced||!p.recovery_object||!ftn_file_detail::same_object(native,*p.recovery_object)||native.bytes!=p.displaced->payload.bytes||native.sha256!=p.displaced->payload.sha256)message_detail::conflict();}
        else if((p.displaced?p.phase!=Phase::displaced:p.phase!=Phase::staged)||!p.staged||!file_detail::same_payload(*p.staged,native))message_detail::conflict();
        SqliteStatement add(db,"INSERT INTO sfx_ftn_file_steps VALUES(?1,?2,?3,?4,?5,?6)","retain immutable exact object/outcome checkpoint");add.bind(1,id);add.bind(2,static_cast<std::int64_t>(phase));add.bind(3,static_cast<std::int64_t>(native.bytes));add.bind(4,native.sha256);add.bind(5,native.volume);add.bind(6,native.file_id);add.step();ftn_continuation_detail::step(db,id,phase,attempt);SqliteStatement next(db,"UPDATE sfx_ftn_file_publications SET phase=?1,reason='' WHERE operation_id=?2","advance recoverable native publication phase");next.bind(1,static_cast<std::int64_t>(phase));next.bind(2,id);next.step();out=ftn_file_detail::read(db,id);return SQLITE_OK;
    });
}
application::StorageStatus SqliteAccountRepository::hold_ftn_file(const application::MessageConfigurationActor& a,const application::Identifier128& id,std::string_view reason){
    if(!ftn_work_detail::owner(a)||!id.valid()||reason.empty()||!ftn_work_detail::reason(reason))return invalid("validate FTN publication hold");
    return impl_->write("retain publication phase and all recovery reservations",4096,[a,id,reason=std::string(reason)](sqlite3* db){message_detail::config_authority(db,a);SqliteStatement q(db,"UPDATE sfx_ftn_file_publications SET reason=?1 WHERE operation_id=?2 AND phase<>6","hold without orphaning either payload");q.bind(1,reason);q.bind(2,id);q.step();return sqlite3_changes(db)==1?SQLITE_OK:SQLITE_ABORT;});
}
application::StorageResult<application::FtnFilePublication> SqliteAccountRepository::commit_ftn_file(const application::MessageConfigurationActor& a,const application::Identifier128& id,const std::vector<application::FtnOutboundJob>& jobs,const application::Identifier128& attempt){
    namespace app=application;using Result=app::FtnFilePublication;using Phase=app::FtnFilePhase;
    if(!ftn_work_detail::owner(a)||!id.valid()||jobs.size()>4096||jobs.size()%2)return read_failure<Result>(invalid("validate TIC publication forwarding pairs"));
    file_detail::Fingerprint completion("FTN-TIC-completion-v1");
    for(const auto& j:jobs){if(!ftn_work_detail::valid_job(j)||!j.echo_tag.empty()||j.operation.bytes!=id.bytes||j.kind==app::FtnOutboundKind::packet)return read_failure<Result>(invalid("validate immutable TIC forwarding artifact"));for(const auto& token:{j.id,j.operation})completion.text({reinterpret_cast<const char*>(token.bytes.data()),token.bytes.size()});completion.number(j.ordinal);completion.text(j.network);completion.text(j.link);completion.number(static_cast<std::int64_t>(j.kind));completion.number(static_cast<std::int64_t>(j.flavor));completion.text(j.file.name);completion.number(static_cast<std::int64_t>(j.file.size));completion.number(static_cast<std::int64_t>(j.file.timestamp));completion.text({reinterpret_cast<const char*>(j.digest.data()),j.digest.size()});}
    const auto fingerprint=completion.value();const auto now=impl_->message_utc_now_();
    return impl_->write_result<Result>("atomically publish TIC catalog, provenance, displacement and forwarding",65536+jobs.size()*1024,[a,id,jobs,fingerprint,now,attempt](sqlite3* db,std::optional<Result>& out){
        message_detail::config_authority(db,a);auto p=ftn_file_detail::read(db,id);const auto& r=p.request;
        if(p.phase==Phase::completed){SqliteStatement prior(db,"SELECT completion_fingerprint FROM sfx_ftn_file_publications WHERE operation_id=?1","reconcile exact completed publication");prior.bind(1,id);if(!prior.row()||file_detail::digest(prior,0)!=fingerprint)message_detail::conflict();out=p;return SQLITE_OK;}
        if(p.phase!=Phase::placed||!p.placed||(p.displaced&&!p.recovered))message_detail::conflict();
        ftn_file_detail::current(db,p,attempt);ftn_forward_plan_detail::commit_jobs(db,p,jobs,attempt);
        const auto c=ftn_detail::config(db);std::vector<std::string> targets;
        for(std::size_t i=0;i<jobs.size();i+=2){const auto& payload=jobs[i];const auto& tic=jobs[i+1];
            ftn_spool_detail::ready(db,payload,r.configuration_revision);ftn_spool_detail::ready(db,tic,r.configuration_revision);
            if(payload.kind!=app::FtnOutboundKind::payload||tic.kind!=app::FtnOutboundKind::tic||payload.ordinal!=i||tic.ordinal!=i+1||!ftn_detail::same(payload.network,r.network)||!ftn_detail::same(tic.network,r.network)||!ftn_detail::same(payload.link,tic.link)||ftn_detail::same(payload.link,r.link)||payload.file.name!=r.filename||payload.file.size!=r.payload.bytes||payload.digest!=r.payload.sha256||!app::ftn_echo_permission(c,r.network,payload.link,r.tag,true,false))message_detail::conflict();
            const auto target=file_detail::key(payload.link);if(std::find(targets.begin(),targets.end(),target)!=targets.end())message_detail::conflict();targets.push_back(target);
        }
        if(!file_detail::destination_free(db,p.area.area.id,r.filename,p.displaced?p.displaced->file.id:app::FileId{},id))message_detail::conflict();
        if(p.displaced){SqliteStatement old(db,"UPDATE sfx_file_catalog SET catalog_state=1,native_volume=?1,native_file_id=?2,revision=revision+1 WHERE file_id=?3 AND revision=?4","retain displaced metadata/counters and native recovery custody");old.bind(1,p.recovered->volume);old.bind(2,p.recovered->file_id);old.bind(3,p.displaced->file.id.value);old.bind(4,p.displaced->file.revision);old.step();if(sqlite3_changes(db)!=1)message_detail::conflict();SqliteStatement displaced(db,"INSERT INTO sfx_ftn_file_displacements VALUES(?1,?2,?3,0,NULL)","record remote TIC displacement separately from owner Remove");displaced.bind(1,id);displaced.bind(2,p.displaced->file.id.value);displaced.bind(3,p.recovery_key);displaced.step();}
        SqliteStatement file(db,"INSERT INTO sfx_file_catalog(area_id,filename,filename_key,short_description,long_description,public_uploader,uploader_account_id,byte_count,content_sha256,native_volume,native_file_id,original_upload_utc,published_utc,publication_sequence,download_count,origin,catalog_state,sysop_only,revision) VALUES(?1,?2,?3,?4,?5,'',NULL,?6,?7,?8,?9,?10,?10,0,0,3,0,0,1)","publish new FTN file without local uploader or upload credit");file.bind(1,p.area.area.id.value);file.bind(2,r.filename);file.bind(3,file_detail::key(r.filename));file.bind(4,r.descriptions.short_text);file.bind(5,r.descriptions.long_text);file.bind(6,static_cast<std::int64_t>(r.payload.bytes));file.bind(7,r.payload.sha256);file.bind(8,p.placed->volume);file.bind(9,p.placed->file_id);file.bind(10,now);file.step();const app::FileId result{sqlite3_last_insert_rowid(db)};
        SqliteStatement receipt(db,"INSERT INTO sfx_ftn_file_receipts VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21)","bind immutable TIC/payload provenance to new file identity");receipt.bind(1,id);receipt.bind(2,ftn_file_detail::fingerprint(r));receipt.bind(3,r.tic_receipt);receipt.bind(4,r.payload_receipt);receipt.bind(5,r.network);receipt.bind(6,r.link);receipt.bind(7,r.tag);receipt.bind(8,p.area.area.id.value);receipt.bind(9,r.configuration_revision);receipt.bind(10,r.area_revision);receipt.bind(11,r.tic.sha256);receipt.bind(12,r.payload.sha256);receipt.bind(13,static_cast<std::int64_t>(r.payload.bytes));receipt.bind(14,static_cast<std::int64_t>(r.crc));receipt.bind(15,p.placed->volume);receipt.bind(16,p.placed->file_id);receipt.bind(17,r.origin);receipt.bind(18,r.from);receipt.bind(19,r.filename);receipt.bind(20,result.value);receipt.bind(21,now);receipt.step();
        ftn_continuation_detail::commit(db,id,attempt);SqliteStatement lineage(db,"INSERT INTO sfx_ftn_file_lineage VALUES(?1,?2,NULL,NULL)","record original FTN content lineage");lineage.bind(1,result.value);lineage.bind(2,id);lineage.step();file_detail::publish_file(db,result,p.area.area.id,now);ftn_work_detail::queue(db,c,jobs,now);
        for(std::size_t i=0;i<jobs.size();++i){SqliteStatement link(db,"INSERT INTO sfx_ftn_file_job_echoes VALUES(?1,?2,?3,?4)","bind file permission and payload-before-TIC acknowledgement");link.bind(1,jobs[i].id);link.bind(2,id);link.bind(3,r.tag);if(i%2)link.bind(4,jobs[i-1].id);else link.bind_null(4);link.step();}
        SqliteStatement finish(db,"UPDATE sfx_ftn_file_publications SET phase=6,result_file=?1,reason='',completion_fingerprint=?2 WHERE operation_id=?3 AND phase=5","commit publication once with recovery receipts");finish.bind(1,result.value);finish.bind(2,fingerprint);finish.bind(3,id);finish.step();if(sqlite3_changes(db)!=1)message_detail::conflict();file_detail::audit(db,a,id,"ftn.file.received",result,p.area.area.id,now);out=ftn_file_detail::read(db,id);return SQLITE_OK;
    });
}
