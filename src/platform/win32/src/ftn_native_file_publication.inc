namespace {
bool tic_same_payload(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b){return a.bytes==b.bytes&&a.sha256==b.sha256&&a.volume==b.volume&&a.file_id==b.file_id;}
bool tic_same_object(const app::FileObjectIdentity& a,const app::FileObjectIdentity& b){return a.volume==b.volume&&a.file_id==b.file_id;}
app::FilePayloadIdentity tic_object_record(const app::FileObjectIdentity& object){return {0,{},object.volume,object.file_id};}
app::FileDescriptions tic_descriptions(const app::FtnTic& tic){
    std::string short_text,long_text;for(const auto& row:tic.descriptions){if(!short_text.empty())short_text+=' ';short_text+=row;}for(const auto& row:tic.long_descriptions){if(!long_text.empty())long_text+='\n';long_text+=row;}
    auto short_value=app::sanitize_file_text(short_text,app::kFileShortDescriptionBytes,false);if(short_value.find_first_not_of(' ')==short_value.npos)short_value=app::sanitize_file_text(tic.file,app::kFileShortDescriptionBytes,false);
    return {std::move(short_value),app::sanitize_file_text(long_text,app::kFileLongDescriptionBytes,true)};
}
void tic_finish_custody(app::IFtnMailStorage& storage,const ins::Receipt& installation,const app::Identifier128& id,const std::string& network,const app::FilePayloadIdentity& expected,const std::function<bool()>& running){
    require(running(),"ftn_tic_cancelled");const auto base=installation.root/L"FTN";
    auto processed=FilePayloadDirectory::private_store(base/L"Processed"/std::filesystem::u8path(network),installation.owner);
    const auto receipt=storage.ftn_ingress_receipt(owner,id);require(receipt&&receipt.value,"ftn_tic_original_receipt_missing");
    const auto name=processed.absent(receipt.value->file.name)?receipt.value->file.name:ftn_bad_original_name(*receipt.value);
    std::optional<FilePayloadLease> file;file.emplace(open_ftn_received(storage,installation,*receipt.value,running,true,network));
    require(tic_same_payload(file->identity([&](auto,auto){return running();}),expected),"ftn_tic_original_changed");if(file->path().parent_path()!=processed.path())file->rename_to(processed,name);
    require(storage.advance_ftn_ingress(owner,id,expected.bytes,expected.bytes,expected.sha256,app::FtnIngressState::processed,{}).succeeded(),"ftn_tic_original_checkpoint_failed");
}
}
#include "ftn_native_file_forward.inc"
#include "ftn_native_tic_continuation.inc"
FtnNativeTossResult publish_ftn_native_tic(app::IFtnMailStorage& storage,app::IFileStorage& files,const app::FtnConfiguration& configuration,const ins::Receipt& installation,const app::FtnIngressReceipt& receipt,const std::function<bool()>& running,const std::function<void(std::string_view)>& boundary,bool force_displacement_copy,const app::Identifier128& attempt){
    using Phase=app::FtnFilePhase;FtnNativeTossResult result;const auto id=operation(receipt.id,0,"file-publication");bool journaled{};std::optional<app::FilePayloadIdentity> observed_tic;std::string observed_from,observed_echo,observed_payload;
    const auto point=[&](std::string_view name){require(running&&running(),"ftn_tic_cancelled");if(boundary)boundary(name);};
    try{
        ins::verify_ftn_directory(installation);point("begin");require(configuration.settings.mode!=app::FtnMailerMode::disabled,"ftn_tic_disabled");
        std::optional<app::FtnTicReview> exact_review;if(attempt.valid()){const auto checked=storage.check_ftn_tic_retry(owner,attempt);require(checked&&checked.value&&checked.value->receipt.id.bytes==receipt.id.bytes,"ftn_tic_review_stale");exact_review=*checked.value;}
        auto prior=storage.ftn_file_publication(owner,id);std::optional<app::FtnFilePublication> publication;
        if(prior&&prior.value){publication=*prior.value;journaled=true;}else require(prior.status.code==app::StorageErrorCode::not_found,"ftn_tic_journal_lookup_failed");
        if(publication&&publication->phase==Phase::completed){const auto& r=publication->request;tic_finish_custody(storage,installation,r.payload_receipt,r.network,r.payload,running);tic_finish_custody(storage,installation,r.tic_receipt,r.network,r.tic,running);result.processed=true;return result;}
        auto review=inspect_ftn_native_tic(storage,configuration,installation,receipt,running,attempt);
        observed_tic=review.identity;observed_from=app::format_ftn_address(review.tic.from);observed_echo=review.tic.area;observed_payload=review.tic.file;
        require(!app::ftn_message_wire_name(review.tic.file),"ftn_tic_message_name_ambiguous");
        std::optional<FtnNativeTicPair> pair;
        if(publication){const auto original=storage.ftn_ingress_receipt(owner,publication->request.payload_receipt);require(original&&original.value,"ftn_tic_original_receipt_missing");pair.emplace(FtnNativeTicPair{*original.value,inspect_ftn_native_tic_payload(storage,installation,*original.value,review.tic,running)});}
        else pair=pair_ftn_native_tic_payload(storage,installation,receipt,review.tic,running);
        if(!pair){result.deferred=true;result.reason="ftn_tic_payload_pending";return result;}
        const auto config=files.file_configuration(owner);require(config&&config.value,"ftn_tic_file_catalog_unavailable");
        const auto mapping=std::find_if(configuration.file_echoes.begin(),configuration.file_echoes.end(),[&](const auto& e){return same(e.network,review.source.network)&&same(e.tag,review.tic.area);});require(mapping!=configuration.file_echoes.end(),"ftn_tic_mapping_missing");
        const auto area=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& a){return a.area.id.value==mapping->file_area;});require(area!=config.value->areas.end(),"ftn_tic_area_missing");
        if(publication&&!attempt.valid())require(publication->request.configuration_revision==configuration.revision&&publication->request.area_revision==area->area.revision,"ftn_tic_continuation_review_required");
        app::FtnFileRequest request;request.operation=id;request.tic_receipt=receipt.id;request.payload_receipt=pair->receipt.id;request.configuration_revision=configuration.revision;request.area_revision=area->area.revision;request.network=review.source.network;request.link=review.source.link;request.tag=review.tic.area;request.filename=review.tic.file;request.replaces=review.tic.replaces;request.origin=app::format_ftn_address(review.tic.origin);request.from=app::format_ftn_address(review.tic.from);request.descriptions=tic_descriptions(review.tic);request.tic=review.identity;request.payload=pair->payload.identity;request.crc=pair->payload.crc;request.password_verified=review.password_verified;request.authenticated_same_link=review.authenticated_same_link;
        std::optional<app::FtnFileForwardPlan> forward_plan;
        if(publication){const auto retained=storage.ftn_file_forward_plan(owner,id);require(retained&&retained.value,"ftn_tic_forward_manifest_missing");forward_plan=*retained.value;}
        else {app::FtnFilePublication candidate;candidate.request=request;candidate.created=std::time(nullptr);require(candidate.created>0,"ftn_tic_forward_clock_unavailable");forward_plan=plan_tic_forwarding(configuration,installation,review.tic,candidate,running);}
        std::vector<FilePayloadLease> reviewed_pins;if(attempt.valid()&&publication){require(exact_review.has_value(),"ftn_tic_review_missing");reviewed_pins=pin_tic_continuation(configuration,*config.value,installation,*publication,*forward_plan,*exact_review,pair->payload.lease,running);point("review_native_pinned");}
        const auto prepared=storage.prepare_ftn_file(owner,request,forward_plan,attempt);if(!prepared&&tic_conclusive_rejection(prepared.status.operation))throw std::runtime_error(prepared.status.operation);require(prepared&&prepared.value,"ftn_tic_prepare_refused");publication=*prepared.value;journaled=true;auto& p=*publication;if(attempt.valid())point("review_authority_bound");reviewed_pins.clear();
        auto forwarding=prepare_tic_forwarding(storage,configuration,installation,p,*forward_plan,pair->payload.lease,running,boundary,attempt);
        point("reserved");std::vector<std::filesystem::path> others;for(const auto& a:config.value->areas)if(!(a.area.id==p.area.area.id))others.push_back(std::filesystem::u8path(a.storage_path));
        auto destination=FilePayloadDirectory::area(installation.root,std::filesystem::u8path(p.area.storage_path),others,{L"Bin",L"Config",L"Data",L"Display",L"Backup",L"Logs",L"Work",L"Doors",L"BotGate",L"FTN"},installation.owner);
        auto removed=FilePayloadDirectory::private_store(installation.root/L"Data"/L"FileRemoved",installation.owner);
        const auto current=[&]{point("before_mutation");const auto authority=storage.ftn_file_authority(owner,id,attempt);if(!authority&&tic_conclusive_rejection(authority.status.operation))throw std::runtime_error(authority.status.operation);require(authority&&authority.value&&authority.value->configuration_revision==request.configuration_revision&&authority.value->area_revision==request.area_revision,"ftn_tic_configuration_changed");};
        const auto advance=[&](Phase phase,const app::FilePayloadIdentity& native){const auto next=storage.advance_ftn_file(owner,id,phase,native,attempt);require(next&&next.value,"ftn_tic_journal_checkpoint_failed");p=*next.value;};
        const auto verify=[&](FilePayloadLease& lease,const app::FilePayloadIdentity& expected){require(tic_same_payload(lease.identity([&](auto,auto){return running();}),expected),"ftn_tic_journal_payload_changed");};
        const auto copy=[&](FilePayloadLease& target,const FilePayloadLease& source,const app::FilePayloadIdentity& expected,std::string_view point_name){
            auto offset=target.byte_count();require(offset<=expected.bytes,"ftn_tic_copy_size_changed");while(offset<expected.bytes){current();const auto bytes=source.read(offset,65536);require(!bytes.empty(),"ftn_tic_copy_read_failed");target.append(bytes.data(),bytes.size());offset+=bytes.size();point(point_name);}target.flush();const auto actual=target.identity([&](auto,auto){return running();});require(actual.bytes==expected.bytes&&actual.sha256==expected.sha256,"ftn_tic_copy_integrity_failed");return actual;
        };
        std::optional<FilePayloadLease> staged,recovery;
        if(p.phase==Phase::reserved){current();require(destination.absent(p.stage_name),"ftn_tic_unbound_stage");staged.emplace(destination.create(p.stage_name,std::max<std::uint64_t>(1,request.payload.bytes)));point("stage_created_before_binding");advance(Phase::stage_bound,tic_object_record(staged->object_identity()));point("stage_bound");}
        if(p.phase==Phase::stage_bound){current();require(p.stage_object.has_value(),"ftn_tic_stage_binding_missing");if(!staged)staged.emplace(destination.resume_owned_copy(p.stage_name,*p.stage_object,pair->payload.lease,[&](auto,auto){return running();}));const auto actual=copy(*staged,pair->payload.lease,request.payload,"stage_copy_chunk");point("stage_verified_before_checkpoint");advance(Phase::staged,actual);point("stage_verified");}
        if(!staged){
            const bool stage_present=!destination.absent(p.stage_name);require(p.phase!=Phase::placed||!stage_present,"ftn_tic_placement_name_mismatch");const auto name=stage_present?p.stage_name:request.filename;
            require(stage_present||p.phase==Phase::placed||p.phase==Phase::displaced||(!p.displaced&&p.phase==Phase::staged),"ftn_tic_stage_missing");staged.emplace(destination.read(name,true));require(p.staged.has_value(),"ftn_tic_staged_checkpoint_missing");verify(*staged,*p.staged);
        }
        if(p.displaced&&p.phase<Phase::displaced){
            current();std::optional<FilePayloadLease> original;
            if(!destination.absent(request.filename)){original.emplace(destination.read(request.filename,true));verify(*original,p.displaced->payload);}
            if(p.phase==Phase::staged){require(original.has_value(),"ftn_tic_original_target_missing");require(removed.absent(p.recovery_key),"ftn_tic_unbound_recovery");
                if(!force_displacement_copy&&destination.volume()==removed.volume())advance(Phase::recovery_bound,tic_object_record(original->object_identity()));
                else{recovery.emplace(removed.create(p.recovery_key,std::max<std::uint64_t>(1,p.displaced->payload.bytes)));point("recovery_created_before_binding");advance(Phase::recovery_bound,tic_object_record(recovery->object_identity()));}point("recovery_bound");
            }
            require(p.recovery_object.has_value(),"ftn_tic_recovery_binding_missing");
            if(original){current();
                if(tic_same_object(original->object_identity(),*p.recovery_object)){require(removed.absent(p.recovery_key),"ftn_tic_recovery_name_occupied");original->rename_to(removed,p.recovery_key);recovery.emplace(std::move(*original));original.reset();point("original_displaced_before_checkpoint");}
                else{if(!recovery)recovery.emplace(removed.resume_owned_copy(p.recovery_key,*p.recovery_object,*original,[&](auto,auto){return running();}));copy(*recovery,*original,p.displaced->payload,"recovery_copy_chunk");point("recovery_verified_before_remove");current();original->remove_exact();original.reset();point("original_displaced_before_checkpoint");}
            }else if(!recovery)recovery.emplace(removed.read(p.recovery_key,true));
            require(recovery&&tic_same_object(recovery->object_identity(),*p.recovery_object),"ftn_tic_recovery_object_changed");const auto actual=recovery->identity([&](auto,auto){return running();});require(actual.bytes==p.displaced->payload.bytes&&actual.sha256==p.displaced->payload.sha256&&destination.absent(request.filename),"ftn_tic_displacement_not_verified");advance(Phase::displaced,actual);point("displacement_verified");
        }
        if(p.displaced&&!recovery){require(p.recovered.has_value(),"ftn_tic_recovery_checkpoint_missing");recovery.emplace(removed.read(p.recovery_key));verify(*recovery,*p.recovered);}
        if(p.phase!=Phase::placed){current();
            if(staged->name()==p.stage_name){require(destination.absent(request.filename),"ftn_tic_destination_occupied");staged->rename_to(destination,request.filename);point("stage_renamed_before_checkpoint");}
            require(staged->name()==request.filename&&p.staged.has_value(),"ftn_tic_placement_name_mismatch");verify(*staged,*p.staged);advance(Phase::placed,*p.staged);point("placement_verified");
        }
        current();point("before_catalog_commit");const auto committed=storage.commit_ftn_file(owner,id,forwarding.jobs,attempt);require(committed&&committed.value&&committed.value->phase==Phase::completed,"ftn_tic_catalog_commit_failed");point("after_catalog_commit");
        // Leases protect bytes through the complete catalog/provenance transaction.
        staged.reset();recovery.reset();pair.reset();review.lease.reset();tic_finish_custody(storage,installation,request.payload_receipt,request.network,request.payload,running);tic_finish_custody(storage,installation,request.tic_receipt,request.network,request.tic,running);result.processed=true;
    }catch(const std::exception& error){result.reason=reason(error);if(!journaled && result.reason=="ftn_inbound_file_missing_or_changed" && running && running()){result.held=storage.advance_ftn_ingress(owner,receipt.id,receipt.received,receipt.received,receipt.digest,app::FtnIngressState::held,result.reason).succeeded();result.deferred=!result.held;return result;}if(std::string_view(error.what())=="file_copy_resume_prefix_changed")result.reason="ftn_tic_native_prefix_changed";result.deferred=true;if(observed_tic&&(tic_conclusive_rejection(result.reason)||tic_hard_review_condition(result.reason))&&!storage.reject_ftn_tic(owner,{receipt.id,attempt,configuration.revision,*observed_tic,result.reason,observed_from,observed_echo,observed_payload}))result.reason="ftn_tic_rejection_checkpoint_failed";if(journaled){const auto saved=storage.ftn_file_publication(owner,id);if(!saved||!saved.value||(saved.value->phase!=Phase::completed&&!storage.hold_ftn_file(owner,id,result.reason)))result.reason="ftn_tic_hold_checkpoint_failed";}}
    return result;
}
FtnNativeTossResult reprocess_ftn_native_tic(app::IFtnMailStorage& storage,app::IFileStorage& files,const app::FtnConfiguration& configuration,const ins::Receipt& installation,const app::FtnTicRetryRecord& retry,const std::function<bool()>& running,const std::function<void(std::string_view)>& boundary){
    FtnNativeTossResult result;result.deferred=true;
    if(!running||!running()){result.reason="ftn_tic_cancelled";return result;}
    const auto latest=storage.review_ftn_tic(owner,retry.request.ingress);
    if(!latest||!latest.value||!latest.value->has_value()||!(**latest.value).retry||(**latest.value).retry->request.operation.bytes!=retry.request.operation.bytes){result.reason="ftn_tic_review_lookup_failed";return result;}
    const auto& saved=*(**latest.value).retry;
    if(saved.finished){result.processed=saved.processed;result.deferred=false;result.completion_reconciled=true;result.reason=saved.reason;return result;}
    const auto checked=storage.check_ftn_tic_retry(owner,retry.request.operation);
    if(!checked||!checked.value){result.reason="ftn_tic_review_stale";if(checked.status.code==app::StorageErrorCode::conflict){if(storage.finish_ftn_tic_retry(owner,retry.request.operation,false,result.reason))result.held=true;else result.reason="ftn_tic_retry_result_pending";}return result;}
    if(configuration.revision!=checked.value->configuration_revision){result.reason="ftn_tic_worker_reload_pending";result.configuration_changed=true;return result;}
    const bool completed_before=checked.value->receipt.state==app::FtnIngressState::processed;
    result=publish_ftn_native_tic(storage,files,configuration,installation,checked.value->receipt,running,boundary,false,retry.request.operation);
    if(result.processed){result.completion_reconciled=completed_before;if(!storage.finish_ftn_tic_retry(owner,retry.request.operation,true,{})){result.processed=false;result.deferred=true;result.reason="ftn_tic_retry_result_pending";}return result;}
    bool final=tic_conclusive_rejection(result.reason)||tic_hard_review_condition(result.reason);
    for(const auto* hard:{"ftn_tic_review_native_changed","ftn_tic_review_prefix_changed","ftn_tic_review_frontier_changed","ftn_tic_review_destination_changed","ftn_tic_review_placement_changed","ftn_tic_review_stage_missing","ftn_tic_review_unbound_stage","ftn_tic_review_recovery_changed","ftn_tic_review_recovery_incomplete","ftn_tic_review_recovery_missing","ftn_tic_review_unbound_recovery","ftn_tic_review_displacement_changed","ftn_tic_review_destination_occupied","ftn_tic_forward_manifest_missing","ftn_tic_forward_local_identity_changed","ftn_tic_forward_source_identity_changed","ftn_tic_forward_permission_changed","ftn_tic_forward_identity_or_credential_changed","ftn_tic_forward_original_bytes_changed","ftn_tic_forward_unbound_object","ftn_tic_forward_unplanned_object"})if(result.reason==hard)final=true;
    const auto still_current=storage.check_ftn_tic_retry(owner,retry.request.operation);if(!still_current&&still_current.status.code==app::StorageErrorCode::conflict)final=true;
    if(final){if(storage.finish_ftn_tic_retry(owner,retry.request.operation,false,result.reason))result.held=true;else result.reason="ftn_tic_retry_result_pending";}
    // Busy files/SQLite, unavailable DPAPI, cooperative stop, and unknown local
    // failures keep the same pending request for bounded sole-worker recovery.
    return result;
}
