            namespace native=platform::linux_host;
            native::InstallationLock guard;
            if(!guard.acquire(root_,".sfx-runtime.lock"))throw std::runtime_error("contents_restore_requires_stopped_management");
            auto bindings=native::LinuxFilePayloadBindings::open(root_);
            const auto configuration=native::read_backup_file_configuration(root_,bindings);
            auto collection=native::open_backup_collection(root_,bindings,archive::BackupKind::manual,result.archive.parent_path().string(),false);
            publish("Inspecting file-content restore point");
            auto members=native::verified_backup_members(root_,bindings,collection,collection.kind,cancelled);
            auto selected=std::find_if(members.begin(),members.end(),[&](const auto& member){return member.path==result.archive;});
            // An ordinary custom destination may share the Upgrades directory.
            // Match the authenticated ownership pool, never just its pathname.
            if(selected==members.end()&&result.archive.parent_path()==root_/"backup/upgrades"){
                collection=native::open_backup_collection(root_,bindings,archive::BackupKind::setup_upgrade,{},false);
                members=native::verified_backup_members(root_,bindings,collection,collection.kind,cancelled);
                selected=std::find_if(members.begin(),members.end(),[&](const auto& member){return member.path==result.archive;});
            }
            if(selected==members.end())throw std::runtime_error("contents_restore_point_not_owned_or_verified");
            const auto position=static_cast<std::size_t>(selected-members.begin());native::verify_content_dependencies(members,{position});
            result.archive_files=selected->inspection.files.size();result.content_areas=selected->contents.areas;
            if(result.content_areas.empty())throw std::runtime_error("contents_restore_point_has_no_selected_area_snapshot");
            const auto token=[&]{std::string out;for(unsigned i=0;i<3;++i){const auto id=store_.identifiers().generate();for(const auto byte:id.bytes){out+="0123456789ABCDEF"[byte>>4U];out+="0123456789ABCDEF"[byte&15U];}}return out;};
            result.awaiting_area_selection=true;result.confirmation=token();result.detail="Choose the file areas to preview. Nothing has been restored. Keep this restore point and its managed companion archives together.";publish("Choose file areas");
            std::vector<std::int64_t> areas;
            {std::unique_lock<std::mutex> state(offline_state_mutex_);
                const auto answered=offline_confirmation_.wait_for(state,std::chrono::minutes(5),[&]{return content_selection_.has_value()||cancelled();});
                if(!answered||cancelled())throw std::runtime_error("contents_restore_selection_cancelled_or_expired");
                areas=*content_selection_;
            }
            result.awaiting_area_selection=false;result.confirmation.clear();publish("Checking selected destinations");
            auto restore=native::FileContentRestore::preview(root_,bindings,configuration,collection,result.archive,areas,cancelled);
            for(const auto& destination:restore->destinations()){
                LinuxContentDestination entry;entry.area=destination.first;entry.path=destination.second;
                for(const auto& item:restore->items())if(item.area==entry.area){
                    if(item.state==native::FileContentRestoreItem::State::missing)++entry.missing;
                    else if(item.state==native::FileContentRestoreItem::State::identical)++entry.identical;
                    else ++entry.conflicts;
                }
                result.content_destinations.push_back(std::move(entry));
            }
            result.awaiting_confirmation=true;result.confirmation=token();result.detail="Review every current destination and count. Only missing files will be restored. Identical files stay untouched; conflicts are skipped. Core settings and catalog identities do not change.";publish("Review file contents");
            {std::unique_lock<std::mutex> state(offline_state_mutex_);
                const auto answered=offline_confirmation_.wait_for(state,std::chrono::minutes(5),[&]{return offline_confirmed_||cancelled();});
                if(!answered||cancelled())throw std::runtime_error("contents_restore_review_cancelled_or_expired");
            }
            result.awaiting_confirmation=false;result.confirmation.clear();publish("Restoring missing file contents");
            const auto current=native::read_backup_file_configuration(root_,bindings);
            result.contents_restored=restore->execute(current,cancelled);result.success=result.contents_restored.completed;
            result.diagnostic=result.contents_restored.diagnostic;
            result.detail=(result.success?"File contents restore completed. ":"File contents restore is incomplete. ")+std::to_string(result.contents_restored.restored)+" restored; "+std::to_string(result.contents_restored.identical)+" identical; "+std::to_string(result.contents_restored.conflicts)+" conflicts skipped. Core settings and catalog identities are unchanged. Use File Manager Inspect before serving restored files. The BBS remains stopped.";
