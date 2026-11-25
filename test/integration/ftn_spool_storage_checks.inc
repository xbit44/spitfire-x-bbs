    check(!db.commit_ftn_file(owner,first.operation,jobs),"unprepared outgoing objects cannot publish catalog or queue");
    for(const auto& spool_job:jobs){
        const auto intent=db.prepare_ftn_spool(owner,c.revision,spool_job);check(intent&&!intent.value->object&&!intent.value->ready,"immutable outgoing descriptor reserved before native creation");
        auto changed_intent=spool_job;changed_intent.digest[0]^=1;check(!db.prepare_ftn_spool(owner,c.revision,changed_intent),"regenerated metadata digest cannot replace saved outgoing intent");
        app::FilePayloadIdentity verified{spool_job.file.size,spool_job.digest,"333",std::to_string(7000+spool_job.ordinal)};
        check(!db.verify_ftn_spool(owner,spool_job.id,verified),"ready checkpoint refuses unbound native object");
        check(db.bind_ftn_spool(owner,spool_job.id,{verified.volume,verified.file_id}).succeeded(),"synthetic native binding persisted before first write");
        check(!db.bind_ftn_spool(owner,spool_job.id,{verified.volume,verified.file_id+"9"}),"native binding cannot be replaced");
        auto wrong_bytes=verified;wrong_bytes.sha256[0]^=1;check(!db.verify_ftn_spool(owner,spool_job.id,wrong_bytes),"changed observed content cannot become ready");
        check(db.shutdown().succeeded()&&db.start().succeeded(),"restart retains spool descriptor and object binding");
        const auto resumed=db.prepare_ftn_spool(owner,c.revision,spool_job);check(resumed&&resumed.value->object&&resumed.value->object->file_id==verified.file_id&&!resumed.value->ready,"same interrupted spool intent resumes exact binding");
        check(db.verify_ftn_spool(owner,spool_job.id,verified).value->ready,"exact full observed bytes permit ready checkpoint");
        check(db.verify_ftn_spool(owner,spool_job.id,verified).value->ready,"ready retry reconciles without duplicate job");
    }
