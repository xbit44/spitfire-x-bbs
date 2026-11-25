std::optional<app::FileIntakePurge> LinuxFileManager::preview_intake_purge(app::Identifier128 id,const Progress& progress){
    try{
        const auto prior=storage_.file_intake_purge(actor_,id);if(prior&&prior.value)return *prior.value;
        const auto config=storage_.file_configuration(actor_);const auto intake=storage_.file_intake(actor_,id);
        require(config&&config.value&&intake&&intake.value&&intake.value->state==app::FileIntakeState::held&&!intake.value->accepted_file.value,"file_purge_held_required");
        const auto& r=*intake.value;const auto staging=FilePayloadDirectory::private_store(installation_/"data"/"fileintake",bindings_);
        app::FileIntakePurge result;result.operation=id;
        const auto observe=[&](const FilePayloadDirectory& directory,const std::string& name,const std::optional<app::FileObjectIdentity>& bound)->std::optional<app::FilePayloadIdentity>{
            if(directory.absent(name))return {};
            require(bound.has_value(),"file_purge_unknown_object_retained");auto file=directory.read(name);const auto identity=file.identity(progress);
            require(identity.volume==bound->volume&&identity.file_id==bound->file_id,"file_purge_replacement_retained");return identity;
        };
        result.staging=observe(staging,r.stage_key,r.staged_object);
        if(r.placed_object){const auto& name=app::file_intake_payload_name(r);require(app::valid_file_name(name),"file_purge_invalid_placement");result.placed=observe(area(*config.value,r.placement_area),name,r.placed_object);}
        return result;
    }catch(...){return {};}
}
NativeFileOperationOutcome LinuxFileManager::purge_held_intake(const app::FileIntakePurge& preview,const Progress& progress){
    bool journal{};
    try{
        const auto prior=storage_.file_intake_purge(actor_,preview.operation);
        if(prior&&prior.value){
            const auto equal=[](const auto& left,const auto& right){return left.has_value()==right.has_value()&&(!left||same(*left,*right));};
            require(equal(prior.value->staging,preview.staging)&&equal(prior.value->placed,preview.placed),"file_purge_preview_changed");
            journal=true;if(prior.value->completed)return {true,false,true,{},"Explicit purge already committed; no new objects touched."};
        }
        const auto config=storage_.file_configuration(actor_);const auto intake=storage_.file_intake(actor_,preview.operation);
        require(config&&config.value&&intake&&intake.value&&intake.value->state==app::FileIntakeState::held&&!intake.value->accepted_file.value,"file_purge_held_required");
        const auto& r=*intake.value;const auto staging=FilePayloadDirectory::private_store(installation_/"data"/"fileintake",bindings_);
        std::optional<FilePayloadDirectory> placed_directory;std::optional<FilePayloadLease> stage,placed;
        const auto acquire=[&](const FilePayloadDirectory& directory,const std::string& name,const std::optional<app::FilePayloadIdentity>& expected)->std::optional<FilePayloadLease>{
            if(directory.absent(name)){require(journal||!expected,"file_purge_preview_no_longer_present");if(journal&&expected)reconcile_removal(directory.path()/name,*expected);return {};}
            require(expected.has_value(),"file_purge_new_object_retained");auto file=directory.read(name,true);
            require(same(file.identity(progress),*expected),"file_purge_changed_object_retained");return file;
        };
        stage=acquire(staging,r.stage_key,preview.staging);
        if(r.placed_object){const auto& name=app::file_intake_payload_name(r);require(app::valid_file_name(name),"file_purge_invalid_placement");placed_directory=area(*config.value,r.placement_area);placed=acquire(*placed_directory,name,preview.placed);}
        else require(!preview.placed,"file_purge_unbound_placement");
        auto request=preview;request.completed=false;
        require(storage_.prepare_file_intake_purge(actor_,request).succeeded(),"file_purge_intent_unconfirmed");journal=true;
        if(boundary_)boundary_(FileManagerBoundary::before_change);
        if(stage){stage->remove_exact();stage.reset();}
        if(boundary_)boundary_(FileManagerBoundary::after_staging_purge);
        if(placed){placed->remove_exact();placed.reset();}
        require(staging.absent(r.stage_key)&&(!placed_directory||placed_directory->absent(app::file_intake_payload_name(r))),"file_purge_absence_unconfirmed");
        if(boundary_)boundary_(FileManagerBoundary::before_commit);
        require(storage_.finish_file_intake_purge(actor_,request).succeeded(),"file_purge_completion_unconfirmed");
        return {true,false,true,{},"Explicit Sysop purge completed. No scan result or upload credit added."};
    }catch(...){return {false,false,journal,{},"Purge not confirmed; unresolved objects and journal retained for explicit reconciliation."};}
}
