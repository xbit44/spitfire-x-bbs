bool LinuxRuntimeHost::choose_deletion_backup(std::uint64_t operation,std::string_view token,bool backup){
    std::lock_guard<std::mutex> lock(offline_state_mutex_);
    if(offline_.kind!=LinuxOfflineKind::message_deletion||!operation||offline_.operation!=operation||!offline_.active||!offline_.awaiting_backup_choice||deletion_backup_choice_||offline_cancel_.load()||token!=offline_.confirmation)return false;
    deletion_backup_choice_=backup;offline_.awaiting_backup_choice=false;offline_.confirmation.clear();offline_confirmation_.notify_all();return true;
}
bool LinuxRuntimeHost::confirm_message_deletion(std::uint64_t operation,std::string_view token){
    std::lock_guard<std::mutex> lock(offline_state_mutex_);
    if(offline_.kind!=LinuxOfflineKind::message_deletion||!operation||offline_.operation!=operation||!offline_.active||!offline_.awaiting_confirmation||offline_confirmed_||offline_cancel_.load()||token!=offline_.confirmation)return false;
    offline_confirmed_=true;offline_.awaiting_confirmation=false;offline_.confirmation.clear();offline_confirmation_.notify_all();return true;
}
std::uint64_t LinuxRuntimeHost::request_message_deletion(const app::MessageDeletionTarget& target){
    std::lock_guard<std::mutex> lifecycle(compaction_lifecycle_);
    if(compaction_shutdown_||compaction_status().active||offline_status().active||target.id<=0||target.expected_revision<=0||(target.kind!=app::MessageDeletionKind::group&&target.kind!=app::MessageDeletionKind::echo))return 0;
    if(offline_thread_.joinable())offline_thread_.join();
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!store_ready()||bbs_owned()||!jobs_||jobs_->busy()||(file_administration_&&file_administration_->active()))return 0;
    if(clamav_){const auto state=clamav_->snapshot();if(state.active||state.queued)return 0;}
    std::uint64_t operation{};
    {std::lock_guard<std::mutex> state(offline_state_mutex_);if(offline_.operation==UINT64_MAX)return 0;operation=offline_.operation+1;offline_={};offline_.operation=operation;offline_.kind=LinuxOfflineKind::message_deletion;offline_.deletion_target=target;offline_.active=true;offline_.stage="Queued";offline_confirmed_=false;deletion_backup_choice_.reset();}
    offline_cancel_.store(false);
    try{offline_thread_=std::thread([this]{message_deletion_worker();});}
    catch(...){std::lock_guard<std::mutex> state(offline_state_mutex_);offline_.active=false;offline_.completed=true;offline_.detail="Deletion preview could not start. No messages were deleted.";return 0;}
    return operation;
}
void LinuxRuntimeHost::message_deletion_worker() noexcept{
    auto result=offline_status();const auto cancelled=[this]{return offline_cancel_.load();};bool quiesced=false;
    const auto publish=[&](const char* stage){result.stage=stage;std::lock_guard<std::mutex> state(offline_state_mutex_);offline_=result;};
    std::lock_guard<std::recursive_mutex> lock(operations_);
    try{
        if(!store_ready()||bbs_owned()||!jobs_||jobs_->busy()||(file_administration_&&file_administration_->active())||cancelled())throw std::runtime_error("message_deletion_requires_idle_stopped_bbs");
        const auto settings=jobs_->scheduled_job(app::kBackupJobId);if(!settings.available)throw std::runtime_error("message_deletion_backup_settings_unavailable");
        publish("Preparing stopped deletion review");stop_jobs();quiesced=true;stop_ftn();
        if(clamav_)clamav_->stop();
        clamav_.reset();if(file_administration_)file_administration_->stop();file_administration_.reset();ftn_publisher_.reset();ftn_hatch_publisher_.reset();store_.other_board_log({});
        auto backup_journal=durable_;
        if(durable_)durable_->stop();
        durable_.reset();if(journal_&&!journal_->flush())throw std::runtime_error("message_deletion_journal_flush_failed");
        // Store retains its installation lock continuously from preview through
        // optional backup, final confirmation, commit and exact reconciliation.
        const auto preview=store_.storage()->preview_message_deletion({true,{}},result.deletion_target,cancelled);
        if(!preview||!preview.value)throw std::runtime_error("message_deletion_preview_stale_or_unavailable");
        result.deletion_preview=*preview.value;const auto receipt=store_.identifiers().generate();
        const auto hex=[&]{std::string out;const char* digits="0123456789ABCDEF";const auto value=store_.identifiers().generate();for(const auto byte:value.bytes){out+=digits[byte>>4U];out+=digits[byte&15U];}return out;};
        result.confirmation=hex()+hex()+hex();result.awaiting_backup_choice=true;result.detail="Exact area inspected. Choose whether to make a verified backup. Either choice still requires a separate final deletion confirmation.";publish("Choose optional backup");
        bool backup{};
        {std::unique_lock<std::mutex> state(offline_state_mutex_);if(!offline_confirmation_.wait_for(state,std::chrono::minutes(5),[&]{return deletion_backup_choice_.has_value()||cancelled();})||cancelled())throw std::runtime_error("message_deletion_cancelled_before_commit");backup=*deletion_backup_choice_;}
        result.awaiting_backup_choice=false;result.confirmation.clear();
        if(backup){
            publish("Creating requested backup");app::ScheduledRun run;run.job_id=app::kBackupJobId;run.run_id=hex();run.configuration_revision=settings.configuration.revision;run.backup_destination=settings.configuration.backup_destination;run.retained_backups=settings.configuration.retained_backups;run.started_utc=clock_.unix_seconds();
            const auto saved=execute_linux_backup(root_,*store_.storage(),store_.file_bindings(),{},journal_,backup_journal,std::to_string(store_.configuration()->security_policy_revision),run,cancelled,&result.deletion_backup);
            backup_journal.reset();
            result.backup_verified=!result.deletion_backup.empty();if(saved.first!=app::ScheduledJobResult::success)throw std::runtime_error("message_deletion_requested_backup_failed: "+saved.second);
        }
        backup_journal.reset();
        for(unsigned attempt=0;attempt<2;++attempt){
            if(cancelled())throw std::runtime_error(attempt?"message_deletion_reconciliation_cancelled":"message_deletion_cancelled_before_commit");
            {std::lock_guard<std::mutex> state(offline_state_mutex_);offline_confirmed_=false;}
            result.awaiting_confirmation=true;result.deletion_reconcile=attempt!=0;result.confirmation=hex()+hex()+hex();result.detail=attempt?"Deletion did not report success. Review the actual result. You may explicitly reconcile the same exact operation once; no target will be substituted.":"Review the exact area and counts. Delete includes Permanent messages. Cancel keeps this area; any verified backup remains.";publish(attempt?"Review uncertain deletion":"Final deletion review");
            {std::unique_lock<std::mutex> state(offline_state_mutex_);if(!offline_confirmation_.wait_for(state,std::chrono::minutes(5),[&]{return offline_confirmed_||cancelled();})||cancelled())throw std::runtime_error(attempt?"message_deletion_reconciliation_cancelled":"message_deletion_cancelled_before_commit");}
            result.awaiting_confirmation=false;result.confirmation.clear();publish(attempt?"Reconciling exact deletion":"Deleting confirmed area");
            result.deletion_result=store_.storage()->commit_message_deletion({true,{}},receipt,result.deletion_preview,cancelled);
            result.success=bool(result.deletion_result)&&result.deletion_result.value.has_value();if(result.success)break;
        }
        result.detail=result.success?"Exact confirmed area and its included messages deleted. Unrelated areas, private mail, accounts and backups were preserved. The BBS remains stopped.":"Deletion did not report success. Inspect the actual catalog and retained backup; no complete deletion or rollback is claimed.";
    }catch(const std::exception& error){result.diagnostic=error.what();
        result.detail=result.diagnostic=="message_deletion_cancelled_before_commit"?"Deletion review was cancelled or expired before commit. The selected area and messages remain; any verified backup was retained.":result.diagnostic.rfind("message_deletion_requested_backup_failed",0)==0?"The requested backup did not finish successfully. This deletion stopped without a no-backup fallback. Preserve the backup and work files and review the result.":"Deletion preview or execution did not finish. Preserve the current board and any backup; inspect the actual catalog before another attempt.";
    }catch(...){result.detail="Deletion failed unexpectedly. Preserve the current board and backups; no success is claimed.";}
    result.awaiting_confirmation=false;result.awaiting_backup_choice=false;result.confirmation.clear();
    try{result.management_reopened=store_.owns_installation();result.ready=store_ready();if(quiesced&&result.ready)result.ready=ensure_journals();}
    catch(...){result.ready=false;result.detail+=" Management needs recovery before the BBS can start.";}
    result.active=false;result.completed=true;result.stage="Finished";{std::lock_guard<std::mutex> state(offline_state_mutex_);offline_=std::move(result);}
}
