namespace {
// Read-only pins held through the writer's activation. Native mutation later
// reopens the exact bound object under the existing exclusive identity/prefix
// checks; activation never permits a name-based adoption or truncation.
std::vector<FilePayloadLease> pin_tic_continuation(const app::FtnConfiguration& config,
    const app::FileConfiguration& files,const ins::Receipt& installation,
    const app::FtnFilePublication& p,const app::FtnFileForwardPlan& plan,
    const app::FtnTicReview& reviewed,const FilePayloadLease& payload,
    const std::function<bool()>& running){
    using Phase=app::FtnFilePhase;std::vector<FilePayloadLease> pins;const auto& request=p.request;
    require(reviewed.publication&&reviewed.publication->request.operation.bytes==request.operation.bytes&&reviewed.publication->phase==p.phase,"ftn_tic_review_frontier_changed");
    const auto tick=[&](auto,auto){require(running&&running(),"ftn_tic_cancelled");return true;};
    const auto full=[&](FilePayloadLease& lease,const app::FilePayloadIdentity& expected){require(tic_same_payload(lease.identity(tick),expected),"ftn_tic_review_native_changed");};
    const auto prefix=[&](FilePayloadLease& target,const app::FileObjectIdentity& expected,const FilePayloadLease* source,const std::string_view* generated,std::uint64_t maximum){
        require(tic_same_object(target.object_identity(),expected)&&target.byte_count()<=maximum,"ftn_tic_review_native_changed");
        for(std::uint64_t offset=0;offset<target.byte_count();){tick(offset,maximum);const auto amount=static_cast<std::size_t>(std::min<std::uint64_t>(65536,target.byte_count()-offset));const auto bytes=target.read(offset,amount);require(bytes.size()==amount,"ftn_tic_review_prefix_read_failed");
            if(generated)require(generated->size()>=offset+amount&&std::equal(bytes.begin(),bytes.end(),reinterpret_cast<const std::uint8_t*>(generated->data()+offset)),"ftn_tic_review_prefix_changed");
            else{require(source!=nullptr,"ftn_tic_review_prefix_source_missing");const auto original=source->read(offset,amount);require(bytes==original,"ftn_tic_review_prefix_changed");}offset+=amount;
        }
    };
    const auto current_area=std::find_if(files.areas.begin(),files.areas.end(),[&](const auto& area){return area.area.id==p.area.area.id;});require(current_area!=files.areas.end()&&current_area->storage_path==p.area.storage_path,"ftn_tic_review_destination_changed");
    std::vector<std::filesystem::path> others;for(const auto& area:files.areas)if(!(area.area.id==p.area.area.id))others.push_back(std::filesystem::u8path(area.storage_path));
    const auto destination=FilePayloadDirectory::area(installation.root,std::filesystem::u8path(p.area.storage_path),others,{L"Bin",L"Config",L"Data",L"Display",L"Backup",L"Logs",L"Work",L"Doors",L"BotGate",L"FTN"},installation.owner);
    const auto removed=FilePayloadDirectory::private_store(installation.root/L"Data"/L"FileRemoved",installation.owner);
    const bool stage_present=!destination.absent(p.stage_name),target_present=!destination.absent(request.filename);bool target_is_new{};
    if(p.stage_object){
        require(p.phase!=Phase::placed||!stage_present,"ftn_tic_review_placement_changed");
        if(stage_present){auto stage=destination.read(p.stage_name);if(p.staged)full(stage,*p.staged);else prefix(stage,*p.stage_object,&payload,nullptr,request.payload.bytes);pins.push_back(std::move(stage));}
        else{require(p.staged&&target_present&&(p.phase==Phase::placed||p.phase==Phase::displaced||(!p.displaced&&p.phase==Phase::staged)),"ftn_tic_review_stage_missing");auto placed=destination.read(request.filename);full(placed,*p.staged);if(p.placed)full(placed,*p.placed);pins.push_back(std::move(placed));target_is_new=true;}
    }else require(!stage_present&&p.phase==Phase::reserved,"ftn_tic_review_unbound_stage");
    std::optional<FilePayloadLease> original;
    if(p.displaced){
        if(target_present&&!target_is_new){original.emplace(destination.read(request.filename));full(*original,p.displaced->payload);}
        if(p.recovery_object){
            if(!removed.absent(p.recovery_key)){auto recovery=removed.read(p.recovery_key);require(tic_same_object(recovery.object_identity(),*p.recovery_object),"ftn_tic_review_recovery_changed");
                if(p.recovered)full(recovery,*p.recovered);
                else if(original)prefix(recovery,*p.recovery_object,&*original,nullptr,p.displaced->payload.bytes);
                else{const auto actual=recovery.identity(tick);require(actual.bytes==p.displaced->payload.bytes&&actual.sha256==p.displaced->payload.sha256,"ftn_tic_review_recovery_incomplete");}
                pins.push_back(std::move(recovery));
            }else require(original&&tic_same_object(original->object_identity(),*p.recovery_object)&&p.phase==Phase::recovery_bound,"ftn_tic_review_recovery_missing");
        }else require(original&&removed.absent(p.recovery_key),"ftn_tic_review_unbound_recovery");
        if(p.phase>=Phase::displaced)require(!original,"ftn_tic_review_displacement_changed");
    }else require(!target_present||target_is_new,"ftn_tic_review_destination_occupied");
    if(original)pins.push_back(std::move(*original));
    const auto network=std::find_if(config.networks.begin(),config.networks.end(),[&](const auto& n){return same(n.code,request.network);});require(network!=config.networks.end(),"ftn_tic_forward_network_missing");const auto local=app::parse_ftn_address(network->address),saved_local=app::parse_ftn_address(plan.local_address);require(local&&saved_local&&app::same_ftn_address(*local,*saved_local,true),"ftn_tic_forward_local_identity_changed");
    const auto source_peer=std::find_if(config.links.begin(),config.links.end(),[&](const auto& link){return same(link.network,request.network)&&same(link.code,request.link);});require(source_peer!=config.links.end()&&digest(app::ftn_forward_peer_identity_material(*source_peer))==plan.source_peer_identity,"ftn_tic_forward_source_identity_changed");
    auto parsed=app::decode_ftn_tic(plan.metadata_template,local->domain);require(parsed.tic&&parsed.tic->password.empty()&&!parsed.tic->to,"ftn_tic_forward_template_invalid");
    const auto outgoing=FilePayloadDirectory::private_store(installation.root/L"FTN"/L"Outbound"/std::filesystem::u8path(request.network),installation.owner);std::size_t observed{};
    for(const auto& target:plan.targets){
        tick(0,0);const auto link=std::find_if(config.links.begin(),config.links.end(),[&](const auto& value){return same(value.network,request.network)&&same(value.code,target.link);});require(link!=config.links.end()&&app::ftn_outbound(*link)&&app::ftn_echo_permission(config,request.network,target.link,request.tag,true,false),"ftn_tic_forward_permission_changed");require(tic_credential_generation(link->credentials.tic)==target.credential_generation&&digest(app::ftn_forward_peer_identity_material(*link))==target.peer_identity,"ftn_tic_forward_identity_or_credential_changed");
        auto forward=*parsed.tic;TextErase wipe{forward.password};forward.to=*app::parse_ftn_address(target.address);if(!link->credentials.tic.empty()){auto secret=unprotect_ftn_secret(installation.root.u8string(),request.network,link->code,FtnCredentialKind::tic,link->credentials.tic);require(secret.has_value(),"ftn_tic_forward_secret_unavailable");forward.password.assign(secret->view());}auto encoded=app::encode_ftn_tic(forward);require(encoded.has_value(),"ftn_tic_forward_metadata_invalid");Bytes bytes{std::move(*encoded)};require(bytes.value.size()==target.tic.file.size&&digest(bytes.value)==target.tic.digest,"ftn_tic_forward_original_bytes_changed");const std::string_view generated=bytes.value;
        for(const auto* job:{&target.payload,&target.tic}){const auto state=std::find_if(reviewed.spool.begin(),reviewed.spool.end(),[&](const auto& value){return value.job.id.bytes==job->id.bytes;});const auto name=ftn_existing_spool_name(outgoing,*job);
            if(state==reviewed.spool.end()){require(outgoing.absent(name),"ftn_tic_forward_unbound_object");continue;}++observed;
            require(state->configuration_revision==request.configuration_revision&&state->job.operation.bytes==request.operation.bytes&&state->job.digest==job->digest&&state->job.file.name==job->file.name&&state->job.file.size==job->file.size&&state->job.ordinal==job->ordinal,"ftn_tic_forward_plan_changed");
            if(!state->object){require(outgoing.absent(name),"ftn_tic_forward_unbound_object");continue;}
            auto file=outgoing.read(name);if(state->ready)full(file,{job->file.size,job->digest,state->object->volume,state->object->file_id});else prefix(file,*state->object,job->kind==app::FtnOutboundKind::payload?&payload:nullptr,job->kind==app::FtnOutboundKind::tic?&generated:nullptr,job->file.size);pins.push_back(std::move(file));
        }
    }
    require(observed==reviewed.spool.size(),"ftn_tic_forward_unplanned_object");return pins;
}
}
