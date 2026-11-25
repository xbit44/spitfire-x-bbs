namespace {
FilePayloadLease materialize_ftn_spool(app::IFtnMailStorage& storage,std::int64_t revision,
    const FilePayloadDirectory& directory,const app::FtnOutboundJob& job,
    const FilePayloadLease& source,const std::string_view* generated,
    const std::function<void()>& current,const std::function<void(std::string_view)>& point,
    const std::function<bool(std::uint64_t,std::uint64_t)>& progress={},const app::Identifier128& attempt={}){
        const auto tick=[&](std::uint64_t done,std::uint64_t total){current();require(!progress||progress(done,total),"ftn_spool_cancelled");return true;};
        current();const auto prepared=storage.prepare_ftn_spool(owner,revision,job,attempt);require(prepared&&prepared.value,"ftn_tic_forward_intent_conflict");auto state=*prepared.value;
        const auto name=ftn_existing_spool_name(directory,job);const std::string phase=generated?"forward_tic_":"forward_payload_";
        std::optional<FilePayloadLease> file;
        if(!state.object){
            require(directory.absent(name),"ftn_tic_forward_unbound_object");file.emplace(directory.create(name,std::max<std::uint64_t>(1,job.file.size)));point(phase+"created_before_binding");
            const auto bound=storage.bind_ftn_spool(owner,job.id,file->object_identity(),attempt);require(bound&&bound.value&&bound.value->object,"ftn_tic_forward_binding_failed");state=*bound.value;point(phase+"bound");
        }else if(state.ready)file.emplace(directory.read(name));
        else if(generated){app::FilePayloadIdentity expected{job.file.size,job.digest,state.object->volume,state.object->file_id};file.emplace(directory.resume_owned_bytes(name,expected,*generated,tick));}
        else file.emplace(directory.resume_owned_copy(name,*state.object,source,tick));
        require(state.object&&tic_same_object(file->object_identity(),*state.object),"ftn_tic_forward_object_changed");
        if(!state.ready){
            auto offset=file->byte_count();require(offset<=job.file.size,"ftn_tic_forward_excess_bytes");
            while(offset<job.file.size){
                tick(offset,job.file.size);const auto amount=static_cast<std::size_t>(std::min<std::uint64_t>(65536,job.file.size-offset));
                if(generated)file->append(reinterpret_cast<const std::uint8_t*>(generated->data()+offset),amount);
                else{const auto bytes=source.read(offset,amount);require(bytes.size()==amount,"ftn_tic_forward_source_read_failed");file->append(bytes.data(),bytes.size());}
                file->flush();offset+=amount;point(phase+"copy_chunk");
            }
        }
        const auto verified=file->identity(tick);require(verified.bytes==job.file.size&&verified.sha256==job.digest,"ftn_tic_forward_bytes_changed");point(phase+"verified_before_ready");
        const auto ready=storage.verify_ftn_spool(owner,job.id,verified,attempt);require(ready&&ready.value&&ready.value->ready,"ftn_tic_forward_ready_checkpoint_failed");point(phase+"ready");return std::move(*file);
}
}
