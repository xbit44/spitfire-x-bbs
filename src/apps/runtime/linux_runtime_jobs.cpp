#include "linux_runtime_jobs.hpp"
#include "sfx/platform/linux/scheduler_files.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <algorithm>
#include <limits>

namespace sfx::apps::runtime {
using application::AlphaControlResponse;
using application::LocalControlResponseCode;
LinuxRuntimeJobs::LinuxRuntimeJobs(std::filesystem::path root,adapters::storage::SqliteStorage& storage,
    application::IIdentifierGenerator& identifiers,application::IClock& clock,std::string zone,
    std::shared_ptr<AlphaOperationalJournal> journal,Backup backup,ValidateDestination validate):
    installation_root_(std::move(root)),storage_(&storage),identifiers_(identifiers),clock_(clock),zone_(std::move(zone)),
    journal_(std::move(journal)),backup_(std::move(backup)),validate_destination_(std::move(validate)){}
LinuxRuntimeJobs::~LinuxRuntimeJobs(){stop();}
bool LinuxRuntimeJobs::start(bool paused){
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    if(scheduler_thread_.joinable()||ready_)return false;
    if(!backup_||!validate_destination_){scheduler_diagnostic_="scheduler_native_capabilities_missing";return false;}
    ready_=true;scheduler_stop_=false;scheduler_paused_.store(paused);
    try{if(initialize_scheduler_locked())return true;}
    catch(...){scheduler_diagnostic_="scheduler_initialization_failed";}
    ready_=false;scheduler_.reset();for(auto& job:message_schedulers_)job.reset();return false;
}
void LinuxRuntimeJobs::stop()noexcept{
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);stop_scheduler();
    std::lock_guard<std::mutex> lock(scheduler_mutex_);ready_=false;
    // An unexecuted queued run remains durably active for interrupted recovery.
    pending_scheduler_run_.reset();scheduler_.reset();for(auto& job:message_schedulers_)job.reset();
}
void LinuxRuntimeJobs::pause(bool paused){
    std::lock_guard<std::mutex> lock(scheduler_mutex_);scheduler_paused_.store(paused);scheduler_wake_.notify_all();
}
bool LinuxRuntimeJobs::busy()const{return scheduler_has_work();}
bool LinuxRuntimeJobs::reconfigure_time_zone(const std::string& zone,const std::function<bool()>& publish){
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);std::lock_guard<std::mutex> lock(scheduler_mutex_);
    if(!ready_||scheduler_stop_||!scheduler_||scheduler_has_work_locked()||!publish)return false;
    try{
        auto selected=zone;
        if(zone==zone_)return publish();
        const auto rules=platform::linux_host::LinuxBbsTimeZoneRuleProvider::create(zone);if(!rules)return false;
        const auto clone=[&](const std::unique_ptr<application::RuntimeScheduler>& current){
            if(!current)throw std::runtime_error("message_scheduler_unavailable");
            auto next=std::make_unique<application::RuntimeScheduler>(current->configuration(),current->state(),
                [zone](std::int64_t after,std::uint8_t h,std::uint8_t m){return platform::linux_host::next_bbs_daily_run(zone,after,h,m);},
                [zone](std::int64_t after,std::uint8_t h,std::uint8_t m,std::optional<std::uint8_t> day,std::int64_t slot){return platform::linux_host::next_bbs_maintenance_run(zone,after,h,m,day,slot);});
            if(!next->recompute_next(now_utc_seconds()))throw std::runtime_error("scheduler_zone_unavailable");
            return next;
        };
        auto backup=clone(scheduler_);std::array<std::unique_ptr<application::RuntimeScheduler>,2> messages{{clone(message_schedulers_[0]),clone(message_schedulers_[1])}};
        if(!publish())return false;
        scheduler_=std::move(backup);message_schedulers_=std::move(messages);zone_=std::move(selected);scheduler_wake_.notify_all();return true;
    }catch(const std::exception&){return false;}
}
LinuxRuntimeJobs::Outcome LinuxRuntimeJobs::execute_scheduler_backup(const application::ScheduledRun& run)noexcept{
    try{return backup_(run,[this]{return scheduler_cancel_.load();});}
    catch(const std::exception&){return {application::ScheduledJobResult::failed,"backup_failed_work_preserved"};}
    catch(...){return {application::ScheduledJobResult::failed,"backup_unknown_failure_work_preserved"};}
}
namespace {
std::string identifier_text(const application::Identifier128& identifier) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(32);
    for (const auto byte : identifier.bytes) {
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 0x0fU]);
    }
    return result;
}

