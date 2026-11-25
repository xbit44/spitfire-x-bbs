#pragma once

#include "sfx/application/caller_inactivity.hpp"
#include "sfx/application/terminal_stream.hpp"
#include <functional>
#include <algorithm>
#include <optional>

namespace sfx::application {

// Login/registration has no authenticated exemption yet. The underlying stream
// must poll boundedly; consume those poll timeouts here, exposing only the actual
// inactivity deadline to the terminal. No input, including secrets, is retained.
class LoginInactivityStream final : public IInteractiveSessionByteStream {
public:
    LoginInactivityStream(IInteractiveSessionByteStream& stream, std::uint16_t seconds,
        std::function<std::uint64_t()> clock, std::function<void()> before_disconnect={},
        std::function<bool()> authenticated={})
        : stream_(stream), limit_ms_(static_cast<std::uint64_t>(seconds)*1000), clock_(std::move(clock)),
          before_disconnect_(std::move(before_disconnect)),authenticated_(std::move(authenticated)) {}
    InteractiveStreamReadResult read_some(std::size_t maximum) override {
        if (!maximum || maximum > kInteractiveStreamMaximumReadBytes)
            return {InteractiveStreamOutcome::failed, {}};
        if (!last_key_) last_key_=clock_();
        for (;;) {
            const auto now=clock_();
            const auto elapsed=now>=*last_key_ ? now-*last_key_ : 0;
            const bool after_login=authenticated_ && authenticated_();
            if (!after_login && elapsed>=limit_ms_) {
                if(!ended_) {ended_=true;if(before_disconnect_) before_disconnect_();}
                return {InteractiveStreamOutcome::timed_out, {}};
            }
            if (!after_login && !warned_ && elapsed>=limit_ms_-std::min<std::uint64_t>(10000,limit_ms_)) {
                warned_=true;
                auto remaining = std::string_view("\r\nLogin idle: disconnecting in 10 seconds.\r\n");
                while (!remaining.empty()) {
                    const auto sent = stream_.write_some(remaining);
                    if (!valid_interactive_stream_write_result(remaining.size(), sent) ||
                        sent.outcome != InteractiveStreamOutcome::completed || !sent.bytes_written)
                        return {InteractiveStreamOutcome::failed, {}};
                    remaining.remove_prefix(sent.bytes_written);
                }
            }
            auto input = stream_.read_some(maximum);
            if (!valid_interactive_stream_read_result(maximum, input))
                return {InteractiveStreamOutcome::failed, {}};
            if (input.outcome == InteractiveStreamOutcome::timed_out) continue;
            if (input.outcome == InteractiveStreamOutcome::completed && !input.bytes.empty())
                { last_key_=clock_(); warned_=false; }
            return input;
        }
    }
    InteractiveStreamWriteResult write_some(std::string_view bytes) override {
        return stream_.write_some(bytes); // output never resets inactivity
    }
private:
    IInteractiveSessionByteStream& stream_;
    std::uint64_t limit_ms_;
    bool warned_{}, ended_{};
    std::function<std::uint64_t()> clock_;
    std::function<void()> before_disconnect_;
    std::function<bool()> authenticated_;
    std::optional<std::uint64_t> last_key_;
};
} // namespace sfx::application
