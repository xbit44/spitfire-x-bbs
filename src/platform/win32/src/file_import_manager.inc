FilePayloadDirectory WindowsFileManager::import_source(const app::FileConfiguration& config,const app::FileImportRequest& request)const{
    const auto source=resolve_portable_owned_path(installation_,std::filesystem::u8path(request.source_path));
    ins::require(source.is_absolute()&&app::valid_file_name(source.filename().u8string()),"file_import_absolute_source_required");
    if(request.mode==app::FileImportMode::in_place){
        auto directory=area(config,request.registration.area);
        ins::require(_wcsicmp(source.parent_path().c_str(),directory.path().c_str())==0&&source.filename().u8string()==request.registration.filename,"file_import_in_place_path_mismatch");return directory;
    }
    std::vector<std::filesystem::path> protected_paths{installation_};
    for(const auto& configured:config.areas)protected_paths.push_back(resolve_portable_owned_path(installation_,std::filesystem::u8path(configured.storage_path)));
    return FilePayloadDirectory::import_source(source.parent_path(),protected_paths,owner_,request.mode==app::FileImportMode::move);
}
std::optional<app::FileImportRequest> WindowsFileManager::preview_import(const NativeFileImportChoice& choice,const Progress& progress){
    try{
        ins::require(choice.operation.valid()&&choice.area.value>0&&static_cast<unsigned>(choice.mode)<=2,"file_import_choice_invalid");
        const auto config=storage_.file_configuration(actor_);ins::require(config&&config.value,"file_import_config_unavailable");
        const auto found=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& a){return a.area.id==choice.area;});ins::require(found!=config.value->areas.end(),"file_import_area_unavailable");
        app::FileImportRequest request;request.mode=choice.mode;request.source_path=choice.source.u8string();auto& r=request.registration;
        r.operation=choice.operation;r.area=choice.area;r.expected_area_revision=found->area.revision;r.filename=choice.filename.empty()?choice.source.filename().u8string():choice.filename;
        ins::require(app::valid_file_name(r.filename),"file_import_name_invalid");
        auto source_dir=import_source(*config.value,request);auto source=source_dir.read(choice.source.filename().u8string());
        r.payload=source.identity(progress);request.source_path=source.path().u8string();r.allow_duplicate_content=choice.allow_duplicate_content;r.sysop_only=choice.sysop_only;
        if(choice.public_uploader.empty()){const auto importer=storage_.file_importer_name(actor_);ins::require(importer&&importer.value,"file_importer_name_unavailable");r.public_uploader=*importer.value;}
        else r.public_uploader=choice.public_uploader;
        ins::require(!r.public_uploader.empty()&&r.public_uploader.size()<=64&&app::sanitize_file_text(r.public_uploader,64,false)==r.public_uploader,"file_import_public_name_invalid");
        if(actor_.caller)r.uploader=actor_.caller->account;
        r.original_upload_utc=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        r.descriptions=app::file_import_descriptions(r.filename,{});
        if(choice.descriptions)r.descriptions=*choice.descriptions;
        else if(choice.use_diz){
            auto extension=choice.source.extension().u8string();std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(c>='A'&&c<='Z'?c-'A'+'a':c);});
            if(extension==".zip"){
                bool cancelled{};const auto diz=archive::read_file_diz({r.payload.bytes,[&](auto offset,auto n){return source.read(offset,n);},[&]{cancelled=progress&&!progress(r.payload.bytes,r.payload.bytes);return cancelled;}});
                ins::require(!cancelled,"file_import_preview_cancelled");
                if(diz.result.ok()&&diz.descriptions)r.descriptions=*diz.descriptions;
            }
        }
        ins::require(app::valid_file_descriptions(r.descriptions),"file_import_descriptions_invalid");
        if(choice.mode!=app::FileImportMode::in_place)ins::require(area(*config.value,choice.area).absent(r.filename),"file_import_existing_destination");
        return request;
    }catch(...){return {};}
}
NativeFileOperationOutcome WindowsFileManager::hold_import(app::Identifier128 id){
    const std::string text="Import not committed; preserve source and destination for explicit reconciliation.";
    return {false,false,storage_.hold_file_import(actor_,id,text).succeeded(),{},text};
}
NativeFileOperationOutcome WindowsFileManager::import_file(const app::FileImportRequest& request,const Progress& progress){
    const auto existing=storage_.file_import(actor_,request.registration.operation);
    if(existing&&existing.value){
        const auto exact=storage_.prepare_file_import(actor_,request);if(!exact||!exact.value)return {false,false,false,{},"Import identity differs from the recorded operation."};
        return run_import(*exact.value,false,progress);
    }
    try{
        const auto config=storage_.file_configuration(actor_);ins::require(config&&config.value,"file_import_config_unavailable");
        auto directory=import_source(*config.value,request);auto source=directory.read(std::filesystem::u8path(request.source_path).filename().u8string());
        ins::require(same(source.identity(progress),request.registration.payload),"file_import_preview_source_changed");
        if(request.mode!=app::FileImportMode::in_place)ins::require(area(*config.value,request.registration.area).absent(request.registration.filename),"file_import_existing_destination");
        const auto prepared=storage_.prepare_file_import(actor_,request);ins::require(prepared&&prepared.value,"file_import_refused_review_duplicate_warning");
    }catch(...){return {false,false,false,{},"Import refused; refresh preview, destination or duplicate-content choice."};}
    const auto prepared=storage_.file_import(actor_,request.registration.operation);if(!prepared||!prepared.value)return hold_import(request.registration.operation);
    return run_import(*prepared.value,false,progress);
}
NativeFileOperationOutcome WindowsFileManager::reconcile_import(app::Identifier128 id,const Progress& progress){
    const auto op=storage_.file_import(actor_,id);if(!op||!op.value)return {false,false,false,{},"Import unavailable."};return run_import(*op.value,true,progress);
}
NativeFileOperationOutcome WindowsFileManager::run_import(const app::FileImport& op,bool recovery,const Progress& progress){
    if(op.state==app::FileOperationState::completed)return {true,false,true,op.result,"Import already committed."};
    if(op.state==app::FileOperationState::cancelled)return {false,true,true,{},"Import already cancelled."};
    const auto& request=op.request;const auto& r=request.registration;const auto at=[this](FileManagerBoundary point){if(boundary_)boundary_(point);};
    try{
        const auto config=storage_.file_configuration(actor_);ins::require(config&&config.value,"file_import_config_unavailable");
        auto input=import_source(*config.value,request);auto output=area(*config.value,r.area);const auto source_name=std::filesystem::u8path(request.source_path).filename().u8string();
        app::FileOperationCompletion proof;proof.source=r.payload;std::optional<FilePayloadLease> source,dest;
        if(!input.absent(source_name)){source.emplace(input.read(source_name,request.mode==app::FileImportMode::move));ins::require(same(source->identity(progress),r.payload),"file_import_source_identity_changed");}
        else ins::require(recovery&&request.mode==app::FileImportMode::move,"file_import_source_missing");
        at(FileManagerBoundary::before_change);
        if(request.mode==app::FileImportMode::in_place){ins::require(source.has_value(),"file_import_in_place_source_missing");proof.destination=source->identity(progress);proof.destination_present=true;}
        else{
            if(!output.absent(r.filename)){
                ins::require(recovery&&op.placed_object,"file_import_unknown_existing_destination");dest.emplace(output.read(r.filename));
                ins::require(object(dest->object_identity(),*op.placed_object),"file_import_destination_identity_changed");
            }else{
                ins::require(source.has_value(),"file_import_both_paths_absent");dest.emplace(output.create(r.filename,(std::max)(std::uint64_t{1},r.payload.bytes)));
                ins::require(storage_.bind_file_import_destination(actor_,r.operation,dest->object_identity()).succeeded(),"file_import_destination_checkpoint_failed");
                at(FileManagerBoundary::destination_created);
                for(std::uint64_t offset=0;offset<r.payload.bytes;){ins::require(!progress||progress(offset,r.payload.bytes),"file_import_copy_cancelled");const auto bytes=source->read(offset);ins::require(!bytes.empty(),"file_import_short_read");dest->append(bytes.data(),bytes.size());offset+=bytes.size();}
            }
            proof.destination=dest->identity(progress);proof.destination_present=true;
            ins::require(proof.destination.bytes==r.payload.bytes&&proof.destination.sha256==r.payload.sha256,"file_import_copy_content_mismatch");at(FileManagerBoundary::after_copy);
            if(request.mode==app::FileImportMode::move){if(source){source->remove_exact();source.reset();}ins::require(input.absent(source_name),"file_import_move_absence_unconfirmed");proof.source_absent=true;}
        }
        at(FileManagerBoundary::after_change);at(FileManagerBoundary::before_commit);
        const auto committed=storage_.commit_file_import(actor_,r.operation,proof);if(!committed||!committed.value)return hold_import(r.operation);return {true,false,true,committed.value->id,"Imported; no caller transfer credit."};
    }catch(...){return hold_import(r.operation);}
}
NativeFileOperationOutcome WindowsFileManager::cancel_import_unchanged(app::Identifier128 id,const Progress& progress){
    const auto result=storage_.file_import(actor_,id);if(!result||!result.value)return {false,false,false,{},"Import unavailable."};const auto& op=*result.value;
    if(op.state==app::FileOperationState::completed)return {true,false,true,op.result,"Completed import cannot be cancelled."};if(op.state==app::FileOperationState::cancelled)return {false,true,true,{},"Import already cancelled."};
    try{
        const auto config=storage_.file_configuration(actor_);ins::require(config&&config.value,"file_import_config_unavailable");auto input=import_source(*config.value,op.request);auto source=input.read(std::filesystem::u8path(op.request.source_path).filename().u8string());
        const auto identity=source.identity(progress);ins::require(same(identity,op.request.registration.payload),"file_import_source_not_unchanged");
        if(op.request.mode!=app::FileImportMode::in_place)ins::require(area(*config.value,op.request.registration.area).absent(op.request.registration.filename),"file_import_destination_requires_reconciliation");
        ins::require(storage_.cancel_file_import(actor_,id,{identity,{},false,false}).succeeded(),"file_import_cancellation_unconfirmed");return {false,true,true,{},"Cancelled; original file unchanged."};
    }catch(...){return hold_import(id);}
}
