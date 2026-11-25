// Shared host adapter instantiated with each platform's existing private
// custody/spool implementation. Caller owns the BBS-running cancellation gate.
namespace external=platform::external_ftn;
struct ExternalFtnProgress {
    std::map<std::string,std::string> incoming_after;
    app::Identifier128 publication_after;
    std::size_t network{},link{};
    std::chrono::steady_clock::time_point next_inbox_audit{};
};
void external_require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
bool external_same(std::string_view a,std::string_view b){return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),[](char x,char y){return (x>='A'&&x<='Z'?x+32:x)==(y>='A'&&y<='Z'?y+32:y);});}
std::string external_token(const app::Identifier128& id){std::string out;for(auto b:id.bytes){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}return out;}
using ExternalReport=std::function<void(std::string,const std::string&,std::string_view,std::string_view)>;
using ExternalDirectories=std::map<std::string,std::pair<native::FilePayloadDirectory,native::FilePayloadDirectory>>;
std::optional<app::FtnPeerMatch> external_packet_route(const std::filesystem::path& root,const app::FtnConfiguration& config,const app::FtnExternalReceiveRecord& record,const std::function<bool()>& running){
    external_require(app::ftn_message_wire_name(std::filesystem::u8path(record.relative_file).filename().u8string()),"ftn_external_waiting_for_file_route");
    external_require(record.size<=app::kFtnPacketMaximumBytes,"ftn_packet_size_limit");
    struct Bytes {std::string text;~Bytes(){if(!text.empty())sodium_memzero(text.data(),text.size());}} bytes;
    external::read_completed(root,record,[&](auto part){external_require(part.size()<=record.size-bytes.text.size(),"ftn_external_file_changed");bytes.text.append(part);},running);
    const auto routes=native::inspect_ftn_bundle_routes(bytes.text);external_require(!routes.empty(),"ftn_external_packet_route_missing");
    std::optional<app::FtnPeerMatch> result;const bool unknown=app::ftn_external_nonsecure_receive(config,record);
    for(const auto& envelope:routes){
        std::optional<app::FtnPeerMatch> candidate;
        for(const auto& network:config.networks)if(network.enabled&&app::ftn_local_address(network,envelope.second)){
            auto scoped=record;
            const auto remove=[&](auto& list){list.erase(std::remove_if(list.begin(),list.end(),[&](const auto& a){return !app::same_ftn_address(a,envelope.first,true);}),list.end());};
            remove(scoped.remote);remove(scoped.authenticated_remote);
            const auto peer=app::match_ftn_external_receive(config,scoped,network.code);
            if(!peer&&!unknown)continue;
            external_require(!candidate,"ftn_external_file_network_ambiguous");candidate=peer?*peer:app::FtnPeerMatch{network.code,{}};
        }
        external_require(candidate.has_value(),"ftn_external_file_peer_or_destination_unmatched");
        if(unknown)candidate=app::FtnPeerMatch{};
        external_require(!result||(result->network==candidate->network&&result->link==candidate->link),"ftn_external_bundle_mixed_routes_retained");result=candidate;
    }
    return result;
}
std::optional<app::FtnPeerMatch> external_tic_route(const app::FtnConfiguration& config,const app::FtnExternalReceiveRecord& record,const app::FtnTic& tic){
    std::optional<app::FtnPeerMatch> result;
    for(const auto& network:config.networks)if(network.enabled&&(!tic.to||app::ftn_local_address(network,*tic.to))){
        if(std::none_of(config.file_echoes.begin(),config.file_echoes.end(),[&](const auto& echo){return external_same(echo.network,network.code)&&external_same(echo.tag,tic.area);}))continue;
        auto scoped=record;const auto remove=[&](auto& list){list.erase(std::remove_if(list.begin(),list.end(),[&](const auto& address){return !app::same_ftn_address(address,tic.from,true);}),list.end());};remove(scoped.remote);remove(scoped.authenticated_remote);
        const auto peer=app::match_ftn_external_receive(config,scoped,network.code);if(!peer)continue;
        external_require(!result,"ftn_external_tic_network_ambiguous");result=peer;
    }
    return result;
}
struct ExternalTicContents {
    std::optional<app::FtnTic> tic;
    ~ExternalTicContents(){if(tic&&!tic->password.empty())sodium_memzero(tic->password.data(),tic->password.size());}
    ExternalTicContents(const std::filesystem::path& root,const app::FtnExternalReceiveRecord& record,const std::function<bool()>& running){
        external_require(record.size<=65536,"ftn_external_tic_too_large");
        struct Bytes {std::string text;~Bytes(){if(!text.empty())sodium_memzero(text.data(),text.size());}} bytes;
        external::read_completed(root,record,[&](auto part){external_require(part.size()<=65536-bytes.text.size(),"ftn_external_tic_too_large");bytes.text.append(part);},running);
        auto parsed=app::decode_ftn_tic(bytes.text);external_require(parsed.tic.has_value(),"ftn_external_tic_invalid");tic=std::move(parsed.tic);
    }
};
std::optional<app::FtnPeerMatch> external_file_route(const std::filesystem::path& root,const app::FtnConfiguration& config,const app::FtnExternalReceiveRecord& record,const std::function<bool()>& running,const ExternalReport& report){
    const auto name=std::filesystem::u8path(record.relative_file).filename().u8string();
    if(app::ftn_message_wire_name(name))return external_packet_route(root,config,record,running);
    auto extension=std::filesystem::u8path(name).extension().u8string();for(auto& c:extension)if(c>='A'&&c<='Z')c=static_cast<char>(c+32);
    if(extension==".tic"){ExternalTicContents contents(root,record,running);const auto route=external_tic_route(config,record,*contents.tic);external_require(route.has_value(),"ftn_external_tic_peer_or_area_unmatched");return route;}
    const auto candidates=external::tic_records_for_payload(root,name,[&](auto label,auto why){report("External TIC index "+std::string(label),std::string(why),{},{});});external_require(!candidates.empty(),"ftn_external_payload_waiting_for_tic");
    app::FtnCrc32 crc;external::read_completed(root,record,[&](auto bytes){crc.update(bytes);},running);
    std::optional<app::FtnPeerMatch> result;
    for(const auto& candidate:candidates){
        if(!running())break;
        const auto label="External TIC candidate "+external_token(candidate.id);
        try{
        ExternalTicContents contents(root,candidate,running);const auto& tic=*contents.tic;
        external_require(external_same(tic.file,name),"ftn_external_tic_index_filename_conflict");
        if((tic.size&&*tic.size!=record.size)||(tic.crc&&*tic.crc!=crc.value()))continue;
        const auto source=external_tic_route(config,candidate,tic);
        const auto payload=external_tic_route(config,record,tic);
        if(!source||!payload||source->network!=payload->network||source->link!=payload->link)continue;
        external_require(!result||(result->network==payload->network&&result->link==payload->link),"ftn_external_payload_network_ambiguous");result=payload;
        report(label,{},{},{});
        }catch(const std::exception& error){
            const auto why=diagnostic(error);if(why=="ftn_external_tic_network_ambiguous"||why=="ftn_external_payload_network_ambiguous")throw;
            report(label,why,{},{});
        }
    }
    external_require(running()&&result.has_value(),"ftn_external_payload_tic_not_matched");return result;
}
bool receive_external_page(const std::filesystem::path& root,app::IFtnMailStorage& storage,const app::FtnConfiguration& config,
    const ExternalDirectories& directories,const native::FilePayloadDirectory& completed,const native::FilePayloadDirectory& nonsecure,ExternalFtnProgress& progress,
    const std::function<bool()>& running,const ExternalReport& report,bool legacy=false){
    if(config.networks.empty())return false;
    const bool shared=!legacy;const auto index=shared?0:progress.network++%config.networks.size();
    if(shared&&std::chrono::steady_clock::now()>=progress.next_inbox_audit){
        progress.next_inbox_audit=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        try{external::audit_inbox(root,[&](auto label,auto why){report(std::string(label),std::string(why),{},{});});}
        catch(const std::exception& error){report("External inbox audit",diagnostic(error),{},{});}
    }
    if(!shared&&!config.networks[index].enabled)return false;
    const auto network=shared?std::string{}:config.networks[index].code;
    auto& after=progress.incoming_after[network];
    const std::size_t maximum=shared?16:1;
    const auto records=external::pending_records(root,network,maximum,[&](auto why){report("External receive records",std::string(why),network,{});},after);
    bool work{};const app::MessageConfigurationActor owner{true,{}};
    for(const auto& record:records){
        if(!running())break;
        after=external_token(record.id)+".rec";const auto label="External receive "+after;
        try{
            auto match=record.run.empty()?app::match_ftn_external_receive(config,record):external_file_route(root,config,record,running,report);
            const bool unknown=(match&&match->network.empty())||(!match&&app::ftn_external_nonsecure_receive(config,record));
            external_require(match.has_value()||unknown,"ftn_external_peer_missing_or_ambiguous");
            if(unknown)match=app::FtnPeerMatch{};
            const auto directory=directories.find(match->network);external_require(unknown||directory!=directories.end(),"ftn_external_network_directory_missing");
            app::FtnIngressReceipt request;request.id=record.id;request.session=record.id;
            const auto context=std::string("SFX external receive custody v1\n")+*app::encode_ftn_external_receive(record);
            crypto_hash_sha256(request.identity.data(),reinterpret_cast<const unsigned char*>(context.data()),context.size());
            request.network=match->network;request.link=match->link;request.proof=unknown?app::FtnIngressProof::nonsecure:app::FtnIngressProof::external_packet;
            request.external_session_authenticated=record.protected_session;request.configuration_revision=config.revision;
            request.file={std::filesystem::u8path(record.relative_file).filename().u8string(),record.size,record.timestamp};
            const auto prior=storage.ftn_ingress_receipt(owner,record.id);
            if(prior&&prior.value){
                const auto& saved=*prior.value;
                external_require(saved.network==request.network&&saved.link==request.link&&saved.proof==request.proof&&
                    !saved.outbound&&saved.external_session_authenticated==request.external_session_authenticated&&
                    saved.session==request.session&&saved.file.name==request.file.name&&saved.file.size==request.file.size&&saved.file.timestamp==request.file.timestamp,"ftn_external_receipt_context_conflict");
                if(saved.state!=app::FtnIngressState::partial){
                    external_require(saved.received==record.size&&saved.digest==record.digest,"ftn_external_receipt_content_conflict");
                    external::mark_record_consumed(root,record);report(label,{},match->network,match->link);work=true;continue;
                }
                external_require(saved.configuration_revision==config.revision,"ftn_external_partial_configuration_changed");
            }else external_require(prior.status.code==app::StorageErrorCode::not_found,"ftn_external_receipt_lookup_failed");
            auto receive=native::FtnNativeReceive::open(unknown?nonsecure:directory->second.first,storage,request,running,unknown?nullptr:&completed);
            external_require(receive.receipt().id==record.id,"ftn_external_receipt_identity_conflict");
            auto skip=receive.offset();const auto checkpoint=storage.ftn_ingress_checkpoint(owner,record.id);
            external_require(checkpoint&&checkpoint.value&&checkpoint.value->has_value()&&(**checkpoint.value).bytes==skip,"ftn_external_checkpoint_missing");
            crypto_hash_sha256_state prefix{};crypto_hash_sha256_init(&prefix);
            external::read_completed(root,record,[&](std::string_view bytes){
                if(skip){const auto n=static_cast<std::size_t>(std::min<std::uint64_t>(skip,bytes.size()));crypto_hash_sha256_update(&prefix,reinterpret_cast<const unsigned char*>(bytes.data()),n);bytes.remove_prefix(n);skip-=n;
                    if(!skip){app::FtnDigest hash{};crypto_hash_sha256_final(&prefix,hash.data());external_require(hash==(**checkpoint.value).sha256,"ftn_external_resume_prefix_conflict");}}
                if(!bytes.empty())receive.append(bytes);
            },running);
            external_require(!skip,"ftn_external_resume_prefix_incomplete");
            const auto actual=receive.finish();external_require(actual.sha256==record.digest,"ftn_external_custody_hash_conflict");
            external::mark_record_consumed(root,record);report(label,{},match->network,match->link);work=true;
        }catch(const std::exception& error){report(label,diagnostic(error),network,{});}
    }
    if(records.size()<maximum)after.clear();
    return work;
}
bool publish_external_page(const std::filesystem::path& root,app::IFtnMailStorage& storage,const app::FtnConfiguration& config,
    const ExternalDirectories& directories,const app::Identifier128& generation,ExternalFtnProgress& progress,
    const std::function<bool()>& running,const ExternalReport& report){
    const app::MessageConfigurationActor owner{true,{}};bool work{};
    const external::ReadOutbound source=[&](const auto& job,const auto& consume){
        const auto found=directories.find(job.network);external_require(found!=directories.end(),"ftn_external_outbound_directory_missing");
        auto file=found->second.second.read(native::ftn_existing_spool_name(found->second.second,job));
        const auto actual=file.identity([&](auto,auto){return running();});external_require(actual.bytes==job.file.size&&actual.sha256==job.digest,"ftn_external_original_changed");
        std::uint64_t offset{};while(offset<job.file.size){external_require(running(),"ftn_external_cancelled");const auto bytes=file.read(offset,static_cast<std::size_t>(std::min<std::uint64_t>(16384,job.file.size-offset)));external_require(!bytes.empty(),"ftn_external_original_read_failed");consume({reinterpret_cast<const char*>(bytes.data()),bytes.size()});offset+=bytes.size();}
        const auto after=file.identity([&](auto,auto){return running();});external_require(after.bytes==actual.bytes&&after.sha256==actual.sha256,"ftn_external_original_changed");
    };
    const auto publish=[&](const auto& batch){
        const auto& job=batch.front().job;const auto label="External handoff "+external_token(job.claim);
        try{const auto result=external::publish_handoff(root,storage,batch,source,running);work=work||result.published;
            report(label,result.uncertain?"ftn_external_publication_uncertain":result.busy?"ftn_external_waiting_for_binkd_busy_file":"",job.network,job.link);
        }catch(const std::exception& error){report(label,diagnostic(error),job.network,job.link);}
    };
    const auto pending=storage.pending_ftn_external_handoff(owner,progress.publication_after);
    external_require(pending&&pending.value,"ftn_external_pending_handoff_unavailable");
    if(pending.value->empty())progress.publication_after={};
    else{progress.publication_after=pending.value->front().job.claim;publish(*pending.value);}
    if(!running()||config.links.empty())return work;
    const auto& link=config.links[progress.link++%config.links.size()];if(!enabled(config,link))return work;
    app::Identifier128 claim;randombytes_buf(claim.bytes.data(),claim.bytes.size());external_require(claim.valid(),"ftn_external_claim_random_failed");
    const auto jobs=storage.claim_ftn_jobs(owner,link.network,link.code,generation,claim,64,true);
    if(!jobs){if(jobs.status.code==app::StorageErrorCode::backpressure)return work;external_require(false,"ftn_external_queue_claim_failed");}
    if(jobs.value->empty())return work;
    const auto prepared=storage.prepare_ftn_external_handoff(owner,config.revision,claim);
    external_require(prepared&&prepared.value&&!prepared.value->empty(),"ftn_external_handoff_prepare_failed");publish(*prepared.value);return work;
}
