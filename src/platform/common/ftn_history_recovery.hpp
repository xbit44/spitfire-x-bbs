#pragma once
#include "sfx/application/ftn_recovery_receipt.hpp"
#include "sfx/application/ftn_history_recovery.hpp"
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace sfx::platform::ftn_history {
inline constexpr const char* confirmation="This restored BBS will replace the previous copy. Any other copy using these FTN addresses is stopped.";
inline constexpr const char* success="FTN recovery complete. You can start the BBS.";
inline void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
// Known stock busy/publication fences are refused; this is not a detector of
// arbitrary independent or remote writers. Stopping them is the Sysop's duty.
inline void verify_external_stopped(const std::filesystem::path& root){
#ifdef _WIN32
    const auto external=root/"FTN/External";
#else
    const auto external=root/"ftn/external";
#endif
    if(!std::filesystem::exists(external))return;
    std::size_t count{};
    for(const auto& entry:std::filesystem::recursive_directory_iterator(external)){
        require(++count<=100000,"ftn_recovery_external_review_limit");
        const auto status=entry.symlink_status();
        require(!std::filesystem::is_symlink(status),"ftn_recovery_external_redirect_refused");
        auto name=entry.path().filename().u8string();
        for(auto& c:name)if(c>='A'&&c<='Z')c=static_cast<char>(c+'a'-'A');
        require(entry.path().extension()!=".bsy"&&name.find(".bsy")==std::string::npos,
            "ftn_recovery_stop_external_binkd_and_review_busy_file");
    }
}
template<class Lease,class Storage,class Outbound,class Incoming>
std::vector<Lease> verify_payloads(Storage& storage,Outbound outgoing,Incoming incoming){
    const application::MessageConfigurationActor host{true,{}};
    const auto pending=storage.ftn_history_recovery_queue(host);
    require(bool(pending)&&pending.value.has_value(),"ftn_recovery_queue_review_failed");
    std::vector<Lease> leases;
    for(const auto& job:pending.value->outbound){
        auto file=outgoing(job);const auto actual=file.identity();
        require(actual.bytes==job.file.size&&actual.sha256==job.digest,"ftn_recovery_outbound_missing_or_changed");
        leases.push_back(std::move(file));
    }
    for(const auto& receipt:pending.value->ingress){
        auto file=incoming(receipt);const auto actual=file.identity();
        const auto saved=storage.ftn_ingress_checkpoint(host,receipt.id);
        require(bool(saved)&&saved.value&&saved.value->has_value(),"ftn_recovery_ingress_checkpoint_missing");
        const auto& expected=**saved.value;
        require(actual.volume==expected.volume&&actual.file_id==expected.file_id&&
            actual.bytes==expected.bytes&&actual.sha256==expected.sha256&&actual.bytes==receipt.received,
            "ftn_recovery_ingress_missing_or_changed");
        if(receipt.state!=application::FtnIngressState::partial)
            require(actual.bytes==receipt.file.size&&actual.sha256==receipt.digest,"ftn_recovery_ingress_digest_mismatch");
        leases.push_back(std::move(file));
    }
    return leases;
}
// Native owns the genuine maintenance lock, database pin and verified payload
// capabilities. There is no live caller or remote-control reset entry point.
template<class Native> class Operation final:public application::IFtnHistoryRecovery {
    Native native_;
    application::FtnHistoryRecoveryPreview preview_;
    application::FtnSerialRecoveryRequest request_;
    std::string token_;
    decltype(std::declval<Native>().payloads()) payloads_;
public:
    template<class... Args> explicit Operation(Args&&... args):native_(std::forward<Args>(args)...){
        native_.verify();
        auto& storage=native_.storage();
        const auto state=storage.ftn_serial_state({true,{}});
        require(bool(state)&&state.value.has_value(),"ftn_recovery_history_invalid");
        preview_.state=*state.value;
        preview_.utc=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if(state.value->recovery_required)require(preview_.utc>=0&&preview_.utc<=UINT32_MAX&&state.value->high_water<UINT32_MAX,"ftn_recovery_clock_or_serial_range_invalid");
        request_.expected=*state.value;request_.operation=native_.fresh_id();
        token_=application::ftn_recovery_operation(request_.operation);
        const auto queue=storage.ftn_history_recovery_queue({true,{}});
        require(bool(queue)&&queue.value.has_value(),"ftn_recovery_queue_invalid");
        preview_.pending_outbound=queue.value->outbound.size();preview_.pending_ingress=queue.value->ingress.size();
        const auto page=storage.ftn_queue_page({true,{}},0);
        require(bool(page)&&page.value.has_value(),"ftn_recovery_hold_review_failed");
        preview_.held_outbound=page.value->outbound[4];preview_.held_ingress=page.value->ingress[3];
        if(state.value->recovery_required){native_.verify_writers();payloads_=native_.payloads();}
    }
    const application::FtnHistoryRecoveryPreview& preview()const noexcept override{return preview_;}
    const std::string& confirmation_token()const noexcept override{return token_;}
    application::FtnHistoryRecoveryResult execute(std::string_view token)override{
        application::FtnHistoryRecoveryResult result;
        try{
            require(token==token_,"ftn_recovery_preview_confirmation_required");
            native_.verify();
            auto& storage=native_.storage();
            if(request_.expected.recovery_required){
                native_.verify_writers();payloads_.clear();payloads_=native_.payloads();
                const auto recovered=storage.recover_ftn_serial_history({true,{}},request_);
                result.status=recovered.status;if(!recovered||!recovered.value)return result;
                result.receipt=*recovered.value;
            }else{
                const auto saved=storage.last_ftn_serial_recovery({true,{}});
                result.status=saved.status;if(!saved||!saved.value||!saved.value->has_value())return result;
                result.receipt=**saved.value;
            }
            // The committed DB receipt is authoritative. A failed text export
            // never reruns/rolls back recovery or authorizes another hold clear.
            try{native_.report(*result.receipt);result.report_written=true;}catch(const std::exception&){}
        }catch(const std::exception& error){result.status={application::StorageErrorCode::io,error.what()};}
        return result;
    }
};
}