std::string bounded_scheduler_detail(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    std::replace(value.begin(), value.end(), '\n', ' ');
    if (value.size() > 2048) value.resize(2048);
    return value;
}

}
// Same worker, cancellation, protected publication and ownership as backup.
// This is not a parallel timer/service and never owns caller permission.
namespace {
std::optional<application::Identifier128> maintenance_identifier(std::string_view text){
    if(text.size()!=32)return {};
    application::Identifier128 value;
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
application::RuntimeScheduler* LinuxRuntimeJobs::scheduler_for_job(std::string_view job) const{
    if(job==application::kBackupJobId)return scheduler_.get();
    if(job==application::kMessageRetentionJobId)return message_schedulers_[0].get();
    if(job==application::kMessageIntegrityJobId)return message_schedulers_[1].get();
    return nullptr;
}
bool LinuxRuntimeJobs::scheduler_has_work_locked()const{
    if(pending_scheduler_run_ || (scheduler_ && scheduler_->state().active))return true;
    for(const auto& job:message_schedulers_)if(job && job->state().active)return true;
    return false;
}
bool LinuxRuntimeJobs::scheduler_has_work()const{
    std::lock_guard<std::mutex> lock(scheduler_mutex_);return scheduler_has_work_locked();
}
void LinuxRuntimeJobs::initialize_message_schedulers_locked(){
    const auto reconciled=storage_->recover_interrupted_message_maintenance({true,{}});
    for(std::size_t index=0;index<2;++index){
        const auto kind=static_cast<application::MessageMaintenanceKind>(index);const auto* id=application::message_job_id(kind);
        auto& diagnostic=message_scheduler_diagnostics_[index];
        try{
            const auto config_file=platform::linux_host::read_scheduler_configuration_file(installation_root_,id);
            const bool new_config=config_file.state==platform::linux_host::SchedulerFileState::missing;
            auto config=application::default_message_scheduler_configuration(id);
            if(new_config){const auto saved=platform::linux_host::publish_scheduler_configuration_file(installation_root_,config);if(!saved){diagnostic=saved.operation;continue;}}
            else if(config_file.value)config=*config_file.value;else{diagnostic=config_file.diagnostic_code;continue;}
            const auto state_file=platform::linux_host::read_scheduler_state_file(installation_root_,id);
            auto state=application::default_message_scheduler_state(id);state.configuration_revision=config.revision;
            if(state_file.value)state=*state_file.value;
            else if(state_file.state!=platform::linux_host::SchedulerFileState::missing){diagnostic=state_file.diagnostic_code;continue;}
            else if(kind==application::MessageMaintenanceKind::integrity && !new_config)state.requires_fresh_check=true;
            if(!reconciled){diagnostic="message_maintenance_reconciliation_failed";continue;}
            const auto zone=zone_;
            auto candidate=std::make_unique<application::RuntimeScheduler>(config,state,
                [zone](std::int64_t after,std::uint8_t h,std::uint8_t m){return platform::linux_host::next_bbs_daily_run(zone,after,h,m);},
                [zone](std::int64_t after,std::uint8_t h,std::uint8_t m,std::optional<std::uint8_t> day,std::int64_t consumed){return platform::linux_host::next_bbs_maintenance_run(zone,after,h,m,day,consumed);});
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
            const auto saved=platform::linux_host::publish_scheduler_state_file(installation_root_,candidate->state());
            if(!saved){diagnostic=saved.operation;continue;}
            message_schedulers_[index]=std::move(candidate);diagnostic="message_maintenance_ready";
        }catch(...){diagnostic="message_maintenance_initialization_failed";}
    }
}
application::MessageMaintenanceStatus LinuxRuntimeJobs::message_maintenance_status(application::MessageMaintenanceKind kind){
std::lock_guard<std::mutex> lock(scheduler_mutex_);
    application::MessageMaintenanceStatus result;if(kind>application::MessageMaintenanceKind::integrity){result.diagnostic="message_job_unknown";return result;}
    const auto index=static_cast<std::size_t>(kind);const auto* id=application::message_job_id(kind);
    result.kind=kind;result.configuration=application::default_message_scheduler_configuration(id);result.state=application::default_message_scheduler_state(id);
    result.diagnostic=message_scheduler_diagnostics_[index];result.shared_verification_required=!message_schedulers_[1] || message_schedulers_[1]->state().requires_fresh_check;
    if(!storage_ || !message_schedulers_[index] || !ready_)return result;
    result.configuration=message_schedulers_[index]->configuration();result.state=message_schedulers_[index]->state();
    const auto held=storage_->message_integrity_holds({true,{}});if(!held || !held.value){result.diagnostic="message_holds_unavailable";return result;}result.holds=*held.value;
    const auto current_id=maintenance_identifier(result.state.active?result.state.active_run_id:result.state.history.empty()?std::string{}:result.state.history.back().run_id);
    if(current_id){const auto actual=storage_->message_maintenance_run({true,{}},*current_id);if(actual && actual.value)result.latest=*actual.value;
        else if(actual.status.code!=application::StorageErrorCode::not_found){result.diagnostic="message_report_unavailable";return result;}}
    result.available=true;return result;
}
LinuxRuntimeJobs::MessageJobOutcome LinuxRuntimeJobs::execute_scheduler_message(const application::ScheduledRun& run)noexcept{
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
                if(reason==R::cancelled)outcome=O::interrupted;
                break;}
            if(batch.value->complete){reason=batch.value->policy_changed?R::policy_changed:batch.value->run.reason;
                if(reason!=R::none)outcome=batch.value->run.removed_public || batch.value->run.expired_unread?O::partial:O::skipped;
                break;}
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
application::AlphaControlResponse LinuxRuntimeJobs::run_message_maintenance_now(application::MessageMaintenanceKind kind){
std::lock_guard<std::mutex> lock(scheduler_mutex_);
    if(kind>application::MessageMaintenanceKind::integrity)return {LocalControlResponseCode::invalid_request,"message_job_unknown"};
    if(!ready_ || scheduler_paused_.load())return {LocalControlResponseCode::shutting_down,"message_runtime_not_ready"};
    auto* job=message_schedulers_[static_cast<std::size_t>(kind)].get();
    if(!job || scheduler_has_work_locked())return {LocalControlResponseCode::invalid_request,job?"scheduler_work_already_active":"message_job_unavailable"};
    const auto run=job->begin_manual(now_utc_seconds(),identifier_text(identifiers_.generate()));
    if(!run)return {LocalControlResponseCode::invalid_request,"message_job_not_started"};
    const auto saved=platform::linux_host::publish_scheduler_state_file(installation_root_,job->state());
    if(!saved){message_schedulers_[static_cast<std::size_t>(kind)].reset();message_scheduler_diagnostics_[static_cast<std::size_t>(kind)]="message_job_state_publication_failed";return {LocalControlResponseCode::configuration_failure,"message_job_state_publication_failed"};}
    pending_scheduler_run_=*run;scheduler_wake_.notify_all();return {LocalControlResponseCode::ok,"message_job_queued"};
}
application::AlphaControlResponse LinuxRuntimeJobs::configure_message_schedule(const application::MessageMaintenanceScheduleRequest& request){
std::lock_guard<std::mutex> lock(scheduler_mutex_);
    if(application::encode_message_maintenance_schedule(request).empty())return {LocalControlResponseCode::invalid_request,"message_schedule_invalid"};
    if(!ready_ || scheduler_stop_)return {LocalControlResponseCode::shutting_down,"message_runtime_not_ready"};
    const auto index=static_cast<std::size_t>(request.kind);auto& job=message_schedulers_[index];
    if(!job)return {LocalControlResponseCode::configuration_failure,"message_job_unavailable"};
    const auto old=job->configuration();const auto old_state=job->state();
    if(old.revision!=request.expected_revision || old.revision==UINT64_MAX)return {LocalControlResponseCode::invalid_request,"message_schedule_revision_conflict"};
    auto config=old;++config.revision;config.enabled=request.enabled;config.local_hour=request.hour;config.local_minute=request.minute;config.local_weekday=request.weekday;
    const auto zone=zone_;
    auto candidate=std::make_unique<application::RuntimeScheduler>(old,old_state,
        [zone](std::int64_t after,std::uint8_t h,std::uint8_t m){return platform::linux_host::next_bbs_daily_run(zone,after,h,m);},
        [zone](std::int64_t after,std::uint8_t h,std::uint8_t m,std::optional<std::uint8_t> day,std::int64_t slot){return platform::linux_host::next_bbs_maintenance_run(zone,after,h,m,day,slot);});
    if(!candidate->reconfigure(config,now_utc_seconds()))return {LocalControlResponseCode::configuration_failure,"message_schedule_time_unavailable"};
    const auto config_saved=platform::linux_host::publish_scheduler_configuration_file(installation_root_,config);
    const auto state_saved=config_saved?platform::linux_host::publish_scheduler_state_file(installation_root_,candidate->state()):application::StorageStatus{application::StorageErrorCode::io,"message_configuration_unconfirmed"};
    if(!config_saved || !state_saved){
        const auto config_back=platform::linux_host::publish_scheduler_configuration_file(installation_root_,old);
        const auto state_back=platform::linux_host::publish_scheduler_state_file(installation_root_,old_state);
        if(!config_back || !state_back){job.reset();message_scheduler_diagnostics_[index]="message_schedule_rollback_failed";}
        return {LocalControlResponseCode::configuration_failure,!config_back || !state_back?"message_schedule_rollback_failed":"message_schedule_save_failed"};
    }
    job=std::move(candidate);message_scheduler_diagnostics_[index]="message_maintenance_ready";scheduler_wake_.notify_all();
    return {LocalControlResponseCode::ok,"message_schedule_saved_no_purge"};
}

bool LinuxRuntimeJobs::initialize_scheduler_locked() {
    if (scheduler_) return true;
    if (!ready_ || !storage_) {
        scheduler_diagnostic_ = "scheduler_runtime_not_ready";
        return false;
    }
    auto configuration_file = platform::linux_host::read_scheduler_configuration_file(
        installation_root_);
    application::SchedulerConfiguration configuration;
    if (configuration_file.state == platform::linux_host::SchedulerFileState::missing) {
        const auto saved = platform::linux_host::publish_scheduler_configuration_file(
            installation_root_, configuration);
        if (!saved) {
            scheduler_diagnostic_ = saved.operation;
            return false;
        }
    } else if (configuration_file.state == platform::linux_host::SchedulerFileState::loaded &&
               configuration_file.value) {
        configuration = *configuration_file.value;
    } else {
        scheduler_diagnostic_ = configuration_file.diagnostic_code;
        return false;
    }

    application::SchedulerState state;
    state.configuration_revision = configuration.revision;
    const auto state_file = platform::linux_host::read_scheduler_state_file(
        installation_root_);
    if (state_file.state == platform::linux_host::SchedulerFileState::loaded &&
        state_file.value) {
        state = *state_file.value;
    } else if (state_file.state != platform::linux_host::SchedulerFileState::missing) {
        scheduler_diagnostic_ = state_file.diagnostic_code;
        return false;
    }

    const std::string scheduler_zone = zone_;
    auto candidate = std::make_unique<application::RuntimeScheduler>(
        configuration, state,
        [scheduler_zone](const std::int64_t after, const std::uint8_t hour,
                         const std::uint8_t minute) {
            return platform::linux_host::next_bbs_daily_run(
                scheduler_zone, after, hour, minute);
        });
    if (!candidate->recover_on_start(now_utc_seconds())) {
        scheduler_diagnostic_ = "scheduler_next_run_unavailable";
        return false;
    }
    const auto state_saved = platform::linux_host::publish_scheduler_state_file(
        installation_root_, candidate->state());
    if (!state_saved) {
        scheduler_diagnostic_ = state_saved.operation;
        return false;
    }
    scheduler_ = std::move(candidate);
    initialize_message_schedulers_locked();
    scheduler_diagnostic_ = "scheduler_ready";
    scheduler_stop_ = false;
    scheduler_cancel_.store(false);
    try {
        scheduler_thread_ = std::thread([this] { scheduler_loop(); });
    } catch (...) {
        scheduler_.reset();
        scheduler_diagnostic_ = "scheduler_worker_start_failed";
        return false;
    }
    return true;
}

LinuxScheduledJobSnapshot LinuxRuntimeJobs::scheduled_job(std::string_view id) const {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);LinuxScheduledJobSnapshot result;
    const auto* job=scheduler_for_job(id);result.paused=scheduler_paused_.load();result.any_active=scheduler_has_work_locked();
    if(!ready_||!job){result.diagnostic="job_unavailable";return result;}
    result.available=true;result.configuration=job->configuration();result.state=job->state();result.queued=pending_scheduler_run_&&pending_scheduler_run_->job_id==id;
    result.diagnostic=id==application::kBackupJobId?scheduler_diagnostic_:message_scheduler_diagnostics_[id==application::kMessageRetentionJobId?0:1];return result;
}
application::AlphaSchedulerStatus LinuxRuntimeJobs::scheduler_status() const {
    std::lock_guard<std::mutex> lock(scheduler_mutex_);
    application::AlphaSchedulerStatus result;
    result.diagnostic_code = scheduler_diagnostic_;
    if (!scheduler_) return result;
    const auto& configuration = scheduler_->configuration();
    const auto& state = scheduler_->state();
    result.configuration_loaded = true;
    result.job_id = configuration.job_id;
    result.enabled = configuration.enabled;
    result.configuration_revision = configuration.revision;
    result.local_hour = configuration.local_hour;
    result.local_minute = configuration.local_minute;
    result.next_run_utc = state.next_run_utc;
    result.active = state.active;
    result.active_scheduled = state.active_scheduled;
    result.queued = pending_scheduler_run_.has_value();
    result.active_run_id = state.active_run_id;
    result.active_started_utc = state.active_started_utc;
    result.last_run_utc = state.last_run_utc;
    result.last_result = state.last_result;
    result.last_detail = state.last_detail;
    result.history = state.history;
    for (auto& item : result.history)
        item.detail = application::bounded_scheduler_status_history_detail(item.detail);
    return result;
}


void LinuxRuntimeJobs::scheduler_loop() noexcept {
    std::unique_lock<std::mutex> lock(scheduler_mutex_);
    try {
    while (!scheduler_stop_) {
        if (ready_ && !scheduler_has_work_locked() && scheduler_ && !scheduler_paused_.load()) {
            auto due = scheduler_->begin_due(
                now_utc_seconds(), identifier_text(identifiers_.generate()));
            if (due) {
                const auto saved = platform::linux_host::publish_scheduler_state_file(
                    installation_root_, scheduler_->state());
                if (saved) pending_scheduler_run_ = std::move(due);
                else {
                    scheduler_.reset();
                    scheduler_diagnostic_ = saved.operation;
                }
            }
        }
        if(ready_ && !pending_scheduler_run_ && !scheduler_has_work_locked() && !scheduler_paused_.load())for(auto& job:message_schedulers_){
            if(!job)continue;
            auto message_due=job->begin_due(now_utc_seconds(),identifier_text(identifiers_.generate()));
            if(!message_due)continue;
            const auto saved=platform::linux_host::publish_scheduler_state_file(installation_root_,job->state());
            if(saved)pending_scheduler_run_=std::move(message_due);
            else{
                // Native publication may have renamed before a directory
                // sync failed. Preserve that evidence and require reopening;
                // do not pretend the previous disk state is still current.
                const auto index=job->configuration().job_id==application::kMessageRetentionJobId?0U:1U;
                job.reset();
                message_scheduler_diagnostics_[index]="message_job_state_publication_failed";
            }
            break;
        }
        if (!pending_scheduler_run_) {
            scheduler_wake_.wait_for(lock, std::chrono::seconds(1), [this] {
                return scheduler_stop_ || pending_scheduler_run_.has_value();
            });
            continue;
        }
        const auto run = *pending_scheduler_run_;
        pending_scheduler_run_.reset();
        const auto message_started=std::chrono::steady_clock::now();
        lock.unlock();
        const bool backup=run.job_id==application::kBackupJobId;
        if (backup && journal_) journal_->emit({application::OperationalKind::backup_started,{}, {}});
        std::pair<application::ScheduledJobResult,std::string> outcome;
        std::optional<bool> verification_required;
        if(backup)outcome=execute_scheduler_backup(run);
        else{const auto message=execute_scheduler_message(run);outcome={message.result,message.detail};verification_required=message.requires_fresh_check;}
        lock.lock();
        bool completed = false;
        auto* completed_job=scheduler_for_job(run.job_id);
        if (completed_job && completed_job->complete(run, now_utc_seconds(),
                                               outcome.first, outcome.second)) {
            if(verification_required){auto state=completed_job->state();state.requires_fresh_check=*verification_required;completed_job->replace_state(std::move(state));}
            const auto saved = platform::linux_host::publish_scheduler_state_file(
                installation_root_, completed_job->state());
            if(backup)scheduler_diagnostic_ = saved ? "scheduler_ready" : saved.operation;
            else{
                const auto index=run.job_id==application::kMessageRetentionJobId?0U:1U;
                message_scheduler_diagnostics_[index]=saved?"message_maintenance_ready":"message_completion_publication_failed";
                if(!saved)message_schedulers_[index].reset();
            }
            if(!saved && backup)scheduler_.reset();
            completed = saved && outcome.first == application::ScheduledJobResult::success;
        } else {
            if(backup){scheduler_diagnostic_ = "scheduler_completion_state_failed";scheduler_.reset();}
            else{
                const auto index=run.job_id==application::kMessageRetentionJobId?0U:1U;
                message_scheduler_diagnostics_[index]="message_completion_state_failed";
                message_schedulers_[index].reset(); // unknown next slot never retries each tick
            }
        }
        if (backup && journal_) journal_->emit({completed ? application::OperationalKind::backup_completed :
            application::OperationalKind::backup_failed,{}, {}});
        if(!backup && journal_){
            const auto id=maintenance_identifier(run.run_id);
            const auto actual=id?storage_->message_maintenance_run({true,{}},*id):application::StorageResult<application::MessageMaintenanceRun>{};
            bool overage=false;if(actual && actual.value)for(const auto& e:actual.value->echoes)overage=overage || e.protected_overage>0;
            application::OperationalContext context;context.resource_id=1;
            context.duration_seconds=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-message_started).count());
            context.resource_name=(run.job_id==application::kMessageRetentionJobId?"Retention: ":"Integrity: ")+outcome.second;
            if(context.resource_name.size()>128)context.resource_name.resize(128);
            context.diagnostic_code=outcome.first==application::ScheduledJobResult::success?"complete":
                outcome.first==application::ScheduledJobResult::partial?"partial":outcome.first==application::ScheduledJobResult::skipped?"skipped":"failed";
            journal_->emit({completed && !overage?application::OperationalKind::message_maintenance:application::OperationalKind::message_maintenance_problem,{},context});
            if(actual && actual.value)for(const auto& e:actual.value->echoes)if(e.removed || e.protected_overage){
                application::OperationalContext area;area.resource_id=static_cast<std::uint64_t>(e.echo.value);
                area.resource_name="Echo #"+std::to_string(e.echo.value)+": committed removals="+std::to_string(e.removed)+"; protected overage="+std::to_string(e.protected_overage);
                journal_->emit({e.protected_overage?application::OperationalKind::message_maintenance_problem:application::OperationalKind::message_maintenance,{},std::move(area)});
            }
        }
    }
    }catch(...){
        if(!lock.owns_lock())lock.lock();
        ready_=false;scheduler_stop_=true;scheduler_cancel_.store(true);
        scheduler_diagnostic_="scheduler_worker_failed_active_state_preserved";
    }
}

