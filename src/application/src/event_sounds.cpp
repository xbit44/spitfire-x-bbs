#include "sfx/application/event_sounds.hpp"
#include "sfx/application/operational_log.hpp"
#include <algorithm>
#include <limits>

namespace sfx::application {
std::optional<SoundEvent> operational_sound_event(const OperationalEvent& e) noexcept {
    using K=OperationalKind;
    if(e.kind==K::authenticated && e.context.account_id>0){
        if(e.context.transport==OperationalTransport::telnet)return SoundEvent::telnet_login;
        if(e.context.transport==OperationalTransport::ssh)return SoundEvent::ssh_login;
    }
    // Confirmed malware has its own typed producer and Security route.
    if(e.kind==K::file_maintenance_problem && (e.context.diagnostic_code=="clamav_detection_deleted" || e.context.diagnostic_code=="clamav_detection_retained"))return {};
    if(e.outcome==OperationalOutcome::storage_failure)return SoundEvent::operational_alert;
    switch(e.kind){
        case K::runtime_failed:case K::listener_failed:case K::backup_failed:
        case K::feed_update_failed:case K::durable_journal_degraded:
        case K::assignment_quarantined:case K::message_maintenance_problem:
        case K::file_maintenance_problem:return SoundEvent::operational_alert;
        default:return {};
    }
}
void EventSoundStream::publish(SoundEvent event,std::uint64_t now,bool startup) noexcept {
    try{
        const auto kind=static_cast<std::size_t>(event);if(kind>=kSoundEventCount)return;
        std::lock_guard<std::mutex> lock(mutex_);
        const bool alert=event==SoundEvent::security_alert || event==SoundEvent::operational_alert;
        if(alert && last_[kind] && now>=*last_[kind] && now-*last_[kind]<1000)return;
        if(sequence_==std::numeric_limits<std::uint64_t>::max())return;
        if(events_.size()==32)events_.pop_front();
        events_.push_back({++sequence_,now,event});last_[kind]=now;
        if(startup && event==SoundEvent::operational_alert)startup_sequence_=sequence_;
    }catch(...){/* optional sound must never change event/authentication state */}
}
void EventSoundStream::observe(const OperationalEvent& event,std::uint64_t now) noexcept {
    if(const auto sound=operational_sound_event(event))publish(*sound,now);
}
SoundPollSnapshot EventSoundStream::poll(const SoundPollRequest& r,const EventSoundConfiguration& c,std::uint64_t now) noexcept {
    try{
        std::lock_guard<std::mutex> lock(mutex_);SoundPollSnapshot out;out.epoch=epoch_;out.through=sequence_;
        if(!r.owner.valid())return out;
        const bool leased=owner_.valid() && now>=lease_ms_ && now-lease_ms_<=5000;
        if(leased && owner_!=r.owner)return out;
        const bool attach=!leased || owner_!=r.owner || r.epoch!=epoch_;
        owner_=r.owner;lease_ms_=now;out.owner=true;out.configuration=c;
        if(r.after>sequence_)return out;
        for(const auto& e:events_){
            if(e.at_ms>now || now-e.at_ms>3000)continue;
            if(attach){
                // Only the fresh Desktop starter may receive its initial
                // prerequisite failure; attaching otherwise never plays history.
                if(r.fresh_desktop_start && !startup_delivered_ && e.sequence==startup_sequence_){out.events.push_back(e);startup_delivered_=true;}
            }else if(e.sequence>std::max(r.after,delivered_))out.events.push_back(e);
        }
        delivered_=sequence_;
        return out;
    }catch(...){return {};}
}
}
