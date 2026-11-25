#pragma once
#include "sfx/archive/qwk_packet_builder.hpp"
#include <utility>
namespace sfx::apps::runtime {
inline archive::QwkPreparedDownload prepare_caller_qwk_download(application::IQwkStorage& storage,
    const application::MessageActor& actor,application::Identifier128 operation,
    application::QwkPacketMetadata metadata,archive::QwkDateFormatter date,archive::CancelCheck cancelled={},archive::QwkPreparationObserver progress={}){
    if(cancelled&&cancelled()){archive::QwkPreparedDownload failure;failure.result={archive::Error::cancelled,"Packet preparation cancelled."};return failure;}
    // First Download has the same authenticated owner-policy-derived defaults
    // as Settings. This port inserts missing defaults only; saved choices stay.
    const auto preferences=storage.qwk_preferences(actor);
    if(!preferences||!preferences.value){archive::QwkPreparedDownload failure;failure.result={archive::Error::source_changed,"Packet preferences unavailable."};
        failure.storage=preferences.status;if(failure.storage.succeeded())failure.storage={application::StorageErrorCode::internal,"Missing packet preferences result"};return failure;}
    return archive::prepare_qwk_download(storage,actor,operation,std::move(metadata),std::move(date),std::move(cancelled),std::move(progress));
}
inline std::string qwk_preparation_progress_text(const archive::QwkPreparationEvent& event){
    if(event.stage==archive::QwkPreparationStage::packing)return "Packing your QWK packet...";
    return "Scanning "+(event.group_name.empty()?std::string{}:event.group_name+" / ")+event.area_name+"...";
}
}
