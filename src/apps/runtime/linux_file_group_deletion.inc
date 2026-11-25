bool LinuxRuntimeHost::confirm_file_group_deletion(std::uint64_t operation,std::string_view token,bool disk){
    std::lock_guard<std::mutex> lock(offline_state_mutex_);
    if(offline_.kind!=LinuxOfflineKind::file_group_deletion||!operation||offline_.operation!=operation||!offline_.active||!offline_.awaiting_confirmation||offline_confirmed_||offline_cancel_.load()||token!=offline_.confirmation||(offline_.file_deletion_resume&&disk!=offline_.file_deletion_change.group.delete_disk_files))return false;
    offline_.file_deletion_change.delete_disk_files=disk;offline_confirmed_=true;offline_.awaiting_confirmation=false;offline_.confirmation.clear();offline_confirmation_.notify_all();return true;
}
std::uint64_t LinuxRuntimeHost::request_file_group_deletion(app::FileGroupId group,std::int64_t revision,std::int64_t catalog_revision){
    std::lock_guard<std::mutex> lifecycle(compaction_lifecycle_);
    if(compaction_shutdown_||compaction_status().active||offline_status().active||group.value<=0||revision<=0||catalog_revision<=0)return 0;
    if(offline_thread_.joinable())offline_thread_.join();
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!store_ready()||bbs_owned()||!jobs_||jobs_->busy()||(file_administration_&&file_administration_->active()))return 0;
    if(clamav_){const auto state=clamav_->snapshot();if(state.active||state.queued)return 0;}
    std::uint64_t operation{};
    {std::lock_guard<std::mutex> state(offline_state_mutex_);if(offline_.operation==UINT64_MAX)return 0;operation=offline_.operation+1;offline_={};offline_.operation=operation;offline_.kind=LinuxOfflineKind::file_group_deletion;offline_.file_deletion_change.group.id=group;offline_.file_deletion_change.group.revision=revision;offline_.file_deletion_change.expected_catalog_revision=catalog_revision;offline_.active=true;offline_.stage="Queued";offline_confirmed_=false;}
    offline_cancel_.store(false);
    try{offline_thread_=std::thread([this]{file_group_deletion_worker();});}
    catch(...){std::lock_guard<std::mutex> state(offline_state_mutex_);offline_.active=false;offline_.completed=true;offline_.detail="File Group review could not start. No deletion was run.";return 0;}
    return operation;
}
void LinuxRuntimeHost::file_group_deletion_worker() noexcept{
    auto result=offline_status();bool quiesced=false;const auto cancelled=[this]{return offline_cancel_.load();};
    const auto publish=[&](const char* stage){result.stage=stage;std::lock_guard<std::mutex> state(offline_state_mutex_);offline_=result;};
    std::lock_guard<std::recursive_mutex> lock(operations_);
    try{
        if(!store_ready()||bbs_owned()||!jobs_||jobs_->busy()||(file_administration_&&file_administration_->active())||cancelled())throw std::runtime_error("file_group_deletion_requires_idle_stopped_bbs");
        publish("Inspecting stopped File Group");stop_jobs();quiesced=true;stop_ftn();if(clamav_)clamav_->stop();clamav_.reset();
        if(file_administration_)file_administration_->stop();
        file_administration_.reset();ftn_publisher_.reset();ftn_hatch_publisher_.reset();store_.other_board_log({});
        if(durable_)durable_->stop();
        durable_.reset();if(journal_&&!journal_->flush())throw std::runtime_error("file_group_deletion_journal_flush_failed");
        // Keep installation ownership and the serialized native store throughout
        // preview and final execution. The browser supplies no disk plan or IDs.
        const auto current=store_.storage()->file_configuration({true,{}});if(!current||!current.value||current.value->policy.catalog_revision!=result.file_deletion_change.expected_catalog_revision)throw std::runtime_error("file_group_deletion_stale_catalog");
        const auto selected=std::find_if(current.value->groups.begin(),current.value->groups.end(),[&](const auto& g){return g.id==result.file_deletion_change.group.id&&g.revision==result.file_deletion_change.group.revision;});
        if(selected==current.value->groups.end())throw std::runtime_error("file_group_deletion_stale_group");
        result.file_deletion_change.group=*selected;result.file_deletion_resume=selected->deletion_operation.valid();result.file_deletion_change.delete_disk_files=result.file_deletion_resume&&selected->delete_disk_files;
        result.file_deletion_change.operation=result.file_deletion_resume?selected->deletion_operation:store_.identifiers().generate();result.file_deletion_change.action=app::FileConfigurationAction::preview_group_delete;
        platform::linux_host::LinuxFileConfigurationPaths paths(root_,store_.file_bindings(),true);app::LocalFileAdministration admin(*store_.storage(),app::LocalAccountAuthority::installation_owner,&paths);
        result.file_deletion_preview=admin.execute(result.file_deletion_change);
        if(result.file_deletion_preview.code!=app::LocalAccountResultCode::ok){result.detail=result.file_deletion_preview.detail;throw std::runtime_error("file_group_deletion_preview_refused");}
        const auto hex=[&]{std::string out;const char* digits="0123456789ABCDEF";const auto value=store_.identifiers().generate();for(const auto byte:value.bytes){out+=digits[byte>>4U];out+=digits[byte&15U];}return out;};
        result.confirmation=hex()+hex()+hex();result.awaiting_confirmation=true;result.detail=result.file_deletion_resume?"Review the saved deletion intent. Its disk-file choice cannot change. Files already deleted cannot be recovered by cancelling.":"Choose whether to keep files on disk or delete them. A separate final confirmation is required; nothing has been deleted by this preview.";publish("Review File Group deletion");
        {std::unique_lock<std::mutex> state(offline_state_mutex_);if(!offline_confirmation_.wait_for(state,std::chrono::minutes(5),[&]{return offline_confirmed_||cancelled();})||cancelled())throw std::runtime_error("file_group_deletion_cancelled_before_attempt");result.file_deletion_change.delete_disk_files=offline_.file_deletion_change.delete_disk_files;}
        if(cancelled())throw std::runtime_error("file_group_deletion_cancelled_before_attempt");
        result.awaiting_confirmation=false;result.confirmation.clear();result.file_deletion_started=true;result.file_deletion_change.action=app::FileConfigurationAction::begin_group_delete;publish("Deleting confirmed File Group");
        // The established journaled native deletion completes or retains an
        // explicit recoverable intent. Closing the browser cannot undo it.
        result.file_deletion_result=admin.execute(result.file_deletion_change);result.success=result.file_deletion_result.code==app::LocalAccountResultCode::ok;
        result.detail=result.file_deletion_result.detail.empty()?(result.success?"File Group deletion completed. The BBS remains stopped.":"File Group deletion was not confirmed. Preserve the board and review the actual catalog and files before continuing."):result.file_deletion_result.detail;
    }catch(const std::exception& error){result.diagnostic=error.what();
        if(result.diagnostic=="file_group_deletion_cancelled_before_attempt")result.detail=result.file_deletion_resume?"Review cancelled. No new deletion attempt was started. The earlier saved deletion intent and any earlier file removals remain.":"Review cancelled or expired before deletion. The File Group, Areas and files remain.";
        else if(result.diagnostic!="file_group_deletion_preview_refused"||result.detail.empty())result.detail="File Group review or deletion could not finish. Preserve the board and inspect the actual catalog and files. No success or rollback is claimed.";
    }catch(...){result.detail="File Group deletion failed unexpectedly. Preserve remaining files and the saved intent; no complete deletion or rollback is claimed.";}
    result.awaiting_confirmation=false;result.confirmation.clear();
    try{result.management_reopened=store_.owns_installation();result.ready=store_ready();if(quiesced&&result.ready)result.ready=ensure_journals();}catch(...){result.ready=false;result.detail+=" Management needs recovery before the BBS can start.";}
    result.active=false;result.completed=true;result.stage="Finished";{std::lock_guard<std::mutex> state(offline_state_mutex_);offline_=std::move(result);}
}
