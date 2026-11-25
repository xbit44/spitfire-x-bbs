// Adapted from SFX's own Windows intake transaction; native Linux custody.
#include "sfx/platform/linux/file_intake.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/file_manager.hpp"
#include "sfx/archive/file_zip.hpp"
#include <stdexcept>
#include <algorithm>

namespace sfx::platform::linux_host {
namespace app=application;
namespace { void require(bool value,const char* why){if(!value)throw std::runtime_error(why);} }
LinuxFileIntake::LinuxFileIntake(app::IFileStorage& storage,app::FileConfigurationActor owner,
    app::FileActor actor,app::FileIntakeStart start,FilePayloadDirectory intake,Boundary boundary):
    storage_(storage),owner_(std::move(owner)),actor_(actor),staging_(std::move(intake)),boundary_(std::move(boundary)) {
    // Prove recovery authority before creating a journal or payload.
    require(storage_.file_configuration(owner_).succeeded(),"file_intake_host_authority_required");
    const auto begun=storage_.begin_file_intake(actor_,start);
    require(begun.succeeded()&&begun.value.has_value(),"file_intake_start_refused");record_=*begun.value;
    require(record_.state==app::FileIntakeState::receiving,"file_intake_already_received");
    try{
        payload_.emplace(staging_.create(record_.stage_key,record_.maximum_bytes));
        const auto object=payload_->object_identity();
        require(storage_.bind_file_intake_object(actor_,record_.operation,false,object).succeeded(),"file_intake_staging_checkpoint_failed");
        record_.staged_object=object;this->boundary(FileIntakeBoundary::staging_bound);
    }
    catch(...){hold("Receive storage unavailable; held for Sysop review.");throw;}
}
void LinuxFileIntake::boundary(FileIntakeBoundary at){if(boundary_)boundary_(at);}
NativeFileIntakeOutcome LinuxFileIntake::hold(std::string_view detail){
    outcome_.state=app::FileIntakeState::held;outcome_.detail=std::string(detail);
    const auto resolved=storage_.resolve_file_intake(owner_,record_.operation,record_.payload,false,detail);
    outcome_.journal_confirmed=resolved.succeeded();if(resolved)record_.state=app::FileIntakeState::held;
    // A failed DB write retains the earlier durable intent and exact owned file.
    // Recovery never guesses acceptance/deletion from this in-memory outcome.
    return outcome_;
}
void LinuxFileIntake::append(const std::uint8_t* data,std::size_t count){
    require(payload_.has_value()&&!finished_&&!receive_failed_,"file_intake_receive_closed");
    try{payload_->append(data,count);}catch(...){receive_failed_=true;throw;}
}
FileDescriptor LinuxFileIntake::begin_receive_transfer(){
    require(payload_.has_value()&&!finished_&&!receive_failed_,"file_intake_receive_closed");
    try{return payload_->begin_receive_transfer();}catch(...){receive_failed_=true;throw;}
}
void LinuxFileIntake::finish_receive_transfer_after_reap(){
    require(payload_.has_value()&&!finished_,"file_intake_receive_closed");
    try{payload_->finish_receive_transfer_after_reap();}catch(...){receive_failed_=true;throw;}
}
NativeFileIntakeOutcome LinuxFileIntake::finish(std::string_view filename,bool complete,const Scan& scan){
    if(finished_)return outcome_;
    finished_=true;
    if(!payload_||!app::valid_file_name(filename))return hold("Invalid upload filename; held for Sysop review.");
    app::FileIntakeReceived received;received.operation=record_.operation;received.filename=std::string(filename);
    try{received.payload=payload_->identity();}catch(...){return hold("Upload identity could not be verified; held for Sysop review.");}
    received.transfer_complete=complete&&!receive_failed_;
    if(!received.transfer_complete)received.scan={app::FileScanVerdict::incomplete,{},"Transfer did not complete."};
    else if(record_.scan_required){
        try{received.scan=scan?scan(*payload_,record_.maximum_bytes):app::FileScanResult{app::FileScanVerdict::error,{},"Scanner unavailable."};}
        catch(...){received.scan={app::FileScanVerdict::error,{},"Scanner could not complete."};}
    }else received.scan={app::FileScanVerdict::not_scanned,{},"Scanning is Off."};
    outcome_.scan=received.scan;
    // Inspection has no filesystem extraction, execution, terminal rendering or
    // source rewriting. This is an OPTIONAL preview/DIZ parser, not the malware
    // scanner. Its narrower format support cannot overwrite an actual clean
    // scanner verdict. Scanner encrypted/limit/incomplete/error guards remain.
    // Without a real clean scan, retain the existing archive safety gate.
    if(received.transfer_complete&&(received.scan.verdict==app::FileScanVerdict::clean||received.scan.verdict==app::FileScanVerdict::not_scanned)){
        try{
            auto lower=received.filename;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(c>='A'&&c<='Z'?c-'A'+'a':c);});
            const auto prefix=payload_->read(0,4);
            if((lower.size()>=4&&lower.substr(lower.size()-4)==".zip")||(prefix.size()==4&&prefix[0]=='P'&&prefix[1]=='K')){
                const archive::FileZipSource source{received.payload.bytes,[this](std::uint64_t at,std::size_t n){return payload_->read(at,n);},{}};
                const auto inspected=archive::inspect_file_zip(source);
                if(!inspected.result.ok()||inspected.encrypted){if(received.scan.verdict!=app::FileScanVerdict::clean)received.scan={app::FileScanVerdict::incomplete,{},"ZIP inspection could not verify this format while malware scanning is Off."};}
                else {const auto diz=archive::read_file_diz(source);if(diz.result.ok())outcome_.diz=diz.descriptions;}
            }
        }catch(...){if(received.scan.verdict!=app::FileScanVerdict::clean)received.scan={app::FileScanVerdict::incomplete,{},"ZIP inspection unavailable while malware scanning is Off."};}
    }
    outcome_.scan=received.scan;
    const auto saved=storage_.record_file_intake(actor_,received);
    if(!saved||!saved.value)return hold("Upload result could not be committed; held for Sysop review.");
    record_=*saved.value;outcome_.state=record_.state;outcome_.journal_confirmed=true;
    if(app::incoming_file_action(record_.scan_required,record_.scan)==app::IncomingFileAction::delete_detected_staging&&
        record_.state==app::FileIntakeState::held&&!record_.scan.detections.empty()){
        try{boundary(FileIntakeBoundary::before_delete);payload_->remove_exact();payload_.reset();outcome_.deletion_confirmed=true;}
        catch(...){return hold("Detected upload could not be deleted; held for Sysop review.");}
        const auto deleted=storage_.resolve_file_intake(owner_,record_.operation,record_.payload,true,"Positive detection; exact newly staged payload deletion verified.");
        outcome_.journal_confirmed=deleted.succeeded();
        // Actual deletion and durable metadata completion are separate facts.
        if(deleted){record_.state=app::FileIntakeState::deleted;outcome_.state=record_.state;outcome_.detail="Detected upload deleted.";}
        else{outcome_.state=app::FileIntakeState::held;outcome_.detail="Payload deleted; journal reconciliation required.";}
    }else if(record_.state==app::FileIntakeState::ready)outcome_.detail="Ready for addition.";
    else if(record_.scan.verdict==app::FileScanVerdict::clean)outcome_.detail="Scan clean; file not added: upload policy, size or content validation failed. Private copy retained.";
    else outcome_.detail="File not added: "+app::sanitize_file_text(record_.scan.detail,240,false)+" Private copy retained.";
    return outcome_;
}
NativeFileIntakeOutcome LinuxFileIntake::accept(const std::filesystem::path& installation,
    const FilePayloadDirectory& destination,const app::FileDescriptions& descriptions,bool sysop_only){
    if(outcome_.accepted)return outcome_;
    if(!finished_||record_.state!=app::FileIntakeState::ready||!payload_)return hold("Upload is not ready for acceptance.");
    const auto config=storage_.file_configuration(owner_);
    if(!config||!config.value)return hold("Upload destination unavailable; held for Sysop review.");
    const auto found=std::find_if(config.value->areas.begin(),config.value->areas.end(),[this](const auto& a){return a.area.id==record_.placement_area;});
    if(found==config.value->areas.end()||found->area.revision!=record_.placement_revision)return hold("Upload destination changed; held for Sysop review.");
    const auto configured=std::filesystem::u8path(found->storage_path);
    if(staging_.path()!=installation/"data/fileintake")return hold("Upload staging context changed; held privately.");
    const auto expected=staging_.resolve_path(configured);
    if(expected!=destination.path())return hold("Upload destination identity mismatch; held for Sysop review.");
    const auto prepared=storage_.prepare_file_intake(actor_,record_.operation,descriptions,sysop_only);
    if(!prepared||!prepared.value)return hold(record_.private_submission?"Upload requires Sysop review; held privately.":"Upload metadata or filename unavailable; held for Sysop review.");
    record_=*prepared.value;std::optional<FilePayloadLease> copied;app::FilePayloadIdentity placed;
    try{
        boundary(FileIntakeBoundary::before_placement);
        // A same-volume, same-binding move is atomic. Other placement makes a NEW
        // independent copy, leaving the durable original until DB acceptance.
        // No failure removes/overwrites any destination or guesses a commit.
        if(payload_->can_rename_to(destination)){
            const auto object=payload_->object_identity();
            require(storage_.bind_file_intake_object(actor_,record_.operation,true,object).succeeded(),"file_intake_destination_checkpoint_failed");
            record_.placed_object=object;boundary(FileIntakeBoundary::destination_bound);
            payload_->rename_to(destination,app::file_intake_payload_name(record_));placed=payload_->identity();
        }
        else{
            copied.emplace(destination.create(app::file_intake_payload_name(record_),record_.maximum_bytes));
            const auto object=copied->object_identity();
            require(storage_.bind_file_intake_object(actor_,record_.operation,true,object).succeeded(),"file_intake_destination_checkpoint_failed");
            record_.placed_object=object;boundary(FileIntakeBoundary::destination_bound);
            for(std::uint64_t at=0;at<record_.payload.bytes;){const auto bytes=payload_->read(at);require(!bytes.empty(),"file_copy_short_read");copied->append(bytes.data(),bytes.size());at+=bytes.size();}
            placed=copied->identity();
        }
        require(placed.bytes==record_.payload.bytes&&placed.sha256==record_.payload.sha256,"file_placement_content_mismatch");
        boundary(FileIntakeBoundary::after_placement);boundary(FileIntakeBoundary::before_commit);
    }catch(...){return hold("Upload placement incomplete; reconcile private staging and intended destination.");}
    const auto committed=storage_.commit_file_intake(actor_,record_.operation,placed);
    if(!committed||!committed.value)return hold("Upload acceptance unconfirmed; reconcile staging and intended destination.");
    outcome_.accepted=*committed.value;outcome_.state=app::FileIntakeState::accepted;outcome_.journal_confirmed=true;
    record_.state=outcome_.state;record_.accepted_file=committed.value->id;outcome_.detail="Upload added.";
    try{
        boundary(FileIntakeBoundary::before_staging_cleanup);
        if(copied){
        // This is only the redundant original of a committed exact copy, not a
        // library purge or rejection deletion. On failure preserve it for audit.
            payload_->remove_exact();payload_.reset();
        }
        require(staging_.absent(record_.stage_key)&&record_.staged_object&&storage_.finish_file_intake_cleanup(owner_,record_.operation,*record_.staged_object).succeeded(),"file_intake_staging_cleanup_unconfirmed");
        record_.staging_clean=true;
    }catch(...){outcome_.detail="Upload added; staging reconciliation remains pending.";}
    return outcome_;
}
bool cleanup_accepted_file_intake(app::IFileStorage& storage,const app::FileConfigurationActor& owner,
    app::Identifier128 operation,const FilePayloadDirectory& staging,const std::filesystem::path& installation,
    std::shared_ptr<IFilePayloadBindings> bindings,const std::function<bool(std::uint64_t,std::uint64_t)>& progress){
    try{
        const auto root=installation;require(root.is_absolute()&&root==root.lexically_normal(),"file_intake_cleanup_root_invalid");const auto expected=root/"data"/"fileintake";
        require(staging.path()==expected,"file_intake_cleanup_context_mismatch");
        const auto result=storage.file_intake(owner,operation);require(result&&result.value,"file_intake_recovery_unavailable");const auto& r=*result.value;
        require(r.state==app::FileIntakeState::accepted&&r.staged_object&&r.placed_object,"file_intake_not_accepted");
        if(r.staging_clean)return true;
        std::optional<FilePayloadLease> final;
        {
            // The rename case cannot leave its original object at both names.
            const bool copied=r.staged_object->volume!=r.placed_object->volume||r.staged_object->file_id!=r.placed_object->file_id;
            require(copied||staging.absent(r.stage_key),"file_intake_unexpected_staging_after_rename");
            // A durable acceptance is not evidence that its final copy still
            // exists after a crash/manual removal. Keep the CURRENT surviving
            // catalog object pinned and verified before discarding recovery.
            const auto config=storage.file_configuration(owner);const auto managed=storage.managed_file(owner,r.accepted_file);
            require(config&&config.value&&managed&&managed.value&&!managed.value->purged&&!managed.value->operation_pending,"file_intake_cleanup_final_unavailable");const auto& current=*managed.value;
            require(current.payload.bytes==r.payload.bytes&&current.payload.sha256==r.payload.sha256,"file_intake_cleanup_final_content_changed");
            std::optional<FilePayloadDirectory> directory;std::string name;
            if(current.file.state==app::FileCatalogState::removed){require(!current.removed_key.empty(),"file_intake_cleanup_removed_unbound");directory=FilePayloadDirectory::private_store(root/"data"/"fileremoved",bindings);name=current.removed_key;}
            else{
                require(current.removed_key.empty(),"file_intake_cleanup_final_location_changed");const auto selected=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& a){return a.area.id==current.file.area;});require(selected!=config.value->areas.end(),"file_intake_cleanup_final_area_unavailable");std::vector<std::filesystem::path> others;for(const auto& a:config.value->areas)if(!(a.area.id==current.file.area))others.push_back(std::filesystem::u8path(a.storage_path));
                directory=FilePayloadDirectory::area(root,std::filesystem::u8path(selected->storage_path),others,{"bin","config","data","display","backup","logs","work","doors","botgate"},bindings);name=app::file_managed_payload_name(current);
            }
            require(!directory->absent(name),"file_intake_cleanup_final_missing");final.emplace(directory->read(name));const auto surviving=final->identity(progress);
            require(surviving.volume==current.payload.volume&&surviving.file_id==current.payload.file_id&&surviving.bytes==current.payload.bytes&&surviving.sha256==current.payload.sha256,"file_intake_cleanup_final_replaced_or_changed");
            if(!staging.absent(r.stage_key)){
            auto payload=staging.read(r.stage_key,true);const auto actual=payload.identity(progress);
            require(actual.volume==r.payload.volume&&actual.file_id==r.payload.file_id&&actual.bytes==r.payload.bytes&&actual.sha256==r.payload.sha256,"file_intake_redundant_staging_changed");
            require(actual.volume!=surviving.volume||actual.file_id!=surviving.file_id,"file_intake_cleanup_not_redundant");
            require(!progress||progress(actual.bytes,actual.bytes),"file_intake_cleanup_cancelled");
            payload.remove_exact();
            }
            if(copied){
                const auto map=std::dynamic_pointer_cast<LinuxFilePayloadBindings>(bindings);require(bool(map),"file_intake_recovery_bindings_unavailable");
                require(r.staged_object->volume==r.payload.volume&&r.staged_object->file_id==r.payload.file_id,"file_intake_staging_receipt_mismatch");
                map->reconcile_recorded_removal(staging.path()/r.stage_key,r.payload);
            }
        }
        require(staging.absent(r.stage_key),"file_intake_cleanup_absence_unconfirmed");
        return storage.finish_file_intake_cleanup(owner,operation,*r.staged_object).succeeded();
    }catch(...){return false;}
}
bool reconcile_file_intake_placement(app::IFileStorage& storage,const app::FileConfigurationActor& owner,
    app::Identifier128 operation,const std::filesystem::path& root,std::shared_ptr<IFilePayloadBindings> bindings){
    try{
        const auto result=storage.file_intake(owner,operation);const auto config=storage.file_configuration(owner);
        require(result&&result.value&&config&&config.value,"file_intake_recovery_unavailable");const auto& r=*result.value;
        require((r.state==app::FileIntakeState::held||r.state==app::FileIntakeState::prepared)&&r.transfer_complete&&r.staged_object&&r.placed_object&&app::valid_file_name(r.stage_key)&&app::valid_file_name(app::file_intake_payload_name(r)),"file_intake_move_intent_unavailable");
        require(r.staged_object->volume==r.payload.volume&&r.staged_object->file_id==r.payload.file_id&&r.placed_object->volume==r.payload.volume&&r.placed_object->file_id==r.payload.file_id,"file_intake_move_identity_mismatch");
        const auto selected=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& area){return area.area.id==r.placement_area;});
        require(selected!=config.value->areas.end()&&selected->area.revision==r.placement_revision,"file_intake_move_area_changed");
        std::vector<std::filesystem::path> others;for(const auto& area:config.value->areas)if(!(area.area.id==r.placement_area))others.push_back(std::filesystem::u8path(area.storage_path));
        const auto destination=FilePayloadDirectory::area(root,std::filesystem::u8path(selected->storage_path),others,{"bin","config","data","display","backup","logs","work","doors","botgate"},bindings);
        const auto map=std::dynamic_pointer_cast<LinuxFilePayloadBindings>(bindings);require(bool(map),"file_intake_recovery_bindings_unavailable");
        map->reconcile_recorded_move(root/"data/fileintake"/r.stage_key,destination.path()/app::file_intake_payload_name(r),r.payload);
        return true;
    }catch(...){return false;}
}
std::shared_ptr<LinuxFilePayloadBindings> prepare_file_intake_bindings(app::IFileStorage& storage,const app::FileConfigurationActor& owner,
    const std::filesystem::path& root,const std::function<bool()>& cancelled){
    const auto config=storage.file_configuration(owner);require(config&&config.value,"file_prepare_configuration_unavailable");
    FileBindingPrepareProofs proofs;app::Identifier128 after{};std::size_t count{};
    for(;;){
        require(!cancelled||!cancelled(),"file_prepare_cancelled");const auto pending=storage.pending_file_intake(owner,256,after);require(pending&&pending.value,"file_prepare_intake_unavailable");
        for(const auto& r:*pending.value){
            require(++count<=1000000,"file_prepare_intake_limit");
            const auto purge=storage.file_intake_purge(owner,r.operation);
            if(purge&&purge.value){
                if(purge.value->staging&&app::valid_file_name(r.stage_key))proofs.deletions.emplace_back(std::filesystem::path("data/fileintake")/r.stage_key,*purge.value->staging);
                if(purge.value->placed&&app::valid_file_name(app::file_intake_payload_name(r))){
                    const auto area=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& value){return value.area.id==r.placement_area;});
                    if(area!=config.value->areas.end())proofs.deletions.emplace_back(std::filesystem::u8path(area->storage_path)/app::file_intake_payload_name(r),*purge.value->placed);
                }
                continue;
            }
            if(!r.staged_object||!r.placed_object||!r.transfer_complete||!app::valid_file_name(r.stage_key)||r.staged_object->volume!=r.payload.volume||r.staged_object->file_id!=r.payload.file_id)continue;
            const auto from=std::filesystem::path("data/fileintake")/r.stage_key;
            if((r.state==app::FileIntakeState::held||r.state==app::FileIntakeState::prepared)&&r.placed_object->volume==r.payload.volume&&r.placed_object->file_id==r.payload.file_id){
                const auto area=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& value){return value.area.id==r.placement_area;});
                if(area==config.value->areas.end()||area->area.revision!=r.placement_revision||!app::valid_file_name(app::file_intake_payload_name(r)))continue;
                proofs.moves.push_back({from,std::filesystem::u8path(area->storage_path)/app::file_intake_payload_name(r),r.payload});
            }else if(r.state==app::FileIntakeState::accepted&&!r.staging_clean&&
                (r.placed_object->volume!=r.payload.volume||r.placed_object->file_id!=r.payload.file_id)){
                const auto managed=storage.managed_file(owner,r.accepted_file);if(!managed||!managed.value||managed.value->purged||managed.value->operation_pending)continue;const auto& current=*managed.value;
                if(current.payload.bytes!=r.payload.bytes||current.payload.sha256!=r.payload.sha256)continue;
                std::filesystem::path final;
                if(current.file.state==app::FileCatalogState::removed){if(!app::valid_file_name(current.removed_key))continue;final=std::filesystem::path("data/fileremoved")/current.removed_key;}
                else{
                    if(!current.removed_key.empty()||!app::valid_file_name(app::file_managed_payload_name(current)))continue;
                    const auto area=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& value){return value.area.id==current.file.area;});if(area==config.value->areas.end())continue;
                    final=std::filesystem::u8path(area->storage_path)/app::file_managed_payload_name(current);
                }
                proofs.removals.push_back({from,final,r.payload,current.payload});
            }
        }
        if(pending.value->size()<256)break;
        require(pending.value->back().operation.bytes>after.bytes,"file_prepare_intake_cursor_invalid");after=pending.value->back().operation;
    }
    append_file_manager_prepare_proofs(storage,owner,*config.value,proofs,cancelled);
    return LinuxFilePayloadBindings::prepare(root,cancelled,proofs);
}
} // namespace sfx::platform::linux_host
