#include "linux_runtime_host.hpp"
#include "../product_version.h"
#include "sfx/application/door_bulletin_preview_context.hpp"
#include "sfx/platform/linux/door_bulletin_sources.hpp"
#include "linux_system_monitor.hpp"
#include "linux_node_controls.hpp"
#include "linux_security_publication.hpp"
#include "linux_backup_job.hpp"
#include "sfx/adapters/authentication/phase3a_authentication_adapters.hpp"
#include "sfx/platform/linux/ftn_secrets.hpp"
#include "sfx/platform/linux/ftn_native_tic.hpp"
#include "sfx/platform/linux/ftn_native_toss.hpp"
#include "sfx/platform/linux/installation_layout.hpp"
#include "sfx/application/ftn_retention.hpp"
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace sfx::apps::runtime {
namespace {
namespace app=application;
using Code=app::LocalControlResponseCode;
using Type=app::LocalControlMessageType;
std::vector<app::NodeAssignmentIdentity> occupied(const LinuxCallerListenerStatus& status){
    std::vector<app::NodeAssignmentIdentity> result;
    for(const auto& slot:status.pool.slots)if(slot.occupied)result.push_back(slot.identity);
    return result;
}
}
LinuxRuntimeHost::LinuxRuntimeHost(std::filesystem::path root,app::IClock& clock,std::function<bool()> seeds):
    root_(std::move(root)),clock_(clock),store_(root_,*this,clock_,std::move(seeds)){}
LinuxRuntimeHost::~LinuxRuntimeHost(){(void)shutdown();}
app::AlphaControlResponse LinuxRuntimeHost::open(){
    std::lock_guard<std::mutex> maintenance(compaction_lifecycle_);if(compaction_status().active||offline_status().active)return {Code::invalid_request,"maintenance_active"};compaction_shutdown_=false;
    std::lock_guard<std::recursive_mutex> lock(operations_);const auto result=store_.open();
    if(result.code==Code::ok||result.code==Code::setup_required){epoch_=store_.identifiers().generate();generations_={};}
    else if(result.code==Code::recovery_required){epoch_={};generations_={};}
    if(result.code==Code::ok&&!ensure_journals())return {Code::recovery_required,journal_fault_};
    if(result.code==Code::ok){try{node_enablement_=read_linux_node_controls(root_);}catch(const std::exception& error){journal_fault_=error.what();return {Code::recovery_required,journal_fault_};}}
    return result;
}
bool LinuxRuntimeHost::bbs_owned()const{return bool(std::atomic_load(&engine_));}
bool LinuxRuntimeHost::management_owned()const{std::lock_guard<std::recursive_mutex> lock(operations_);return store_.owns_installation();}
bool LinuxRuntimeHost::management_work_active(){
    if(compaction_status().active||offline_status().active)return true;
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(bbs_owned()||(jobs_&&jobs_->busy())||(file_administration_&&file_administration_->active()))return true;
    if(clamav_){const auto value=clamav_->snapshot();if(value.active||value.queued)return true;}
    return false;
}
bool LinuxRuntimeHost::revoke(const app::SessionRevocationCommand& value){
    if(value.kind==app::SessionRevocationKind::sysop_level_all){
        auto previous=web_authentication_epoch_.load();
        while(previous!=UINT64_MAX&&!web_authentication_epoch_.compare_exchange_weak(previous,previous+1)){}
    }
    const auto engine=std::atomic_load(&engine_);return !engine||engine->revoke(value);
}
bool LinuxRuntimeHost::refresh_account_access(app::AccountId account){
    const auto engine=std::atomic_load(&engine_);return !engine||engine->refresh_account_access(account);
}
bool LinuxRuntimeHost::disconnect(app::NodeAssignmentIdentity identity){
    std::lock_guard<std::recursive_mutex> lock(operations_);const auto engine=std::atomic_load(&engine_);return engine&&engine->disconnect(identity);
}
app::AlphaLocalSessionStartResult LinuxRuntimeHost::reserve_local_node(std::uint16_t node){
    std::shared_ptr<LinuxCallerEngine> engine;
    {std::lock_guard<std::recursive_mutex> lock(operations_);engine=std::atomic_load(&engine_);
        if(!engine||!journal_fault_.empty())return {{Code::invalid_request,"bbs_not_ready"},{},0,{},0};}
    // Admission may wait on a supervised resolver. Keep its engine alive,
    // but let Stop acquire operations_ and cancel/join that admission.
    return engine->reserve_local_node(node);
}
bool LinuxRuntimeHost::cancel_local_node(app::NodeAssignmentIdentity identity){
    std::lock_guard<std::recursive_mutex> lock(operations_);const auto engine=std::atomic_load(&engine_);return engine&&engine->cancel_local_node(identity);
}
LinuxBrowserNodeStart LinuxRuntimeHost::open_browser_node(std::uint16_t node){
    std::shared_ptr<LinuxCallerEngine> engine;
    {std::lock_guard<std::recursive_mutex> lock(operations_);engine=std::atomic_load(&engine_);
        if(!engine||!journal_fault_.empty())return {{Code::invalid_request,"bbs_not_ready"},{},platform::linux_host::FileDescriptor{}};}
    return engine->open_browser_node(node);
}
void LinuxRuntimeHost::retain_generations(const LinuxCallerListenerStatus& status){
    for(std::size_t i=0;i<generations_.size();++i)generations_[i]=std::max(generations_[i],status.pool.slots[i].identity.generation);
}
app::AlphaControlResponse LinuxRuntimeHost::start_bbs(){
    if(offline_status().active||compaction_status().active)return {Code::invalid_request,"maintenance_active"};
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!journal_fault_.empty())return {Code::recovery_required,journal_fault_};
    if(bbs_owned())return {Code::invalid_request,"caller_engine_already_owned"};
    if(jobs_&&jobs_->busy())return {Code::invalid_request,"scheduler_work_already_active"};
    if(store_.state()!=app::AlphaRuntimePresentationState::ready)return {Code::recovery_required,store_.diagnostic()};
    if(!ensure_journals())return {Code::recovery_required,journal_fault_};
    if(run_sequence_==UINT64_MAX)return {Code::recovery_required,"bbs_run_sequence_exhausted"};
    try{node_enablement_=read_linux_node_controls(root_);}catch(const std::exception& error){journal_fault_=error.what();return {Code::recovery_required,journal_fault_};}
    auto engine=std::make_shared<LinuxCallerEngine>(root_,store_,clock_,epoch_,journal_,durable_,generations_,node_enablement_);
    engine->set_external_event_gate(true, 0);
    std::atomic_store(&engine_,engine);const auto result=engine->start();
    if (result.code == Code::ok) {
        external_events_ = std::make_unique<ExternalEventRunner>(root_, std::to_wstring(geteuid()),
            store_.configuration()->bbs_time_zone, clock_, ExternalEventCallerGate{
                [engine](bool held, std::int64_t cutoff) { engine->set_external_event_gate(held, cutoff); },
                [engine] { return engine->status().pool.active == 0; },
                [engine] { engine->end_calls_for_external_event(); }}, journal_, durable_);
        (void)external_events_->start(true);
    }
    if(result.code==Code::ok){bbs_started_boot_ms_=linux_boot_milliseconds();++run_sequence_;drain_deadline_.reset();lifecycle_diagnostic_="started";active_configuration_=*store_.configuration();pending_configuration_.reset();configuration_activation_=app::AlphaConfigurationActivation::applied;configuration_detail_="Applied";start_ftn();engine->attach_sysop_ftn(ftn_engine_);if(jobs_)jobs_->pause(false);}
    if(result.code!=Code::ok&&engine->stop()){retain_generations(engine->status());std::atomic_store(&engine_,std::shared_ptr<LinuxCallerEngine>{});}
    return result;
}
LinuxStopResult LinuxRuntimeHost::stop_bbs(std::optional<std::uint64_t> confirmation){
    return stop_request(confirmation,false);
}
LinuxStopResult LinuxRuntimeHost::stop_request(std::optional<std::uint64_t> confirmation,bool restart){
    std::lock_guard<std::recursive_mutex> lock(operations_);const auto engine=std::atomic_load(&engine_);
    if(!engine)return {{Code::ok,"stopped"},0,0};
    if(!engine->set_draining(true))return {{Code::recovery_required,"admission_not_quiescent"},0,0};
    const auto callers=occupied(engine->status());
    if(!callers.empty()&&(!confirmation||*confirmation!=pending_ticket_||callers!=pending_callers_||pending_restart_!=restart)){
        if(ticket_sequence_==UINT64_MAX)return {{Code::recovery_required,"stop_ticket_exhausted"},0,0};
        pending_ticket_=++ticket_sequence_;pending_callers_=callers;pending_restart_=restart;
        if(!drain_deadline_)(void)engine->set_draining(false);
        return {{Code::invalid_request,"confirm_caller_stop"},static_cast<std::uint16_t>(callers.size()),pending_ticket_};
    }
    return stop_owned_bbs();
}
LinuxStopResult LinuxRuntimeHost::stop_owned_bbs(){
    const auto engine=std::atomic_load(&engine_);if(!engine)return {{Code::ok,"stopped"},0,0};
    (void)engine->set_draining(true);
    external_events_.reset();
    stop_jobs();
    if(!engine->stop()){lifecycle_diagnostic_="caller_cleanup_required";return {{Code::recovery_required,lifecycle_diagnostic_},0,0};}
    stop_ftn();
    retain_generations(engine->status());
    std::atomic_store(&engine_,std::shared_ptr<LinuxCallerEngine>{});pending_ticket_=0;pending_callers_.clear();
    bbs_started_boot_ms_.reset();drain_deadline_.reset();lifecycle_diagnostic_="stopped";
    active_configuration_.reset();pending_configuration_.reset();configuration_activation_=app::AlphaConfigurationActivation::saved_next_start;configuration_detail_="BBS stopped";
    ensure_jobs();
    return {{Code::ok,"stopped"},0,0};
}
void LinuxRuntimeHost::cancel_stop(){std::lock_guard<std::recursive_mutex> lock(operations_);pending_ticket_=0;pending_callers_.clear();pending_restart_=false;}
app::AlphaControlResponse LinuxRuntimeHost::shutdown(){
    std::unique_lock<std::mutex> maintenance(compaction_lifecycle_);compaction_shutdown_=true;compaction_cancel_.store(true);
    offline_cancel_.store(true);offline_confirmation_.notify_all();
    if(compaction_thread_.joinable())compaction_thread_.join();
    if(offline_thread_.joinable())offline_thread_.join();
    std::lock_guard<std::recursive_mutex> lock(operations_);auto engine=std::atomic_load(&engine_);
    if (engine) (void)engine->set_draining(true);
    external_events_.reset();
    stop_jobs();
    if(engine&&!engine->stop())return {Code::recovery_required,"caller_cleanup_required"};
    stop_ftn();
    std::atomic_store(&engine_,std::shared_ptr<LinuxCallerEngine>{});engine.reset();
    bbs_started_boot_ms_.reset();sqlite_rates_.reset();sqlite_sample_={};sqlite_sample_ms_.reset();drain_deadline_.reset();pending_ticket_=0;pending_callers_.clear();
    active_configuration_.reset();pending_configuration_.reset();
    if(clamav_)clamav_->stop();
    clamav_.reset();
    if(file_administration_)file_administration_->stop();
    file_administration_.reset();
    ftn_publisher_.reset();ftn_hatch_publisher_.reset();
    store_.other_board_log({});
    if(durable_)durable_->stop();
    durable_.reset();
    if(journal_)journal_->emit({app::OperationalKind::runtime_stopped,{}, {}});
    if(journal_&&!journal_->flush()){journal_fault_="management_journal_flush_failed";return {Code::recovery_required,journal_fault_};}
    journal_.reset();door_snapshot_cache_.clear();door_bulletin_snapshot_cache_.clear();journal_fault_.clear();
    return store_.close_for_maintenance();
}
app::AlphaRuntimeStatus LinuxRuntimeHost::status(){
    std::lock_guard<std::recursive_mutex> lock(operations_);apply_pending_configuration();auto value=store_.stopped_snapshot(epoch_,++sequence_);
    if(active_configuration_){const auto& active=*active_configuration_;value.configuration_revision=active.configuration_revision;value.bbs_name=active.bbs_name;value.bbs_time_zone=active.bbs_time_zone;value.lifecycle.configured_nodes=active.logical_nodes;value.telnet_configured=active.telnet.enabled;value.ssh_configured=active.ssh.enabled;value.telnet_bind=active.telnet.bind_address;value.ssh_bind=active.ssh.bind_address;value.telnet_port=active.telnet.port;value.ssh_port=active.ssh.port;value.hostname_lookup=active.hostname_lookup;value.allow_new_users=active.allow_new_users;}
    if(file_administration_)file_administration_->expire_idle_views();
    const auto engine=std::atomic_load(&engine_);
    if(!engine)for(auto& node:value.nodes)if(node.number&&node.number<=node_enablement_.size())node.enabled=node_enablement_[node.number-1];
    if(engine){const auto current=engine->status();value.nodes.resize(value.lifecycle.configured_nodes);
        for(std::size_t i=0;i<value.nodes.size();++i){auto& node=value.nodes[i];const auto& slot=current.pool.slots[i];
            node.number=static_cast<std::uint16_t>(i+1);node.enabled=slot.enabled;node.available=current.listening&&!slot.quarantined;
            node.state=node.available&&node.enabled?core::NodeState::ready:core::NodeState::offline;node.assignment_sequence=slot.identity.generation;
            const auto caller=std::find_if(current.callers.begin(),current.callers.end(),[&](const auto& item){return item.number==node.number;});
            if(caller!=current.callers.end()){node=*caller;node.enabled=slot.enabled;}
            else if(current.event_held && node.enabled && node.available) {
                node.available=false;node.state=core::NodeState::offline;
                node.activity="External event";node.details="Caller admission paused";
            }
        }
        value.telnet_active=current.listening&&current.telnet_port;value.ssh_active=current.listening&&current.ssh_port;
        if(current.telnet_port)value.telnet_port=current.telnet_port;
        if(current.ssh_port)value.ssh_port=current.ssh_port;
        value.lifecycle.active_work.caller_sessions=current.pool.active;
        value.state=current.listening?(current.draining?app::AlphaRuntimePresentationState::draining:app::AlphaRuntimePresentationState::ready):app::AlphaRuntimePresentationState::recovery_required;
        value.lifecycle.state=current.listening?(current.draining?app::RuntimeState::draining:app::RuntimeState::online):app::RuntimeState::recovery_required;
        value.diagnostic_code=current.diagnostic;}
    if(journal_){value.logs=journal_->snapshot();value.log_health=journal_->health();value.reporting=journal_->reporting();}
    if(jobs_){value.scheduler=jobs_->scheduler_status();if(jobs_->busy())++value.lifecycle.active_work.supervised_processes;}
    if(!journal_fault_.empty()){value.state=app::AlphaRuntimePresentationState::recovery_required;value.lifecycle.state=app::RuntimeState::recovery_required;value.diagnostic_code=journal_fault_;}
    return value;
}
app::AlphaControlResponse LinuxRuntimeHost::bootstrap(app::AlphaBootstrapRequest& request){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!journal_fault_.empty())return {Code::recovery_required,journal_fault_};
    if(bbs_owned())return {Code::invalid_request,"bootstrap_bbs_running"};
    const auto result=store_.bootstrap(request);
    if(result.code==Code::ok&&!ensure_journals())return {Code::recovery_required,journal_fault_};
    return result;
}
app::AlphaControlResponse LinuxRuntimeHost::prepare_copied_file_bindings(const std::function<bool()>& cancelled){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(bbs_owned())return {Code::invalid_request,"copied_file_prepare_bbs_running"};
    if(file_administration_&&file_administration_->active())return {Code::invalid_request,"copied_file_prepare_file_job_active"};
    if(jobs_&&jobs_->busy())return {Code::invalid_request,"copied_file_prepare_scheduler_active"};
    stop_jobs();
    file_administration_.reset();
    if(!journal_fault_.empty())return {Code::recovery_required,journal_fault_};
    const auto result=store_.prepare_copied_file_bindings(cancelled);
    if(result.code==Code::ok){
        if(!epoch_.valid()){epoch_=store_.identifiers().generate();generations_={};}
        pending_ticket_=0;pending_callers_.clear();
        if(!ensure_journals())return {Code::recovery_required,journal_fault_};
    }
    return result;
}
std::optional<app::AlphaConfigurationSnapshot> LinuxRuntimeHost::configuration(){
    std::lock_guard<std::recursive_mutex> lock(operations_);apply_pending_configuration();if(!store_.configuration())return {};
    return app::AlphaConfigurationSnapshot{active_configuration_.value_or(*store_.configuration()),*store_.configuration(),
        bbs_owned()?configuration_activation_:app::AlphaConfigurationActivation::saved_next_start,bbs_owned()?configuration_detail_:"BBS stopped"};
}
app::AlphaConfigurationUpdateResult LinuxRuntimeHost::save_configuration(const app::AlphaConfigurationUpdateRequest& request){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!journal_fault_.empty()){
        app::AlphaConfigurationUpdateResult result;result.code=Code::recovery_required;result.diagnostic_code=journal_fault_;
        result.detail="Settings were not saved. Close and recover management before making changes.";
        if(store_.configuration())result.active_revision=result.saved_revision=store_.configuration()->configuration_revision;
        return result;
    }
    if(!bbs_owned()){
        if(jobs_&&jobs_->busy()){app::AlphaConfigurationUpdateResult result;result.code=Code::configuration_failure;result.diagnostic_code="scheduler_work_already_active";return result;}
        stop_jobs();
        auto result=store_.save_stopped_configuration(request);
        if(result.code==Code::ok&&!ensure_journals()){
            result.code=Code::recovery_required;result.diagnostic_code=journal_fault_;
            result.detail="Settings were saved. Journal configuration needs recovery before the BBS can start.";
        }
        ensure_jobs();
        return result;
    }
    auto result=store_.check_configuration_update(request);if(result.code!=Code::ok)return result;
    const auto engine=std::atomic_load(&engine_);if(!engine||!active_configuration_){result.code=Code::configuration_failure;result.diagnostic_code="configuration_active_state_unavailable";return result;}
    const auto& candidate=request.configuration;const auto& active=*active_configuration_;
    result.active_revision=active.configuration_revision;result.saved_revision=store_.configuration()->configuration_revision;result.activation=app::AlphaConfigurationActivation::refused;
    const bool restart=candidate.logical_nodes!=active.logical_nodes||candidate.password_mode!=active.password_mode||candidate.maximum_connections_per_source!=active.maximum_connections_per_source||candidate.maximum_sessions_per_account!=active.maximum_sessions_per_account||candidate.source_connection_exceptions!=active.source_connection_exceptions;
    const bool telnet=candidate.telnet.enabled!=active.telnet.enabled||candidate.telnet.bind_address!=active.telnet.bind_address||candidate.telnet.port!=active.telnet.port;
    const bool ssh=candidate.ssh.enabled!=active.ssh.enabled||candidate.ssh.bind_address!=active.ssh.bind_address||candidate.ssh.port!=active.ssh.port;
    const bool hostname=candidate.hostname_lookup!=active.hostname_lookup;
    const auto callers=engine->status().pool.active;
    if(!restart&&callers&&((telnet&&ssh)||(hostname&&(telnet||ssh)))){
        result.code=Code::configuration_failure;result.diagnostic_code=telnet&&ssh?"multiple_listener_swap_requires_zero_active_callers":"hostname_and_listener_change_requires_zero_active_callers";result.detail="Settings were not saved. This combined change needs zero active callers.";return result;
    }
    if(restart||(callers&&hostname)){
        auto saved=store_.save_live_configuration(request);saved.active_revision=active.configuration_revision;
        if(saved.code!=Code::ok)return saved;
        if(restart){pending_configuration_.reset();configuration_activation_=app::AlphaConfigurationActivation::restart_required;configuration_detail_="Saved. Restart the BBS to apply the node, password or connection policy.";saved.diagnostic_code="configuration_saved_restart_required";}
        else{pending_configuration_=candidate;configuration_activation_=app::AlphaConfigurationActivation::pending_safe_boundary;configuration_detail_="Saved. Waiting for active callers to finish.";saved.diagnostic_code="configuration_pending_active_callers";}
        saved.activation=configuration_activation_;saved.detail=configuration_detail_;journal_->emit({app::OperationalKind::configuration_saved,{}, {}});return saved;
    }
    if(!activate_configuration(candidate,[&]{result=store_.save_live_configuration(request);return result.code==Code::ok;})){
        if(result.code==Code::ok){result.code=Code::configuration_failure;result.diagnostic_code="configuration_live_activation_refused";result.detail="Settings were not applied. The listener, scheduler or journal could not accept the change.";}
        result.active_revision=active_configuration_->configuration_revision;result.saved_revision=store_.configuration()->configuration_revision;result.activation=app::AlphaConfigurationActivation::refused;return result;
    }
    result.code=Code::ok;result.activation=app::AlphaConfigurationActivation::applied;result.active_revision=result.saved_revision=candidate.configuration_revision;result.diagnostic_code="configuration_applied";result.detail="Applied";return result;
}
bool LinuxRuntimeHost::activate_configuration(const app::OperationalConfiguration& candidate,const std::function<bool()>& publish){
    const auto engine=std::atomic_load(&engine_);if(!engine||!active_configuration_||!jobs_||!journal_||!durable_||!publish)return false;
    auto prepared=candidate;const auto previous=*active_configuration_;
    const auto rollback_journals=[&]{const bool log=journal_->configure(previous.logging);const bool durable=durable_->configure_logging(previous.logging);const bool zone=durable_->reconfigure_time_zone(previous.bbs_time_zone);if(!log||!durable||!zone)journal_fault_="configuration_journal_rollback_failed";};
    const bool applied=jobs_->reconfigure_time_zone(candidate.bbs_time_zone,[&]{return engine->reconfigure(candidate,[&]{
        if(!journal_->configure(candidate.logging)||!durable_->configure_logging(candidate.logging)||!durable_->reconfigure_time_zone(candidate.bbs_time_zone)){rollback_journals();return false;}
        if(!publish()){rollback_journals();return false;}
        return true;
    });});
    if(!applied){if(store_.state()!=app::AlphaRuntimePresentationState::ready)journal_fault_=store_.diagnostic();if(!journal_fault_.empty())(void)engine->set_draining(true);return false;}
    active_configuration_=std::move(prepared);pending_configuration_.reset();configuration_activation_=app::AlphaConfigurationActivation::applied;configuration_detail_="Applied";
    if(const auto mailer=std::atomic_load(&ftn_engine_))mailer->set_bbs_time_zone(candidate.bbs_time_zone);
    journal_->emit({app::OperationalKind::configuration_saved,{}, {}});return true;
}
void LinuxRuntimeHost::apply_pending_configuration(){
    const auto engine=std::atomic_load(&engine_);if(!pending_configuration_||!engine||!journal_fault_.empty()||engine->status().pool.active||!jobs_||jobs_->busy())return;
    if(!engine->set_draining(true))return;
    if(engine->status().pool.active){(void)engine->set_draining(false);return;}
    const auto candidate=*pending_configuration_;
    (void)activate_configuration(candidate,[&]{return store_.configuration()&&app::equivalent_operational_configuration(*store_.configuration(),candidate);});
    if(journal_fault_.empty())(void)engine->set_draining(false);
}
bool LinuxRuntimeHost::store_ready()const noexcept{
    return store_.state()==app::AlphaRuntimePresentationState::ready&&store_.configuration().has_value()&&journal_fault_.empty();
}
bool LinuxRuntimeHost::ensure_journals(){
    if(!journal_fault_.empty())return false;
    if(store_.state()!=app::AlphaRuntimePresentationState::ready||!store_.configuration()){journal_fault_="management_journals_require_setup";return false;}
    try{
        const auto& config=*store_.configuration();const auto owner=std::to_wstring(geteuid());
        if(!sounds_||sound_epoch_!=epoch_){sounds_=std::make_shared<app::EventSoundStream>(epoch_);sound_epoch_=epoch_;}
        if(!journal_)journal_=std::make_shared<AlphaOperationalJournal>(root_/"logs/sfxruntime.log",owner,epoch_,sounds_);
        if(!journal_->configure(config.logging))throw std::runtime_error("management_journal_configuration_failed");
        if(!durable_){
            auto durable=std::make_shared<AlphaDurableJournal>(*store_.storage(),store_.identifiers(),root_,owner,config.bbs_time_zone,journal_);
            if(!durable->configure_logging(config.logging)||!durable->start())throw std::runtime_error("management_durable_recovery_failed");
            durable_=std::move(durable);
            journal_->attention_sink([weak=std::weak_ptr<AlphaDurableJournal>(durable_)](app::AttentionRecord record){
                if(const auto durable=weak.lock())durable->attention(std::move(record));
            });
            journal_->emit({app::OperationalKind::runtime_started,{}, {}});
        }else if(!durable_->configure_logging(config.logging)||!durable_->reconfigure_time_zone(config.bbs_time_zone))
            throw std::runtime_error("management_durable_configuration_failed");
        store_.other_board_log([weak=std::weak_ptr<AlphaDurableJournal>(durable_)](const auto& event){
            const auto journal=weak.lock();return journal&&journal->other_board(event);
        });
        ensure_clamav();journal_fault_.clear();ensure_jobs();return true;
    }catch(const std::exception& error){journal_fault_=error.what();return false;}
}
void LinuxRuntimeHost::ensure_clamav(){
    if(clamav_)return;
    namespace native=platform::linux_host;
    bool policy_failure{};
    try{
        auto bin=native::open_owned_directory(root_/"bin");struct stat info{};
        if(fstatat(bin.get(),"clamav",&info,AT_SYMLINK_NOFOLLOW)!=0){if(errno==ENOENT)return;throw std::runtime_error("clamav_component_unavailable");}
        auto files=store_.storage()->file_configuration({true,{}});
        if(!files||!files.value){policy_failure=true;throw std::runtime_error("clamav_initial_scan_policy_unavailable");}
        if(files.value->policy.revision==1){app::FileConfigurationChange change;change.operation=store_.identifiers().generate();change.action=app::FileConfigurationAction::policy;
            change.expected_catalog_revision=files.value->policy.catalog_revision;change.policy=files.value->policy;change.policy.scan_uploads=true;
            if(!store_.storage()->change_file_configuration({true,{}},change)){policy_failure=true;throw std::runtime_error("clamav_initial_scan_policy_unavailable");}}
        native::initialize_linux_clamav_data(root_);
        clamav_=std::make_unique<LinuxClamAvWorker>(root_,clock_,journal_);
    }catch(...){journal_->clamav_health(false,false,false,"clamav_component_unavailable");if(policy_failure)throw;}
}
LinuxClamAvWorkerSnapshot LinuxRuntimeHost::clamav_status(){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(clamav_){auto value=clamav_->snapshot();
        if(value.active||value.queued){value.status.busy=true;value.status.definitions_verified=false;value.status.diagnostic="clamav_status_operation_busy";}
        else value.status=platform::linux_host::inspect_linux_clamav(root_);
        return value;}
    LinuxClamAvWorkerSnapshot value;value.status=platform::linux_host::inspect_linux_clamav(root_);return value;
}
std::optional<std::string> LinuxRuntimeHost::clamav_update_log(){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    try{auto data=platform::linux_host::open_owned_directory(root_/"data/clamav");
        const auto bytes=platform::linux_host::read_private_file(data.get(),"update-last.log",65536);
        std::string text(bytes.begin(),bytes.end());for(char& c:text)if((static_cast<unsigned char>(c)<32&&c!='\n'&&c!='\t')||static_cast<unsigned char>(c)>=127)c='?';return text;
    }catch(...){return {};}
}
std::uint64_t LinuxRuntimeHost::request_clamav_update(){std::lock_guard<std::recursive_mutex> lock(operations_);if(store_ready())ensure_clamav();return clamav_?clamav_->request_update():0;}
bool LinuxRuntimeHost::cancel_clamav_update(std::uint64_t operation){std::lock_guard<std::recursive_mutex> lock(operations_);return clamav_&&clamav_->cancel(operation);}
void LinuxRuntimeHost::pause_clamav_automatic(bool paused){std::lock_guard<std::recursive_mutex> lock(operations_);if(clamav_)clamav_->pause_automatic(paused);}
std::optional<app::OperationalLogPage> LinuxRuntimeHost::log_page(app::OperationalLogCursor cursor){
    std::lock_guard<std::recursive_mutex> lock(operations_);return journal_?journal_->page(cursor):std::nullopt;
}
app::LocalMessageConfigurationSnapshot LinuxRuntimeHost::message_configuration_snapshot(const app::LocalMessageConfigurationQuery& query){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    return app::LocalMessageAdministration(*store_.storage(),app::LocalAccountAuthority::installation_owner).snapshot(query);
}
app::LocalMessageConfigurationResult LinuxRuntimeHost::change_message_configuration(const app::MessageConfigurationChange& change){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    // Deletion belongs to the existing held offline transaction, as on Windows.
    if(change.action==app::MessageConfigurationAction::delete_echo||change.action==app::MessageConfigurationAction::delete_group)
        return {app::LocalAccountResultCode::unauthorized};
    return app::LocalMessageAdministration(*store_.storage(),app::LocalAccountAuthority::installation_owner).execute(change);
}
app::LocalDoorSnapshot LinuxRuntimeHost::door_configuration_snapshot(const app::LocalDoorQuery& query){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    return door_snapshot_cache_.query(*store_.storage(),app::LocalAccountAuthority::installation_owner,query);
}
std::optional<app::DoorCatalog> LinuxRuntimeHost::complete_door_catalog(){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    const auto result=store_.storage()->door_configuration({true,{}});return result&&result.value?std::optional<app::DoorCatalog>(*result.value):std::nullopt;
}
app::LocalDoorSnapshot LinuxRuntimeHost::door_bulletin_snapshot(const app::LocalDoorBulletinQuery& query){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    return door_bulletin_snapshot_cache_.query(*store_.storage(),app::LocalAccountAuthority::installation_owner,query,[&](app::DoorBulletinId id,bool ansi,std::int64_t revision){return preview_door_bulletin(id,ansi,revision);});
}
std::optional<app::DoorBulletinCatalog> LinuxRuntimeHost::complete_door_bulletin_catalog(){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};const auto result=store_.storage()->door_bulletin_configuration({true,{}});return result&&result.value?result.value:std::nullopt;
}
app::LocalDoorResult LinuxRuntimeHost::change_door_bulletin(const app::DoorBulletinChange& change){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};const auto result=app::administer_door_bulletin(*store_.storage(),app::LocalAccountAuthority::installation_owner,change);if(result.code==app::LocalAccountResultCode::ok)door_bulletin_snapshot_cache_.clear();return result;
}
app::DoorBulletinPreview LinuxRuntimeHost::preview_door_bulletin(app::DoorBulletinId id,bool ansi,std::int64_t expected_revision){
    std::lock_guard<std::recursive_mutex> lock(operations_);app::DoorBulletinPreview failed;failed.id=id;failed.title="Door Bulletin "+std::to_string(id.value);failed.diagnostic="BBS management is unavailable.";if(!store_ready())return failed;
    const auto sources=platform::linux_host::door_bulletin_sources(root_,store_.file_bindings(),[weak=std::weak_ptr<AlphaOperationalJournal>(journal_)](app::DoorBulletinId id,std::string_view diagnostic){if(const auto log=weak.lock())log->problem(app::AttentionKind::door_bulletin_unavailable,"Door Bulletin "+std::to_string(id.value),std::string(diagnostic));});
    auto request=app::door_bulletin_preview_request(id,ansi?app::GraphicsMode::ansi:app::GraphicsMode::ascii,store_.configuration()->bbs_name,SFX_PRODUCT_DISPLAY_VERSION,store_.configuration()->logical_nodes);return app::preview_door_bulletin(*store_.storage(),id,std::move(request),sources,expected_revision);
}
app::LocalDoorResult LinuxRuntimeHost::change_door_configuration(const app::DoorConfigurationChange& change){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    const auto result=app::administer_door(*store_.storage(),app::LocalAccountAuthority::installation_owner,change);
    if(result.code==app::LocalAccountResultCode::ok){
        door_snapshot_cache_.clear();
    }
    door_bulletin_snapshot_cache_.clear();
    return result;
}
app::LocalDoorResult LinuxRuntimeHost::remove_door_with_placements(const app::DoorConfigurationChange& change){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    if(change.action!=app::DoorConfigurationAction::remove_door||!app::valid_door_change(change))return {app::LocalAccountResultCode::invalid_request};
    const auto current=store_.storage()->door_configuration({true,{}});
    if(!current||!current.value)return {app::LocalAccountResultCode::storage_failure};
    if(current.value->revision!=change.expected_revision)return {app::LocalAccountResultCode::conflict};
    if(!app::find_door(*current.value,change.door.id))return {app::LocalAccountResultCode::policy_rejected};
    auto next=change;bool changed{};
    // Match the Windows editor's placement-first workflow. Each step retains
    // shared storage CAS/receipts. A later refusal must report possible partial
    // work rather than claiming the whole removal was rolled back.
    for(const auto& entry:current.value->entries){
        if(entry.kind!=app::DoorEntryKind::door||entry.target!=change.door.id)continue;
        app::DoorConfigurationChange remove;remove.operation=store_.identifiers().generate();remove.expected_revision=next.expected_revision;remove.action=app::DoorConfigurationAction::remove_entry;remove.entry=entry;
        const auto result=app::administer_door(*store_.storage(),app::LocalAccountAuthority::installation_owner,remove);
        if(result.code!=app::LocalAccountResultCode::ok)return changed?app::LocalDoorResult{app::LocalAccountResultCode::storage_failure}:result;
        changed=true;next.expected_revision=result.result.revision;door_snapshot_cache_.clear();door_bulletin_snapshot_cache_.clear();
    }
    const auto result=app::administer_door(*store_.storage(),app::LocalAccountAuthority::installation_owner,next);
    if(result.code==app::LocalAccountResultCode::ok){
        door_snapshot_cache_.clear();
    }
    door_bulletin_snapshot_cache_.clear();
    return changed&&result.code!=app::LocalAccountResultCode::ok?app::LocalDoorResult{app::LocalAccountResultCode::storage_failure}:result;
}
#include "linux_file_management.inc"
app::LocalQwkPolicyResult LinuxRuntimeHost::qwk_policy_snapshot(){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    return app::LocalQwkAdministration(*store_.storage(),app::LocalAccountAuthority::installation_owner).snapshot();
}
app::LocalQwkPolicyResult LinuxRuntimeHost::change_qwk_policy(const app::QwkPolicyChange& change){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    return app::LocalQwkAdministration(*store_.storage(),app::LocalAccountAuthority::installation_owner).execute(change);
}
app::LocalClassicInformationSnapshot LinuxRuntimeHost::classic_information_snapshot(std::int64_t after){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    return app::LocalClassicInformationAdministration(*store_.storage(),store_.identifiers(),clock_,app::LocalAccountAuthority::installation_owner).snapshot(after);
}
app::LocalAccountResult LinuxRuntimeHost::change_classic_information(const app::ClassicInformationChange& change){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    return app::LocalClassicInformationAdministration(*store_.storage(),store_.identifiers(),clock_,app::LocalAccountAuthority::installation_owner).execute(change);
}
app::LocalAccountSnapshot LinuxRuntimeHost::account_snapshot(const app::LocalAccountQuery& query){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready()||!store_.authentication())return {};
    app::LocalAccountAdministration service(*store_.storage(),*store_.storage(),*store_.authentication(),*this,store_.identifiers(),clock_,*store_.configuration(),app::LocalAccountAuthority::installation_owner);
    auto result=service.snapshot(query);const auto engine=std::atomic_load(&engine_);
    result.online=query.account_id&&engine&&engine->account_online(query.account_id);return result;
}
app::LocalSecuritySnapshot LinuxRuntimeHost::complete_security_snapshot(){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    LinuxSecurityPublication publication;
    return app::LocalSecurityAdministration(*store_.storage(),publication,store_.identifiers(),clock_,app::LocalAccountAuthority::installation_owner).snapshot();
}
LinuxSecurityRemovalPreview LinuxRuntimeHost::security_removal_preview(std::uint16_t level,std::uint32_t offset){
    std::lock_guard<std::recursive_mutex> lock(operations_);LinuxSecurityRemovalPreview result;
    if(!store_ready()||level<1||level>999||offset>1000000)return result;
    result.security=complete_security_snapshot();if(result.security.code!=app::LocalAccountResultCode::ok)return result;
    const auto found=std::find_if(result.security.levels.begin(),result.security.levels.end(),[&](const auto& value){return value.level==level&&value.state==app::SecurityLevelState::active;});
    if(found==result.security.levels.end()){result.code=app::LocalAccountResultCode::invalid_request;return result;}
    std::vector<std::pair<app::AccountId,std::int64_t>> revisions;bool complete=false;
    for(std::size_t position=0;position<=1000000;position+=256){
        app::AccountListQuery query;query.security_level=level;query.limit=256;query.offset=position;
        const auto page=store_.storage()->list_accounts(query);if(!page||!page.value)return {};
        for(const auto& account:*page.value)if(account.state!=app::AccountState::tombstoned){
            if(result.total>=offset&&result.accounts.size()<32)result.accounts.emplace_back(account.account_id,account.display_name.value_or(""));
            ++result.total;revisions.emplace_back(account.account_id,account.revision);
        }
        if(page.value->size()<256){complete=true;break;}
    }
    if(!complete||offset>result.total)return {};
    LinuxSecurityPublication publication;result.affected_hash=publication.fingerprint(app::security_reassignment_definition(std::move(revisions)));
    if(result.affected_hash.size()!=64)return {};
    result.code=app::LocalAccountResultCode::ok;return result;
}
app::LocalSecuritySnapshot LinuxRuntimeHost::security_snapshot(std::uint32_t offset){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready()||offset>999)return {};
    LinuxSecurityPublication publication;auto result=app::LocalSecurityAdministration(*store_.storage(),publication,store_.identifiers(),clock_,app::LocalAccountAuthority::installation_owner).snapshot();
    if(result.code!=app::LocalAccountResultCode::ok)return result;
    if(offset>result.levels.size())return {};
    result.levels.erase(result.levels.begin(),result.levels.begin()+offset);result.has_more=result.levels.size()>32;
    if(result.has_more)result.levels.resize(32);
    return result;
}
app::LocalAccountResult LinuxRuntimeHost::change_security(const app::LocalSecurityChange& change){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    LinuxSecurityPublication publication;publication.apply=[&](const app::LocalSecuritySnapshot& truth){
        const auto published=store_.publish_security_snapshot(truth);if(!published)return published;
        const auto engine=std::atomic_load(&engine_);
        if(active_configuration_){active_configuration_->security_policy_revision=store_.configuration()->security_policy_revision;active_configuration_->security_policy_hash=store_.configuration()->security_policy_hash;
            if(configuration_activation_==app::AlphaConfigurationActivation::applied)active_configuration_->configuration_revision=store_.configuration()->configuration_revision;}
        if(pending_configuration_)pending_configuration_=*store_.configuration();
        return !engine||engine->refresh_security_policy(active_configuration_.value_or(*store_.configuration()))?app::StorageStatus{}:app::StorageStatus{app::StorageErrorCode::internal,"live_security_enforcement_failed"};
    };
    const auto result=app::LocalSecurityAdministration(*store_.storage(),publication,store_.identifiers(),clock_,app::LocalAccountAuthority::installation_owner).execute(change);
    if(result.committed)journal_->emit({app::OperationalKind::account_administration,{}, {}});
    if(result.committed&&result.code!=app::LocalAccountResultCode::ok){
        const auto engine=std::atomic_load(&engine_);
        if(engine)(void)engine->set_draining(true);
        const bool retired=!engine||engine->stop();
        journal_fault_=retired?"security_policy_publication_failed":"security_policy_caller_cleanup_required";
    }
    return result;
}
app::AlphaControlResponse LinuxRuntimeHost::clear_throttle(const app::LocalThrottleClearRequest& request){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!app::valid_local_throttle_clear(request))return {Code::invalid_request,"throttle_clear_invalid"};
    if(!store_ready()||!store_.storage()||!journal_)return {Code::configuration_failure,"throttle_clear_not_ready"};
    const app::AuthenticationAuditContext audit{store_.identifiers().generate(),store_.identifiers().generate(),{},clock_.unix_seconds()};
    const auto engine=std::atomic_load(&engine_);app::StorageStatus cleared;
    if(engine)cleared=engine->clear_throttle(request,audit);
    else{adapters::authentication::Phase3aRateControlCoordinator rates(*store_.storage(),audit.occurred_utc_seconds,4095);cleared=rates.clear_locally(request,audit);}
    if(cleared)journal_->emit({app::OperationalKind::throttle_cleared,{}, {}});
    return cleared?app::AlphaControlResponse{Code::ok,"throttle_cleared"}:app::AlphaControlResponse{Code::storage_failure,"throttle_clear_failed"};
}
LinuxWebAuthenticationResult LinuxRuntimeHost::authenticate_web(std::string& password,std::string_view source_ip){
    struct Wipe{std::string& text;~Wipe(){app::detail::explicitly_erase_bytes(text.data(),text.size());text.clear();}}wipe{password};
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(offline_status().active||compaction_status().active)return {};
    if(!store_ready()||!store_.authentication()||!app::valid_source_ip_text(source_ip)||password.empty()||password.size()>app::kPasswordMaximum||web_authentication_epoch()==UINT64_MAX)return {};
    const app::Phase3aRateScopeSet scopes{{},std::string(source_ip),true,true,true};const auto now=clock_.unix_seconds();
    const auto engine=std::atomic_load(&engine_);std::unique_ptr<adapters::authentication::Phase3aRateControlCoordinator> stopped;
    if(!engine)stopped=std::make_unique<adapters::authentication::Phase3aRateControlCoordinator>(*store_.storage(),now,4095);
    const auto admission=engine?engine->web_authentication_admission(scopes,now):stopped->try_admission(scopes,now);
    if(admission.code!=app::Phase3aRateGateCode::admitted)return {false,admission.applied_delay_seconds,0};
    const app::AuthenticationAuditContext audit{store_.identifiers().generate(),store_.identifiers().generate(),{},now};
    app::CancellationSource cancellation;const auto result=store_.authentication()->verify_system_password(password,now,audit,cancellation.token());
    const bool verified=result.authenticated();
    const auto recorded=engine?engine->web_authentication_result(scopes,now,verified):verified?stopped->record_success(scopes,now):stopped->record_failure(scopes,now);
    if(!recorded||!verified)return {};
    return {true,0,web_authentication_epoch()};
}
app::LocalAccountResult LinuxRuntimeHost::administer_account(app::LocalAccountRequest& request){
    struct Wipe{app::LocalAccountRequest& value;~Wipe(){app::erase_local_account_secrets(value);}}wipe{request};
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready()||!store_.authentication())return {};
    app::LocalAccountAdministration service(*store_.storage(),*store_.storage(),*store_.authentication(),*this,store_.identifiers(),clock_,*store_.configuration(),app::LocalAccountAuthority::installation_owner);
    app::CancellationSource cancellation;const auto result=service.execute(request,cancellation.token());
    if(result.committed)journal_->emit({app::OperationalKind::account_administration,{}, {}});
    if(result.code==app::LocalAccountResultCode::revocation_failed){
        const auto engine=std::atomic_load(&engine_);if(engine)(void)engine->set_draining(true);
        journal_fault_="account_administration_revocation_failed";
    }
    return result;
}
std::optional<app::LocalControlFrame> LinuxRuntimeHost::control(app::LocalControlFrame& frame){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    struct Wipe{std::vector<std::uint8_t>& bytes;~Wipe(){app::detail::explicitly_erase_bytes(bytes.data(),bytes.size());}}wipe{frame.payload};
    app::LocalControlFrame response;response.request_id=frame.request_id;
    if(frame.type==Type::status_request){const auto request=app::decode_status_request(frame.payload);if(!request)return {};
        if(!request->snapshot_sequence&&!status_cache_.capture(status(),request->screen_node))return {};
        response.type=Type::status_response;response.payload=status_cache_.chunk(*request);
    }else if(frame.type==Type::log_page_request){
        const auto cursor=app::decode_log_cursor(frame.payload);if(!cursor)return {};
        const auto page=log_page(*cursor);if(!page)return {};
        response.type=Type::log_page_response;response.payload=app::encode_log_page(*page);
    }else if(frame.type==Type::node_command_request){const auto request=app::decode_node_command_request(frame.payload);if(!request)return {};
        response.type=Type::node_command_response;response.payload=app::encode_control_response(command_node(*request));
    }else if(frame.type==Type::lifecycle_command_request){const auto request=app::decode_lifecycle_command_request(frame.payload);if(!request)return {};
        response.type=Type::lifecycle_command_response;response.payload=app::encode_control_response(command_lifecycle(*request));
    }else if(frame.type==Type::bootstrap_request){auto request=app::decode_bootstrap_request(frame.payload);if(!request)return {};
        struct Erase{app::AlphaBootstrapRequest& request;~Erase(){app::erase_bootstrap_secrets(request);}}erase{*request};
        response.type=Type::bootstrap_response;response.payload=app::encode_control_response(bootstrap(*request));
    }else if(frame.type==Type::configuration_snapshot_request&&frame.payload.empty()){
        const auto value=configuration();if(!value)return {};response.type=Type::configuration_snapshot_response;response.payload=app::encode_configuration_snapshot(*value);
    }else if(frame.type==Type::configuration_update_request){const auto request=app::decode_configuration_update_request(frame.payload);if(!request)return {};
        response.type=Type::configuration_update_response;response.payload=app::encode_configuration_update_result(save_configuration(*request));
    }else if(frame.type==Type::scheduler_settings_request){const auto request=app::decode_scheduler_settings_request(frame.payload);if(!request)return {};
        response.type=Type::scheduler_settings_response;response.payload=app::encode_control_response(configure_scheduler(*request));
    }else if(frame.type==Type::scheduler_run_request){const auto request=app::decode_scheduler_run_request(frame.payload);if(!request)return {};
        response.type=Type::scheduler_run_response;response.payload=app::encode_control_response(run_scheduler_now(*request));
    }else if(frame.type==Type::message_maintenance_query_request){const auto kind=app::decode_message_maintenance_kind(frame.payload);if(!kind)return {};
        response.type=Type::message_maintenance_query_response;response.payload=app::encode_message_maintenance_status(message_maintenance_status(*kind));
    }else if(frame.type==Type::message_maintenance_schedule_request){const auto request=app::decode_message_maintenance_schedule(frame.payload);if(!request)return {};
        response.type=Type::message_maintenance_schedule_response;response.payload=app::encode_control_response(configure_message_schedule(*request));
    }else if(frame.type==Type::message_maintenance_run_request){const auto kind=app::decode_message_maintenance_kind(frame.payload);if(!kind)return {};
        response.type=Type::message_maintenance_run_response;response.payload=app::encode_control_response(run_message_maintenance_now(*kind));
    }else if(frame.type==Type::message_configuration_query_request){
        const auto request=app::decode_message_configuration_query(frame.payload);if(!request)return {};
        response.type=Type::message_configuration_query_response;response.payload=app::encode_message_configuration_snapshot(message_configuration_snapshot(*request));
    }else if(frame.type==Type::message_configuration_change_request){
        const auto request=app::decode_message_configuration_change(frame.payload);if(!request)return {};
        response.type=Type::message_configuration_change_response;response.payload=app::encode_message_configuration_result(change_message_configuration(*request));
    }else if(frame.type==Type::door_configuration_query_request){
        const auto request=app::decode_door_configuration_query(frame.payload);if(!request)return {};
        response.type=Type::door_configuration_query_response;response.payload=app::encode_door_configuration_snapshot(door_configuration_snapshot(*request));
    }else if(frame.type==Type::door_configuration_change_request){
        const auto request=app::decode_door_configuration_change(frame.payload);if(!request)return {};
        response.type=Type::door_configuration_change_response;response.payload=app::encode_door_configuration_result(change_door_configuration(*request));
    }else if(frame.type==Type::door_bulletin_query_request){
        const auto request=app::decode_door_bulletin_query(frame.payload);if(!request)return {};
        response.type=Type::door_bulletin_query_response;response.payload=app::encode_door_configuration_snapshot(door_bulletin_snapshot(*request));
    }else if(frame.type==Type::door_bulletin_change_request){
        const auto request=app::decode_door_bulletin_change(frame.payload);if(!request)return {};
        response.type=Type::door_bulletin_change_response;response.payload=app::encode_door_configuration_result(change_door_bulletin(*request));
    }else if(frame.type==Type::file_configuration_query_request){
        const auto request=app::decode_file_configuration_query(frame.payload);if(!request)return {};
        response.type=Type::file_configuration_query_response;response.payload=app::encode_file_configuration_snapshot(file_configuration_snapshot(*request));
    }else if(frame.type==Type::file_configuration_change_request){
        const auto request=app::decode_file_configuration_change(frame.payload);if(!request)return {};
        response.type=Type::file_configuration_change_response;response.payload=app::encode_file_configuration_result(change_file_configuration(*request));
    }else if(frame.type==Type::file_management_query_request){
        const auto request=app::decode_file_management_read_request(frame.payload);if(!request)return {};
        response.type=Type::file_management_query_response;response.payload=app::encode_file_management_read_snapshot(query_file_management(*request));
    }else if(frame.type==Type::file_management_start_request){
        const auto request=app::decode_file_management_task(frame.payload);if(!request)return {};
        response.type=Type::file_management_start_response;response.payload=app::encode_file_management_status(start_file_management(*request));
    }else if(frame.type==Type::file_management_status_request||frame.type==Type::file_management_cancel_request){
        const auto request=app::decode_file_management_job(frame.payload);if(!request)return {};
        const bool cancel=frame.type==Type::file_management_cancel_request;
        response.type=cancel?Type::file_management_cancel_response:Type::file_management_status_response;
        response.payload=app::encode_file_management_status(cancel?cancel_file_management(*request):file_management_status(*request));
    }else if(frame.type==Type::ftn_configuration_request||frame.type==Type::ftn_hatch_catalog_request){
        const auto request=app::decode_ftn_configuration_request(frame.payload);if(!request)return {};
        const bool catalog=frame.type==Type::ftn_hatch_catalog_request;
        response.type=catalog?Type::ftn_hatch_catalog_response:Type::ftn_configuration_response;
        response.payload=app::encode_ftn_configuration_response(catalog?ftn_hatch_catalog(*request):ftn_configuration(*request));
    }else if(frame.type==Type::ftn_runtime_request){
        const auto request=app::decode_ftn_runtime_request(frame.payload);if(!request)return {};
        response.type=Type::ftn_runtime_response;response.payload=app::encode_ftn_runtime_response(ftn_runtime(*request));
    }else if(frame.type==Type::ftn_held_request){
        const auto request=app::decode_ftn_held_request(frame.payload);if(!request)return {};
        response.type=Type::ftn_held_response;response.payload=app::encode_ftn_held_response(ftn_held(*request));
    }else if(frame.type==Type::qwk_policy_query_request&&frame.payload.empty()){
        response.type=Type::qwk_policy_query_response;response.payload=app::encode_qwk_policy_result(qwk_policy_snapshot());
    }else if(frame.type==Type::qwk_policy_change_request){
        const auto request=app::decode_qwk_policy_change(frame.payload);if(!request)return {};
        response.type=Type::qwk_policy_change_response;response.payload=app::encode_qwk_policy_result(change_qwk_policy(*request));
    }else if(frame.type==Type::classic_information_query_request){
        const auto request=app::decode_classic_information_query(frame.payload);if(!request)return {};
        response.type=Type::classic_information_query_response;response.payload=app::encode_classic_information_snapshot(classic_information_snapshot(*request));
    }else if(frame.type==Type::classic_information_change_request){
        const auto request=app::decode_classic_information_change(frame.payload);if(!request)return {};
        response.type=Type::classic_information_change_response;response.payload=app::encode_account_result(change_classic_information(*request));
    }else if(frame.type==Type::chat_configuration_query_request&&frame.payload.empty()){
        response.type=Type::chat_configuration_query_response;response.payload=app::encode_chat_result(chat_configuration());
    }else if(frame.type==Type::chat_configuration_change_request){
        const auto request=app::decode_chat_configuration(frame.payload);if(!request)return {};
        response.type=Type::chat_configuration_change_response;response.payload=app::encode_chat_result(change_chat_configuration(*request));
    }else if(frame.type==Type::chat_operator_request){
        const auto request=app::decode_chat_operator_request(frame.payload);if(!request)return {};
        response.type=Type::chat_operator_response;response.payload=app::encode_chat_operator_snapshot(chat_operator(*request));
    }else if(frame.type==Type::chat_notice_request){
        const auto request=app::decode_chat_notice(frame.payload);if(!request)return {};
        response.type=Type::chat_notice_response;response.payload={static_cast<std::uint8_t>(send_chat_notice(*request))};
    }else if(frame.type==Type::security_query_request){
        const auto request=app::decode_account_query(frame.payload);if(!request||request->account_id||!request->name_prefix.empty())return {};
        response.type=Type::security_query_response;response.payload=app::encode_security_snapshot(security_snapshot(request->offset));
    }else if(frame.type==Type::security_change_request){
        const auto request=app::decode_security_change(frame.payload);if(!request)return {};
        response.type=Type::security_change_response;response.payload=app::encode_account_result(change_security(*request));
    }else if(frame.type==Type::account_query_request){
        const auto request=app::decode_account_query(frame.payload);if(!request)return {};
        response.type=Type::account_query_response;response.payload=app::encode_account_snapshot(account_snapshot(*request));
    }else if(frame.type==Type::account_command_request){
        auto request=app::decode_account_request(frame.payload);if(!request)return {};
        response.type=Type::account_command_response;response.payload=app::encode_account_result(administer_account(*request));
    }else return {};
    if(response.payload.empty())return {};
    return response;
}
app::ChatConfigurationResult LinuxRuntimeHost::chat_configuration(){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    const auto result=store_.storage()->chat_configuration({true,{}});
    return result&&result.value?app::ChatConfigurationResult{app::ChatResult::ok,*result.value}:app::ChatConfigurationResult{};
}
app::ChatConfigurationResult LinuxRuntimeHost::change_chat_configuration(const app::ChatConfiguration& value){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    const auto engine=std::atomic_load(&engine_);if(engine)return engine->change_chat_configuration(value);
    const auto result=store_.storage()->change_chat_configuration({true,{}},value);
    return result&&result.value?app::ChatConfigurationResult{app::ChatResult::ok,*result.value}:app::ChatConfigurationResult{app::ChatResult::conflict};
}
app::ChatOperatorSnapshot LinuxRuntimeHost::chat_operator(const app::ChatOperatorRequest& value){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    const auto engine=std::atomic_load(&engine_);return engine?engine->chat_operator(value):app::ChatOperatorSnapshot{};
}
app::ChatResult LinuxRuntimeHost::send_chat_notice(const app::ChatNoticeRequest& value){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return app::ChatResult::unavailable;
    const auto engine=std::atomic_load(&engine_);return engine?engine->send_chat_notice(value):app::ChatResult::unavailable;
}
void LinuxRuntimeHost::start_ftn(){
    if(ftn_engine_||!store_ready())return;
    try{
        const auto config=store_.storage()->ftn_configuration({true,{}});
        if(!config||!config.value){ftn_initialization_error_="ftn_configuration_unavailable";return;}
        auto mailer=std::make_shared<LinuxFtnEngine>(platform::linux_host::FtnInstallation{root_,store_.file_bindings()},*store_.storage(),*store_.storage(),epoch_,clock_,sounds_);
        mailer->set_bbs_time_zone(store_.configuration()->bbs_time_zone);mailer->configure(*config.value);std::atomic_store(&ftn_engine_,std::move(mailer));
    }catch(...){ftn_initialization_error_="ftn_engine_initialization_failed";}
}
void LinuxRuntimeHost::stop_ftn(){const auto engine=std::atomic_exchange(&ftn_engine_,std::shared_ptr<LinuxFtnEngine>{});if(engine)engine->stop();ftn_initialization_error_="ftn_bbs_stopped";}
void LinuxRuntimeHost::ensure_jobs(){
    if(jobs_||!store_ready()||!store_.file_bindings())return;
    const auto bindings=store_.file_bindings();const auto generation=std::to_string(store_.configuration()->security_policy_revision);
    jobs_=std::make_unique<LinuxRuntimeJobs>(root_,*store_.storage(),store_.identifiers(),clock_,store_.configuration()->bbs_time_zone,journal_,
        [this,bindings,generation](const app::ScheduledRun& run,const std::function<bool()>& cancelled){
            return execute_linux_backup(root_,*store_.storage(),bindings,std::atomic_load(&ftn_engine_),journal_,durable_,generation,run,cancelled);
        },[this,bindings](const std::string& destination)->std::shared_ptr<void>{
            return std::make_shared<platform::linux_host::BackupCollection>(platform::linux_host::open_backup_collection(root_,bindings,archive::BackupKind::manual,destination));
        });
    (void)jobs_->start(!bbs_owned());
}
void LinuxRuntimeHost::stop_jobs(){if(jobs_)jobs_->stop();jobs_.reset();}
LinuxScheduledJobSnapshot LinuxRuntimeHost::scheduled_job(std::string_view id){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready()||!jobs_)return {};return jobs_->scheduled_job(id);
}
app::AlphaControlResponse LinuxRuntimeHost::configure_scheduler(const app::AlphaSchedulerSettingsRequest& request,std::optional<std::uint32_t> retained){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready()||!jobs_)return {Code::configuration_failure,"scheduler_runtime_not_ready"};return jobs_->configure_scheduler(request,retained);
}
app::AlphaControlResponse LinuxRuntimeHost::run_scheduler_now(const app::AlphaSchedulerRunRequest& request){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(drain_deadline_)return {Code::shutting_down,"runtime_draining"};if(!store_ready()||!jobs_)return {Code::configuration_failure,"scheduler_runtime_not_ready"};return jobs_->run_scheduler_now(request);
}
app::MessageMaintenanceStatus LinuxRuntimeHost::message_maintenance_status(app::MessageMaintenanceKind kind){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready()||!jobs_)return {};return jobs_->message_maintenance_status(kind);
}
app::AlphaControlResponse LinuxRuntimeHost::configure_message_schedule(const app::MessageMaintenanceScheduleRequest& request){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready()||!jobs_)return {Code::configuration_failure,"scheduler_runtime_not_ready"};return jobs_->configure_message_schedule(request);
}
app::AlphaControlResponse LinuxRuntimeHost::run_message_maintenance_now(app::MessageMaintenanceKind kind){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready()||!jobs_)return {Code::configuration_failure,"scheduler_runtime_not_ready"};return jobs_->run_message_maintenance_now(kind);
}
app::SoundPollSnapshot LinuxRuntimeHost::sound_poll(const app::SoundPollRequest& request){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!store_ready()||!journal_)return {};
    return journal_->sound_poll(request,active_configuration_.value_or(*store_.configuration()).event_sounds);
}
#include "linux_ftn_management.inc"
#include "linux_compaction.inc"
#include "linux_offline_maintenance.inc"
#include "linux_message_deletion.inc"
#include "linux_file_group_deletion.inc"
} // namespace sfx::apps::runtime
