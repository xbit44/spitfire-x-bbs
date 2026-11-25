// Native's payload-GOT dependency is unchanged. External publishes both
// transfer copies together; never split a pending pair at a page boundary.
if(c.settings.mode==app::FtnMailerMode::external_binkd){
    bool changed=true;
    while(changed){
        changed=false;
        for(auto i=result.begin();i!=result.end();){
            SqliteStatement dependencies(db,
                "SELECT p.job_id,p.state FROM sfx_ftn_file_job_echoes f JOIN sfx_ftn_outbound p ON p.job_id=f.payload_job WHERE f.job_id=?1 "
                "UNION ALL SELECT t.job_id,t.state FROM sfx_ftn_file_job_echoes f JOIN sfx_ftn_outbound t ON t.job_id=f.job_id WHERE f.payload_job=?1",
                "require complete pending payload and TIC publication pair");
            dependencies.bind(1,i->id);bool ready=true;
            while(dependencies.row()){
                const auto dependency=dependencies.identifier(0);const auto state=dependencies.int64(1);
                if(state!=3&&state!=5&&std::none_of(result.begin(),result.end(),[&](const auto& job){return job.id==dependency;}))ready=false;
            }
            if(!ready){i=result.erase(i);changed=true;}else ++i;
        }
    }
}
