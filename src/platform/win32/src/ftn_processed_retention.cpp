#include "sfx/platform/win32/ftn_processed_expiry.hpp"
#include "sfx/application/ftn_work.hpp"
#include <bcrypt.h>

namespace sfx::platform::win32 {
FtnRetentionPageResult process_ftn_retention_page(application::IFtnMailStorage& storage,
    const installation::Receipt& installation,const std::shared_ptr<std::atomic_bool>& cancelled,
    application::Identifier128& cursor){
    namespace app=application;
    FtnRetentionPageResult result;
    if(!cancelled||cancelled->load())return result;
    const app::MessageConfigurationActor owner{true,{}};
    const auto page=storage.ftn_retention_candidates(owner,cursor,4);
    if(!page||!page.value){result.reason="ftn_retention_queue_unavailable";return result;}
    for(const auto& original:*page.value){
        if(cancelled->load())return result;
        cursor=original.ingress;++result.examined;
        try{
            const auto pending=storage.pending_ftn_expiry(owner,original.ingress);
            if(!pending||!pending.value){result.reason="ftn_retention_intent_unavailable";break;}
            // Inspect under a fresh exclusive lease, including reconciliation.
            // A prior dispatch is never replayed or its replacement adopted.
            auto probe=FtnProcessedExpiry::inspect(installation,original,cancelled);
            if(cancelled->load())return result;
            if(pending.value->has_value()){
                const auto outcome=probe->observation()==FtnProcessedExpiry::Observation::missing
                    ?app::FtnExpiryOutcome::absent_uncertain
                    :probe->observation()==FtnProcessedExpiry::Observation::changed
                    ?app::FtnExpiryOutcome::changed_held:app::FtnExpiryOutcome::present_reconciled;
                if(!storage.reconcile_ftn_expiry(owner,**pending.value,outcome)){
                    result.reason="ftn_retention_reconciliation_held";continue;
                }
                ++result.reconciled;
                if(outcome!=app::FtnExpiryOutcome::present_reconciled)continue;
            }
            if(probe->observation()!=FtnProcessedExpiry::Observation::exact){
                result.reason="ftn_retention_original_missing_or_changed";continue;
            }
            if(cancelled->load())return result;
            app::FtnExpiryIntent request;request.original=original;
            for(auto* value:{&request.operation,&request.dispatch})
                installation::require(BCryptGenRandom(nullptr,value->bytes.data(),static_cast<ULONG>(value->bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0&&value->valid(),"ftn_retention_random_failed");
            // The same lease remains held while this intent commits, then
            // authoritative current guards run again at singleton dispatch.
            const auto prepared=storage.prepare_ftn_expiry(owner,request);
            if(!prepared||!prepared.value){result.reason="ftn_retention_policy_or_reference_held";continue;}
            if(cancelled->load())return result;
            auto sealed=probe->seal(*prepared.value);
            const auto executed=storage.execute_ftn_expiry(owner,sealed);
            if(executed){++result.removed;if(!probe->record_confirmed_removal(sealed))result.reason="ftn_retention_identity_snapshot_pending";}
            else {result.reason="ftn_retention_expiry_held";if(executed.code==app::StorageErrorCode::io)break;}
        }catch(const std::exception&){
            // Local paths/exception details are not owner status text. A
            // competing reader or changed ancestry leaves custody untouched.
            result.reason=cancelled->load()?"ftn_retention_cancelled":"ftn_retention_native_custody_held";
        }
    }
    if(page.value->size()<4)cursor={};
    return result;
}
}
