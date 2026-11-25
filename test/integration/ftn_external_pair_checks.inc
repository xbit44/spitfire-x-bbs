// Synthetic host-owned hatch associations exercise the real queue selector.
// No filesystem publication or stock mailer authentication is claimed here.
void pair_pages(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    storage::SqliteStorage db(settings(root),paths);check(db.start().succeeded(),"pair fixture opens");
    const app::MessageConfigurationActor owner{true,{}};auto c=*db.ftn_configuration(owner).value;
    c.settings.mode=app::FtnMailerMode::external_binkd;
    app::FtnNetwork n;n.code="TEST";n.name="Pair fixture";n.address="2:100/1@test";n.enabled=true;c.networks.push_back(n);
    app::FtnLink l;l.network=n.code;l.code="PEER";l.name="Peer";l.address="2:100/2@test";l.enabled=true;c.links.push_back(l);
    c.file_echoes.push_back({n.code,"TEST.FILES","Files",1});c.permissions.push_back({n.code,l.code,"TEST.FILES",true,false,true,false});
    check(db.change_ftn_configuration(owner,id(800).bytes,c).succeeded(),"pair configuration saved");c=*db.ftn_configuration(owner).value;
    std::vector<app::FtnOutboundJob> jobs;
    for(unsigned i=0;i<70;++i){
        app::FtnOutboundJob p;p.id=id(1000+i);p.operation=id(3000+i);p.network=n.code;p.link=l.code;p.kind=app::FtnOutboundKind::payload;p.file={"payload.zip",1,1700000000};p.digest.fill(1);
        auto t=p;t.id=id(2000+i);t.ordinal=1;t.kind=app::FtnOutboundKind::tic;t.file.name="control.tic";jobs.push_back(p);jobs.push_back(t);
    }
    check(db.queue_ftn_jobs(owner,c.revision,jobs).succeeded(),"queue seventy pairs with every payload sorting before every TIC");
    {internal::SqliteConnection sql(root/"SFX.DB",SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        sql.exec("BEGIN","atomic synthetic pair associations");
        for(unsigned i=0;i<70;++i){
            internal::SqliteStatement hatch(sql.get(),"INSERT INTO sfx_ftn_local_hatches(operation_id,request,fingerprint,source_file,source_area,filename,source_origin,payload_bytes,payload_sha256,native_volume,native_file_id,payload_crc,created_utc) VALUES(?1,x'01',zeroblob(32),1,1,'payload.zip',0,1,zeroblob(32),'fixture','fixture',0,1700000000)","synthetic hatch owner");hatch.bind(1,id(3000+i));hatch.step();
            internal::SqliteStatement owned(sql.get(),"INSERT INTO sfx_ftn_spool_owners VALUES(?1,NULL,?1)","synthetic typed owner");owned.bind(1,id(3000+i));owned.step();
            internal::SqliteStatement pair(sql.get(),"INSERT INTO sfx_ftn_file_job_echoes VALUES(?1,?3,'TEST.FILES',NULL),(?2,?3,'TEST.FILES',?1)","immutable pair dependency");pair.bind(1,id(1000+i));pair.bind(2,id(2000+i));pair.bind(3,id(3000+i));pair.step();
        }
        sql.exec("UPDATE sfx_ftn_outbound SET state=4,reason='fixture_held' WHERE job_id=(SELECT job_id FROM sfx_ftn_outbound WHERE kind=1 ORDER BY job_id LIMIT 1);COMMIT","retain first blocked pair to prove later progress");
    }
    check(!db.claim_ftn_jobs(owner,n.code,l.code,id(4000),id(4001),1,true),"external one-file page explicitly refused");
    std::size_t count{};unsigned sequence=4100;
    for(unsigned page=0;page<4;++page){
        const auto claim=id(sequence++);auto batch=db.claim_ftn_jobs(owner,n.code,l.code,id(4000),claim,63,true);
        check(bool(batch),"bounded pair claim succeeds");if(batch.value->empty())break;
        check(batch.value->size()<=63&&batch.value->size()%2==0,"odd page bound never splits a pair");
        for(const auto& job:*batch.value){const auto peers=std::count_if(batch.value->begin(),batch.value->end(),[&](const auto& peer){return peer.operation==job.operation;});check(peers==2,"each pending partner included in same claim");}
        count+=batch.value->size();auto prepared=db.prepare_ftn_external_handoff(owner,c.revision,claim);
        check(prepared&&prepared.value->size()==batch.value->size(),"whole pair page freezes together");
        check(db.finish_ftn_external_handoff(owner,claim,true).succeeded()&&db.release_ftn_external_handoff(owner,claim).succeeded(),"synthetic completed pair page releases link");
    }
    check(count==138,"all sixty-nine ready pairs progress past held first pair and multiple page boundaries");
    check(scalar(root/"SFX.DB","SELECT count(*) FROM sfx_ftn_outbound WHERE state=0")==1&&scalar(root/"SFX.DB","SELECT count(*) FROM sfx_ftn_outbound WHERE state=4")==1,"blocked pair remains intact");
    check(scalar(root/"SFX.DB","SELECT count(*) FROM pragma_foreign_key_check")==0,"pair fixture references remain valid");
    check(db.shutdown().succeeded(),"pair fixture stopped");
}
