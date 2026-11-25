namespace {
bool tic_conclusive_rejection(std::string_view why){
    if(why=="ftn_tic_continuation_review_required")return true;
    for(const auto* policy:{"ftn_tic_area_not_authorized","ftn_tic_reupload_blocked","ftn_tic_size_policy_denied","ftn_tic_replaces_required"})if(why==policy)return true;
    for(const auto known:{"ftn_tic_invalid","ftn_tic_password_wrong","ftn_tic_same_link_proof_required","ftn_tic_path_loop","ftn_tic_wrong_destination","ftn_tic_not_authorized","ftn_tic_source_unknown","ftn_tic_custody_link_mismatch","ftn_tic_name_or_size_invalid","ftn_tic_mapping_missing","ftn_tic_area_missing","ftn_tic_payload_size_mismatch","ftn_tic_payload_crc_mismatch"})if(why==known)return true;
    return false;
}
struct TicRawErase {std::vector<std::uint8_t>& value;~TicRawErase(){if(!value.empty())sodium_memzero(value.data(),value.size());}};
bool tic_hard_review_condition(std::string_view why){
    for(const auto* hard:{"ftn_tic_message_name_ambiguous","ftn_tic_pair_ambiguous","ftn_tic_pair_limit","ftn_tic_unbound_stage","ftn_tic_unbound_recovery","ftn_tic_stage_missing","ftn_tic_journal_payload_changed","ftn_tic_recovery_object_changed","ftn_tic_displacement_not_verified","ftn_tic_placement_name_mismatch","ftn_tic_forward_manifest_missing","ftn_tic_forward_unbound_object","ftn_tic_forward_object_changed","ftn_tic_forward_bytes_changed","ftn_tic_native_prefix_changed"})if(why==hard)return true;
    return false;
}
FilePayloadLease tic_custody(app::IFtnMailStorage& storage,const ins::Receipt& installation,const app::FtnIngressReceipt& receipt,const std::function<bool()>& running,app::FilePayloadIdentity& identity){
    require(running&&running(),"ftn_tic_cancelled");ins::verify_ftn_directory(installation);require(receipt.id.valid()&&(receipt.network.empty()||app::valid_ftn_code(receipt.network)),"ftn_tic_custody_context_invalid");require(receipt.state==app::FtnIngressState::received||receipt.state==app::FtnIngressState::processing||receipt.state==app::FtnIngressState::processed,"ftn_tic_custody_not_complete");
    std::optional<FilePayloadLease> file;file.emplace(open_ftn_received(storage,installation,receipt,running));identity=file->identity([&](auto,auto){return running();});const auto checkpoint=storage.ftn_ingress_checkpoint(owner,receipt.id);require(checkpoint&&checkpoint.value&&checkpoint.value->has_value(),"ftn_tic_checkpoint_missing");const auto& saved=**checkpoint.value;require(identity.volume==saved.volume&&identity.file_id==saved.file_id&&identity.bytes==saved.bytes&&identity.sha256==saved.sha256&&identity.bytes==receipt.file.size&&identity.sha256==receipt.digest,"ftn_tic_custody_changed");return std::move(*file);
}
}
FtnNativeTicReview inspect_ftn_native_tic(app::IFtnMailStorage& storage,const app::FtnConfiguration& configuration,const ins::Receipt& installation,const app::FtnIngressReceipt& receipt,const std::function<bool()>& running,const app::Identifier128& attempt){
    FtnNativeTicReview out;std::string claimed_from,echo,payload;
    try {
    require(configuration.settings.mode!=app::FtnMailerMode::disabled,"ftn_tic_disabled");
    const auto held=storage.review_ftn_tic(owner,receipt.id);require(held&&held.value,"ftn_tic_review_lookup_failed");
    if(held.value->has_value()){require(attempt.valid(),"ftn_tic_review_required");const auto reviewed=storage.check_ftn_tic_retry(owner,attempt);require(reviewed&&reviewed.value&&reviewed.value->receipt.id.bytes==receipt.id.bytes&&reviewed.value->configuration_revision==configuration.revision,"ftn_tic_review_stale");}else require(!attempt.valid(),"ftn_tic_review_missing");
    auto file=tic_custody(storage,installation,receipt,running,out.identity);require(receipt.file.name.size()>4&&same(std::string_view(receipt.file.name).substr(receipt.file.name.size()-4),".tic")&&receipt.file.size<=65536,"ftn_tic_name_or_size_invalid");Bytes bytes;auto raw=file.read(0,65536);TicRawErase wipe_raw{raw};bytes.value.assign(reinterpret_cast<const char*>(raw.data()),raw.size());auto parsed=app::decode_ftn_tic(bytes.value);require(parsed.tic.has_value(),"ftn_tic_invalid");TextErase wipe_first{parsed.tic->password};claimed_from=app::format_ftn_address(parsed.tic->from);echo=parsed.tic->area;payload=parsed.tic->file;const auto peer=app::resolve_ftn_peer(configuration,{parsed.tic->from},receipt.network);require(peer.has_value(),"ftn_tic_source_unknown");const auto network=std::find_if(configuration.networks.begin(),configuration.networks.end(),[&](const auto& n){return n.enabled&&same(n.code,peer->network);});require(network!=configuration.networks.end(),"ftn_tic_network_unavailable");const auto local=app::parse_ftn_address(network->address);require(local.has_value(),"ftn_tic_network_address_invalid");auto canonical=app::decode_ftn_tic(bytes.value,local->domain);require(canonical.tic.has_value(),"ftn_tic_invalid");TextErase wipe{canonical.tic->password};const auto link=std::find_if(configuration.links.begin(),configuration.links.end(),[&](const auto& l){return same(l.network,peer->network)&&same(l.code,peer->link);});require(link!=configuration.links.end(),"ftn_tic_source_unknown");
    if(!receipt.network.empty())require(same(receipt.network,peer->network)&&same(receipt.link,peer->link),"ftn_tic_custody_link_mismatch");out.authenticated_same_link=!receipt.network.empty()&&app::ftn_verified_receipt(receipt);std::optional<app::RloginSecret> secret;if(!link->credentials.tic.empty()){secret=unprotect_ftn_secret(installation.root.u8string(),peer->network,peer->link,FtnCredentialKind::tic,link->credentials.tic);require(secret.has_value(),"ftn_tic_secret_unavailable");}const auto admission=app::admit_ftn_tic(configuration,*peer,*canonical.tic,secret?secret->view():std::string_view{},out.authenticated_same_link,receipt.proof==app::FtnIngressProof::external_packet&&!receipt.external_session_authenticated);
    switch(admission){case app::FtnTicAdmission::accepted:break;case app::FtnTicAdmission::wrong_password:throw std::runtime_error("ftn_tic_password_wrong");case app::FtnTicAdmission::transport_proof_required:throw std::runtime_error("ftn_tic_same_link_proof_required");case app::FtnTicAdmission::loop:throw std::runtime_error("ftn_tic_path_loop");case app::FtnTicAdmission::wrong_destination:throw std::runtime_error("ftn_tic_wrong_destination");default:throw std::runtime_error("ftn_tic_not_authorized");}
    out.password_verified=secret.has_value();out.source=*peer;sodium_memzero(canonical.tic->password.data(),canonical.tic->password.size());canonical.tic->password.clear();out.tic=std::move(*canonical.tic);out.lease.emplace(std::move(file));require(storage.index_ftn_tic(owner,receipt.id,configuration.revision,out.tic.file,{}).succeeded(),"ftn_tic_dispatch_checkpoint_failed");return out;
    }catch(const std::exception& error){
        const auto why=reason(error);
        // Only conclusive parser/admission failures unlock unrelated message
        // processing. A busy/missing/changed file or DPAPI failure stays pending.
        if(tic_conclusive_rejection(why)&&!out.identity.file_id.empty()){
            require(storage.reject_ftn_tic(owner,{receipt.id,attempt,configuration.revision,out.identity,why,claimed_from,echo,payload}).succeeded(),"ftn_tic_rejection_checkpoint_failed");
            (void)storage.index_ftn_tic(owner,receipt.id,configuration.revision,{},why);
        }
        throw;
    }
}
FtnNativeTicPayload inspect_ftn_native_tic_payload(app::IFtnMailStorage& storage,const ins::Receipt& installation,const app::FtnIngressReceipt& receipt,const app::FtnTic& tic,const std::function<bool()>& running){
    require(same(receipt.file.name,tic.file),"ftn_tic_payload_name_mismatch");app::FilePayloadIdentity identity;auto file=tic_custody(storage,installation,receipt,running,identity);require(!tic.size||*tic.size==identity.bytes,"ftn_tic_payload_size_mismatch");app::FtnCrc32 crc;for(std::uint64_t offset=0;offset<identity.bytes;){require(running(),"ftn_tic_cancelled");const auto bytes=file.read(offset,65536);require(!bytes.empty(),"ftn_tic_payload_read_failed");crc.update({reinterpret_cast<const char*>(bytes.data()),bytes.size()});offset+=bytes.size();}require(!tic.crc||*tic.crc==crc.value(),"ftn_tic_payload_crc_mismatch");return {std::move(file),std::move(identity),crc.value()};
}
std::optional<FtnNativeTicPair> pair_ftn_native_tic_payload(app::IFtnMailStorage& storage,const ins::Receipt& installation,const app::FtnIngressReceipt& receipt,const app::FtnTic& tic,const std::function<bool()>& running){
    require(running&&running(),"ftn_tic_cancelled");const auto found=storage.ftn_payload_candidates(owner,receipt.id,tic.file);require(found&&found.value,"ftn_tic_pair_lookup_failed");const auto& candidates=*found.value;if(candidates.empty())return {};
    require(candidates.size()<17,"ftn_tic_pair_limit");const auto& selected=candidates.front();
    for(const auto& item:candidates)require(item.file.size==selected.file.size&&item.digest==selected.digest,"ftn_tic_pair_ambiguous");
    return FtnNativeTicPair{selected,inspect_ftn_native_tic_payload(storage,installation,selected,tic,running)};
}
