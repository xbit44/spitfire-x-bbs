#pragma once
#include "sfx/application/qwk_storage.hpp"
#include <memory>

namespace sfx::application {
struct QwkReplySourceInfo {
    std::string board_id;
    std::array<std::uint8_t,32> logical_sha256{};
    std::uint64_t records{},compressed_bytes{},expanded_bytes{};
};
// Trusted host-created, immutable protected Work spool. No caller file path,
// reader-supplied receipt, unchecked vector of bodies or ZIP enters the writer.
// Runtime validates the entire envelope/codec before sealing this source. The
// writer still validates every record/current authority and recomputes the same
// logical digest, before writes and again during the atomic write pass.
// next() must bound fields before allocation (ordinary subject/name/body/source
// bounds), enforce exact framing/EOF and expose cancellation/identity failures.
class IQwkReplySource {
public:
    virtual ~IQwkReplySource()=default;
    virtual QwkReplySourceInfo info() const=0;
    virtual StorageStatus verify_identity() const=0;
    virtual StorageStatus rewind()=0;
    virtual StorageResult<std::optional<QwkReplyRecord>> next()=0;
    virtual bool cancelled() const noexcept=0;
    // Native host only, after authoritative import outcome/requery. Abandoning
    // a source or a failed import never grants disposable-work cleanup.
    virtual void confirm_completed()noexcept{}
};
// Dedicated optional port preserves existing message-storage consumers/fakes.
class IQwkReplyStorage {
public:
    virtual ~IQwkReplyStorage()=default;
    virtual StorageResult<QwkPolicy> qwk_caller_policy(const MessageActor&)=0;
    virtual StorageResult<QwkImportOutcome> import_qwk_reply(const MessageActor&,
        std::shared_ptr<IQwkReplySource>)=0;
    virtual StorageResult<std::optional<QwkImportOutcome>> qwk_import_outcome(
        const MessageActor&,const QwkReplySourceInfo&)=0;
};
} // namespace sfx::application
