namespace {
namespace gd_app=application;
bool deletion_same(const gd_app::FilePayloadIdentity& a,const gd_app::FilePayloadIdentity& b){return a.bytes==b.bytes&&a.sha256==b.sha256&&a.volume==b.volume&&a.file_id==b.file_id;}
gd_app::Identifier128 deletion_id(gd_app::Identifier128 parent,std::string_view phase,std::string_view key){
    std::string bytes("SFX/GroupDelete/v1;");bytes.append(reinterpret_cast<const char*>(parent.bytes.data()),16);bytes.append(phase);bytes.push_back(0);bytes.append(key);
    unsigned char digest[32]{};crypto_hash_sha256(digest,reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());gd_app::Identifier128 result;std::copy(digest,digest+16,result.bytes.begin());return result;
}
FilePayloadDirectory deletion_directory(const std::filesystem::path& root,const std::shared_ptr<IFilePayloadBindings>& bindings,const gd_app::FileConfiguration& config,const gd_app::FileAreaConfiguration& area){
    std::vector<std::filesystem::path> others;for(const auto& a:config.areas)if(!(a.area.id==area.area.id))others.push_back(std::filesystem::u8path(a.storage_path));
    return FilePayloadDirectory::area(root,std::filesystem::u8path(area.storage_path),others,{"bin","config","data","display","backup","logs","work","doors","botgate"},bindings);
}
std::vector<std::string> deletion_names(const FilePayloadDirectory& directory){
    return directory.strict_names(100000);
}
}
application::LocalFileConfigurationResult LinuxFileConfigurationPaths::group_deletion(application::IFileStorage& storage,const application::FileConfigurationChange& requested){
    using namespace gd_app;const FileConfigurationActor actor{true,{}};
    if(!offline_deletion_)return {LocalAccountResultCode::policy_rejected,{},"Stop the BBS and close File Manager before deleting a File Group. Other configuration remains available live."};
    LocalFileConfigurationResult reply;reply.code=LocalAccountResultCode::policy_rejected;
    try{
        auto read=storage.file_configuration(actor);require(read&&read.value,"group_delete_catalog");auto config=*read.value;
        auto prior=storage.file_group_deletion(actor,requested.group.id);
        if(prior&&prior.value&&prior.value->completed)return {LocalAccountResultCode::ok,{config.policy.catalog_revision,requested.group.id.value,true},"The Group was already removed."};
        const auto selected=std::find_if(config.groups.begin(),config.groups.end(),[&](const auto& g){return g.id==requested.group.id;});
        require(selected!=config.groups.end(),"group_delete_changed");
        const auto group=*selected;const bool resuming=group.deletion_operation.valid();
        if(!resuming)require(config.policy.catalog_revision==requested.expected_catalog_revision&&group.revision==requested.group.revision,"group_delete_changed");
        else require(group.delete_disk_files==requested.delete_disk_files||requested.action==FileConfigurationAction::preview_group_delete,"group_delete_mode_changed");
        std::vector<FileAreaConfiguration> areas;for(const auto& a:config.areas)if(a.area.group==group.id){require(a.area.kind==FileAreaKind::ordinary,"group_delete_system_area");areas.push_back(a);}
        if(auto* ftn=dynamic_cast<IFtnMailStorage*>(&storage)){
            const auto network=ftn->ftn_configuration(actor);require(network&&network.value,"group_delete_ftn_unavailable");
            for(const auto& echo:network.value->file_echoes)for(const auto& a:areas)require(echo.file_area!=a.area.id.value,"group_delete_ftn_mapping");
        }
        reply.result={config.policy.catalog_revision,group.id.value,false};reply.area_count=static_cast<std::uint32_t>(areas.size());
        std::vector<FileGroupDeletionItem> plan;
        const bool inspect_only=requested.action==FileConfigurationAction::preview_group_delete;
        const bool disk=resuming?group.delete_disk_files:requested.delete_disk_files;
        bool unknown_disk{};
        if(!resuming||inspect_only){
            for(const auto& area:areas){
                std::set<std::string> registered;FileId after{};FileManagementQuery query;query.area=area.area.id;
                do{const auto page=storage.managed_file_page(actor,query,after,100);require(page&&page.value,"group_delete_catalog");
                    for(const auto& file:page.value->files)if(!file.purged){++reply.catalog_files;
                        if(disk&&!inspect_only)plan.push_back({area.area.id,file.file.id,file_managed_payload_name(file),file.payload});
                        if(file.file.state!=FileCatalogState::removed)registered.insert(*file_case_key(file_managed_payload_name(file)));
                    }after=page.value->next;
                }while(after.value);
                try{const auto directory=deletion_directory(root_,bindings_,config,area);for(const auto& name:deletion_names(directory)){
                        ++reply.disk_files;require(reply.disk_files<=100000,"group_delete_limit");
                        if(disk&&!inspect_only&&!registered.count(*file_case_key(name))){auto file=directory.read(name);plan.push_back({area.area.id,{},name,file.identity()});}
                    }}catch(...){if(disk&&!inspect_only)throw;unknown_disk=true;}
            }
        }
        if(inspect_only){reply.code=LocalAccountResultCode::ok;if(unknown_disk)reply.detail="Some disk contents could not be inspected. Remove from SFX only can keep those files; disk deletion requires readable flat folders.";return reply;}
        if(!resuming){auto begin=requested;begin.deletion_items=std::move(plan);const auto started=storage.change_file_configuration(actor,begin);if(!started||!started.value){
            if(started.status.code==StorageErrorCode::conflict)throw std::runtime_error("group_delete_changed");
            if(started.status.code==StorageErrorCode::busy)throw std::runtime_error("group_delete_pending_work");
            return {LocalAccountResultCode::storage_failure,{},"Deletion outcome unresolved. Reconcile this same request before starting more file work."};}}
        auto state=storage.file_group_deletion(actor,group.id);require(state&&state.value&&!state.value->completed,"group_delete_journal");
        read=storage.file_configuration(actor);require(read&&read.value,"group_delete_catalog");config=*read.value;
        if(state.value->delete_disk_files&&!state.value->items.empty()){
            LinuxFileManager manager(storage,actor,root_,bindings_);
            for(const auto& item:state.value->items){
                FileId file_id=item.file;
                const auto area=std::find_if(config.areas.begin(),config.areas.end(),[&](const auto& a){return a.area.id==item.area;});require(area!=config.areas.end(),"group_delete_changed");
                if(!file_id.value){
                    const auto import_id=deletion_id(state.value->operation,"import",std::to_string(item.area.value)+"/"+item.name+"/"+item.payload.volume+"/"+item.payload.file_id);
                    const auto imported=storage.file_import(actor,import_id);
                    if(imported&&imported.value){
                        if(imported.value->state!=FileOperationState::completed){const auto fixed=manager.reconcile_import(import_id);require(fixed.completed,"group_delete_import_pending");file_id=fixed.result;}else file_id=imported.value->result;
                    }else{
                        auto directory=deletion_directory(root_,bindings_,config,*area);if(directory.absent(item.name))continue;
                        {auto file=directory.read(item.name);require(deletion_same(file.identity(),item.payload),"group_delete_file_changed");}
                        NativeFileImportChoice choice;choice.operation=import_id;choice.area=item.area;choice.source=directory.path()/std::filesystem::u8path(item.name);choice.mode=FileImportMode::in_place;choice.use_diz=false;choice.allow_duplicate_content=true;choice.public_uploader="sysop";
                        const auto preview=manager.preview_import(choice);require(preview&&deletion_same(preview->registration.payload,item.payload),"group_delete_file_changed");
                        const auto done=manager.import_file(*preview);require(done.completed,"group_delete_import_pending");file_id=done.result;
                    }
                }
                const auto current=storage.managed_file(actor,file_id);require(current&&current.value,"group_delete_catalog");if(current.value->purged)continue;
                require(deletion_same(current.value->payload,item.payload),"group_delete_file_changed");
                const auto purge_id=deletion_id(state.value->operation,"purge",std::to_string(file_id.value));
                const auto existing=storage.file_operation(actor,purge_id);NativeFileOperationOutcome done;
                if(existing&&existing.value)done=manager.reconcile(purge_id);
                else{FileOperationRequest purge;purge.operation=purge_id;purge.action=FileOperationAction::purge;purge.file=file_id;purge.expected_revision=current.value->file.revision;done=manager.execute(purge);}
                require(done.completed,"group_delete_purge_pending");
                if(boundary_)boundary_(FileGroupDeletionBoundary::file_deleted);
            }
        }
        if(state.value->delete_disk_files)for(const auto& area:areas){const auto directory=deletion_directory(root_,bindings_,config,area);require(deletion_names(directory).empty(),"group_delete_unreviewed_files");}
        const auto latest=storage.file_configuration(actor);require(latest&&latest.value,"group_delete_catalog");
        FileConfigurationChange finish;finish.action=FileConfigurationAction::finish_group_delete;finish.operation=deletion_id(state.value->operation,"finish",std::to_string(group.id.value));finish.expected_catalog_revision=latest.value->policy.catalog_revision;finish.group=group;finish.group.deletion_operation=state.value->operation;finish.delete_disk_files=state.value->delete_disk_files;
        if(boundary_)boundary_(FileGroupDeletionBoundary::before_retire);
        const auto completed=storage.change_file_configuration(actor,finish);if(!completed||!completed.value)return {LocalAccountResultCode::storage_failure,{},"Deletion completion is unresolved. Reconcile this same request; the saved intent and file journals are retained."};
        reply.code=LocalAccountResultCode::ok;reply.result=*completed.value;reply.detail=state.value->delete_disk_files?"Group, Areas and file listings removed; reviewed files deleted from disk. Empty storage folders remain.":"Group, Areas and file listings removed from SFX. Files on disk were kept.";return reply;
    }catch(const std::exception& e){
        reply.result={};const std::string why=e.what();
        if(why=="group_delete_system_area")reply.detail="Move the required Uploads and Upload to Sysop Areas to another Group before deleting this Group.";
        else if(why=="group_delete_ftn_mapping")reply.detail="Remove this Group's File Area mappings from FTN File Echoes before deleting the Group.";
        else if(why=="group_delete_changed")reply.detail="File settings changed. Reload before reviewing this Group deletion again.";
        else if(why=="group_delete_mode_changed")reply.detail="Continue this Group's existing deletion choice; its saved disk-file choice cannot be changed.";
        else if(why=="group_delete_nested_or_link")reply.detail="Disk deletion requires flat File Area folders without nested folders or links. No unreviewed contents were deleted.";
        else if(why=="group_delete_pending_work")reply.detail="File work is pending in this Group. Finish or reconcile it in File Manager before deleting the Group.";
        else if(why=="group_delete_unreviewed_files"||why=="group_delete_file_changed")reply.detail="Files changed after review. Deletion stopped; Group/Area definitions remain. Preserve the remaining files and review before continuing.";
        else if(why=="linux_file_map_unknown_payload_preserved")reply.detail="A file in this Group has no known SFX identity. It was preserved and disk deletion stopped. Review the file in File Manager before another attempt; Remove from SFX only can keep files on disk.";
        else reply.detail="Deletion could not finish. Definitions and any saved deletion intent remain; some reviewed files may already be deleted. Review the actual catalog and any pending File Manager work before continuing.";
        return reply;
    }
}
