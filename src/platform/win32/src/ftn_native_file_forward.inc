#include "ftn_native_spool.inc"
namespace {
struct TicForwardSpool {std::vector<app::FtnOutboundJob> jobs;std::vector<FilePayloadLease> leases;};
app::FtnDigest tic_credential_generation(const std::vector<std::uint8_t>& envelope){const auto retained=app::retained_portable_ftn_generation(envelope);return retained?*retained:digest({reinterpret_cast<const char*>(envelope.data()),envelope.size()});}
app::FtnFileForwardPlan plan_tic_forwarding(const app::FtnConfiguration& config,
    const ins::Receipt& installation,const app::FtnTic& incoming,const app::FtnFilePublication& publication,
    const std::function<bool()>& running){
    app::FtnFileForwardPlan result;const auto& request=publication.request;result.publication=request.operation;result.created=publication.created;result.network=request.network;
    const auto network=std::find_if(config.networks.begin(),config.networks.end(),[&](const auto& n){return same(n.code,request.network);});
    require(network!=config.networks.end(),"ftn_tic_forward_network_missing");const auto local=app::parse_ftn_address(network->address);require(local.has_value(),"ftn_tic_forward_local_address_invalid");
    result.local_address=app::format_ftn_address(*local);
    const auto source_peer=std::find_if(config.links.begin(),config.links.end(),[&](const auto& link){return same(link.network,request.network)&&same(link.code,request.link);});require(source_peer!=config.links.end(),"ftn_tic_forward_source_missing");result.source_peer_identity=digest(app::ftn_forward_peer_identity_material(*source_peer));
    const auto seen=[&](const app::FtnAddress& address){
        if(app::same_ftn_address(address,incoming.origin,true)||app::same_ftn_address(address,incoming.from,true))return true;
        for(const auto& hop:incoming.path)if(app::same_ftn_address(address,hop.address,true))return true;
        return std::any_of(incoming.seen_by.begin(),incoming.seen_by.end(),[&](const auto& prior){return app::same_ftn_address(address,prior,true);});
    };
    std::vector<const app::FtnLink*> destinations;
    for(const auto& link:config.links){
        if(!same(link.network,request.network)||same(link.code,request.link)||!app::ftn_outbound(link)||!app::ftn_echo_permission(config,request.network,link.code,request.tag,true,false))continue;
        const auto primary=app::parse_ftn_address(link.address);require(primary.has_value(),"ftn_tic_forward_peer_invalid");bool loop=seen(*primary);
        for(const auto& text:link.additional_addresses){const auto address=app::parse_ftn_address(text);require(address.has_value(),"ftn_tic_forward_peer_invalid");loop=loop||seen(*address);}
        if(!loop)destinations.push_back(&link);
    }
    require(destinations.size()<=2048,"ftn_tic_forward_target_limit");
    const auto current=[&]{require(running&&running(),"ftn_tic_forward_cancelled");};
    std::vector<app::FtnAddress> seen_by=incoming.seen_by;
    const auto remember=[&](const app::FtnAddress& address){if(std::none_of(seen_by.begin(),seen_by.end(),[&](const auto& prior){return app::same_ftn_address(address,prior,true);}))seen_by.push_back(address);};
    remember(incoming.from);remember(*local);for(const auto* destination:destinations)remember(*app::parse_ftn_address(destination->address));
    auto base=incoming;TextErase wipe_base{base.password};base.password.clear();base.from=*local;base.to.reset();base.size=request.payload.bytes;base.crc=request.crc;base.seen_by=seen_by;base.path.push_back({*local,static_cast<std::uint64_t>(publication.created),{}});const auto template_bytes=app::encode_ftn_tic(base);require(template_bytes&&template_bytes->size()<=65535,"ftn_tic_forward_metadata_invalid");result.metadata_template=*template_bytes;
    for(const auto* link:destinations){
        current();app::FtnOutboundJob payload;payload.operation=request.operation;payload.id=operation(request.operation,0,link->code+"/file-payload");payload.ordinal=static_cast<std::uint16_t>(result.targets.size()*2);payload.network=request.network;payload.link=link->code;payload.kind=app::FtnOutboundKind::payload;payload.file={request.filename,request.payload.bytes,static_cast<std::uint64_t>(std::min<std::int64_t>(UINT32_MAX,publication.created))};payload.digest=request.payload.sha256;
        app::FtnTic forward=base;TextErase wipe_password{forward.password};forward.to=*app::parse_ftn_address(link->address);
        if(!link->credentials.tic.empty()){auto secret=unprotect_ftn_secret(installation.root.u8string(),request.network,link->code,FtnCredentialKind::tic,link->credentials.tic);require(secret.has_value(),"ftn_tic_forward_secret_unavailable");forward.password.assign(secret->view());}
        auto encoded=app::encode_ftn_tic(forward);require(encoded&&encoded->size()<=65536,"ftn_tic_forward_metadata_invalid");
        Bytes bytes{std::move(*encoded)};
        app::FtnOutboundJob tic=payload;tic.id=operation(request.operation,0,link->code+"/file-tic");tic.ordinal=static_cast<std::uint16_t>(payload.ordinal+1);tic.kind=app::FtnOutboundKind::tic;tic.file.name=ftn_native_artifact_name(tic.id).substr(0,32)+".tic";tic.file.size=bytes.value.size();tic.digest=digest(bytes.value);
        result.targets.push_back({link->code,app::format_ftn_address(*app::parse_ftn_address(link->address)),tic_credential_generation(link->credentials.tic),digest(app::ftn_forward_peer_identity_material(*link)),std::move(payload),std::move(tic)});
    }
    require(!app::encode_ftn_file_forward_plan(result).empty(),"ftn_tic_forward_plan_invalid");return result;
}
TicForwardSpool prepare_tic_forwarding(app::IFtnMailStorage& storage,const app::FtnConfiguration& config,const ins::Receipt& installation,const app::FtnFilePublication& publication,const app::FtnFileForwardPlan& plan,const FilePayloadLease& source,const std::function<bool()>& running,const std::function<void(std::string_view)>& boundary,const app::Identifier128& attempt){
    TicForwardSpool result;const auto& request=publication.request;require(plan.publication.bytes==request.operation.bytes&&plan.created==publication.created&&plan.network==request.network,"ftn_tic_forward_plan_changed");
    const auto network=std::find_if(config.networks.begin(),config.networks.end(),[&](const auto& n){return same(n.code,request.network);});require(network!=config.networks.end(),"ftn_tic_forward_network_missing");const auto local=app::parse_ftn_address(network->address),original=app::parse_ftn_address(plan.local_address);require(local&&original&&app::same_ftn_address(*local,*original,true),"ftn_tic_forward_local_identity_changed");
    auto parsed=app::decode_ftn_tic(plan.metadata_template,local->domain);require(parsed.tic&&parsed.tic->password.empty()&&!parsed.tic->to,"ftn_tic_forward_template_invalid");
    const auto source_peer=std::find_if(config.links.begin(),config.links.end(),[&](const auto& link){return same(link.network,request.network)&&same(link.code,request.link);});require(source_peer!=config.links.end()&&digest(app::ftn_forward_peer_identity_material(*source_peer))==plan.source_peer_identity,"ftn_tic_forward_source_identity_changed");
    const auto current=[&]{require(running&&running(),"ftn_tic_forward_cancelled");const auto authority=storage.ftn_file_authority(owner,request.operation,attempt);if(!authority&&tic_conclusive_rejection(authority.status.operation))throw std::runtime_error(authority.status.operation);require(authority&&authority.value&&authority.value->configuration_revision==config.revision,"ftn_tic_forward_configuration_changed");};
    // Check every original target before creating output. Never reconstruct the
    // intended set from today's links or a subset of materialized spool jobs.
    current();for(const auto& target:plan.targets){const auto link=std::find_if(config.links.begin(),config.links.end(),[&](const auto& l){return same(l.network,request.network)&&same(l.code,target.link);});require(link!=config.links.end()&&app::ftn_outbound(*link)&&app::ftn_echo_permission(config,request.network,target.link,request.tag,true,false),"ftn_tic_forward_permission_changed");require(tic_credential_generation(link->credentials.tic)==target.credential_generation&&digest(app::ftn_forward_peer_identity_material(*link))==target.peer_identity,"ftn_tic_forward_identity_or_credential_changed");}
    if(plan.targets.empty())return result;const auto directory=FilePayloadDirectory::private_store(installation.root/L"FTN"/L"Outbound"/std::filesystem::u8path(request.network),installation.owner);
    const auto point=[&](std::string_view name){current();if(boundary)boundary(name);};const auto artifact=[&](const app::FtnOutboundJob& job,const std::string_view* generated){result.leases.push_back(materialize_ftn_spool(storage,request.configuration_revision,directory,job,source,generated,current,point,{},attempt));result.jobs.push_back(job);};
    for(const auto& target:plan.targets){current();const auto link=std::find_if(config.links.begin(),config.links.end(),[&](const auto& l){return same(l.network,request.network)&&same(l.code,target.link);});require(link!=config.links.end(),"ftn_tic_forward_peer_missing");auto forward=*parsed.tic;TextErase wipe_password{forward.password};forward.to=*app::parse_ftn_address(target.address);if(!link->credentials.tic.empty()){auto secret=unprotect_ftn_secret(installation.root.u8string(),request.network,link->code,FtnCredentialKind::tic,link->credentials.tic);require(secret.has_value(),"ftn_tic_forward_secret_unavailable");forward.password.assign(secret->view());}auto encoded=app::encode_ftn_tic(forward);require(encoded.has_value(),"ftn_tic_forward_metadata_invalid");Bytes bytes{std::move(*encoded)};require(bytes.value.size()==target.tic.file.size&&digest(bytes.value)==target.tic.digest,"ftn_tic_forward_original_bytes_changed");artifact(target.payload,nullptr);const std::string_view metadata=bytes.value;artifact(target.tic,&metadata);}
    return result;
}
}
