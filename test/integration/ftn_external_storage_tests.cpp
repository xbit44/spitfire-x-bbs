// Real SQLite migration/admission boundaries; synthetic host observations.
// This is not a stock BinkD authentication or filesystem publication test.
#define main retained_file_fixture_main
#include "file_storage_tests.cpp"
#undef main
namespace {
void upgrade(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    auto s=settings(root);auto previous=storage::sfx_storage_migrations();previous.resize(27);std::vector<std::uint8_t> prior_configuration;
    {storage::SqliteStorage old(s,paths,previous);check(old.start().succeeded()&&old.health().schema_version==27,"exact predecessor opens");const app::MessageConfigurationActor owner{true,{}};auto c=*old.ftn_configuration(owner).value;
        app::FtnNetwork n;n.code="NATIVE";n.name="Prior Native Network";n.address="2:101/1@native";n.enabled=true;c.networks.push_back(n);
        app::FtnLink l;l.network=n.code;l.code="PEER";l.name="Prior Peer";l.address="2:101/2@native";l.host="127.0.0.1";l.enabled=true;l.credentials.session={7,8,9};c.links.push_back(l);
        check(bool(old.change_ftn_configuration(owner,id(700).bytes,c)),"populated Native predecessor settings saved");c=*old.ftn_configuration(owner).value;prior_configuration=app::encode_ftn_configuration(c);
        app::FtnOutboundJob j;j.id=id(701);j.operation=id(702);j.network=n.code;j.link=l.code;j.file={"prior.pkt",7,1700000000};j.digest.fill(42);
        check(bool(old.queue_ftn_jobs(owner,c.revision,{j})),"predecessor Native queue retains actual job");
        check(old.shutdown().succeeded(),"predecessor stopped");}
    const auto file=root/"SFX.DB";
    {internal::SqliteConnection db(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        db.exec("INSERT INTO sfx_ftn_ingress VALUES(zeroblob(16),zeroblob(32),'TEST','PEER',5,0,zeroblob(16),1,'old.pkt',0,0,0,zeroblob(32),1,'',1,1); INSERT INTO sfx_ftn_ingress_sessions VALUES(zeroblob(16),zeroblob(16),5,1,0,1)","retain preexisting unverified external rows");
        db.exec("INSERT INTO sfx_ftn_ingress SELECT X'01000000000000000000000000000000',X'0100000000000000000000000000000000000000000000000000000000000000','NATIVE','PEER',1,0,X'01000000000000000000000000000000',2,'prior-native.pkt',7,1700000000,7,zeroblob(32),4,'',1700000000,1700000001; INSERT INTO sfx_ftn_ingress_sessions VALUES(X'01000000000000000000000000000000',X'01000000000000000000000000000000',1,2,0,1700000000)","retain prior Native provenance and successful custody");}
    s.bootstrap_boundary_hook=[](std::string_view at){return at=="before-ftn-external-migration-commit"?app::StorageStatus{app::StorageErrorCode::io,"injected external migration failure"}:app::StorageStatus::success();};
    {storage::SqliteStorage failed(s,paths);check(!failed.start(),"interrupted migration fails");}
    check(scalar(file,"PRAGMA user_version")==27,"interrupted additive migration rolled back");
    s.bootstrap_boundary_hook={};
    {storage::SqliteStorage current(s,paths);check(current.start().succeeded()&&current.health().schema_version==28,"predecessor upgrades normally");check(app::encode_ftn_configuration(*current.ftn_configuration({true,{}}).value)==prior_configuration,"upgrade preserves exact Native settings and protected credential bytes");check(current.shutdown().succeeded(),"upgraded fixture stopped");}
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_ingress WHERE proof=5 AND external_session_authenticated=0")==1,"old external proof is not promoted");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_ingress_sessions WHERE proof=5 AND external_session_authenticated=0")==1,"old session observation preserved without authentication");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_ingress WHERE proof=1 AND state=4 AND network='NATIVE' AND wire_name='prior-native.pkt' AND declared_size=7 AND received_bytes=7 AND external_session_authenticated=0")==1,"old Native custody remains Native and exact");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_outbound WHERE network='NATIVE' AND link='PEER' AND state=0 AND wire_name='prior.pkt' AND size=7")==1,"prior Native pending job survives unchanged");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_external_publications")==0,"migration invents no external handoffs");
    check(scalar(file,"SELECT count(*) FROM pragma_foreign_key_check")==0,"upgraded references clean");
}
void admission(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    auto s=settings(root);storage::SqliteStorage db(s,paths);check(db.start().succeeded(),"current fixture opens");const app::MessageConfigurationActor owner{true,{}};
    auto c=*db.ftn_configuration(owner).value;app::FtnNetwork n;n.code="TEST";n.name="Test";n.address="2:100/1@test";n.enabled=true;c.networks.push_back(n);
    app::FtnLink l;l.network=n.code;l.code="PEER";l.name="Peer";l.address="2:100/2@test";l.host="127.0.0.1";l.enabled=true;l.credentials.session={1};c.links.push_back(l);
    check(db.change_ftn_configuration(owner,id(30).bytes,c).succeeded(),"Native synthetic policy saved");c=*db.ftn_configuration(owner).value;
    app::FtnIngressReceipt r;r.id=id(1);r.session=id(2);r.identity.fill(1);r.network=n.code;r.link=l.code;r.proof=app::FtnIngressProof::external_packet;r.file={"test.pkt",0,1700000000};r.configuration_revision=c.revision;r.external_session_authenticated=true;
    check(!db.begin_ftn_ingress(owner,r),"Native mode rejects external authenticated begin");
    c.settings.mode=app::FtnMailerMode::external_binkd;c.settings.external_executable="binkd";
    check(db.change_ftn_configuration(owner,id(31).bytes,c).succeeded(),"External synthetic policy saved");c=*db.ftn_configuration(owner).value;r.configuration_revision=c.revision;
    check(!db.begin_ftn_ingress({},r),"caller cannot assert local-hook authentication");
    auto result=db.begin_ftn_ingress(owner,r);check(result&&result.value->external_session_authenticated&&app::ftn_verified_receipt(*result.value),"explicit external observation retained independently");
    check(!app::ftn_verified_peer(result.value->proof,false)&&!app::ftn_queue_offer_allowed(result.value->proof,false),"external observation grants neither Native method nor Native queue pickup");
    auto altered=r;altered.external_session_authenticated=false;check(!db.begin_ftn_ingress(owner,altered),"same durable receipt cannot change authentication");
    altered=r;altered.outbound=true;check(!db.begin_ftn_ingress(owner,altered),"hook cannot invent outgoing-session proof");
    altered=r;altered.proof=app::FtnIngressProof::inbound_cram;check(!db.begin_ftn_ingress(owner,altered),"external field cannot upgrade Native proof");
    altered=r;altered.id=id(3);altered.identity.fill(3);altered.external_session_authenticated=false;
    result=db.begin_ftn_ingress(owner,altered);check(result&&!app::ftn_verified_receipt(*result.value),"unprotected external remains unverified");
    c.links[0].credentials.session.clear();check(db.change_ftn_configuration(owner,id(32).bytes,c).succeeded(),"passwordless synthetic policy saved");c=*db.ftn_configuration(owner).value;r.configuration_revision=c.revision;r.id=id(4);r.identity.fill(4);
    check(bool(db.begin_ftn_ingress(owner,r)),"external-only link relies on BinkD observation without duplicate session secret");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"actual store restart");
    result=db.ftn_ingress_receipt(owner,id(1));check(result&&result.value->external_session_authenticated,"restart preserves exact external observation");
    result=db.ftn_ingress_receipt(owner,id(3));check(result&&!result.value->external_session_authenticated,"restart never promotes unprotected context");
    check(db.shutdown().succeeded(),"current fixture stopped");
}
#include "ftn_external_pair_checks.inc"
void handoff(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    auto s=settings(root);storage::SqliteStorage db(s,paths);check(db.start().succeeded(),"handoff fixture opens");const app::MessageConfigurationActor owner{true,{}};
    auto c=*db.ftn_configuration(owner).value;c.settings.mode=app::FtnMailerMode::external_binkd;
    app::FtnNetwork n;n.code="TEST";n.name="Test";n.address="2:100/1@test";n.enabled=true;c.networks.push_back(n);
    app::FtnLink l;l.network=n.code;l.code="PEER";l.name="Peer";l.address="2:100/2@test";l.enabled=true;l.host.clear();l.port=0;c.links.push_back(l);
    check(db.change_ftn_configuration(owner,id(100).bytes,c).succeeded(),"external-only configuration saved without Native endpoint or session secret");c=*db.ftn_configuration(owner).value;
    const auto job=[&](unsigned i){app::FtnOutboundJob j;j.id=id(i);j.operation=id(i+1000);j.network=n.code;j.link=l.code;j.file={"test.pkt",1,1700000000};j.digest.fill(static_cast<std::uint8_t>(i));return j;};
    auto first=job(101);check(db.queue_ftn_jobs(owner,c.revision,{first}).succeeded(),"queue first immutable job");
    auto claimed=db.claim_ftn_jobs(owner,n.code,l.code,id(110),id(111),64,true);check(claimed&&claimed.value->size()==1,"claim first external batch");
    check(!db.finish_ftn_job(owner,first.id,id(111),app::FtnOutboundState::external_handoff,{}),"bare handoff transition refused");
    auto prepared=db.prepare_ftn_external_handoff(owner,c.revision,id(111));check(prepared&&prepared.value->size()==1&&prepared.value->front().copy_path.find("FTN/External/test/Copies/")==0,"durable job-scoped publication before filesystem access");
    check(!db.prepare_ftn_external_handoff({},c.revision,id(111)),"caller cannot prepare mailer publication");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"restart during prepared publication");
    check(db.recover_ftn_claims(owner,id(112)).succeeded(),"generic recovery runs");
    prepared=db.pending_ftn_external_handoff(owner);check(prepared&&prepared.value->size()==1&&prepared.value->front().job.state==app::FtnOutboundState::claimed,"prepared publication is not blindly requeued");
    check(!db.claim_ftn_jobs(owner,n.code,l.code,id(112),id(113),64,true),"unreconciled publication protects link");
    check(!db.finish_ftn_external_handoff(owner,id(114),true),"different claim cannot finish publication");
    check(db.finish_ftn_external_handoff(owner,id(111),true).succeeded()&&db.finish_ftn_external_handoff(owner,id(111),true).succeeded(),"synthetic filesystem completion committed atomically and idempotently");
    check(!db.finish_ftn_external_handoff(owner,id(111),false),"committed handoff cannot be downgraded");
    prepared=db.pending_ftn_external_handoff(owner);check(prepared&&prepared.value->size()==1&&prepared.value->front().state==1,"committed publication remains recoverable until busy release");
    check(db.release_ftn_external_handoff(owner,id(111)).succeeded()&&db.release_ftn_external_handoff(owner,id(111)).succeeded(),"committed release is idempotent");
    check(db.pending_ftn_external_handoff(owner).value->empty(),"released publication never re-exported");
    auto page=db.ftn_queue_page(owner,0);check(page&&page.value->outbound[5]==1&&page.value->outbound[3]==0&&page.value->rows.empty(),"handoff history is counted separately from active queue and delivered");
    auto second=job(102);check(db.queue_ftn_jobs(owner,c.revision,{second}).succeeded(),"later mail remains queueable");
    claimed=db.claim_ftn_jobs(owner,n.code,l.code,id(112),id(115),64,true);check(claimed&&claimed.value->size()==1&&claimed.value->front().id==second.id,"handoff releases link and old work is never reoffered");
    check(bool(db.prepare_ftn_external_handoff(owner,c.revision,id(115))),"prepare second publication");
    check(db.finish_ftn_external_handoff(owner,id(115),false).succeeded(),"uncertain publication explicitly held");
    check(db.recover_ftn_claims(owner,id(116)).succeeded(),"recovery retains uncertainty");
    page=db.ftn_queue_page(owner,0);check(page&&page.value->rows.size()==1&&page.value->rows.front().state==4&&page.value->rows.front().reason=="external_publication_uncertain","uncertainty remains visible without replay");
    check(db.shutdown().succeeded(),"handoff fixture stopped");
    check(scalar(root/"SFX.DB","SELECT count(*) FROM sfx_ftn_external_publications WHERE state=1")==1&&scalar(root/"SFX.DB","SELECT count(*) FROM sfx_ftn_external_publications WHERE state=2")==1,"distinct committed and uncertain publication history");
}
}
int main(int argc,char** argv){try{check(argc==2,"new owned fixture root required");const std::filesystem::path root=argv[1];check(!std::filesystem::exists(root),"previous evidence retained");sfx::test::FakeStoragePathPolicy paths;upgrade(root/"upgrade",paths);admission(root/"admission",paths);handoff(root/"handoff",paths);pair_pages(root/"pairs",paths);std::cout<<"PASS external FTN storage "<<checks<<" checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
