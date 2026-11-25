#pragma once
#include "sfx/application/ftn_work.hpp"
#include "sfx/application/ftn_configuration_transport.hpp"
#include <variant>
namespace sfx::application {
using FtnHeldControlRequest=std::variant<FtnHeldReviewQuery,FtnHeldRecipientQuery,FtnRecipientResolutionRequest,FtnReprocessRequest,FtnTicRetryRequest>;
struct FtnHeldControlResponse {
    FtnConfigurationCode code{FtnConfigurationCode::unavailable};
    std::variant<std::monostate,FtnHeldReview,FtnHeldRecipients,FtnRecipientResolution,FtnReprocessRecord,FtnTicRetryRecord> value;
};
std::vector<std::uint8_t> encode_ftn_held_request(const FtnHeldControlRequest&);
std::optional<FtnHeldControlRequest> decode_ftn_held_request(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_ftn_held_response(const FtnHeldControlResponse&);
std::optional<FtnHeldControlResponse> decode_ftn_held_response(const std::vector<std::uint8_t>&);
// Authority comes only from the host's protected owner channel or exclusive
// verified offline session. The wire request cannot supply actor/proof data.
inline FtnHeldControlResponse execute_ftn_held_control(IFtnMailStorage& storage,const MessageConfigurationActor& actor,const FtnHeldControlRequest& request){
    FtnHeldControlResponse out;if(encode_ftn_held_request(request).empty()){out.code=FtnConfigurationCode::invalid;return out;}
    const auto result=[&](const auto& r){if(r&&r.value){out.code=FtnConfigurationCode::ok;out.value=*r.value;}else out.code=r.status.code==StorageErrorCode::conflict?FtnConfigurationCode::conflict:r.status.code==StorageErrorCode::backpressure?FtnConfigurationCode::busy:FtnConfigurationCode::unavailable;};
    switch(request.index()){
    case 0:result(storage.ftn_held_review(actor,std::get<0>(request)));break;
    case 1:result(storage.ftn_held_recipients(actor,std::get<1>(request)));break;
    case 2:result(storage.resolve_ftn_recipient(actor,std::get<2>(request)));break;
    case 3:result(storage.request_ftn_reprocess(actor,std::get<3>(request)));break;
    case 4:result(storage.request_ftn_tic_retry(actor,std::get<4>(request)));break;
    default:out.code=FtnConfigurationCode::invalid;break;
    }return out;
}
}
