namespace {
std::string publication_manifest(const std::vector<app::FtnExternalPublication>& batch){
    std::string result="SFX-BINKD-PUBLICATION-1\n";
    for(const auto& row:batch){
        result+=token(row.job.id)+"\n"+row.busy_path+"\n"+row.flow_path+"\n"+row.copy_path+"\n"+std::to_string(row.job.file.size)+"\n";
        for(auto b:row.job.digest){result+="0123456789abcdef"[b>>4];result+="0123456789abcdef"[b&15];}result+='\n';
    }require(result.size()<=65536,"ftn_external_publication_too_large");return result;
}
void rename_owned(const std::filesystem::path& from,const std::filesystem::path& to,bool replace){
    require(replace||!std::filesystem::exists(to),"ftn_external_publication_collision");
#ifdef _WIN32
    require(MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0))!=0,"ftn_external_publication_rename_failed");
#else
    // The caller holds this link's exclusive BSO busy file. Copy names are
    // unique to its database-owned job; only this publisher writes this pool.
    require(rename(from.c_str(),to.c_str())==0,"ftn_external_publication_rename_failed");
#endif
    flush_parent(to);
}
void copy_outbound(const std::filesystem::path& root,const app::FtnExternalPublication& row,const ReadOutbound& source,const std::function<bool()>& running){
    require(row.job.file.size<=app::kFtnTransferMaximumBytes,"ftn_external_copy_too_large");
    const auto destination=checked(root,row.copy_path,true);
    if(std::filesystem::exists(destination)){
        File existing(destination);require(existing.size()==row.job.file.size&&hash_file(existing,{},running)==row.job.digest,"ftn_external_copy_conflict");return;
    }
    const auto temporary=checked(root,row.copy_path+".tmp",true);
    {
        File staged(temporary,!std::filesystem::exists(temporary),true);
        const auto prefix=staged.size();require(prefix<=row.job.file.size,"ftn_external_copy_prefix_conflict");
        crypto_hash_sha256_state hash{};crypto_hash_sha256_init(&hash);std::uint64_t total{};
        source(row.job,[&](std::string_view bytes){
            require(running()&&total<=row.job.file.size&&bytes.size()<=row.job.file.size-total,"ftn_external_copy_bound");
            crypto_hash_sha256_update(&hash,reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());
            if(total<prefix){
                auto compare=std::min<std::uint64_t>(prefix-total,bytes.size());std::array<char,16384> buffer{};
                while(compare){const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(compare,buffer.size()));const auto got=staged.read(buffer.data(),count);require(got==count&&bytes.substr(0,count)==std::string_view(buffer.data(),count),"ftn_external_copy_prefix_conflict");bytes.remove_prefix(count);total+=count;compare-=count;}
            }
            staged.write(bytes);total+=bytes.size();
        });
        app::FtnDigest digest{};crypto_hash_sha256_final(&hash,digest.data());
        require(total==row.job.file.size&&digest==row.job.digest,"ftn_external_source_changed");staged.flush();
    }
    rename_owned(temporary,destination,false);
}
void replace_flow(const std::filesystem::path& root,const std::string& relative,const std::string& bytes,const app::Identifier128& claim){
    require(bytes.size()<=8*1024*1024,"ftn_external_flow_full");const auto target=checked(root,relative,true);
    const auto name=relative+".sfx-"+token(claim)+".tmp";const auto temporary=checked(root,name,true);
    if(std::filesystem::exists(temporary))require(read_small(root,name,8*1024*1024)==bytes,"ftn_external_flow_stage_conflict");
    else{File file(temporary,true);file.write(bytes);file.flush();}
    rename_owned(temporary,target,true);
}
void release_busy(const std::filesystem::path& root,const std::string& relative,const std::string& own){
    const auto path=checked(root,relative);
    if(!std::filesystem::exists(path))return;
    // A subsequent mailer's lock is never ours to delete after a committed
    // handoff. A missing/foreign lock cannot cause the batch to be re-exported.
    if(read_small(root,relative,4096)!=own)return;
#ifdef _WIN32
    require(DeleteFileW(path.c_str())!=0,"ftn_external_busy_release_failed");
#else
    require(unlink(path.c_str())==0,"ftn_external_busy_release_failed");
#endif
    flush_parent(path);
}
}
PublicationResult publish_handoff(const std::filesystem::path& root,app::IFtnMailStorage& storage,const std::vector<app::FtnExternalPublication>& batch,const ReadOutbound& source,const std::function<bool()>& running,const std::function<void(std::string_view)>& boundary){
    require(!batch.empty()&&batch.size()<=64&&running&&running(),"ftn_external_publication_invalid");
    const app::MessageConfigurationActor owner{true,{}};const auto claim=batch.front().job.claim;
    const auto busy=batch.front().busy_path;const auto claim_token=token(claim);const auto lock_text="SFX-BINKD-BUSY-1\n"+claim_token+"\n";
    require(claim.valid(),"ftn_external_publication_invalid");
    for(const auto& row:batch)require(row.job.claim==claim&&row.busy_path==busy&&row.state==batch.front().state,"ftn_external_publication_mixed_batch");
    const auto notify=[&](std::string_view at){if(boundary)boundary(at);};
    if(batch.front().state==1){release_busy(root,busy,lock_text);require(storage.release_ftn_external_handoff(owner,claim).succeeded(),"ftn_external_release_record_failed");return {true,false,false};}
    require(batch.front().state==0&&source,"ftn_external_publication_not_pending");
    // A prepared batch has not been handed off. Recheck current policy before
    // touching its spool; a disabled or retargeted connection must not resume
    // the old destination. Committed batches above only release their own lock.
    const auto current=storage.ftn_configuration(owner);require(current&&current.value,"ftn_external_configuration_unavailable");
    require(current.value->settings.mode==app::FtnMailerMode::external_binkd,"ftn_external_mode_changed");
    for(const auto& row:batch){
        const auto& c=*current.value;
        const auto network=std::find_if(c.networks.begin(),c.networks.end(),[&](const auto& n){return n.code==row.job.network&&n.enabled;});
        const auto link=std::find_if(c.links.begin(),c.links.end(),[&](const auto& l){return l.network==row.job.network&&l.code==row.job.link&&l.enabled;});
        require(network!=c.networks.end()&&link!=c.links.end(),"ftn_external_connection_disabled");
        const auto paths=app::ftn_external_outbound_paths(*network,*link,row.job);
        require(paths&&paths->busy==row.busy_path&&paths->flow==row.flow_path&&paths->copy==row.copy_path,"ftn_external_destination_changed");
    }
    const auto journal=app::ftn_external_root(batch.front().job.network)+"/Journal/"+claim_token;
    const bool started=std::filesystem::exists(checked(root,journal+".intent",true));
    const auto lock_path=checked(root,busy,true);
    bool acquired=false;
    if(std::filesystem::exists(lock_path))acquired=read_small(root,busy,4096)==lock_text;
    else if(!started){try{File lock(lock_path,true);lock.write(lock_text);lock.flush();flush_parent(lock_path);acquired=true;}catch(const std::exception&){if(!std::filesystem::exists(lock_path))throw;}}
    if(!acquired){
        if(!started)return {false,true,false};
        require(storage.finish_ftn_external_handoff(owner,claim,false).succeeded(),"ftn_external_uncertainty_record_failed");return {false,false,true};
    }
    auto guard=std::make_unique<File>(lock_path);require(read_small(root,busy,4096)==lock_text,"ftn_external_busy_changed");
    const auto manifest=publication_manifest(batch);publish_once(root,journal+".intent",manifest);notify("after-publication-intent");
    for(const auto& row:batch){require(running(),"ftn_external_cancelled");guard->verify();copy_outbound(root,row,source,running);notify("after-transfer-copy");}
    // Every copy is durable before any FLO exposes the batch. Preserve all
    // earlier entries, and list file payloads before their TICs in each flow.
    std::map<std::string,std::string> flows;
    for(const auto& row:batch){
        auto at=flows.find(row.flow_path);
        if(at==flows.end()){
            const auto path=checked(root,row.flow_path,true);std::string text;
            if(std::filesystem::exists(path))text=read_small(root,row.flow_path,8*1024*1024);
            require(text.empty()||text.back()=='\n',"ftn_external_partial_flow_retained");at=flows.emplace(row.flow_path,std::move(text)).first;
        }
        const auto line=app::ftn_external_flow_line({native_relative(row.busy_path),native_relative(row.flow_path),native_relative(row.copy_path)},path_style);require(!line.empty(),"ftn_external_flow_line_invalid");
        const auto found=at->second.find(line);
        if(found==std::string::npos)at->second+=line;
        else require((found==0||at->second[found-1]=='\n')&&at->second.find(line,found+line.size())==std::string::npos,"ftn_external_flow_duplicate");
    }
    for(const auto& flow:flows){require(running(),"ftn_external_cancelled");guard->verify();replace_flow(root,flow.first,flow.second,claim);notify("after-flow-publication");}
    publish_once(root,journal+".ready",manifest);guard->verify();notify("before-handoff-commit");
    require(storage.finish_ftn_external_handoff(owner,claim,true).succeeded(),"ftn_external_handoff_commit_failed");notify("after-handoff-commit");
    guard.reset();release_busy(root,busy,lock_text);notify("after-busy-release");
    require(storage.release_ftn_external_handoff(owner,claim).succeeded(),"ftn_external_release_record_failed");return {true,false,false};
}
