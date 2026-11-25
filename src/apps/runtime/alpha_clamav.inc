// Optional definitions ownership belongs to this Runtime instance, not caller
// workers or the upload-scanning switch. No daemon/global scheduled task.
application::AlphaControlResponse AlphaRuntimeHost::manual_clamav_notice(const application::ManualClamAvNotice& notice){
    namespace app=application;namespace native=platform::win32;
    if(!native::clamav_available())return {app::LocalControlResponseCode::invalid_request,"optional_component_unavailable_on_this_os"};
    if(app::encode_manual_clamav_notice(notice).empty())return {app::LocalControlResponseCode::invalid_request,"clamav_notice_invalid"};
    std::lock_guard<std::mutex> lock(clamav_notice_mutex_);
    native::ClamAvStatus verified_status;
    if(!journal_)return {app::LocalControlResponseCode::internal_failure,"clamav_events_unavailable"};
    auto found=std::find_if(clamav_notices_.begin(),clamav_notices_.end(),[&](const auto& n){return n.operation==notice.operation;});
    if(notice.phase==app::ManualClamAvPhase::started){
        if(found!=clamav_notices_.end())return {app::LocalControlResponseCode::ok,"clamav_notice_already_recorded"};
        if(clamav_notices_.size()==32){const auto old=std::find_if(clamav_notices_.begin(),clamav_notices_.end(),[](const auto& n){return n.phase!=app::ManualClamAvPhase::started;});
            if(old==clamav_notices_.end())return {app::LocalControlResponseCode::invalid_request,"clamav_notice_bound"};clamav_notices_.erase(old);}
        clamav_notices_.push_back(notice);
    }else{
        if(found==clamav_notices_.end())return {app::LocalControlResponseCode::invalid_request,"clamav_notice_missing_start"};
        if(found->phase!=app::ManualClamAvPhase::started)return app::encode_manual_clamav_notice(*found)==app::encode_manual_clamav_notice(notice)?
            app::AlphaControlResponse{app::LocalControlResponseCode::ok,"clamav_notice_already_recorded"}:
            app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,"clamav_notice_changed"};
        if(notice.phase==app::ManualClamAvPhase::verified){
            try{
            const auto receipt=native::installation::read_receipt(installation_root_,owner_sid_);
            const auto status=native::inspect_clamav_status(installation_root_,owner_sid_,receipt.id);verified_status=status;
            if(!status.history_available||status.last_mode!=native::ClamAvUpdateMode::manual||status.last_attempt!=notice.attempt_utc||
               status.last_result!=notice.diagnostic||!status.definitions_verified)
                return {app::LocalControlResponseCode::invalid_request,"clamav_notice_success_unverified"};
            }catch(...){return {app::LocalControlResponseCode::invalid_request,"clamav_notice_success_unverified"};}
        }
        *found=notice;
    }
    const bool failed=notice.phase==app::ManualClamAvPhase::failed;
    app::OperationalContext context;context.resource_id=1;
    context.resource_name=notice.phase==app::ManualClamAvPhase::started?"Manual ClamAV definition update started":
        notice.phase==app::ManualClamAvPhase::verified?"Manual ClamAV definition update completed; definitions verified":
        notice.phase==app::ManualClamAvPhase::cancelled?"Manual ClamAV definition update cancelled":"Manual ClamAV definition update failed";
    context.diagnostic_code=notice.phase==app::ManualClamAvPhase::started?"clamav_manual_started":notice.diagnostic;
    if(notice.phase==app::ManualClamAvPhase::verified){
        context.resource_name="Manual ClamAV: "+native::clamav_change_text(verified_status.change);
        if(verified_status.databases[1])context.resource_name+="; daily "+std::to_string(verified_status.databases[1]->version);
        journal_->clamav_health(false,true,true,{});
    }else if(failed){
        native::ClamAvStatus status;try{const auto receipt=native::installation::read_receipt(installation_root_,owner_sid_);status=native::inspect_clamav_status(installation_root_,owner_sid_,receipt.id);}catch(...){}
        journal_->clamav_health(false,false,status.definitions_verified,notice.diagnostic);
        context.resource_name+=status.definitions_verified?"; validated definitions retained":"; definitions unavailable/unverified";
    }
    journal_->emit({failed?app::OperationalKind::file_maintenance_problem:app::OperationalKind::file_maintenance,
        failed?app::OperationalOutcome::unavailable:notice.phase==app::ManualClamAvPhase::cancelled?app::OperationalOutcome::cancelled:app::OperationalOutcome::normal,std::move(context)});
    return {app::LocalControlResponseCode::ok,"clamav_notice_recorded"};
}
bool AlphaRuntimeHost::initialize_clamav_locked(){
    namespace native=platform::win32;
    try{
        if(native::windows7_no_av()){
            const auto configuration=storage_->file_configuration({true,{}});
            if(!configuration||!configuration.value)return false;
            if(configuration.value->policy.scan_uploads){
                application::FileConfigurationChange change;change.operation=identifiers_.generate();
                change.action=application::FileConfigurationAction::policy;change.expected_catalog_revision=configuration.value->policy.catalog_revision;
                change.policy=configuration.value->policy;change.policy.scan_uploads=false;
                if(!storage_->change_file_configuration({true,{}},change))return false;
            }
            return true; // No component probe, directories, vendor process or updater thread.
        }
        if(!native::clamav_available())return true;
        if(!native::clamav_installed(installation_root_))return true;
        // Revision1 is the untouched seed. Every explicit saved policy advances
        // it, including saving Off unchanged; never reset a saved choice.
        auto configuration=storage_->file_configuration({true,{}});
        if(!configuration||!configuration.value)return false;
        if(configuration.value->policy.revision==1){
            application::FileConfigurationChange change;change.operation=identifiers_.generate();
            change.action=application::FileConfigurationAction::policy;change.expected_catalog_revision=configuration.value->policy.catalog_revision;
            change.policy=configuration.value->policy;change.policy.scan_uploads=true;
            if(!storage_->change_file_configuration({true,{}},change))return false;
        }
        // The saved scan policy is essential; optional engine/definition work
        // starts on its own worker after core initialization has completed.
    }catch(...){
        application::OperationalContext context;context.resource_id=1;context.resource_name="ClamAV unavailable; verify component/configuration in Setup";context.diagnostic_code="clamav_component_unavailable";
        journal_->clamav_health(false,false,false,"clamav_component_unavailable");
        journal_->emit({application::OperationalKind::file_maintenance_problem,application::OperationalOutcome::unavailable,std::move(context)});
    }
    return true;
}
void AlphaRuntimeHost::clamav_loop()noexcept{
    try{
        if(clamav_stop_.load())return;
        const auto receipt=platform::win32::installation::read_receipt(installation_root_,owner_sid_);
        clamav_installation_id_=receipt.id;
        if(clamav_stop_.load())return;
        // update_clamav_definitions verifies the engine before launching it and
        // initializes its private data; uploads retain their own verifier.
        std::unique_lock<std::mutex> lock(clamav_mutex_);std::int64_t completed_day=-1,retry_after{};
        while(!clamav_stop_.load()){
            const auto now=now_utc_seconds();
            if(now>0&&now/86400>completed_day&&now>=retry_after&&!scheduler_paused_.load()){
                lock.unlock();
                {application::OperationalContext context;context.resource_id=1;context.resource_name="Automatic ClamAV definition check started";context.diagnostic_code="clamav_automatic_started";journal_->emit({application::OperationalKind::file_maintenance,application::OperationalOutcome::normal,std::move(context)});}
                const auto update=platform::win32::update_clamav_definitions(installation_root_,owner_sid_,clamav_installation_id_,now,[this]{return clamav_stop_.load()||scheduler_paused_.load();});
                if(update.attempted||update.diagnostic=="clamav_update_already_attempted_today")completed_day=now/86400;
                else retry_after=now+3600; // admission failure cannot create a tight retry loop
                {
                    application::OperationalContext context;context.resource_id=1;
                    context.resource_name=update.succeeded?"Automatic ClamAV: "+platform::win32::clamav_change_text(update.change):update.cancelled?"Automatic ClamAV definition check cancelled":update.diagnostic=="clamav_update_already_attempted_today"?"Automatic ClamAV check skipped: already attempted today":"ClamAV update needs attention: "+update.diagnostic;
                    if(update.succeeded&&update.daily_version)context.resource_name+="; daily "+std::to_string(update.daily_version);
                    context.resource_name.resize(std::min<std::size_t>(context.resource_name.size(),128));context.diagnostic_code=update.diagnostic;
                    const bool problem=!update.succeeded&&!update.cancelled&&update.diagnostic!="clamav_update_already_attempted_today";
                    journal_->emit({problem?application::OperationalKind::file_maintenance_problem:application::OperationalKind::file_maintenance,
                        update.succeeded?application::OperationalOutcome::normal:update.cancelled?application::OperationalOutcome::cancelled:application::OperationalOutcome::unavailable,std::move(context)});
                }
                if(update.succeeded)journal_->clamav_health(false,true,true,{});
                else if(!update.cancelled&&update.diagnostic!="clamav_update_already_attempted_today"){
                    const auto status=platform::win32::inspect_clamav_status(installation_root_,owner_sid_,clamav_installation_id_);
                    journal_->clamav_health(false,false,status.definitions_verified,update.diagnostic);
                }else if(update.diagnostic=="clamav_update_already_attempted_today"){
                    const auto status=platform::win32::inspect_clamav_status(installation_root_,owner_sid_,clamav_installation_id_);
                    if(!status.definitions_verified)journal_->clamav_health(false,false,false,"clamav_definitions_unavailable");
                }
                lock.lock();
            }
            clamav_wake_.wait_for(lock,std::chrono::seconds(60),[this]{return clamav_stop_.load();});
        }
    }catch(...){try{application::OperationalContext context;context.resource_id=1;context.resource_name="ClamAV daily updater stopped; restart after checking logs";context.diagnostic_code="clamav_worker_failed";journal_->emit({application::OperationalKind::file_maintenance_problem,application::OperationalOutcome::internal_failure,std::move(context)});const auto status=platform::win32::inspect_clamav_status(installation_root_,owner_sid_,clamav_installation_id_);journal_->clamav_health(false,false,status.definitions_verified,"clamav_worker_failed");}catch(...){}}
}
void AlphaRuntimeHost::stop_clamav()noexcept{
    clamav_stop_.store(true);clamav_wake_.notify_all();if(clamav_thread_.joinable())clamav_thread_.join();
}
