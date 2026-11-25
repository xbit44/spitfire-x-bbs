#include "linux_runtime_host.hpp"
#include "linux_node_controls.hpp"

namespace sfx::apps::runtime {
namespace app=application;
namespace {using Code=app::LocalControlResponseCode;}
LinuxStopResult LinuxRuntimeHost::restart_bbs(std::uint64_t expected_run,std::optional<std::uint64_t> confirmation){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    // A repeated or delayed Restart cannot stop the new BBS lifetime.
    if(!expected_run||expected_run!=run_sequence_||!bbs_owned())return {{Code::invalid_request,"restart_lifetime_changed"},0,0};
    const auto stopped=stop_request(confirmation,true);
    if(stopped.response.code!=Code::ok)return stopped;
    const auto result=start_bbs();
    lifecycle_diagnostic_=result.code==Code::ok?"restarted":result.diagnostic_code;
    return {result,0,0};
}
app::AlphaControlResponse LinuxRuntimeHost::command_lifecycle(const app::AlphaLifecycleCommandRequest& request,std::optional<std::uint64_t> expected_run){
    std::lock_guard<std::recursive_mutex> lock(operations_);const auto engine=std::atomic_load(&engine_);
    if(app::encode_lifecycle_command_request(request).empty()||!engine||!journal_fault_.empty()||
        (expected_run&&*expected_run!=run_sequence_))return {Code::invalid_request,"lifecycle_command_invalid"};
    if(request.command==app::AlphaLifecycleCommandKind::cancel_drain){
        if(!drain_deadline_)return {Code::invalid_request,"drain_no_longer_cancellable"};
        if(!engine->set_draining(false))return {Code::recovery_required,"admission_not_quiescent"};
        drain_deadline_.reset();if(jobs_)jobs_->pause(false);lifecycle_diagnostic_="drain_cancelled";
        if(journal_)journal_->emit({app::OperationalKind::drain_cancelled,{}, {}});
        return {Code::ok,lifecycle_diagnostic_};
    }
    const auto current=engine->status();
    if(drain_deadline_||!current.listening||current.draining)return {Code::invalid_request,"runtime_not_ready_to_drain"};
    const auto now=clock_.monotonic_millis(),duration=static_cast<std::uint64_t>(request.timeout_seconds)*1000;
    if(now>UINT64_MAX-duration)return {Code::invalid_request,"drain_deadline_exhausted"};
    if(!engine->set_draining(true))return {Code::recovery_required,"admission_not_quiescent"};
    if(jobs_)jobs_->pause(true);
    drain_deadline_=now+duration;lifecycle_diagnostic_="draining_new_callers_refused";
    pending_ticket_=0;pending_callers_.clear();
    if(journal_)journal_->emit({app::OperationalKind::drain_started,{}, {}});
    return {Code::ok,"drain_started"};
}
LinuxLifecycleSnapshot LinuxRuntimeHost::lifecycle_controls(){
    std::lock_guard<std::recursive_mutex> lock(operations_);const auto now=clock_.monotonic_millis();
    return {run_sequence_,drain_deadline_&&*drain_deadline_>now?*drain_deadline_-now:0,drain_deadline_.has_value(),lifecycle_diagnostic_};
}
void LinuxRuntimeHost::tick_controls(){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!drain_deadline_)return;
    const auto engine=std::atomic_load(&engine_);if(!engine){drain_deadline_.reset();return;}
    const bool finished=engine->status().pool.active==0&&(!jobs_||!jobs_->busy());
    if(finished||clock_.monotonic_millis()>=*drain_deadline_){
        const auto stopped=stop_owned_bbs();
        if(stopped.response.code!=Code::ok){journal_fault_=stopped.response.diagnostic_code;drain_deadline_.reset();}
    }
}
app::AlphaControlResponse LinuxRuntimeHost::command_node(const app::AlphaNodeCommandRequest& request,std::optional<app::NodeAssignmentIdentity> observed){
    std::lock_guard<std::recursive_mutex> lock(operations_);const auto engine=std::atomic_load(&engine_);
    if(!engine||!active_configuration_||!journal_fault_.empty()||app::encode_node_command_request(request).empty()||
        request.node_number>active_configuration_->logical_nodes)return {Code::invalid_request,"node_not_controllable"};
    const auto current=engine->status();const auto& slot=current.pool.slots[request.node_number-1];
    if(!current.listening)return {Code::invalid_request,"node_not_controllable"};
    if(observed&&(!(observed->runtime_epoch==epoch_)||observed->node!=request.node_number||observed->generation!=slot.identity.generation))return {Code::invalid_request,"node_selection_changed"};
    const app::NodeAssignmentIdentity target{request.expected_epoch,request.node_number,request.expected_generation};
    if(request.command==app::AlphaNodeCommandKind::disconnect){
        const bool requested=engine->disconnect(target);
        return {requested?Code::ok:Code::invalid_request,requested?"disconnect_requested":"caller_assignment_changed"};
    }
    if(request.command==app::AlphaNodeCommandKind::terminate_door){
        const bool completed=engine->terminate_door(target,request.expected_door_launch);
        return {completed?Code::ok:Code::invalid_request,completed?"door_terminated":"door_changed_or_termination_unconfirmed"};
    }
    if(current.draining)return {Code::invalid_request,"node_enablement_while_draining"};
    const bool enable=request.command==app::AlphaNodeCommandKind::enable;
    if(node_enablement_[request.node_number-1]==enable)return {Code::ok,enable?"node_already_enabled":"node_already_disabled"};
    auto candidate=node_enablement_;candidate[request.node_number-1]=enable;
    try{
        if(!engine->set_enabled(request.node_number,enable,observed,[&]{publish_linux_node_controls(root_,candidate);}))return {Code::invalid_request,"node_selection_changed"};
    }
    catch(const std::exception& error){
        // Publication can have reached disk before verification failed. Retire
        // the owned BBS and require management recovery before another Start.
        (void)stop_owned_bbs();journal_fault_=error.what();return {Code::recovery_required,journal_fault_};
    }
    node_enablement_=candidate;
    app::OperationalContext context;context.node=request.node_number;
    if(journal_)journal_->emit({enable?app::OperationalKind::node_enabled:app::OperationalKind::node_disabled,{},std::move(context)});
    return {Code::ok,enable?"node_enabled":"node_disabled"};
}
std::optional<app::AlphaNodeStatus> LinuxRuntimeHost::view_node(app::NodeAssignmentIdentity identity){
    std::lock_guard<std::recursive_mutex> lock(operations_);
    if(!identity.valid()||!(identity.runtime_epoch==epoch_))return {};
    const auto engine=std::atomic_load(&engine_);if(!engine)return {};
    const auto current=engine->status();
    const auto found=std::find_if(current.callers.begin(),current.callers.end(),[&](const auto& row){return row.screen_owner==identity&&row.caller_connected;});
    return found==current.callers.end()?std::nullopt:std::optional<app::AlphaNodeStatus>(*found);
}
}
