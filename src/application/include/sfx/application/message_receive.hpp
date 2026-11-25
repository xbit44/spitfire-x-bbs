#pragma once
#include "sfx/application/message_source.hpp"
#include "sfx/application/terminal_stream.hpp"
#include <functional>

namespace sfx::application {
inline constexpr std::uint64_t kMessageReceiveIdleMillis=60000;
inline constexpr std::uint64_t kMessageReceiveTotalMillis=300000;
inline constexpr std::size_t kMessageReceiveQueueBytes=65536;
inline constexpr std::size_t kMessageReceiveHighWater=49152;
inline constexpr std::size_t kMessageReceiveLowWater=16384;
enum class MessageReceiveCode : std::uint8_t { completed, unavailable, refused, cancelled, timed_out, disconnected, failed };
enum class MessageReceiveRefusal : std::uint8_t { none, extension, too_large };
struct MessageReceiveRequest { MessageActor actor; ComposeContext context; };
// No caller filename, filesystem path, native handle or helper argument crosses
// this boundary. A completed result owns the verified, bounded file snapshot.
struct MessageReceiveResult {
    MessageReceiveCode code{MessageReceiveCode::failed};
    std::optional<MessageSource> source;
    MessageReceiveRefusal refusal{MessageReceiveRefusal::none};
};
class IMessageSourceReceiver {
public:
    virtual ~IMessageSourceReceiver()=default;
    virtual bool available(const MessageReceiveRequest&)=0;
    virtual MessageReceiveResult receive(const MessageReceiveRequest&)=0;
};
// Session owner leases an already-decoded channel exclusively. Implementations
// must poll reads/writes (never block beyond 100 ms), suppress ordinary terminal
// IO, and disconnect on a failed/uncertain binary handoff. No protocol byte is
// fed to a menu/editor decoder. Deadlines use the same monotonic clock.
struct DecodedMessageReceiveChannel {
    std::function<bool()> acquire;
    std::function<void(bool synchronized)> release;
    std::function<InteractiveStreamReadResult(std::size_t)> read;
    std::function<InteractiveStreamWriteResult(std::string_view)> write;
    std::function<std::uint64_t()> now;
    // Non-extending caller/drain/total ceiling, or zero. Never return a sliding
    // receive-idle deadline here: the receiver tracks last inbound separately.
    std::function<std::uint64_t()> absolute_deadline;
    std::function<bool(const MessageReceiveRequest&)> authorized;
};
bool same_message_receive_context(const ComposeContext&,const ComposeContext&) noexcept;
bool valid_message_receive_channel(const DecodedMessageReceiveChannel&) noexcept;
} // namespace sfx::application
