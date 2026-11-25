namespace ftn_forward_plan_detail {
namespace app=application;
app::FtnDigest hash(const std::vector<std::uint8_t>& bytes){return message_detail::source_digest({reinterpret_cast<const char*>(bytes.data()),bytes.size()});}
app::FtnDigest credential_generation(const std::vector<std::uint8_t>& bytes){const auto retained=app::retained_portable_ftn_generation(bytes);return retained?*retained:hash(bytes);}
app::FtnFileForwardPlan read(sqlite3* db,const app::Identifier128& id){
    SqliteStatement q(db,"SELECT f.plan,f.plan_sha256,f.publication_fingerprint,p.fingerprint,p.created_utc FROM sfx_ftn_file_forward_plans f JOIN sfx_ftn_file_publications p USING(operation_id) WHERE operation_id=?1","read complete original forwarding plan, never infer empty from absent jobs");q.bind(1,id);if(!q.row())message_detail::unavailable();const auto bytes=q.blob(0);const auto plan=app::decode_ftn_file_forward_plan(bytes);if(!plan||plan->publication.bytes!=id.bytes||hash(bytes)!=file_detail::digest(q,1)||file_detail::digest(q,2)!=file_detail::digest(q,3)||plan->created!=q.int64(4))file_detail::corrupt();return *plan;
}
void validate(sqlite3* db,const app::FtnFileRequest& r,const app::FtnFileForwardPlan& plan){
    const auto config=ftn_detail::config(db);const auto source=std::find_if(config.links.begin(),config.links.end(),[&](const auto& link){return ftn_detail::same(link.network,r.network)&&ftn_detail::same(link.code,r.link);});if(source==config.links.end()||message_detail::source_digest(app::ftn_forward_peer_identity_material(*source))!=plan.source_peer_identity)message_detail::conflict();
    if(plan.publication.bytes!=r.operation.bytes||plan.network!=r.network)message_detail::conflict();
    const auto c=ftn_detail::config(db);if(c.revision!=r.configuration_revision)message_detail::conflict();const auto& network=ftn_work_detail::network(c,r.network);const auto local=app::parse_ftn_address(network.address),original_local=app::parse_ftn_address(plan.local_address);if(!local||!original_local||!app::same_ftn_address(*local,*original_local,true))message_detail::conflict();
    const auto parsed=app::decode_ftn_tic(plan.metadata_template,local->domain);if(!parsed.tic||!parsed.tic->password.empty()||parsed.tic->to||!app::same_ftn_address(parsed.tic->from,*local,true)||!ftn_detail::same(parsed.tic->file,r.filename)||!ftn_detail::same(parsed.tic->area,r.tag)||!parsed.tic->size||*parsed.tic->size!=r.payload.bytes||!parsed.tic->crc||*parsed.tic->crc!=r.crc||parsed.tic->path.empty()||!app::same_ftn_address(parsed.tic->path.back().address,*local,true)||parsed.tic->path.back().timestamp!=static_cast<std::uint64_t>(plan.created))message_detail::conflict();
    const auto origin=app::parse_ftn_address(r.origin);if(!origin||!app::same_ftn_address(parsed.tic->origin,*origin,true)||parsed.tic->replaces!=r.replaces)message_detail::conflict();
    for(const auto& target:plan.targets){const auto& link=ftn_work_detail::link(c,r.network,target.link,true);const auto address=app::parse_ftn_address(link.address),planned=app::parse_ftn_address(target.address);if(!address||!planned||!app::same_ftn_address(*address,*planned,true)||!app::ftn_outbound(link)||ftn_detail::same(link.code,r.link)||!app::ftn_echo_permission(c,r.network,link.code,r.tag,true,false)||credential_generation(link.credentials.tic)!=target.credential_generation||message_detail::source_digest(app::ftn_forward_peer_identity_material(link))!=target.peer_identity||target.payload.file.name!=r.filename||target.payload.file.size!=r.payload.bytes||target.payload.digest!=r.payload.sha256)message_detail::conflict();}
}
void insert(sqlite3* db,const app::FtnFileRequest& request,const app::FtnFileForwardPlan& plan,const std::vector<std::uint8_t>& bytes){
    validate(db,request,plan);SqliteStatement q(db,"INSERT INTO sfx_ftn_file_forward_plans VALUES(?1,?2,?3,?4)","atomically bind complete zero-or-more target plan before native artifacts");q.bind(1,request.operation);q.bind(2,ftn_file_detail::fingerprint(request));q.bind(3,hash(bytes));q.bind(4,bytes);q.step();
}
bool present(sqlite3* db,const app::Identifier128& id){SqliteStatement q(db,"SELECT 1 FROM sfx_ftn_file_forward_plans WHERE operation_id=?1","distinguish explicit zero-target plan from legacy absence");q.bind(1,id);return q.row();}
void authorize_job(sqlite3* db,std::int64_t revision,const app::FtnOutboundJob& job,const app::Identifier128& attempt){
    // Legacy low-level fixtures retain their original API. Native publication
    // requires a complete plan, including zero targets, before reaching here;
    // missing legacy plans never qualify for reviewed continuation.
    if(!present(db,job.operation)){if(attempt.valid())message_detail::conflict();return;}const auto plan=read(db,job.operation);const auto p=ftn_file_detail::read(db,job.operation);validate(db,ftn_continuation_detail::effective(db,p,attempt),plan);
    for(const auto& target:plan.targets)for(const auto* original:{&target.payload,&target.tic})if(original->id.bytes==job.id.bytes){if(ftn_spool_detail::fingerprint(revision,*original)!=ftn_spool_detail::fingerprint(revision,job))message_detail::conflict();return;}
    message_detail::conflict();
}
void commit_jobs(sqlite3* db,const app::FtnFilePublication& p,const std::vector<app::FtnOutboundJob>& jobs,const app::Identifier128& attempt){
    if(!present(db,p.request.operation)){if(attempt.valid())message_detail::conflict();return;}const auto plan=read(db,p.request.operation);validate(db,ftn_continuation_detail::effective(db,p,attempt),plan);if(jobs.size()!=plan.targets.size()*2)message_detail::conflict();for(std::size_t i=0;i<plan.targets.size();++i){const auto revision=p.request.configuration_revision;if(ftn_spool_detail::fingerprint(revision,plan.targets[i].payload)!=ftn_spool_detail::fingerprint(revision,jobs[i*2])||ftn_spool_detail::fingerprint(revision,plan.targets[i].tic)!=ftn_spool_detail::fingerprint(revision,jobs[i*2+1]))message_detail::conflict();}
}
}
