    const auto offline_operation=[](const Json& value){const auto text=value.template get<std::string>();const auto id=number(text);if(!id||std::to_string(id)!=text)throw std::runtime_error("maintenance_identity_invalid");return id;};
    api_get("/api/maintenance",[&](const auto&,auto& response){
        const auto value=runtime.offline_status();Json findings=Json::array();for(const auto& f:value.health.findings)findings.push_back({{"state",f.state==host::BbsHealthState::ok?"OK":f.state==host::BbsHealthState::needs_attention?"Needs attention":"Could not check"},{"subject",f.subject},{"detail",f.detail},{"next",f.next_step}});
        const auto& restored=value.restored.restore;
        Json areas=Json::array(),destinations=Json::array();
        for(const auto& area:value.content_areas)areas.push_back({{"id",std::to_string(area.id)},{"name",area.name}});
        for(const auto& item:value.content_destinations)destinations.push_back({{"area",std::to_string(item.area)},{"path",item.path.string()},{"missing",std::to_string(item.missing)},{"identical",std::to_string(item.identical)},{"conflicts",std::to_string(item.conflicts)}});
        json_response(response,{{"operation",std::to_string(value.operation)},{"kind",value.kind==rt::LinuxOfflineKind::health?"health":value.kind==rt::LinuxOfflineKind::prepare?"prepare":value.kind==rt::LinuxOfflineKind::message_deletion?"message_deletion":value.kind==rt::LinuxOfflineKind::file_group_deletion?"file_group_deletion":value.kind==rt::LinuxOfflineKind::file_contents?"file_contents":value.kind==rt::LinuxOfflineKind::ftn_history?"ftn_history":"restore"},
            {"contents",{{"awaiting_selection",value.awaiting_area_selection},{"areas",areas},{"destinations",destinations},{"restored",std::to_string(value.contents_restored.restored)},{"identical",std::to_string(value.contents_restored.identical)},{"conflicts",std::to_string(value.contents_restored.conflicts)}}},
            {"file_deletion",{{"id",std::to_string(value.file_deletion_change.group.id.value)},{"name",value.file_deletion_change.group.name},{"areas",value.file_deletion_preview.area_count},{"listings",value.file_deletion_preview.catalog_files},{"disk_files",value.file_deletion_preview.disk_files},{"preview_detail",value.file_deletion_preview.detail},{"resume",value.file_deletion_resume},{"delete_disk_files",value.file_deletion_change.delete_disk_files},{"started",value.file_deletion_started}}},
            {"active",value.active},{"completed",value.completed},{"awaiting_confirmation",value.awaiting_confirmation},{"confirmation",value.confirmation},
            {"stage",value.stage},{"detail",value.detail},{"diagnostic",value.diagnostic},{"success",value.success},{"ready",value.ready},{"management_reopened",value.management_reopened},
            {"archive",value.archive.string()},{"archive_files",std::to_string(value.archive_files)},{"findings",findings},{"report",value.health.saved_report?value.health.saved_report->string():""},
            {"live_state_changed",restored.live_state_changed},{"rollback_attempted",restored.rollback_attempted},{"rollback_succeeded",restored.rollback_succeeded},
            {"previous_state",restored.retained_previous_state.string()},{"failed_state",restored.retained_failed_state.string()},{"pre_restore_archive",restored.pre_restore_archive.string()},{"file_prepare_required",value.restored.file_prepare_required},{"ftn_history",{{"utc",std::to_string(value.ftn_history.utc)},{"floor",std::to_string(value.ftn_history.state.high_water)},{"pending_outbound",value.ftn_history.pending_outbound},{"pending_ingress",value.ftn_history.pending_ingress},{"held_outbound",value.ftn_history.held_outbound},{"held_ingress",value.ftn_history.held_ingress},{"report_written",value.ftn_history_result.report_written}}},
            {"awaiting_backup_choice",value.awaiting_backup_choice},{"deletion_reconcile",value.deletion_reconcile},{"deletion_backup",value.deletion_backup.string()},{"backup_verified",value.backup_verified},
            {"deletion",{{"kind",value.deletion_target.kind==app::MessageDeletionKind::group?"group":"echo"},{"id",std::to_string(value.deletion_target.id)},{"name",value.deletion_preview.name},
                {"echoes",std::to_string(value.deletion_preview.echo_count)},{"messages",std::to_string(value.deletion_preview.message_count)},{"permanent",std::to_string(value.deletion_preview.permanent_count)},
                {"committed",value.success&&value.kind==rt::LinuxOfflineKind::message_deletion},{"replayed",value.deletion_result.value&&value.deletion_result.value->replayed}}}});
    });
    api_post("/api/maintenance/start",[&](const auto& request,auto& response){const auto value=parse(request);require_keys(value,{"kind","archive"});const auto kind=value.at("kind").template get<std::string>(),path=value.at("archive").template get<std::string>();
        if((kind!="health"&&kind!="prepare"&&kind!="restore"&&kind!="file_contents"&&kind!="ftn_history")||path.size()>4096||path.find('\0')!=path.npos||((kind=="health"||kind=="prepare"||kind=="ftn_history")&&!path.empty()))throw std::runtime_error("maintenance_request_invalid");
        const auto operation=runtime.request_offline(kind=="health"?rt::LinuxOfflineKind::health:kind=="prepare"?rt::LinuxOfflineKind::prepare:kind=="file_contents"?rt::LinuxOfflineKind::file_contents:kind=="ftn_history"?rt::LinuxOfflineKind::ftn_history:rt::LinuxOfflineKind::restore,std::filesystem::u8path(path),before_restore);
        json_response(response,{{"operation",std::to_string(operation)},{"detail",operation?"Request accepted. Read its actual result; a Restore preview still requires separate confirmation.":"Request refused. Stop the BBS normally and finish other work first."}},operation?202:409);
    });
    api_post("/api/maintenance/contents-select",[&](const auto& request,auto& response){
        const auto value=parse(request);require_keys(value,{"operation","confirmation","areas"});const auto token=value.at("confirmation").template get<std::string>();
        if(token.size()!=96||!value.at("areas").is_array()||value.at("areas").empty()||value.at("areas").size()>10000)throw std::runtime_error("contents_selection_invalid");
        std::vector<std::int64_t> areas;for(const auto& selected:value.at("areas")){const auto id=offline_operation(selected);if(id>=static_cast<std::uint64_t>(INT64_MAX))throw std::runtime_error("contents_selection_invalid");areas.push_back(static_cast<std::int64_t>(id));}
        const auto accepted=runtime.select_restore_contents(offline_operation(value.at("operation")),token,areas);
        json_response(response,{{"accepted",accepted},{"detail",accepted?"Selected areas accepted for destination preview. No file restore is confirmed.":"This selection changed, expired or was already answered. Read the actual preview."}},accepted?202:409);
    });
    api_post("/api/maintenance/cancel",[&](const auto& request,auto& response){const auto value=parse(request);require_keys(value,{"operation"});const auto accepted=runtime.cancel_offline(offline_operation(value.at("operation")));
        json_response(response,{{"accepted",accepted},{"detail",accepted?"Cancellation requested. Wait for the actual result and rollback details.":"This request is no longer active. Read its result."}},accepted?202:409);
    });
    api_post("/api/maintenance/confirm",[&](const auto& request,auto& response){const auto value=parse(request);require_keys(value,{"operation","confirmation"});const auto token=value.at("confirmation").template get<std::string>();if(token.size()!=96&&!(token.size()==32&&runtime.offline_status().kind==rt::LinuxOfflineKind::ftn_history))throw std::runtime_error("restore_confirmation_invalid");
        const auto accepted=runtime.confirm_restore(offline_operation(value.at("operation")),token);
        json_response(response,{{"accepted",accepted},{"detail",accepted?"Restore confirmed. Wait for its actual result; do not repeat the request.":"This preview changed, expired or was already answered. Read its actual result."}},accepted?202:409);
    });
    api_post("/api/config/messages/delete-preview",[&](const auto& request,auto& response){
        const auto value=parse(request);require_keys(value,{"kind","id","revision"});const auto kind=value.at("kind").template get<std::string>();const auto id=offline_operation(value.at("id")),revision=offline_operation(value.at("revision"));
        if((kind!="group"&&kind!="echo")||id>=static_cast<std::uint64_t>(INT64_MAX)||revision>=static_cast<std::uint64_t>(INT64_MAX))throw std::runtime_error("message_deletion_target_invalid");
        const auto operation=runtime.request_message_deletion({kind=="group"?app::MessageDeletionKind::group:app::MessageDeletionKind::echo,static_cast<std::int64_t>(id),static_cast<std::int64_t>(revision)});
        json_response(response,{{"operation",std::to_string(operation)},{"detail",operation?"Exact deletion preview requested. Review optional backup and final counts; no deletion is confirmed.":"Deletion preview refused. Stop the BBS normally and finish other work first."}},operation?202:409);
    });
    api_post("/api/maintenance/deletion-backup",[&](const auto& request,auto& response){
        const auto value=parse(request);require_keys(value,{"operation","confirmation","backup"});const auto token=value.at("confirmation").template get<std::string>();if(token.size()!=96||!value.at("backup").is_boolean())throw std::runtime_error("message_deletion_backup_choice_invalid");
        const auto accepted=runtime.choose_deletion_backup(offline_operation(value.at("operation")),token,value.at("backup").template get<bool>());
        json_response(response,{{"accepted",accepted},{"detail",accepted?"Backup choice accepted. A separate final deletion confirmation is still required.":"This preview changed, expired or was already answered. Read its actual result."}},accepted?202:409);
    });
    api_post("/api/maintenance/deletion-confirm",[&](const auto& request,auto& response){
        const auto value=parse(request);require_keys(value,{"operation","confirmation"});const auto token=value.at("confirmation").template get<std::string>();if(token.size()!=96)throw std::runtime_error("message_deletion_confirmation_invalid");
        const auto accepted=runtime.confirm_message_deletion(offline_operation(value.at("operation")),token);
        json_response(response,{{"accepted",accepted},{"detail",accepted?"Exact deletion confirmation accepted. Read the actual result; do not repeat the request.":"This preview changed, expired or was already answered. Read its actual result."}},accepted?202:409);
    });

    api_post("/api/config/files/delete-preview",[&](const auto& request,auto& response){
        const auto value=parse(request);require_keys(value,{"id","revision","catalog_revision"});const auto id=offline_operation(value.at("id")),revision=offline_operation(value.at("revision")),catalog=offline_operation(value.at("catalog_revision"));
        if(id>=static_cast<std::uint64_t>(INT64_MAX)||revision>=static_cast<std::uint64_t>(INT64_MAX)||catalog>=static_cast<std::uint64_t>(INT64_MAX))throw std::runtime_error("file_deletion_target_invalid");
        const auto operation=runtime.request_file_group_deletion({static_cast<std::int64_t>(id)},static_cast<std::int64_t>(revision),static_cast<std::int64_t>(catalog));
        json_response(response,{{"operation",std::to_string(operation)},{"detail",operation?"File Group preview requested. Choose the disk-file option and review final confirmation; no deletion was confirmed.":"File Group preview refused. Stop the BBS normally and finish other work first."}},operation?202:409);
    });
    api_post("/api/maintenance/file-deletion-confirm",[&](const auto& request,auto& response){
        const auto value=parse(request);require_keys(value,{"operation","confirmation","delete_disk_files"});const auto token=value.at("confirmation").template get<std::string>();if(token.size()!=96||!value.at("delete_disk_files").is_boolean())throw std::runtime_error("file_deletion_confirmation_invalid");
        const auto accepted=runtime.confirm_file_group_deletion(offline_operation(value.at("operation")),token,value.at("delete_disk_files").template get<bool>());
        json_response(response,{{"accepted",accepted},{"detail",accepted?"File Group deletion confirmed. Read its actual result. Closing the browser cannot cancel confirmed deletion.":"The preview expired, changed or was already answered, or its saved disk-file choice differs. Read the actual result."}},accepted?202:409);
    });
