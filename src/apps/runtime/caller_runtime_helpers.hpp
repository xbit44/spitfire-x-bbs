#pragma once
#include "sfx/application/phase3a_private_adapters.hpp"
#include "sfx/application/menu.hpp"
#include <algorithm>
#include <array>
#include <functional>

namespace sfx::apps::runtime {
// Modal page acknowledgements and menu selections share one logical CRLF
// delimiter even when its bytes arrive separately. Transfer that state at
// each read/return; preserve byte ownership, polling, and later commands.
class MenuDelimiterStream final:public application::IInteractiveSessionByteStream {
    application::IInteractiveSessionByteStream& stream_;
    application::StreamCallerTerminal& terminal_;
    application::MenuInputDecoder& decoder_;
public:
    MenuDelimiterStream(application::IInteractiveSessionByteStream& stream,
        application::StreamCallerTerminal& terminal,application::MenuInputDecoder& decoder)
        :stream_(stream),terminal_(terminal),decoder_(decoder){}
    ~MenuDelimiterStream(){if(decoder_.take_pending_delimiter_lf())terminal_.finish_editor_key(true);}
    application::InteractiveStreamReadResult read_some(std::size_t maximum)override{
        decoder_.adopt_pending_delimiter_lf(terminal_.take_pending_delimiter_lf());
        return stream_.read_some(maximum);
    }
    application::InteractiveStreamReadResult read_escape_tail(std::size_t maximum)override{
        decoder_.adopt_pending_delimiter_lf(terminal_.take_pending_delimiter_lf());
        return stream_.read_escape_tail(maximum);
    }
    application::InteractiveStreamOutcome poll_output() override { return stream_.poll_output(); }
    application::InteractiveStreamWriteResult write_some(std::string_view bytes)override{return stream_.write_some(bytes);}
};
// Shared shipping caller helpers, extracted without changing the Windows
// behavior. The owning caller supplies lifetime/accounting/revocation guards.
class MenuModalStream final : public application::IInteractiveSessionByteStream {
public:
    void preserve_poll_timeouts(bool value) noexcept {preserve_poll_timeouts_=value;}
    application::StreamCallerTerminal::EditorByteResult read_editor_byte(application::StreamCallerTerminal& terminal) {
        const bool previous=preserve_poll_timeouts_;
        struct Restore {bool& value;bool previous;~Restore(){value=previous;}} restore{preserve_poll_timeouts_,previous};
        preserve_poll_timeouts_=true;
        return terminal.read_editor_byte();
    }
    std::function<bool()> replace_guard(std::function<bool()> guard){guard_.swap(guard);return guard;}
    MenuModalStream(application::IInteractiveSessionByteStream& stream,
        std::function<bool()> tick,std::function<void()> keyboard,bool preserve_poll_timeouts=false)
        : stream_(stream),tick_(std::move(tick)),keyboard_(std::move(keyboard)),preserve_poll_timeouts_(preserve_poll_timeouts) {}
    application::InteractiveStreamReadResult read_some(std::size_t maximum) override {
        for(;;) {
            if(!tick_() || (guard_&&!guard_())) return {application::InteractiveStreamOutcome::cancelled,{}};
            auto result=stream_.read_some(std::min(maximum,std::size_t{1}));
            if(result.outcome==application::InteractiveStreamOutcome::timed_out && !preserve_poll_timeouts_) continue;
            if(!result.bytes.empty()) keyboard_();
            return result;
        }
    }
    application::InteractiveStreamReadResult read_escape_tail(std::size_t maximum) override {
        // Same lifetime/revocation checks as ordinary modal reads, but do not
        // hide the transport's idle poll used to recognize a bare Escape.
        if(!tick_() || (guard_&&!guard_())) return {application::InteractiveStreamOutcome::cancelled,{}};
        auto result=stream_.read_some(std::min(maximum,std::size_t{1}));
        if(!result.bytes.empty()) keyboard_();
        return result;
    }
    application::InteractiveStreamOutcome poll_output() override {
        if (!tick_() || (guard_ && !guard_())) return application::InteractiveStreamOutcome::cancelled;
        return stream_.poll_output();
    }
    application::InteractiveStreamWriteResult write_some(std::string_view bytes) override {
        const auto ready = poll_output();
        if (ready != application::InteractiveStreamOutcome::completed) return {ready, 0};
        return stream_.write_some(bytes);
    }
private:
    application::IInteractiveSessionByteStream& stream_;
    std::function<bool()> tick_; std::function<void()> keyboard_;
    std::function<bool()> guard_;
    bool preserve_poll_timeouts_{};
};

inline std::optional<std::array<std::int64_t, 2>> bbs_today_range(
    const application::IBbsLocalTimestampFormatter& time,
    const std::int64_t now) {
    const auto label = time.format_utc_seconds(now);
    if (!label || label->size() < 10U) return std::nullopt;
    const std::string date = label->substr(0, 10);
    const auto in_today = [&time, &date](const std::int64_t value) {
        const auto formatted = time.format_utc_seconds(value);
        return formatted && formatted->size() >= 10U &&
            formatted->compare(0, 10, date) == 0;
    };
    std::int64_t first_true = now;
    while (first_true - 3600 >= now - 172800 && in_today(first_true - 3600))
        first_true -= 3600;
    std::int64_t low = first_true - 3600;
    std::int64_t high = first_true;
    while (low + 1 < high) {
        const std::int64_t middle = low + (high - low) / 2;
        if (in_today(middle)) high = middle; else low = middle;
    }
    const std::int64_t begin = high;
    std::int64_t last_true = now;
    while (last_true + 3600 <= now + 172800 && in_today(last_true + 3600))
        last_true += 3600;
    low = last_true;
    high = last_true + 3600;
    while (low + 1 < high) {
        const std::int64_t middle = low + (high - low) / 2;
        if (in_today(middle)) low = middle; else high = middle;
    }
    return std::array<std::int64_t, 2>{begin, high};
}
} // namespace sfx::apps::runtime
