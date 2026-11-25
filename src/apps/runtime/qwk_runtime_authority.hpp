#pragma once
#include "sfx/application/qwk_transfer.hpp"
namespace sfx::apps::runtime {
// Host lifetime/time/access refresh remains at the call site. The storage
// policy read proves the durable actor/session, active account/level, exact
// revisions and published security policy together in one fresh snapshot.
template<class Storage>
bool qwk_runtime_authorized(Storage& storage,application::Identifier128 live_session,
    application::AccountId live_account,const application::QwkTransferRequest& request,
    const application::QwkPacketPlan* active_plan=nullptr){
    if(live_session!=request.actor.session||live_account!=request.actor.account)return false;
    const auto policy=storage.qwk_caller_policy(request.actor);
    if(!policy||!policy.value||policy.value->board_id!=request.board_id||policy.value->revision!=request.board_revision)return false;
    if(active_plan){const auto current=storage.validate_qwk_packet_authority(request.actor,*active_plan);return current&&current.value&&*current.value;}
    return true;
}
}
