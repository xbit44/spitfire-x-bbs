namespace ftn_continuation_detail {
namespace app=application;
bool activated(sqlite3* db,const app::Identifier128& attempt){SqliteStatement q(db,"SELECT 1 FROM sfx_ftn_file_authorizations WHERE attempt_id=?1","exact immutable attempt activation");q.bind(1,attempt);return q.row();}
void unlatched(sqlite3* db,const app::Identifier128& ingress){SqliteStatement q(db,"SELECT 1 FROM sfx_ftn_tic_rejections WHERE ingress_id=?1 LIMIT 1","original automatic work never clears a rejected TIC");q.bind(1,ingress);if(q.row())message_detail::conflict();}
app::FtnFileRequest effective(sqlite3* db,const app::FtnFilePublication& p,const app::Identifier128& attempt){
    if(read_ftn_serial_state(db).recovery_required)message_detail::conflict();
    auto fresh=p.request;
    if(!attempt.valid()){unlatched(db,p.request.tic_receipt);return fresh;}
    SqliteStatement q(db,"SELECT a.original_fingerprint,a.configuration_revision,a.area_revision,a.file_catalog_revision,a.password_verified,a.authenticated_same_link,a.start_phase,r.finished,r.ingress_id,r.review_sha256,a.review_sha256,r.rejection_sequence,(SELECT max(sequence) FROM sfx_ftn_tic_rejections WHERE ingress_id=r.ingress_id) FROM sfx_ftn_file_authorizations a JOIN sfx_ftn_tic_retries r ON r.operation_id=a.attempt_id WHERE a.attempt_id=?1 AND a.publication_id=?2","require exact current pending authority at every native/SQL mutation");q.bind(1,attempt);q.bind(2,p.request.operation);
    if(!q.row()||q.boolean(7)||q.identifier(8).bytes!=p.request.tic_receipt.bytes||file_detail::digest(q,0)!=ftn_file_detail::fingerprint(p.request)||file_detail::digest(q,9)!=file_detail::digest(q,10)||q.int64(11)!=q.int64(12)||static_cast<std::int64_t>(p.phase)<q.int64(6)||file_detail::policy(db).catalog_revision!=q.int64(3))message_detail::conflict();
    const auto latest=ftn_tic_review_detail::latest(db,p.request.tic_receipt);if(!latest||latest->request.operation.bytes!=attempt.bytes||latest->finished)message_detail::conflict();
    fresh.configuration_revision=q.int64(1);fresh.area_revision=q.int64(2);fresh.password_verified=q.boolean(4);fresh.authenticated_same_link=q.boolean(5);ftn_reprocess_detail::active(db,fresh.configuration_revision);
    const auto plan=ftn_forward_plan_detail::read(db,p.request.operation);ftn_forward_plan_detail::validate(db,fresh,plan);return fresh;
}
std::optional<app::FtnTicReview> begin(sqlite3* db,const app::FtnFileRequest& fresh,const app::Identifier128& attempt){
    if(!attempt.valid()){unlatched(db,fresh.tic_receipt);return {};}
    auto reviewed=ftn_tic_review_detail::review(db,fresh.tic_receipt);if(!reviewed||!reviewed->retry||reviewed->retry->finished||reviewed->retry->request.operation.bytes!=attempt.bytes||reviewed->retry->request.configuration_revision!=fresh.configuration_revision||reviewed->receipt.state==app::FtnIngressState::processed)message_detail::conflict();ftn_reprocess_detail::active(db,fresh.configuration_revision);
    if(activated(db,attempt)){if(!reviewed->publication||reviewed->publication->request.operation.bytes!=fresh.operation.bytes)message_detail::conflict();(void)effective(db,*reviewed->publication,attempt);}
    else if(reviewed->review!=reviewed->retry->request.review)message_detail::conflict();
    return reviewed;
}
void activate(sqlite3* db,const app::FtnFilePublication& p,const app::FtnFileRequest& fresh,const app::Identifier128& attempt,const std::optional<app::FtnTicReview>& reviewed,std::int64_t now){
    if(!attempt.valid()||!reviewed||!reviewed->retry||reviewed->retry->request.operation.bytes!=attempt.bytes||p.phase==app::FtnFilePhase::completed)message_detail::conflict();
    auto original_view=fresh;original_view.configuration_revision=p.request.configuration_revision;original_view.area_revision=p.request.area_revision;original_view.password_verified=p.request.password_verified;original_view.authenticated_same_link=p.request.authenticated_same_link;
    if(ftn_file_detail::fingerprint(original_view)!=ftn_file_detail::fingerprint(p.request)||fresh.authenticated_same_link!=p.request.authenticated_same_link)message_detail::conflict();
    const auto area=ftn_file_detail::authorize(db,fresh);if(!(area.area.id==p.area.area.id)||area.storage_path!=p.area.storage_path)message_detail::conflict();
    const auto plan=ftn_forward_plan_detail::read(db,p.request.operation);ftn_forward_plan_detail::validate(db,fresh,plan);
    if(activated(db,attempt)){const auto prior=effective(db,p,attempt);if(ftn_file_detail::fingerprint(prior)!=ftn_file_detail::fingerprint(fresh))message_detail::conflict();ftn_file_detail::current(db,p,attempt);return;}
    if(reviewed->publication&&(reviewed->publication->request.operation.bytes!=p.request.operation.bytes||reviewed->publication->phase!=p.phase))message_detail::conflict();
    const auto frontier=ftn_tic_review_detail::review(db,p.request.tic_receipt);if(!frontier)message_detail::conflict();
    SqliteStatement add(db,"INSERT INTO sfx_ftn_file_authorizations VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)","append reviewed native authority without rewriting original intent");add.bind(1,attempt);add.bind(2,p.request.operation);add.bind(3,ftn_file_detail::fingerprint(p.request));add.bind(4,reviewed->retry->request.review);add.bind(5,frontier->review);add.bind(6,static_cast<std::int64_t>(p.phase));add.bind(7,fresh.configuration_revision);add.bind(8,fresh.area_revision);add.bind(9,file_detail::policy(db).catalog_revision);add.bind(10,fresh.password_verified);add.bind(11,fresh.authenticated_same_link);add.bind(12,now);add.step();ftn_file_detail::current(db,p,attempt);
}
void step(sqlite3* db,const app::Identifier128& id,app::FtnFilePhase phase,const app::Identifier128& attempt){if(!attempt.valid())return;SqliteStatement q(db,"INSERT INTO sfx_ftn_file_step_authorities VALUES(?1,?2,?3)","link this exact new immutable step to actual reviewed authority");q.bind(1,id);q.bind(2,static_cast<std::int64_t>(phase));q.bind(3,attempt);q.step();}
void spool(sqlite3* db,const app::Identifier128& id,std::int64_t stage,const app::Identifier128& attempt){if(!attempt.valid())return;SqliteStatement q(db,"INSERT INTO sfx_ftn_spool_authorities VALUES(?1,?2,?3)","link this new spool binding/readiness to actual reviewed authority");q.bind(1,id);q.bind(2,stage);q.bind(3,attempt);q.step();}
void commit(sqlite3* db,const app::Identifier128& id,const app::Identifier128& attempt){if(!attempt.valid())return;SqliteStatement q(db,"INSERT INTO sfx_ftn_file_commit_authorities VALUES(?1,?2)","retain actual effective policy authority alongside original receipt");q.bind(1,id);q.bind(2,attempt);q.step();}
}
application::StorageResult<application::FtnFileAuthority> SqliteAccountRepository::ftn_file_authority(const application::MessageConfigurationActor& a,const application::Identifier128& id,const application::Identifier128& attempt){
    using Result=application::FtnFileAuthority;if(!ftn_work_detail::owner(a)||!id.valid())return read_failure<Result>(invalid("validate exact publication authority query"));return impl_->readers_.read<Result>("check exact pending authority before native mutation",[a,id,attempt](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);const auto p=ftn_file_detail::read(db,id);if(p.phase==application::FtnFilePhase::completed)message_detail::conflict();ftn_file_detail::current(db,p,attempt);const auto fresh=ftn_continuation_detail::effective(db,p,attempt);return Result{attempt,fresh.configuration_revision,fresh.area_revision,file_detail::policy(db).catalog_revision};});
}