void LinuxRuntimeJobs::stop_scheduler() noexcept {
    {
        std::lock_guard<std::mutex> lock(scheduler_mutex_);
        scheduler_stop_ = true;
        scheduler_cancel_.store(true);
    }
    scheduler_wake_.notify_all();
    if (scheduler_thread_.joinable()) scheduler_thread_.join();
}

std::int64_t LinuxRuntimeJobs::now_utc_seconds() const noexcept {
    return clock_.unix_seconds();
}


AlphaControlResponse LinuxRuntimeJobs::configure_scheduler(
    const application::AlphaSchedulerSettingsRequest& request,std::optional<std::uint32_t> retained) {
    std::lock_guard<std::mutex> scheduler_lock(scheduler_mutex_);

    if (!ready_ || scheduler_stop_) {
        return {LocalControlResponseCode::configuration_failure,
                "scheduler_runtime_not_ready"};
    }

    if (!scheduler_) {
        return {LocalControlResponseCode::configuration_failure,
                scheduler_diagnostic_};
    }
    const auto& current = scheduler_->configuration();
    if (request.job_id != application::kBackupJobId ||
        request.expected_revision != current.revision ||
        request.local_hour >= 24 || request.local_minute >= 60 ||
        (request.backup_destination && !application::valid_backup_destination_text(*request.backup_destination))) {
        return {LocalControlResponseCode::invalid_request,
                request.expected_revision != current.revision
                    ? "scheduler_revision_conflict" : "scheduler_settings_invalid"};
    }
    if (current.revision == std::numeric_limits<std::uint64_t>::max()) {
        return {LocalControlResponseCode::configuration_failure,
                "scheduler_revision_exhausted"};
    }
    if(retained&&(*retained<1||*retained>365||(*retained!=current.retained_backups&&(!scheduler_paused_.load()||scheduler_has_work_locked()))))
        return {LocalControlResponseCode::invalid_request,"backup_retention_requires_idle_stopped_bbs"};
    auto replacement = current;
    if(retained)replacement.retained_backups=*retained;
    replacement.format_version = 2;
    ++replacement.revision;
    replacement.enabled = request.enabled;
    replacement.local_hour = request.local_hour;
    replacement.local_minute = request.local_minute;
    // Hold the validated collection through publication, without changing any
    // active/queued run's captured destination or starting retention work.
    std::shared_ptr<void> destination;
    if (request.backup_destination) {
        try {
            destination = validate_destination_(*request.backup_destination);
            if(!destination)throw std::runtime_error("backup_destination_unavailable_no_fallback");
        } catch (const std::exception& error) {
            return {LocalControlResponseCode::configuration_failure, bounded_scheduler_detail(error.what())};
        }
        replacement.backup_destination = *request.backup_destination;
    }
    const std::string scheduler_zone = zone_;
    auto candidate = std::make_unique<application::RuntimeScheduler>(
        current, scheduler_->state(),
        [scheduler_zone](const std::int64_t after, const std::uint8_t hour,
                         const std::uint8_t minute) {
            return platform::linux_host::next_bbs_daily_run(
                scheduler_zone, after, hour, minute);
        });
    if (!candidate->reconfigure(replacement, now_utc_seconds())) {
        return {LocalControlResponseCode::configuration_failure,
                "scheduler_next_run_unavailable"};
    }
    const auto config_saved = platform::linux_host::publish_scheduler_configuration_file(
        installation_root_, replacement);
    const auto state_saved = config_saved?platform::linux_host::publish_scheduler_state_file(
        installation_root_, candidate->state()):application::StorageStatus{application::StorageErrorCode::io,"scheduler_configuration_unconfirmed"};
    if (!config_saved || !state_saved) {
        const auto rolled_back =
            platform::linux_host::publish_scheduler_configuration_file(
                installation_root_, current);
        const auto state_back=platform::linux_host::publish_scheduler_state_file(installation_root_,scheduler_->state());
        if (!rolled_back || !state_back) {
            ready_ = false;
            scheduler_paused_.store(true);
            diagnostic_code_ = "scheduler_configuration_rollback_failed";
            scheduler_diagnostic_ = diagnostic_code_;
            return {LocalControlResponseCode::recovery_required,
                    diagnostic_code_};
        }
        scheduler_diagnostic_ = state_saved.operation;
        return {LocalControlResponseCode::configuration_failure,
                scheduler_diagnostic_};
    }
    scheduler_ = std::move(candidate);
    scheduler_diagnostic_ = "scheduler_ready";
    scheduler_wake_.notify_all();
    if (journal_) journal_->emit({application::OperationalKind::configuration_saved,{}, {}});
    return {LocalControlResponseCode::ok, "scheduler_settings_saved"};
}

