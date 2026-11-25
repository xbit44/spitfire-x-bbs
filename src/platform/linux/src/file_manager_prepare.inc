void append_file_manager_prepare_proofs(app::IFileStorage& storage,const app::FileConfigurationActor& owner,
    const app::FileConfiguration& config,FileBindingPrepareProofs& proofs,const std::function<bool()>& cancelled){
    const auto area_path=[&](app::FileAreaId id)->std::optional<std::filesystem::path>{
        const auto found=std::find_if(config.areas.begin(),config.areas.end(),[&](const auto& area){return area.area.id==id;});
        if(found==config.areas.end())return {};
        return std::filesystem::u8path(found->storage_path);
    };
    app::Identifier128 after{};std::size_t count{};
    for(;;){
        require(!cancelled||!cancelled(),"file_prepare_cancelled");
        const auto page=storage.pending_file_imports(owner,after,100);require(page&&page.value,"file_prepare_imports_unavailable");
        for(const auto& op:*page.value){
            require(++count<=1000000,"file_prepare_imports_limit");const auto& request=op.request;
            if(request.mode!=app::FileImportMode::in_place)continue;
            const auto area=area_path(request.registration.area);if(!area||!app::valid_file_name(request.registration.filename))continue;
            const auto path=std::filesystem::u8path(request.source_path);
            if(path.filename()!=request.registration.filename)continue;
            proofs.imports.push_back({path,*area/request.registration.filename,request.registration.payload});
        }
        if(page.value->size()<100)break;
        require(page.value->back().request.registration.operation.bytes>after.bytes,"file_prepare_imports_cursor");after=page.value->back().request.registration.operation;
    }
    after={};count=0;
    for(;;){
        require(!cancelled||!cancelled(),"file_prepare_cancelled");
        const auto page=storage.pending_file_operations(owner,after,100);require(page&&page.value,"file_prepare_operations_unavailable");
        for(const auto& op:*page.value){
            require(++count<=1000000,"file_prepare_operations_limit");
            const auto& r=op.request;const auto& record=op.source;std::filesystem::path from,to;
            if(!record.removed_key.empty()){
                if(!app::valid_file_name(record.removed_key))continue;
                from=std::filesystem::path("data/fileremoved")/record.removed_key;
            }else{
                const auto path=area_path(record.file.area);if(!path||!app::valid_file_name(app::file_managed_payload_name(record)))continue;
                from=*path/app::file_managed_payload_name(record);
            }
            if(r.action==Action::purge){proofs.deletions.emplace_back(from,record.payload);continue;}
            if(record.file.state==app::FileCatalogState::missing||r.action==Action::copy||r.action==Action::metadata||r.action==Action::mark_missing)continue;
            if(r.action==Action::remove)to=std::filesystem::path("data/fileremoved")/key(r.operation);
            else{
                const auto path=area_path(r.destination);if(!path||!app::valid_file_name(r.filename))continue;
                const auto selected=std::find_if(config.areas.begin(),config.areas.end(),[&](const auto& area){return area.area.id==r.destination;});
                if(selected->area.revision!=r.expected_destination_revision)continue;
                to=*path/r.filename;
            }
            if(!op.placed_object||object(*op.placed_object,{record.payload.volume,record.payload.file_id}))proofs.moves.push_back({from,to,record.payload});
            else{
                auto final=record.payload;final.volume=op.placed_object->volume;final.file_id=op.placed_object->file_id;
                proofs.removals.push_back({from,to,record.payload,final});
            }
        }
        if(page.value->size()<100)break;
        require(page.value->back().request.operation.bytes>after.bytes,"file_prepare_operations_cursor");after=page.value->back().request.operation;
    }
}
