LinuxCompactionSnapshot LinuxRuntimeHost::compaction_status(){std::lock_guard<std::mutex> lock(compaction_state_mutex_);return compaction_;}
bool LinuxRuntimeHost::cancel_compaction(std::uint64_t operation){
    std::lock_guard<std::mutex> lock(compaction_state_mutex_);if(!operation||operation!=compaction_.operation||!compaction_.active)return false;
    compaction_cancel_.store(true);return true;
}
std::uint64_t LinuxRuntimeHost::request_compaction(){
    std::lock_guard<std::mutex> lifecycle(compaction_lifecycle_);
    if(compaction_shutdown_||compaction_status().active||offline_status().active)return 0;
    if(compaction_thread_.joinable())compaction_thread_.join();
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!store_ready()||bbs_owned()||!jobs_||jobs_->busy()||(file_administration_&&file_administration_->active()))return 0;
    if(clamav_){const auto state=clamav_->snapshot();if(state.active||state.queued)return 0;}
    std::uint64_t operation{};
    {std::lock_guard<std::mutex> lock(compaction_state_mutex_);if(compaction_.operation==UINT64_MAX)return 0;operation=compaction_.operation+1;compaction_={};compaction_.operation=operation;compaction_.active=true;compaction_.stage="Queued";}
    compaction_cancel_.store(false);
    try{compaction_thread_=std::thread([this]{compact_worker();});}
    catch(...){std::lock_guard<std::mutex> lock(compaction_state_mutex_);compaction_.active=false;compaction_.completed=true;compaction_.detail="Compaction worker could not start. Nothing was compacted.";return 0;}
    return operation;
}
void LinuxRuntimeHost::compact_worker() noexcept{
    LinuxCompactionSnapshot result=compaction_status();
    const auto stage=[&](const char* text){result.stage=text;std::lock_guard<std::mutex> lock(compaction_state_mutex_);compaction_=result;};
    const auto cancelled=[this]{return compaction_cancel_.load();};
    std::lock_guard<std::recursive_mutex> lock(operations_);
    bool workers_stopped=false;
    try{
        if(!store_ready()||bbs_owned()||!jobs_||jobs_->busy()||(file_administration_&&file_administration_->active()))throw std::runtime_error("compaction_requires_idle_stopped_bbs");
        if(cancelled())throw std::runtime_error("compaction_cancelled_before_backup");
        const auto settings=jobs_->scheduled_job(app::kBackupJobId);if(!settings.available)throw std::runtime_error("compaction_backup_settings_unavailable");
        stage("Preparing verified backup");stop_jobs();workers_stopped=true;stop_ftn();
        if(clamav_)clamav_->stop();
        clamav_.reset();
        if(file_administration_)file_administration_->stop();
        file_administration_.reset();
        ftn_publisher_.reset();ftn_hatch_publisher_.reset();store_.other_board_log({});
        auto backup_journal=durable_;
        if(durable_)durable_->stop();
        durable_.reset();
        if(journal_&&!journal_->flush())throw std::runtime_error("compaction_journal_flush_failed");
        app::ScheduledRun run;run.job_id=app::kBackupJobId;const auto id=store_.identifiers().generate();const char* hex="0123456789ABCDEF";
        for(const auto byte:id.bytes){run.run_id+=hex[byte>>4U];run.run_id+=hex[byte&15U];}
        run.configuration_revision=settings.configuration.revision;run.backup_destination=settings.configuration.backup_destination;run.retained_backups=settings.configuration.retained_backups;run.started_utc=clock_.unix_seconds();
        const auto backup=execute_linux_backup(root_,*store_.storage(),store_.file_bindings(),{},journal_,backup_journal,std::to_string(store_.configuration()->security_policy_revision),run,cancelled,&result.backup);
        // Retire this joined snapshot-only owner before compaction can replace
        // the storage object referenced by its publication adapter.
        backup_journal.reset();
        result.backup_verified=!result.backup.empty();
        if(backup.first!=app::ScheduledJobResult::success)throw std::runtime_error("Required backup failed; compaction refused. "+backup.second);
        if(cancelled())throw std::runtime_error("Cancelled after verified backup; database compaction was not started.");
        const auto work=platform::linux_host::open_owned_directory(root_/"work");const auto name=platform::linux_host::layout::owned_name("compaction-"+run.run_id);
        if(mkdirat(work.get(),name.c_str(),0700)!=0)throw std::runtime_error("compaction_private_folder_creation_failed");
        result.temporary=root_/"work"/name;const auto pin=platform::linux_host::open_owned_directory(result.temporary);
        struct stat work_info{},temp_info{};if(fstat(work.get(),&work_info)!=0||fstat(pin.get(),&temp_info)!=0||work_info.st_dev!=temp_info.st_dev)throw std::runtime_error("compaction_temporary_volume_mismatch");
        stage("Compacting stopped database");result.result=store_.compact_stopped_storage(result.temporary,result.backup,cancelled);
        result.reopened=store_ready();
        if(result.result.status&&result.result.logical_content_verified){
            stage("Reopening management");result.reopened=ensure_journals();
            result.detail=result.reopened?"Compaction completed; logical contents verified. BBS remains stopped.":"Compaction verified, but management recovery is required. Do not repeat the operation.";
        }else result.detail="Compaction was not fully verified. Management recovery is required. Keep the verified backup and preserved work; do not repeat the operation.";
        // Remove only this newly created, verified, still-identical empty directory.
        struct stat current{};if(result.result.status&&result.result.logical_content_verified&&fstatat(work.get(),name.c_str(),&current,AT_SYMLINK_NOFOLLOW)==0&&current.st_dev==temp_info.st_dev&&current.st_ino==temp_info.st_ino)
            (void)unlinkat(work.get(),name.c_str(),AT_REMOVEDIR);
    }catch(const std::exception& error){result.detail=error.what();if(workers_stopped&&store_ready())result.reopened=ensure_journals();}
    catch(...){result.detail="Compaction failed unexpectedly. Preserve work and inspect the verified backup; no success is claimed.";if(workers_stopped&&store_ready())result.reopened=ensure_journals();}
    result.active=false;result.completed=true;result.stage="Finished";
    {std::lock_guard<std::mutex> lock(compaction_state_mutex_);compaction_=std::move(result);}
}
