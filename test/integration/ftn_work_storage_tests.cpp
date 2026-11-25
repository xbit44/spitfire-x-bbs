// Actual managed SQLite durability; synthetic HOST-created proofs/artifact hashes.
// This does not claim socket authentication, native custody or real peer ACKs.
#define main retained_work_file_fixture_main
#include "file_storage_tests.cpp"
#undef main
namespace {
app::FtnDigest hash(unsigned value){app::FtnDigest result{};result.fill(static_cast<std::uint8_t>(value));return result;}
app::FtnOutboundJob outgoing(unsigned number,bool hold=false){app::FtnOutboundJob j;j.id=id(100+number);j.operation=id(500+number);j.network="TEST";j.link="PEER";j.file={"m"+std::to_string(number)+".pkt",6,1700000000};j.digest=hash(number);if(hold)j.flavor=app::FtnMailFlavor::hold;return j;}
}
int main(int argc,char** argv){try{
    check(argc==2,"new owned fixture root required");const std::filesystem::path root=argv[1];check(!std::filesystem::exists(root),"prior work fixtures preserved");sfx::test::FakeStoragePathPolicy paths;auto s=settings(root);storage::SqliteStorage db(s,paths);check(db.start().succeeded(),"current managed owner ready");const app::MessageConfigurationActor owner{true,{}};
    auto c=*db.ftn_configuration(owner).value;app::FtnNetwork n;n.code="TEST";n.name="Synthetic network";n.address="2:100/1@test";n.enabled=true;c.networks.push_back(n);
    app::FtnLink l;l.code="PEER";l.network="test";l.name="Synthetic peer";l.address="2:100/2@test";l.host="127.0.0.1";l.enabled=true;l.credentials.session={1}; /* synthetic sealed-credential port, not DPAPI/native auth */c.links.push_back(l);check(db.change_ftn_configuration(owner,id(40).bytes,c).succeeded(),"case-folded test configuration saved through existing publisher");c=*db.ftn_configuration(owner).value;
    app::FtnIngressReceipt r;r.id=id(1);r.session=id(2);r.identity=hash(3);r.network="TEST";r.link="PEER";r.proof=app::FtnIngressProof::inbound_cram;r.configuration_revision=c.revision;r.file={"test.pkt",6,1700000000};
    check(!db.begin_ftn_ingress({},r)&&!db.queue_ftn_jobs({},c.revision,{outgoing(1)}),"untrusted/caller work authority refused");
    auto created=db.begin_ftn_ingress(owner,r);check(created&&created.value->id.bytes==r.id.bytes&&created.value->received==0,"bounded durable receipt reserved");
    auto retry=r;retry.id=id(3);check(db.begin_ftn_ingress(owner,retry).value->id.bytes==r.id.bytes,"lost begin reply reconciles exact scoped identity");
    retry.file.size=7;check(!db.begin_ftn_ingress(owner,retry),"changed descriptor cannot reuse durable identity");retry=r;retry.proof=app::FtnIngressProof::inbound_legacy;check(!db.begin_ftn_ingress(owner,retry),"changed proof cannot inherit stronger provenance");
    check(db.pending_ftn_ingress(owner,16).value->empty(),"partial file never reaches tosser");
    check(db.advance_ftn_ingress(owner,r.id,0,3,{},app::FtnIngressState::partial,{}).succeeded(),"flushed partial checkpoint persisted");
    check(!db.advance_ftn_ingress(owner,r.id,0,4,{},app::FtnIngressState::partial,{}),"stale offset cannot overwrite newer durable progress");
    retry=r;retry.session=id(4);const auto resumed=db.begin_ftn_ingress(owner,retry);check(resumed&&resumed.value->received==3,"new authenticated session resumes exact same-link partial");
    check(db.advance_ftn_ingress(owner,r.id,3,6,hash(8),app::FtnIngressState::received,{}).succeeded(),"completion requires bounded full size and owned integrity digest");
    check(db.advance_ftn_ingress(owner,r.id,3,6,hash(8),app::FtnIngressState::received,{}).succeeded(),"lost completion reply is idempotent");
    check(!db.advance_ftn_ingress(owner,r.id,6,6,hash(9),app::FtnIngressState::received,{}),"completed content digest immutable");
    const auto pending=db.pending_ftn_ingress(owner,16);check(pending&&pending.value->size()==1&&pending.value->front().digest==hash(8),"complete receipt has durable processing identity");
    check(db.advance_ftn_ingress(owner,r.id,6,6,hash(8),app::FtnIngressState::processed,{}).succeeded(),"processing result separated from transport custody");
    check(db.pending_ftn_ingress(owner,16).value->empty(),"completed processing not repeated");
    auto empty=l;empty.code="EMPTY";empty.name="Configured unverified peer";empty.address="2:100/3@test";empty.credentials.session.clear();c.links.push_back(empty);check(bool(db.change_ftn_configuration(owner,id(85).bytes,c)),"intentionally empty configured session policy saved");c=*db.ftn_configuration(owner).value;
    auto unverified=r;unverified.id=id(90);unverified.session=id(91);unverified.identity=hash(90);unverified.link="EMPTY";unverified.proof=app::FtnIngressProof::inbound_tcp;unverified.configuration_revision=c.revision;
    const auto empty_context=db.begin_ftn_ingress(owner,unverified);check(empty_context&&empty_context.value->proof==app::FtnIngressProof::inbound_tcp&&!app::ftn_verified_peer(empty_context.value->proof,false),"known intentionally empty inbound context retained as unverified");auto wrong=unverified;wrong.outbound=true;check(!db.begin_ftn_ingress(owner,wrong),"inbound proof6 cannot cross into outbound direction");wrong=unverified;wrong.proof=app::FtnIngressProof::inbound_cram;check(!db.begin_ftn_ingress(owner,wrong),"empty session key cannot acquire CRAM proof or upgrade partial custody");wrong=unverified;wrong.link="PEER";check(!db.begin_ftn_ingress(owner,wrong),"nonblank configured credential cannot downgrade to empty-session proof6");wrong=unverified;--wrong.configuration_revision;check(!db.begin_ftn_ingress(owner,wrong),"stale policy snapshot cannot begin or resume custody");
    check(!app::ftn_queue_offer_allowed(app::FtnIngressProof::inbound_tcp,false)&&!app::ftn_queue_offer_allowed(app::FtnIngressProof::nonsecure,false)&&!app::ftn_queue_offer_allowed(app::FtnIngressProof::external_packet,false),"known-unverified, unknown and external contexts grant no unrelated native queue pickup");check(app::ftn_queue_offer_allowed(app::FtnIngressProof::inbound_cram,false)&&app::ftn_queue_offer_allowed(app::FtnIngressProof::inbound_legacy,false)&&app::ftn_queue_offer_allowed(app::FtnIngressProof::outbound_tcp,true)&&!app::ftn_verified_peer(app::FtnIngressProof::outbound_tcp,true),"explicit queue direction remains distinct from answering-peer verification");
    const auto first=outgoing(1),held=outgoing(2,true);check(db.queue_ftn_jobs(owner,c.revision,{first,held}).succeeded()&&db.queue_ftn_jobs(owner,c.revision,{first,held}).succeeded(),"atomic queue intent and exact replay");auto changed=first;changed.digest=hash(99);check(!db.queue_ftn_jobs(owner,c.revision,{changed}),"changed-content operation replay refused");
    const auto generation=id(70),claim=id(71);auto batch=db.claim_ftn_jobs(owner,"TEST","PEER",generation,claim,64,false);check(batch&&batch.value->size()==1&&batch.value->front().id.bytes==first.id.bytes,"normal poll excludes hold flavor");
    check(!db.finish_ftn_job(owner,first.id,id(72),app::FtnOutboundState::delivered,{}),"another session cannot ACK claimed artifact");
    check(db.finish_ftn_job(owner,first.id,claim,app::FtnOutboundState::awaiting_ack,{}).succeeded(),"wire completion is still awaiting peer acknowledgement");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"actual owner store restart retains pending lease");
    check(db.recover_ftn_claims(owner,id(73)).succeeded(),"new Runtime generation recovers interrupted delivery without false success");
    batch=db.claim_ftn_jobs(owner,"TEST","PEER",id(73),id(74),64,false);check(batch&&batch.value->size()==1&&batch.value->front().attempts==2,"interrupted delivery retries same immutable identity");
    check(db.finish_ftn_job(owner,first.id,id(74),app::FtnOutboundState::delivered,{}).succeeded()&&db.finish_ftn_job(owner,first.id,id(74),app::FtnOutboundState::delivered,{}).succeeded(),"matching claimed ACK and retry cannot double-deliver state");
    check(!db.finish_ftn_job(owner,first.id,id(74),app::FtnOutboundState::queued,"disconnect"),"disconnect cannot roll acknowledged item back into queue");
    batch=db.claim_ftn_jobs(owner,"TEST","PEER",id(73),id(75),64,true);check(batch&&batch.value->size()==1&&batch.value->front().id.bytes==held.id.bytes,"explicit manual poll can include hold");
    check(!db.finish_ftn_job(owner,held.id,id(75),app::FtnOutboundState::external_handoff,{}),"bare state5 cannot claim a durable external publication");
    c.settings.mode=app::FtnMailerMode::external_binkd;c.settings.external_executable="binkd";check(db.change_ftn_configuration(owner,id(78).bytes,c).succeeded(),"external owner configuration saved");c=*db.ftn_configuration(owner).value;
    const auto publication=db.prepare_ftn_external_handoff(owner,c.revision,id(75));check(publication&&publication.value->size()==1,"synthetic host prepares exact publication intent");
    check(db.finish_ftn_external_handoff(owner,id(75),true).succeeded(),"synthetic completed publication is handoff not remote ACK");check(db.recover_ftn_claims(owner,id(76)).succeeded(),"restart retains committed handoff");
    batch=db.claim_ftn_jobs(owner,"TEST","PEER",id(76),id(77),64,true);check(batch&&batch.value->empty(),"handoff releases link without offering handed-off work");
    c=*db.ftn_configuration(owner).value;c.settings.mode=app::FtnMailerMode::disabled;check(db.change_ftn_configuration(owner,id(80).bytes,c).succeeded(),"global FTN Disabled saved");c=*db.ftn_configuration(owner).value;
    check(db.queue_ftn_jobs(owner,c.revision,{outgoing(3)}).succeeded(),"valid local mail may remain queued while global Disabled");check(!db.claim_ftn_jobs(owner,"TEST","PEER",id(81),id(82),64,true),"Disabled cannot start new queue delivery");
    check(db.shutdown().succeeded(),"work fixture closed");
    const auto file=s.data_directory/"SFX.DB";check(scalar(file,"SELECT count(*) FROM sfx_ftn_ingress_sessions")==3,"resume proof history durable, not overwritten");check(scalar(file,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=3")==1&&scalar(file,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=5")==1&&scalar(file,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=0")==1,"durable ACK/external/paused states remain distinct");
    std::cout<<"PASS "<<checks<<" actual SQLite FTN custody/queue checks (synthetic host proof and artifact ports)\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
