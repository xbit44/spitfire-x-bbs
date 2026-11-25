#pragma once
#include "sfx/archive/qwk_archive.hpp"
#include "sfx/application/qwk_storage.hpp"

namespace sfx::archive {
struct QwkRecordDate {std::string date,time,when_written;};
using QwkDateFormatter=std::function<QwkRecordDate(std::int64_t entry_utc)>;
enum class QwkPreparationStage { scanning, packing };
struct QwkPreparationEvent {
    QwkPreparationStage stage;
    application::LocalEchoId echo;
    std::string group_name,area_name;
};
// False means the caller/output is no longer available; never commit delivery.
using QwkPreparationObserver=std::function<bool(const QwkPreparationEvent&)>;
struct QwkPreparedDownload {
    Result result;
    application::QwkCodecError codec_error{application::QwkCodecError::none};
    application::StorageStatus storage;
    std::string zip,notice;
    std::optional<application::QwkDeliveryCommit> progress;
};
// Pure preparation plus metadata-only permanent reference allocation. No mail
// Read, shared pointer or durable continuation write; Runtime calls the commit
// only after owned-file identity + exact actual transfer completion.
QwkPreparedDownload prepare_qwk_download(application::IQwkStorage&,
    const application::MessageActor&,application::Identifier128,
    application::QwkPacketMetadata,QwkDateFormatter,CancelCheck cancelled={},QwkPreparationObserver progress={});
} // namespace sfx::archive
