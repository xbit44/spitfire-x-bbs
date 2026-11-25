LinuxOfflineSnapshot LinuxRuntimeHost::offline_status(){std::lock_guard<std::mutex> lock(offline_state_mutex_);return offline_;}
bool LinuxRuntimeHost::cancel_offline(std::uint64_t operation){
    std::lock_guard<std::mutex> lock(offline_state_mutex_);if(!operation||operation!=offline_.operation||!offline_.active||(offline_.kind==LinuxOfflineKind::file_group_deletion&&(offline_.file_deletion_started||offline_confirmed_))||(offline_.kind==LinuxOfflineKind::ftn_history&&offline_confirmed_))return false;
    offline_cancel_.store(true);offline_confirmation_.notify_all();return true;
}
bool LinuxRuntimeHost::confirm_restore(std::uint64_t operation,std::string_view confirmation){
    std::lock_guard<std::mutex> lock(offline_state_mutex_);
    if((offline_.kind!=LinuxOfflineKind::restore&&offline_.kind!=LinuxOfflineKind::file_contents&&offline_.kind!=LinuxOfflineKind::ftn_history)||!operation||operation!=offline_.operation||!offline_.active||!offline_.awaiting_confirmation||offline_confirmed_||offline_cancel_.load()||confirmation!=offline_.confirmation)return false;
    offline_confirmed_=true;offline_.awaiting_confirmation=false;offline_.confirmation.clear();offline_confirmation_.notify_all();return true;
}
bool LinuxRuntimeHost::select_restore_contents(std::uint64_t operation,std::string_view token,const std::vector<std::int64_t>& selected){
    std::lock_guard<std::mutex> lock(offline_state_mutex_);
    if(offline_.kind!=LinuxOfflineKind::file_contents||!operation||operation!=offline_.operation||!offline_.active||!offline_.awaiting_area_selection||content_selection_||offline_cancel_.load()||token!=offline_.confirmation||selected.empty()||selected.size()>10000)return false;
    auto sorted=selected;std::sort(sorted.begin(),sorted.end());
    if(std::adjacent_find(sorted.begin(),sorted.end())!=sorted.end())return false;
    for(const auto id:sorted)if(std::none_of(offline_.content_areas.begin(),offline_.content_areas.end(),[&](const auto& area){return area.id==id;}))return false;
    content_selection_=std::move(sorted);offline_.awaiting_area_selection=false;offline_.confirmation.clear();offline_confirmation_.notify_all();return true;
}
std::uint64_t LinuxRuntimeHost::request_offline(LinuxOfflineKind kind,const std::filesystem::path& archive,std::function<void()> before_restore){
    std::lock_guard<std::mutex> lifecycle(compaction_lifecycle_);
    if(compaction_shutdown_||compaction_status().active||offline_status().active)return 0;
    if(kind!=LinuxOfflineKind::health&&kind!=LinuxOfflineKind::prepare&&kind!=LinuxOfflineKind::restore&&kind!=LinuxOfflineKind::file_contents&&kind!=LinuxOfflineKind::ftn_history)return 0;
    const bool needs_archive=kind==LinuxOfflineKind::restore||kind==LinuxOfflineKind::file_contents;
    if((needs_archive&&(!archive.is_absolute()||archive.lexically_normal()!=archive))||(!needs_archive&&!archive.empty()))return 0;
    if(offline_thread_.joinable())offline_thread_.join();
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(bbs_owned()||(jobs_&&jobs_->busy())||(file_administration_&&file_administration_->active()))return 0;
    if(clamav_){const auto state=clamav_->snapshot();if(state.active||state.queued)return 0;}
    std::uint64_t operation{};
    {std::lock_guard<std::mutex> lock(offline_state_mutex_);if(offline_.operation==UINT64_MAX)return 0;operation=offline_.operation+1;offline_={};offline_.operation=operation;offline_.kind=kind;offline_.archive=archive;offline_.active=true;offline_.stage="Queued";offline_confirmed_=false;}
    offline_cancel_.store(false);
    content_selection_.reset();
    before_restore_=std::move(before_restore);
    try{offline_thread_=std::thread([this]{offline_worker();});}
    catch(...){std::lock_guard<std::mutex> lock(offline_state_mutex_);offline_.active=false;offline_.completed=true;offline_.detail="Maintenance worker could not start. No operation was run.";return 0;}
    return operation;
}
app::AlphaControlResponse LinuxRuntimeHost::close_stopped_management(){
    if(bbs_owned())return {Code::invalid_request,"maintenance_bbs_running"};
    stop_jobs();stop_ftn();
    if(clamav_)clamav_->stop();
    clamav_.reset();
    if(file_administration_)file_administration_->stop();
    file_administration_.reset();ftn_publisher_.reset();ftn_hatch_publisher_.reset();store_.other_board_log({});
    if(durable_)durable_->stop();
    durable_.reset();
    if(journal_&&!journal_->flush())return {Code::recovery_required,"maintenance_journal_flush_failed"};
    journal_.reset();door_snapshot_cache_.clear();journal_fault_.clear();active_configuration_.reset();pending_configuration_.reset();
    return store_.close_for_maintenance();
}
void LinuxRuntimeHost::offline_worker() noexcept{
    auto result=offline_status();const auto cancelled=[this]{return offline_cancel_.load();};bool closed=false;
    const auto publish=[&](const char* stage){result.stage=stage;std::lock_guard<std::mutex> lock(offline_state_mutex_);offline_=result;};
    std::unique_lock<std::recursive_mutex> lock(operations_);
    try{
        if(bbs_owned()||(jobs_&&jobs_->busy())||(file_administration_&&file_administration_->active()))throw std::runtime_error("maintenance_requires_idle_stopped_bbs");
        if(cancelled())throw std::runtime_error("maintenance_cancelled_before_start");
        publish("Closing stopped management");const auto stopped=close_stopped_management();if(stopped.code!=Code::ok)throw std::runtime_error(stopped.diagnostic_code);closed=true;
        if(result.kind==LinuxOfflineKind::health){
            publish("Checking board health");result.health=platform::linux_host::run_bbs_health_check(root_,cancelled);
            result.success=result.health.stopped&&result.health.saved_report.has_value();result.detail="Health Check finished. Review every finding; a saved report does not mean the board passed all checks.";
        }else if(result.kind==LinuxOfflineKind::prepare){
            publish("Preparing copied file records");const auto opened=store_.open();closed=false;
            const bool deferred=opened.code==Code::ok&&store_.file_bindings()&&store_.file_bindings()->deferred_records()!=0;
            if(opened.code==Code::ok&&!deferred){result.success=true;result.detail="This board is already prepared. Nothing was rebound.";}
            else if(deferred||(opened.code==Code::recovery_required&&opened.diagnostic_code=="linux_file_map_prepare_required")){
                const auto prepared=store_.prepare_copied_file_bindings(cancelled);result.success=prepared.code==Code::ok;
                result.detail=result.success?"Copied/restored file records prepared. The BBS remains stopped.":"Preparation did not finish. Preserve the earlier file records and review Health Check before retrying.";
                if(!result.success)result.diagnostic=prepared.diagnostic_code;
                if(result.success&&store_.file_bindings()->deferred_records()!=0)result.detail+=" Unresolved file records remain: "+std::to_string(store_.file_bindings()->deferred_records())+". Restore matching payloads, then use Prepare again; nothing missing was recreated.";
            }else{result.diagnostic=opened.diagnostic_code;result.detail=opened.diagnostic_code=="interrupted_compaction_requires_verified_recovery"?"Preparation cannot finish an interrupted compaction. Keep the marker and use a verified earlier backup in Restore.":"Preparation needs board recovery first. Run BBS Health Check and review the matching recovery files.";}
        }else if(result.kind==LinuxOfflineKind::ftn_history){
            publish("Verifying FTN history and queued files");
            auto recovery=platform::linux_host::preview_ftn_history_recovery(root_);
            result.ftn_history=recovery->preview();
            if(result.ftn_history.state.recovery_required){
                result.confirmation=recovery->confirmation_token();result.awaiting_confirmation=true;
                result.detail="FTN history recovery is required. Existing message IDs, queue bytes, completed states and separate held items will be preserved. Stop any independently launched BinkD before continuing. The BBS remains stopped.";
                publish("Review required");
                std::unique_lock<std::mutex> review(offline_state_mutex_);
                const auto answered=offline_confirmation_.wait_for(review,std::chrono::minutes(5),[&]{return offline_confirmed_||cancelled();});
                if(!answered||cancelled())throw std::runtime_error("ftn_history_review_cancelled_or_expired");
            }
            result.awaiting_confirmation=false;result.confirmation.clear();
            if(!result.ftn_history.state.recovery_required&&!result.ftn_history.state.recovery_generation){
                result.success=true;result.detail="FTN message ID history is healthy. Recovery is not required. The BBS remains stopped.";
            }else{
                publish("Committing FTN history recovery");
                result.ftn_history_result=recovery->execute(recovery->confirmation_token());
                result.success=bool(result.ftn_history_result.status)&&result.ftn_history_result.receipt.has_value();
                result.detail=result.success?(result.ftn_history_result.report_written?"FTN recovery complete. You can start the BBS.":"FTN history recovery committed. The Logs report is pending. Use Recover FTN History again to retry only the report. The BBS remains stopped."):"FTN recovery did not finish. Preserve the board and review the diagnostic.";
                if(!result.success){result.diagnostic=result.ftn_history_result.status.operation;result.detail=app::ftn_history_recovery_problem(result.diagnostic);}
            }
            recovery.reset();
        }else if(result.kind==LinuxOfflineKind::file_contents){
#include "linux_file_contents_restore.inc"
        }else{
            publish("Inspecting selected backup");auto restore=platform::linux_host::LinuxCoreRestore::preview(root_,result.archive,cancelled,SFX_PRODUCT_VERSION_SHORT_STRING);
            result.archive_files=restore->inspection().files.size();result.confirmation=restore->confirmation_token();result.awaiting_confirmation=true;
            result.detail="Verified backup inspected. Confirm replacement of this stopped board's core files. Live Files and durable intake remain separate.";
            publish("Review required");
            {
                std::unique_lock<std::mutex> lock(offline_state_mutex_);
                // Browser closure never accepts a restore. An abandoned preview
                // releases its stopped ownership after a bounded review window.
                const auto answered=offline_confirmation_.wait_for(lock,std::chrono::minutes(5),[&]{return offline_confirmed_||cancelled();});
                if(!answered||cancelled())throw std::runtime_error("restore_review_cancelled_or_expired_no_publication");
            }
            result.awaiting_confirmation=false;result.confirmation.clear();publish("Restoring verified core");
            // Core storage and every worker are closed; Restore retains the
            // installation lock. Let already-entered management requests finish
            // their unavailable-store response before revoking web sessions.
            // Do not invert the request gate and host-operation lock order.
            if(before_restore_){lock.unlock();try{before_restore_();}catch(...){lock.lock();throw;}lock.lock();}
            result.restored=restore->execute(restore->confirmation_token(),cancelled);result.success=result.restored.restore.result.ok();
            result.detail=result.success?"Verified core restored; previous state retained. The BBS remains stopped.":"Restore did not complete. Review retained state and rollback details before another request.";
            if(result.success&&result.restored.ftn_recovery_required)result.detail+=" FTN sending is paused: message ID history needs recovery. Use Recover FTN History in Backup & Restore.";
            if(!result.success)result.diagnostic=result.restored.restore.result.detail;
            restore.reset();
        }
    }catch(const std::exception& error){
        result.diagnostic=error.what();
        if(result.diagnostic=="ftn_history_review_cancelled_or_expired")result.detail="FTN history recovery was cancelled or expired. No history state was changed.";
        else if(result.kind==LinuxOfflineKind::ftn_history)result.detail=app::ftn_history_recovery_problem(result.diagnostic);
        else if(result.diagnostic=="restore_review_cancelled_or_expired_no_publication")result.detail="Restore review was cancelled or expired. No core files were replaced.";
        else if(result.diagnostic=="contents_restore_selection_cancelled_or_expired"||result.diagnostic=="contents_restore_review_cancelled_or_expired")result.detail="File contents review was cancelled or expired. No file contents were restored.";
        else if(result.diagnostic=="contents_restore_point_has_no_selected_area_snapshot")result.detail="This backup has no selected-area file contents. It can still be reviewed separately for Core Restore. Nothing was restored.";
        else if(result.diagnostic=="maintenance_cancelled_before_start")result.detail="Maintenance was cancelled before it started. No operation was run.";
        else if(result.diagnostic=="private_directory_filesystem_unqualified")result.detail="This folder uses a filesystem that is not qualified for SFX. Choose a private backup on a supported local filesystem.";
        else if(result.diagnostic=="restore_source_has_unresolved_compaction")result.detail="This backup includes an unresolved compaction. Choose an earlier verified backup; retained evidence was preserved.";
        else result.detail="Maintenance could not finish. Preserve this board and its work files. Check the Health report, selected backup and folder access before retrying.";
    }
    catch(...){result.detail="Maintenance failed unexpectedly. Preserve this board and review its recovery files; no success is claimed.";}
    result.awaiting_confirmation=false;result.awaiting_area_selection=false;result.confirmation.clear();
    try{
        if(closed){publish("Reopening stopped management");(void)store_.open();}
        result.management_reopened=store_.owns_installation();
        result.ready=store_ready();
        if(result.ready){epoch_=store_.identifiers().generate();generations_={};result.ready=ensure_journals();}
        if(!result.ready){if(result.diagnostic.empty())result.diagnostic=store_.diagnostic();result.detail+=" The board still requires recovery. Review Health Check or Restore before starting it.";}
    }catch(...){result.ready=false;result.detail+=" Stopped management could not reopen; preserve all recovery files.";}
    result.active=false;result.completed=true;result.stage="Finished";
    {std::lock_guard<std::mutex> lock(offline_state_mutex_);offline_=std::move(result);}
}
