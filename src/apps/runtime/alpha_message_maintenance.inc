// Same worker, cancellation, protected publication and ownership as backup.
// This is not a parallel timer/service and never owns caller permission.
namespace {
std::optional<application::Identifier128> maintenance_identifier(std::string_view text){
    if(text.size()!=32)return {};application::Identifier128 value;
    const auto nibble=[](char ch)->int{return ch>='0' && ch<='9'?ch-'0':ch>='A' && ch<='F'?ch-'A'+10:-1;};
    for(std::size_t n=0;n<16;++n){const auto a=nibble(text[n*2]),b=nibble(text[n*2+1]);if(a<0 || b<0)return {};value.bytes[n]=static_cast<std::uint8_t>(a*16+b);}
    return value.valid()?std::optional(value):std::nullopt;
}
application::ScheduledJobResult maintenance_job_result(application::MessageMaintenanceOutcome outcome){
    using M=application::MessageMaintenanceOutcome;using J=application::ScheduledJobResult;
    switch(outcome){case M::success:return J::success;case M::partial:return J::partial;case M::skipped:return J::skipped;case M::interrupted:return J::interrupted;default:return J::failed;}
}
std::string maintenance_summary(const application::MessageMaintenanceRun& run){
    std::int64_t overage{};for(const auto& e:run.echoes)overage+=e.protected_overage;
    return "Committed public removals="+std::to_string(run.removed_public)+"; unread inbox expiry="+std::to_string(run.expired_unread)+
        "; batches="+std::to_string(run.committed_batches)+"; protected overage="+std::to_string(overage)+"; reason="+std::to_string(static_cast<unsigned>(run.reason));
}
}
application::RuntimeScheduler* AlphaRuntimeHost::scheduler_for_job(std::string_view job) const{
    if(job==application::kBackupJobId)return scheduler_.get();
    if(job==application::kMessageRetentionJobId)return message_schedulers_[0].get();
    if(job==application::kMessageIntegrityJobId)return message_schedulers_[1].get();return nullptr;
}
bool AlphaRuntimeHost::scheduler_has_work_locked()const{
    if(pending_scheduler_run_ || (scheduler_ && scheduler_->state().active))return true;
    for(const auto& job:message_schedulers_)if(job && job->state().active)return true;return false;
}
bool AlphaRuntimeHost::scheduler_has_work()const{
    std::lock_guard<std::mutex> lock(scheduler_mutex_);return scheduler_has_work_locked();
}
void AlphaRuntimeHost::initialize_message_schedulers_locked(){
    const auto reconciled=storage_->recover_interrupted_message_maintenance({true,{}});
    for(std::size_t index=0;index<2;++index){
        const auto kind=static_cast<application::MessageMaintenanceKind>(index);const auto* id=application::message_job_id(kind);
        auto& diagnostic=message_scheduler_diagnostics_[index];
        try{
            const auto config_file=platform::win32::read_scheduler_configuration_file(installation_root_,owner_sid_,id);
            const bool new_config=config_file.state==platform::win32::ConfigurationFileState::missing;
            auto config=application::default_message_scheduler_configuration(id);
            if(new_config){const auto saved=platform::win32::publish_scheduler_configuration_file(installation_root_,owner_sid_,config);if(!saved){diagnostic=saved.operation;continue;}}
            else if(config_file.value)config=*config_file.value;else{diagnostic=config_file.diagnostic_code;continue;}
            const auto state_file=platform::win32::read_scheduler_state_file(installation_root_,owner_sid_,id);
            auto state=application::default_message_scheduler_state(id);state.configuration_revision=config.revision;
            if(state_file.value)state=*state_file.value;
            else if(state_file.state!=platform::win32::ConfigurationFileState::missing){diagnostic=state_file.diagnostic_code;continue;}
            else if(kind==application::MessageMaintenanceKind::integrity && !new_config)state.requires_fresh_check=true;
            if(!reconciled){diagnostic="message_maintenance_reconciliation_failed";continue;}
            const auto zone=configuration_->bbs_time_zone;
            auto candidate=std::make_unique<application::RuntimeScheduler>(config,state,
                [zone](std::int64_t after,std::uint8_t h,std::uint8_t m){return platform::win32::next_bbs_daily_run(zone,after,h,m);},
                [zone](std::int64_t after,std::uint8_t h,std::uint8_t m,std::optional<std::uint8_t> day,std::int64_t consumed){return platform::win32::next_bbs_maintenance_run(zone,after,h,m,day,consumed);});
            if(!candidate->recover_on_start(now_utc_seconds())){diagnostic="message_schedule_time_unavailable";continue;}
            if(state.active){
                const auto previous_id=maintenance_identifier(state.active_run_id);
                const auto actual=previous_id?storage_->message_maintenance_run({true,{}},*previous_id):application::StorageResult<application::MessageMaintenanceRun>{};
                if(actual && actual.value){
                    auto recovered=candidate->state();
                    recovered.last_detail="Runtime interrupted; "+maintenance_summary(*actual.value)+"; no retry";
                    if(actual.value->removed_public || actual.value->expired_unread)recovered.last_result=application::ScheduledJobResult::partial;
                    if(!recovered.history.empty()){recovered.history.back().detail=recovered.last_detail;recovered.history.back().result=recovered.last_result;}
                    candidate->replace_state(std::move(recovered));
                }
            }
            const auto saved=platform::win32::publish_scheduler_state_file(installation_root_,owner_sid_,candidate->state());
            if(!saved){diagnostic=saved.operation;continue;}
            message_schedulers_[index]=std::move(candidate);diagnostic="message_maintenance_ready";
        }catch(...){diagnostic="message_maintenance_initialization_failed";}
    }
}
application::MessageMaintenanceStatus AlphaRuntimeHost::message_maintenance_status(application::MessageMaintenanceKind kind){
    std::lock_guard<std::mutex> host_lock(mutex_);std::lock_guard<std::mutex> lock(scheduler_mutex_);
    application::MessageMaintenanceStatus result;if(kind>application::MessageMaintenanceKind::integrity){result.diagnostic="message_job_unknown";return result;}
    const auto index=static_cast<std::size_t>(kind);const auto* id=application::message_job_id(kind);
    result.kind=kind;result.configuration=application::default_message_scheduler_configuration(id);result.state=application::default_message_scheduler_state(id);
    result.diagnostic=message_scheduler_diagnostics_[index];result.shared_verification_required=!message_schedulers_[1] || message_schedulers_[1]->state().requires_fresh_check;
    if(!storage_ || !message_schedulers_[index] || presentation_state_!=AlphaRuntimePresentationState::ready)return result;
    result.configuration=message_schedulers_[index]->configuration();result.state=message_schedulers_[index]->state();
    const auto held=storage_->message_integrity_holds({true,{}});if(!held || !held.value){result.diagnostic="message_holds_unavailable";return result;}result.holds=*held.value;
    const auto current_id=maintenance_identifier(result.state.active?result.state.active_run_id:result.state.history.empty()?std::string{}:result.state.history.back().run_id);
    if(current_id){const auto actual=storage_->message_maintenance_run({true,{}},*current_id);if(actual && actual.value)result.latest=*actual.value;
        else if(actual.status.code!=application::StorageErrorCode::not_found){result.diagnostic="message_report_unavailable";return result;}}
    result.available=true;return result;
}
AlphaRuntimeHost::MessageJobOutcome AlphaRuntimeHost::execute_scheduler_message(const application::ScheduledRun& run)noexcept{
    using O=application::MessageMaintenanceOutcome;using R=application::MessageMaintenanceReason;using J=application::ScheduledJobResult;
    const bool integrity=run.job_id==application::kMessageIntegrityJobId;
    const auto operation=maintenance_identifier(run.run_id);if(!operation)return {J::failed,"message_operation_identity_invalid",integrity?std::optional(true):std::nullopt};
    try{
        if(integrity){
            const auto checked=storage_->check_message_integrity({true,{}},*operation,[this]{return scheduler_cancel_.load();});
            if(!checked || !checked.value)return {J::failed,"Integrity outcome not confirmed; fresh current check required",true};
            const auto& result=*checked.value;std::int64_t findings{};for(const auto& item:result.findings)findings+=item.count;
            return {maintenance_job_result(result.run.outcome),"Integrity "+std::string(result.complete?(result.clean?"clean":"findings"):"incomplete")+
                "; findings="+std::to_string(findings)+"; cleared corruption hold="+(result.cleared_hold?"Yes":"No")+
                "; reason="+std::to_string(static_cast<unsigned>(result.run.reason)),!result.complete || result.run.replayed};
        }
        if(run.job_id!=application::kMessageRetentionJobId)return {J::failed,"message_job_unknown",{}};
        const auto begun=storage_->begin_message_retention({true,{}},*operation);
        if(!begun || !begun.value)return {J::failed,"Retention did not begin; no removal claimed",{}};
        if(begun.value->outcome!=O::running)return {maintenance_job_result(begun.value->outcome),maintenance_summary(*begun.value),{}};
        bool guarded{};{std::lock_guard<std::mutex> lock(scheduler_mutex_);guarded=!message_schedulers_[1] || message_schedulers_[1]->state().requires_fresh_check;}
        O outcome=O::success;R reason=R::none;
        if(guarded){outcome=O::skipped;reason=R::incomplete;}
        else for(std::int64_t sequence=1;;++sequence){
            if(scheduler_cancel_.load()){outcome=O::interrupted;reason=R::cancelled;break;}
            const auto batch=storage_->message_retention_batch({true,{}},*operation,sequence,[this]{return scheduler_cancel_.load();});
            if(!batch || !batch.value){outcome=O::failed;
                reason=batch.status.operation=="message_maintenance_cancelled"?R::cancelled:batch.status.operation=="message_maintenance_deadline"?R::deadline:
                    batch.status.code==application::StorageErrorCode::busy?R::busy:R::storage_failure;
                if(reason==R::cancelled)outcome=O::interrupted;break;}
            if(batch.value->complete){reason=batch.value->policy_changed?R::policy_changed:batch.value->run.reason;
                if(reason!=R::none)outcome=batch.value->run.removed_public || batch.value->run.expired_unread?O::partial:O::skipped;break;}
            if(sequence==INT64_MAX){outcome=O::failed;reason=R::storage_failure;break;}
            std::this_thread::yield();
        }
        auto finished=storage_->finish_message_retention({true,{}},*operation,outcome,reason);
        if(!finished || !finished.value){
            const auto actual=storage_->message_maintenance_run({true,{}},*operation);
            return {J::failed,actual && actual.value?"Completion unconfirmed; "+maintenance_summary(*actual.value):"Completion unconfirmed; committed totals unavailable",{}};
        }
        return {maintenance_job_result(finished.value->outcome),guarded?"Fresh current integrity check required; "+maintenance_summary(*finished.value):maintenance_summary(*finished.value),{}};
    }catch(...){return {J::failed,"Message maintenance failed; no optimistic completion claimed",integrity?std::optional(true):std::nullopt};}
}
application::AlphaControlResponse AlphaRuntimeHost::run_message_maintenance_now(application::MessageMaintenanceKind kind){
    std::lock_guard<std::mutex> host_lock(mutex_);std::lock_guard<std::mutex> lock(scheduler_mutex_);
    if(kind>application::MessageMaintenanceKind::integrity)return {LocalControlResponseCode::invalid_request,"message_job_unknown"};
    if(presentation_state_!=AlphaRuntimePresentationState::ready || scheduler_paused_.load())return {LocalControlResponseCode::shutting_down,"message_runtime_not_ready"};
    auto* job=message_schedulers_[static_cast<std::size_t>(kind)].get();
    if(!job || scheduler_has_work_locked())return {LocalControlResponseCode::invalid_request,job?"scheduler_work_already_active":"message_job_unavailable"};
    const auto before=job->state();const auto run=job->begin_manual(now_utc_seconds(),identifier_text(identifiers_.generate()));
    if(!run)return {LocalControlResponseCode::invalid_request,"message_job_not_started"};
    const auto saved=platform::win32::publish_scheduler_state_file(installation_root_,owner_sid_,job->state());
    if(!saved){job->replace_state(before);return {LocalControlResponseCode::configuration_failure,"message_job_state_publication_failed"};}
    pending_scheduler_run_=*run;scheduler_wake_.notify_all();return {LocalControlResponseCode::ok,"message_job_queued"};
}
application::AlphaControlResponse AlphaRuntimeHost::configure_message_schedule(const application::MessageMaintenanceScheduleRequest& request){
    std::lock_guard<std::mutex> host_lock(mutex_);std::lock_guard<std::mutex> lock(scheduler_mutex_);
    if(application::encode_message_maintenance_schedule(request).empty())return {LocalControlResponseCode::invalid_request,"message_schedule_invalid"};
    if(presentation_state_!=AlphaRuntimePresentationState::ready)return {LocalControlResponseCode::shutting_down,"message_runtime_not_ready"};
    const auto index=static_cast<std::size_t>(request.kind);auto& job=message_schedulers_[index];
    if(!job)return {LocalControlResponseCode::configuration_failure,"message_job_unavailable"};
    const auto old=job->configuration();const auto old_state=job->state();
    if(old.revision!=request.expected_revision || old.revision==UINT64_MAX)return {LocalControlResponseCode::invalid_request,"message_schedule_revision_conflict"};
    auto config=old;++config.revision;config.enabled=request.enabled;config.local_hour=request.hour;config.local_minute=request.minute;config.local_weekday=request.weekday;
    const auto zone=configuration_->bbs_time_zone;
    auto candidate=std::make_unique<application::RuntimeScheduler>(old,old_state,
        [zone](std::int64_t after,std::uint8_t h,std::uint8_t m){return platform::win32::next_bbs_daily_run(zone,after,h,m);},
        [zone](std::int64_t after,std::uint8_t h,std::uint8_t m,std::optional<std::uint8_t> day,std::int64_t slot){return platform::win32::next_bbs_maintenance_run(zone,after,h,m,day,slot);});
    if(!candidate->reconfigure(config,now_utc_seconds()))return {LocalControlResponseCode::configuration_failure,"message_schedule_time_unavailable"};
    const auto config_saved=platform::win32::publish_scheduler_configuration_file(installation_root_,owner_sid_,config);
    const auto state_saved=config_saved?platform::win32::publish_scheduler_state_file(installation_root_,owner_sid_,candidate->state()):application::StorageStatus{application::StorageErrorCode::io,"message_configuration_unconfirmed"};
    if(!config_saved || !state_saved){
        const auto config_back=platform::win32::publish_scheduler_configuration_file(installation_root_,owner_sid_,old);
        const auto state_back=platform::win32::publish_scheduler_state_file(installation_root_,owner_sid_,old_state);
        if(!config_back || !state_back){job.reset();message_scheduler_diagnostics_[index]="message_schedule_rollback_failed";}
        return {LocalControlResponseCode::configuration_failure,!config_back || !state_back?"message_schedule_rollback_failed":"message_schedule_save_failed"};
    }
    job=std::move(candidate);message_scheduler_diagnostics_[index]="message_maintenance_ready";scheduler_wake_.notify_all();
    return {LocalControlResponseCode::ok,"message_schedule_saved_no_purge"};
}
