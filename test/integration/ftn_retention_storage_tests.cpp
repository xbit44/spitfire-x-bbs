// Real SQLite writer/CAS/time, synthetic host custody identities. No native
// deletion or transport/installed qualification is asserted by this test.
#define main retained_retention_fixture_main
#include "file_storage_tests.cpp"
#undef main
#include <sodium.h>
namespace {
app::FtnDigest digest(unsigned n){app::FtnDigest h{};h[0]=static_cast<std::uint8_t>(n);h[1]=static_cast<std::uint8_t>(n>>8);return h;}
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh fixture required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"prior fixtures preserved");
    auto s=settings(root);std::int64_t now=1800000000;s.utc_now=[&]{return now;};sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorage db(s,paths);check(bool(db.start()),"current schema starts");const app::MessageConfigurationActor owner{true,{}};const auto file=root/"SFX.DB";
    auto c=*db.ftn_configuration(owner).value;app::FtnNetwork n;n.code="TEST";n.name="Retention Test";n.address="2:100/1@test";n.enabled=true;c.networks.push_back(n);
    app::FtnLink l;l.network="TEST";l.code="PEER";l.name="Synthetic peer";l.address="2:100/2@test";l.host="127.0.0.1";l.enabled=true;l.credentials.session={1};c.links.push_back(l);
    unsigned op=10000;
    const auto save=[&](const app::FtnConfiguration& draft){return db.change_ftn_configuration(owner,id(op++).bytes,draft);};
    check(bool(save(c)),"initial default Yes30");c=*db.ftn_configuration(owner).value;
    const auto finish=[&](unsigned number,bool native=true,bool claim=true,app::FtnIngressProof proof=app::FtnIngressProof::inbound_cram){
        const auto current=*db.ftn_configuration(owner).value;app::FtnIngressReceipt r;r.id=id(number);r.session=id(number+1000);r.identity=digest(number);r.network="TEST";r.link="PEER";r.proof=proof;r.outbound=proof==app::FtnIngressProof::outbound_tcp;r.configuration_revision=current.revision;r.file={"test"+std::to_string(number)+".pkt",6,1700000000};
        check(bool(db.begin_ftn_ingress(owner,r)),"reserve synthetic custody");app::FilePayloadIdentity p{0,{},"1",std::to_string(number)};check(sodium_init()>=0&&crypto_hash_sha256(p.sha256.data(),reinterpret_cast<const unsigned char*>(""),0)==0,"real empty-prefix digest");
        if(native){check(bool(db.checkpoint_ftn_ingress(owner,r.id,0,p)),"bind empty native identity");p.bytes=6;p.sha256=digest(number);check(bool(db.checkpoint_ftn_ingress(owner,r.id,0,p)),"full durable identity checkpoint");}
        check(bool(db.advance_ftn_ingress(owner,r.id,native?6:0,6,digest(number),app::FtnIngressState::received,{})),"received full custody");
        if(claim){const auto owned=db.claim_ftn_message(owner,r.id,current.revision);check(owned&&*owned.value,"actual message processing owner");}
        check(bool(db.advance_ftn_ingress(owner,r.id,6,6,digest(number),app::FtnIngressState::processed,{})),"new successful transition");return r;
    };
    finish(1);check(scalar(file,"SELECT deadline-successful_utc FROM sfx_ftn_retained_originals")==30*86400,"default protection from actual success time");
    const auto legacy=finish(2,false);finish(3,true,false);finish(4,true,true,app::FtnIngressProof::outbound_tcp);
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_retained_originals")==1,"unbound, ownerless and unverified are never expiry candidates");
    check(bool(db.advance_ftn_ingress(owner,legacy.id,6,6,digest(2),app::FtnIngressState::processed,{})),"legacy replay reconciles without backfill");
    auto draft=c;draft.networks[0].keep_processed=false;check(bool(save(draft)),"Off future only needs no existing shortening");
    check(scalar(file,"SELECT deadline-successful_utc FROM sfx_ftn_retained_originals")==30*86400,"Off keeps prior protection");finish(5);
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_retained_originals WHERE deadline=successful_utc")==1,"new Off receipt has its successful-time policy");
    draft=*db.ftn_configuration(owner).value;draft.networks[0].keep_processed=true;draft.networks[0].retention_days=0;auto t=id(op++).bytes;
    auto review=db.preview_ftn_retention(owner,t,draft);check(review&&review.value->changed==2&&review.value->shortened==0,"Forever extends both mixed prior policies");check(bool(db.change_ftn_configuration(owner,t,draft)),"exact preview applies extension");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_retained_originals WHERE deadline=0")==2,"Forever durable");
    now+=10*86400;finish(6);draft=*db.ftn_configuration(owner).value;draft.networks[0].retention_days=5;t=id(op++).bytes;
    review=db.preview_ftn_retention(owner,t,draft);check(review&&review.value->changed==3&&review.value->shortened==3&&review.value->networks.size()==1,"all existing shortened objects reviewed");
    const auto& nr=review.value->networks.front();check(nr.network=="TEST"&&nr.due_now==2&&nr.expire_later==1&&nr.from_forever==3&&nr.latest-nr.earliest==10*86400,"named due-now/later/Forever date range");
    const auto encoded=app::encode_ftn_retention_preview(*review.value);check(!encoded.empty()&&app::decode_ftn_retention_preview(encoded).has_value(),"bounded owner projection roundtrip");for(std::size_t cut=0;cut<encoded.size();++cut)check(!app::decode_ftn_retention_preview({encoded.begin(),encoded.begin()+cut}),"every truncated projection refused");auto bad=encoded;bad.push_back(0);check(!app::decode_ftn_retention_preview(bad),"trailing bytes refused");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_retained_originals WHERE deadline=0")==3,"preview/Cancel never activate draft");
    check(!db.preview_ftn_retention({},t,draft)&&!db.change_ftn_configuration({},t,draft),"owner-only plan and Save");
    check(!save(draft),"unreviewed shortening refused");auto changed=draft;changed.networks[0].name="Changed draft";check(!db.change_ftn_configuration(owner,t,changed),"token bound to exact uploaded draft");
    finish(7);check(!db.change_ftn_configuration(owner,t,draft),"later receipt cannot silently join reviewed plan");check(!db.preview_ftn_retention(owner,t,draft),"same token cannot refresh into larger set");
    t=id(op++).bytes;review=db.preview_ftn_retention(owner,t,draft);check(review&&review.value->changed==4,"fresh review includes explicitly refreshed set");
    const auto no_save_revision=db.ftn_configuration(owner).value->revision;check(scalar(file,"SELECT count(*) FROM sfx_ftn_retained_originals WHERE deadline=0")==4,"refusal remains all-or-nothing");
    const auto applied=db.change_ftn_configuration(owner,t,draft);check(applied&&applied.value->applied_revision==no_save_revision+1,"configuration and exact protections commit together");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_retained_originals WHERE deadline=successful_utc+432000")==4,"all exact deadlines published");check(db.change_ftn_configuration(owner,t,draft).value->replayed,"lost Save reply does not shorten twice");
    check(scalar(file,"SELECT count(*) FROM sfx_ftn_retention_plans WHERE applied_revision IS NOT NULL")==2,"only applied plans marked");
    draft=*db.ftn_configuration(owner).value;draft.networks[0].name="Renamed";check(bool(save(draft)),"unrelated Save preserves mixed-policy interpretation");
    now-=20*86400;finish(8);check(scalar(file,"SELECT count(*) FROM sfx_ftn_retained_originals")==4,"clock rollback cannot invent success time for expiry");now+=21*86400;
    check(bool(db.shutdown())&&bool(db.start()),"actual durable reopen");check(scalar(file,"SELECT count(*) FROM sfx_ftn_retained_originals WHERE deadline=successful_utc+432000")==4,"deadlines and immutable provenance survive restart");
    draft=*db.ftn_configuration(owner).value;draft.networks[0].retention_days=1;draft.file_echoes.push_back({"TEST","NO.AREA","Invalid target",999999});t=id(op++).bytes;
    check(bool(db.preview_ftn_retention(owner,t,draft)),"exact review can precede final target validation");check(!db.change_ftn_configuration(owner,t,draft),"later file mapping failure rolls back protection update and plan activation");check(scalar(file,"SELECT count(*) FROM sfx_ftn_retained_originals WHERE deadline=successful_utc+432000")==4&&scalar(file,"SELECT count(*) FROM sfx_ftn_retention_plans WHERE applied_revision IS NOT NULL")==2,"failed Save leaves all prior protections and plan states exact");
    check(bool(db.shutdown()),"stop before synthetic capacity fixture");
    {internal::SqliteConnection direct(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);direct.exec("BEGIN IMMEDIATE","seed synthetic capacity only");
        for(unsigned i=0;i<4093;++i){const auto synthetic=id(100000+i);internal::SqliteStatement ingress(direct.get(),"INSERT INTO sfx_ftn_ingress SELECT ?1,?2,network,link,proof,outbound,session_id,configuration_revision,wire_name,declared_size,wire_time,received_bytes,content_sha256,state,reason,created_utc,updated_utc,external_session_authenticated FROM sfx_ftn_ingress WHERE receipt_id=?3","synthetic legacy-shaped capacity fixture");app::FtnDigest identity{};std::copy(synthetic.bytes.begin(),synthetic.bytes.end(),identity.begin());ingress.bind(1,synthetic);ingress.bind(2,identity);ingress.bind(3,id(1));ingress.step();internal::SqliteStatement protection(direct.get(),"INSERT INTO sfx_ftn_retained_originals SELECT ?1,network,successful_utc,configuration_revision,volume,?2,byte_count,sha256,protection_revision,deadline FROM sfx_ftn_retained_originals WHERE ingress_id=?3","synthetic exact policy capacity fixture, no native deletion");protection.bind(1,synthetic);protection.bind(2,std::to_string(100000+i));protection.bind(3,id(1));protection.step();}
        direct.exec("COMMIT","synthetic capacity fixture complete");}
    check(bool(db.start()),"bounded-capacity fixture reopens");draft=*db.ftn_configuration(owner).value;draft.networks[0].retention_days=1;const auto over=db.preview_ftn_retention(owner,id(op++).bytes,draft);check(!over&&(over.status.code==app::StorageErrorCode::backpressure||over.status.code==app::StorageErrorCode::busy),"4097 changed protections refuse instead of reviewing a subset");check(!save(draft)&&scalar(file,"SELECT count(*) FROM sfx_ftn_retained_originals WHERE deadline=successful_utc+432000")==4097,"oversize Save cannot partially shorten");
    check(bool(db.shutdown()),"clean fixture stop");std::cout<<"PASS "<<checks<<" FTN retention policy SQLite checks (synthetic native identities; no deletion)\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