AlphaControlResponse LinuxRuntimeJobs::run_scheduler_now(
    const application::AlphaSchedulerRunRequest& request) {
    std::lock_guard<std::mutex> scheduler_lock(scheduler_mutex_);

    if (!ready_ || scheduler_stop_) {
        return {LocalControlResponseCode::configuration_failure,
                "scheduler_runtime_not_ready"};
    }

    if (!scheduler_ || request.job_id != application::kBackupJobId) {
        return {LocalControlResponseCode::invalid_request,
                scheduler_ ? "scheduler_job_unknown" : scheduler_diagnostic_};
    }
    if(scheduler_has_work_locked())return {LocalControlResponseCode::invalid_request,"scheduler_work_already_active"};
    auto run = scheduler_->begin_manual(
        now_utc_seconds(), identifier_text(identifiers_.generate()));
    if (!run) {
        return {LocalControlResponseCode::invalid_request,
                "scheduler_job_already_running"};
    }
    const auto saved = platform::linux_host::publish_scheduler_state_file(
        installation_root_, scheduler_->state());
    if (!saved) {
        scheduler_.reset();
        scheduler_diagnostic_ = saved.operation;
        return {LocalControlResponseCode::configuration_failure,
                scheduler_diagnostic_};
    }
    pending_scheduler_run_ = std::move(run);
    scheduler_diagnostic_ = "scheduler_ready";
    scheduler_wake_.notify_all();
    return {LocalControlResponseCode::ok, "scheduler_run_started"};
}


} // namespace sfx::apps::runtime
