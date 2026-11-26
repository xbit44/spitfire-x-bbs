#include "linux_caller_registry.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>

namespace sfx::apps::runtime {
namespace app=application;
LinuxCallerRegistry::LinuxCallerRegistry(app::Identifier128 epoch,Ports ports)
    :epoch_(epoch),ports_(std::move(ports)) {
    if(!epoch.valid()||!ports_.retire||!ports_.stop_admission)
        throw std::invalid_argument("caller_registry_ports_required");
}
LinuxCallerRegistry::Entry* LinuxCallerRegistry::find(app::NodeAssignmentIdentity id) {
    if(!id.valid()||id.runtime_epoch!=epoch_||id.node>entries_.size())return nullptr;
    auto& entry=entries_[id.node-1];
    return entry.assignment&&entry.assignment->identity()==id?&entry:nullptr;
}
bool LinuxCallerRegistry::begin(const std::shared_ptr<app::CallerAssignment>& assignment) {
    if(!assignment)return false;
    const auto id=assignment->identity();
    std::lock_guard<std::mutex> lock(mutex_);
    if(!healthy_||!id.valid()||id.runtime_epoch!=epoch_||id.node>entries_.size()||
        assignment->stopping.load()||assignment->cleanup_health->failed.load())return false;
    const auto index=static_cast<std::size_t>(id.node-1);
    if(entries_[index].assignment||id.generation<=generations_[index])return false;
    entries_[index]={assignment,ledger_.checkpoint(),0,{},false};
    generations_[index]=id.generation;return true;
}
bool LinuxCallerRegistry::publish(app::NodeAssignmentIdentity id,app::AccountId account,
    app::Identifier128 session,bool sysop) {
    std::shared_ptr<app::CallerAssignment> refused;
    {
        std::lock_guard<std::mutex> lock(mutex_);auto* entry=find(id);
        if(!entry)return false;
        if(healthy_&&!entry->account&&!entry->assignment->stopping.load()&&
            !entry->assignment->cleanup_health->failed.load()&&
            ledger_.permits(entry->checkpoint,account,sysop,session)) {
            entry->account=account;entry->session=session;entry->sysop=entry->sysop_proof=sysop;return true;
        }
        refused=entry->assignment;
    }
    refused->cancel();return false;
}
std::uint64_t LinuxCallerRegistry::authorization_checkpoint() const {
    std::lock_guard<std::mutex> lock(mutex_);return ledger_.checkpoint();
}
bool LinuxCallerRegistry::refresh_access(app::NodeAssignmentIdentity id,app::AccountId account,
    app::Identifier128 session,bool sysop,std::uint64_t checkpoint) {
    std::shared_ptr<app::CallerAssignment> refused;
    {
        std::lock_guard<std::mutex> lock(mutex_);auto* entry=find(id);
        if(!entry)return false;
        if(healthy_&&entry->account==account&&entry->session==session&&(!sysop||entry->sysop_proof)&&
            checkpoint>=entry->checkpoint&&!entry->assignment->stopping.load()&&
            !entry->assignment->cleanup_health->failed.load()&&
            ledger_.permits(checkpoint,account,sysop,session)) {
            entry->sysop=sysop;if(!sysop)entry->sysop_proof=false;return true;
        }
        refused=entry->assignment;
    }
    refused->cancel();return false;
}
bool LinuxCallerRegistry::account_online(app::AccountId account) const {
    if(account<=0)return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for(const auto& entry:entries_)if(entry.assignment&&entry.account==account)return true;
    return false;
}
bool LinuxCallerRegistry::enforce_single_session(app::AccountId account) {
    std::vector<std::shared_ptr<app::CallerAssignment>> callers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!healthy_||account<=0)return false;
        for(const auto& entry:entries_)if(entry.assignment&&entry.account==account&&!entry.assignment->stopping.load())callers.push_back(entry.assignment);
    }
    if(callers.size()<2)return true;
    // A demotion from level999 revokes every parallel session; do not choose
    // an arbitrary survivor. The owning workers perform their normal cleanup.
    for(const auto& caller:callers)caller->cancel();
    return false;
}
bool LinuxCallerRegistry::refresh_account_authority(app::AccountId account,bool active,bool sysop,bool parallel){
    std::lock_guard<std::mutex> lock(mutex_);if(!healthy_||account<=0)return false;
    const auto count=std::count_if(entries_.begin(),entries_.end(),[&](const auto& entry){return entry.assignment&&entry.account==account&&!entry.assignment->stopping.load();});
    for(auto& entry:entries_)if(entry.assignment&&entry.account==account){
        if(!active||(!parallel&&count>1)||(sysop&&!entry.sysop_proof))entry.assignment->cancel();
        else{entry.sysop=sysop;if(!sysop)entry.sysop_proof=false;}
    }
    return true;
}
std::vector<app::AccountId> LinuxCallerRegistry::published_accounts()const{
    std::lock_guard<std::mutex> lock(mutex_);std::vector<app::AccountId> accounts;
    for(const auto& entry:entries_)if(entry.assignment&&entry.account&&std::find(accounts.begin(),accounts.end(),entry.account)==accounts.end())accounts.push_back(entry.account);
    return accounts;
}
bool LinuxCallerRegistry::forget(app::NodeAssignmentIdentity id,bool clean) {
    {
        std::lock_guard<std::mutex> lock(mutex_);auto* entry=find(id);
        if(!entry)return false;
        if(clean&&!entry->assignment->cleanup_health->failed.load()) {
            *entry={};return true;
        }
        // Retain the failed assignment and its immutable identity for recovery.
    }
    quarantine();return false;
}
std::optional<app::ChatIdentity> LinuxCallerRegistry::chat_identity(app::NodeAssignmentIdentity id){
    std::lock_guard<std::mutex> lock(mutex_);const auto* entry=find(id);
    if(!healthy_||!entry||!entry->account||!entry->session.valid()||entry->assignment->stopping.load()||entry->assignment->cleanup_health->failed.load())return {};
    return app::ChatIdentity{entry->session,entry->account,id.node};
}
void LinuxCallerRegistry::quarantine() {
    {std::lock_guard<std::mutex> lock(mutex_);healthy_=false;}
    // Never invoke host callbacks under the publication gate.
    try{ports_.stop_admission();}catch(...){}
}
bool LinuxCallerRegistry::sysop_authorized(const app::ChatIdentity& identity)const{
    std::lock_guard<std::mutex> lock(mutex_);
    if(!healthy_||identity.node<1||identity.node>entries_.size())return false;
    const auto& entry=entries_[identity.node-1];
    return entry.assignment&&entry.account==identity.account&&entry.session==identity.session&&entry.sysop&&entry.sysop_proof&&
        !entry.assignment->stopping.load()&&!entry.assignment->cleanup_health->failed.load()&&ledger_.permits(entry.checkpoint,entry.account,true,entry.session);
}
bool LinuxCallerRegistry::sysop_disconnect(const app::ChatIdentity& actor,const app::ChatIdentity& target){
    std::lock_guard<std::mutex> lock(mutex_);
    if(!healthy_||actor.node<1||actor.node>entries_.size()||target.node<1||target.node>entries_.size())return false;
    const auto& source=entries_[actor.node-1];const auto& selected=entries_[target.node-1];
    if(!source.assignment||source.account!=actor.account||source.session!=actor.session||!source.sysop||!source.sysop_proof||source.assignment->stopping.load()||source.assignment->cleanup_health->failed.load()||!ledger_.permits(source.checkpoint,source.account,true,source.session))return false;
    if(!selected.assignment||selected.account!=target.account||selected.session!=target.session||selected.assignment->stopping.load()||selected.assignment->cleanup_health->failed.load())return false;
    selected.assignment->cancel();return true;
}
bool LinuxCallerRegistry::healthy() const {
    std::lock_guard<std::mutex> lock(mutex_);return healthy_;
}
bool LinuxCallerRegistry::revoke(const app::SessionRevocationCommand& command) {
    std::array<std::shared_ptr<app::CallerAssignment>,app::kAlphaNodePoolCapacity> affected{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!healthy_||!ledger_.record(command))return false;
        for(std::size_t n=0;n<entries_.size();++n){
            const auto& entry=entries_[n];
            if(entry.assignment&&entry.account&&app::SessionRevocationLedger::matches(
                command,entry.account,entry.sysop,entry.session))affected[n]=entry.assignment;
        }
    }
    // Cancel the whole affected set first. The single total deadline is shared
    // across all nodes; it is not a ten-second allowance for each caller.
    using Clock=std::chrono::steady_clock;
    const auto deadline=Clock::now()+std::chrono::seconds(10);
    try {
        for(const auto& assignment:affected)if(assignment)assignment->cancel();
        for(const auto& assignment:affected)if(assignment){
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
            if(remaining<=0||!ports_.retire(assignment->identity(),static_cast<std::uint32_t>(remaining))) {
                quarantine();return false;
            }
        }
    }catch(...){quarantine();return false;}
    return healthy();
}
} // namespace sfx::apps::runtime
