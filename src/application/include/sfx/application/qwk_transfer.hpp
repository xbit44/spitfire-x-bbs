#pragma once
#include "sfx/application/qwk_reply_storage.hpp"
#include "sfx/application/terminal_stream.hpp"
#include <functional>
#include <atomic>

namespace sfx::application {
// Actor/board authority only: offline packets do not invent a compose context.
struct QwkTransferRequest {MessageActor actor;std::string board_id;std::int64_t board_revision{};};
enum class QwkTransferCode : std::uint8_t {completed,unavailable,refused,cancelled,disconnected,failed};
enum class QwkTransferRefusal : std::uint8_t {none,extension,too_large,content,skipped};
struct QwkReceiveResult {
    QwkTransferCode code{QwkTransferCode::failed};
    QwkTransferRefusal refusal{QwkTransferRefusal::none};
    std::shared_ptr<IQwkReplySource> source;
    std::shared_ptr<std::atomic<bool>> cancellation;
    QwkReceiveResult()=default;
    QwkReceiveResult(QwkTransferCode c,QwkTransferRefusal r={},std::shared_ptr<IQwkReplySource> s={},std::shared_ptr<std::atomic<bool>> stop={}):code(c),refusal(r),source(std::move(s)),cancellation(std::move(stop)){}
    QwkReceiveResult(QwkReceiveResult&&)=default;
    QwkReceiveResult(const QwkReceiveResult&)=delete;
    // A writer-held source cannot outlive its caller command and later commit.
    ~QwkReceiveResult(){if(cancellation)cancellation->store(true);}
};
struct QwkSendResult {QwkTransferCode code{QwkTransferCode::failed};QwkTransferRefusal refusal{QwkTransferRefusal::none};std::function<void()> confirm_completed;};
struct DecodedQwkTransferChannel {
    std::function<bool()> acquire;
    std::function<void(bool synchronized)> release;
    std::function<InteractiveStreamReadResult(std::size_t)> read;
    std::function<InteractiveStreamWriteResult(std::string_view)> write;
    std::function<std::uint64_t()> now,absolute_deadline;
    std::function<bool(const QwkTransferRequest&)> authorized;
};
class IQwkPacketTransfer {
public:
    virtual ~IQwkPacketTransfer()=default;
    virtual bool available(const QwkTransferRequest&)=0;
    virtual QwkReceiveResult receive(const QwkTransferRequest&)=0;
    // Trusted complete prepared ZIP; caller names/paths never enter the host.
    virtual QwkSendResult send(const QwkTransferRequest&,std::string_view zip)=0;
};
} // namespace sfx::application
