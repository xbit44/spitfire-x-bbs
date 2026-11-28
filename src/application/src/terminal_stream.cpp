#include "sfx/application/terminal_stream.hpp"
#include "sfx/application/menu.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <type_traits>

namespace sfx::application {
namespace {

constexpr std::uint8_t kBackspace = 0x08U;
constexpr std::uint8_t kLineFeed = 0x0aU;
constexpr std::uint8_t kCarriageReturn = 0x0dU;
constexpr std::uint8_t kCancel = 0x11U;
constexpr std::uint8_t kDelete = 0x7fU;
constexpr std::string_view kAutomaticPagingPrompt =
    "MORE: <S>top, <N>onstop, < ENTER > to continue? ";
constexpr std::string_view kForcedPagingPrompt = "< ENTER > ";

// Keep rendition, not cursor commands, for a caller-colored pager. Canonical
// attribute families bound storage independently of newsletter length and
// retain partial SGR changes (foreground, background, intensity, etc.).
class DisplayRendition {
public:
    void apply(std::string_view sgr) {
        if(sgr=="\x1b" "7") {saved=attributes;return;}
        if(sgr=="\x1b" "8") {attributes=saved;return;}
        if(sgr.size()<3 || sgr[1]!='[' || sgr.back()!='m') return;
        std::vector<unsigned> values;
        const auto parameters=sgr.substr(2,sgr.size()-3);
        for(std::size_t start=0;start<=parameters.size();) {
            const auto end=parameters.find(';',start);
            const auto field=parameters.substr(start,end==std::string_view::npos?parameters.size()-start:end-start);
            unsigned value=0;if(!field.empty()) std::from_chars(field.data(),field.data()+field.size(),value);
            values.push_back(value);if(end==std::string_view::npos) break;start=end+1;
        }
        for(std::size_t n=0;n<values.size();++n) {
            const auto value=values[n];if(value==0) {attributes.clear();continue;}
            unsigned family=value;
            if(value==22) {attributes.erase(1);attributes.erase(2);continue;}
            if(value==1 || value==2) family=value;
            else if(value==3 || value==23) family=3;
            else if(value==4 || value==21 || value==24) family=4;
            else if(value==5 || value==6 || value==25) family=5;
            else if(value==7 || value==27) family=7;
            else if(value==8 || value==28) family=8;
            else if(value==9 || value==29) family=9;
            else if(value>=10 && value<=19) family=10;
            else if((value>=30 && value<=39) || (value>=90 && value<=97)) family=30;
            else if((value>=40 && value<=49) || (value>=100 && value<=107)) family=40;
            std::string encoded=std::to_string(value);
            if((value==38 || value==48) && n+1<values.size()) {
                const auto count=values[n+1]==5?2U:values[n+1]==2?4U:0U;
                if(count && n+count<values.size()) for(unsigned extra=0;extra<count;++extra) encoded+=';'+std::to_string(values[++n]);
            }
            attributes[family]=std::move(encoded);
        }
    }
    std::string sequence() const {
        std::string result="\x1b[0";
        for(const auto& entry:attributes) result+=';'+entry.second;
        return result+'m';
    }
private:
    std::map<unsigned,std::string> attributes;
    std::map<unsigned,std::string> saved;
};

TerminalOutcome input_outcome(const InteractiveStreamOutcome outcome) noexcept {
    switch (outcome) {
        case InteractiveStreamOutcome::completed:
            return TerminalOutcome::completed;
        case InteractiveStreamOutcome::cancelled:
            return TerminalOutcome::cancelled;
        case InteractiveStreamOutcome::timed_out:
            return TerminalOutcome::timed_out;
        case InteractiveStreamOutcome::disconnected:
            return TerminalOutcome::disconnected;
        case InteractiveStreamOutcome::shutdown_requested:
            return TerminalOutcome::shutdown_requested;
        case InteractiveStreamOutcome::failed:
            return TerminalOutcome::terminal_failure;
    }
    return TerminalOutcome::terminal_failure;
}

TerminalOutcome output_outcome(const InteractiveStreamOutcome outcome) noexcept {
    switch (outcome) {
        case InteractiveStreamOutcome::completed:
            return TerminalOutcome::completed;
        case InteractiveStreamOutcome::cancelled:
            return TerminalOutcome::cancelled;
        case InteractiveStreamOutcome::timed_out:
            return TerminalOutcome::timed_out;
        case InteractiveStreamOutcome::disconnected:
            return TerminalOutcome::disconnected;
        case InteractiveStreamOutcome::shutdown_requested:
            return TerminalOutcome::shutdown_requested;
        case InteractiveStreamOutcome::failed:
            return TerminalOutcome::output_failure;
    }
    return TerminalOutcome::output_failure;
}

bool printable_input(const std::uint8_t value) noexcept {
    return value >= 0x20U && value <= 0x7eU;
}

bool ascii_letter(const char value) noexcept {
    const auto byte = static_cast<unsigned char>(value);
    return (byte >= static_cast<unsigned char>('A') &&
               byte <= static_cast<unsigned char>('Z')) ||
        (byte >= static_cast<unsigned char>('a') &&
            byte <= static_cast<unsigned char>('z'));
}

char ascii_upper(const char value) noexcept {
    if (value >= 'a' && value <= 'z') {
        return static_cast<char>(value - ('a' - 'A'));
    }
    return value;
}

std::optional<char> canonical_allowed_key(
    const std::vector<char>& allowed,
    const std::uint8_t input) noexcept {
    const char candidate = static_cast<char>(input);
    if (ascii_letter(candidate)) {
        const char folded = ascii_upper(candidate);
        const auto match = std::find_if(allowed.begin(), allowed.end(),
            [folded](const char allowed_key) {
                return ascii_letter(allowed_key) &&
                    ascii_upper(allowed_key) == folded;
            });
        if (match != allowed.end()) return *match;
    }
    const auto exact = std::find(allowed.begin(), allowed.end(), candidate);
    return exact == allowed.end() ? std::nullopt
                                  : std::optional<char>{*exact};
}

void erase_input_value(TerminalInputResult& result) noexcept {
    explicitly_erase_terminal_secret(result.value);
}

bool increment_event_count(std::size_t& count) noexcept {
    if (count == kTerminalMaximumInputEventsPerRequest) return false;
    ++count;
    return true;
}

bool ordinary_logical_text(const std::string_view text) noexcept {
    if (text.empty()) return false;
    for (const unsigned char value : text) {
        if (value == kCarriageReturn || value == kLineFeed) continue;
        if (value < 0x20U || value > 0x7eU) return false;
    }
    return true;
}

static_assert(std::is_nothrow_move_constructible_v<TerminalInputResult>,
    "terminal secret ownership transfer must not allocate or copy");
static_assert(
    std::is_nothrow_move_assignable_v<std::vector<std::uint8_t>>,
    "stream read-buffer ownership transfer must not allocate or copy");

}  // namespace

bool valid_interactive_stream_read_result(
    const std::size_t requested_maximum,
    const InteractiveStreamReadResult& result) noexcept {
    if (requested_maximum == 0 ||
        requested_maximum > kInteractiveStreamMaximumReadBytes) {
        return false;
    }
    if (result.outcome == InteractiveStreamOutcome::completed) {
        return !result.bytes.empty() &&
            result.bytes.size() <= requested_maximum;
    }
    return result.bytes.empty();
}

bool valid_interactive_stream_write_result(
    const std::size_t offered_bytes,
    const InteractiveStreamWriteResult& result) noexcept {
    if (offered_bytes == 0 ||
        result.bytes_written > offered_bytes) {
        return false;
    }
    if (result.outcome == InteractiveStreamOutcome::completed) {
        return result.bytes_written > 0;
    }
    return true;
}

StreamCallerTerminal::StreamCallerTerminal(
    IInteractiveSessionByteStream& stream,
    const StreamCallerTerminalOptions options) noexcept
    : stream_(stream), options_(options) {}

StreamCallerTerminal::~StreamCallerTerminal() {
    wipe_pending();
}
StreamCallerTerminal::EditorByteResult StreamCallerTerminal::read_editor_byte() {
    const auto input=next_byte();return {input.outcome,input.byte};
}
void StreamCallerTerminal::finish_editor_key(bool carriage_return) noexcept {
    if(carriage_return)discard_one_lf_after_cr_=true;
    else discard_coalesced_logical_enter_after_key();
}
StreamCallerTerminal::BinaryInputPrefix StreamCallerTerminal::take_binary_input_prefix(){
    BinaryInputPrefix result;
    result.bytes.assign(pending_.begin()+pending_offset_,pending_.end());
    result.discard_delimiter_lf=discard_one_lf_after_cr_;
    if(result.discard_delimiter_lf && !result.bytes.empty()){
        if(result.bytes.front()=='\n')result.bytes.erase(result.bytes.begin());
        result.discard_delimiter_lf=false;
    }
    wipe_pending();return result;
}

TerminalOutcome StreamCallerTerminal::set_text_style(const TerminalTextStyle style) {
    if (style != TerminalTextStyle::plain && style != TerminalTextStyle::ogsf_ansi)
        return TerminalOutcome::invalid;
    text_style_ = style;
    if(options_.effective_style) *options_.effective_style=style;
    return TerminalOutcome::completed;
}

TerminalOutcome StreamCallerTerminal::system_style(bool caller_color) {
    const auto chosen=caller_color && options_.caller_text_color ? *options_.caller_text_color : 0;
    return text_style_ == TerminalTextStyle::ogsf_ansi
        ? write_all(menu_text_sgr(chosen>0 && chosen<=15 ? static_cast<std::uint8_t>(chosen) : caller_color ? options_.text_color : std::uint8_t{7})) : TerminalOutcome::completed;
}

TerminalOutcome StreamCallerTerminal::set_colors(std::uint8_t text, std::uint8_t input) {
    if (!text || text>15 || !input || input>15) return TerminalOutcome::invalid;
    options_.text_color=text; options_.input_color=input;
    return TerminalOutcome::completed;
}

TerminalOutcome StreamCallerTerminal::write_prompt(const std::string_view text) {
    auto outcome = write_system_text(text);
    if (outcome == TerminalOutcome::completed && text_style_ == TerminalTextStyle::ogsf_ansi)
        outcome = write_all(menu_text_sgr(options_.input_color));
    return outcome;
}

TerminalOutcome StreamCallerTerminal::clear_screen() {
    auto outcome = system_style();
    if (outcome == TerminalOutcome::completed) {
        if (text_style_ == TerminalTextStyle::ogsf_ansi) outcome = write_all("\x1b[2J\x1b[H");
        else {
            // Plain terminals have no portable cursor addressing: scroll the
            // fixed 25-row presentation clear without sending ANSI controls.
            for (unsigned row = 0; row < 25 && outcome == TerminalOutcome::completed; ++row)
                outcome = write_all("\r\n");
        }
    }
    if (outcome != TerminalOutcome::completed) wipe_pending();
    return outcome;
}

TerminalInputResult StreamCallerTerminal::read_visible(
    const TerminalInputRequest& request) {
    return read_line(request, false);
}

TerminalInputResult StreamCallerTerminal::read_secret(
    const TerminalInputRequest& request) {
    return read_line(request, true);
}

TerminalInputResult StreamCallerTerminal::read_line(
    const TerminalInputRequest& request,
    const bool secret) {
    if (!valid_terminal_input_request(request) || (secret && !request.initial_value.empty())) {
        return {TerminalOutcome::invalid, {}};
    }

    TerminalInputResult result{TerminalOutcome::invalid, {}};
    const std::size_t required_capacity =
        secret ? kTerminalSecretStorageCapacity
               : request.maximum_characters;
    try {
        result.value.reserve(required_capacity);
    } catch (...) {
        wipe_pending();
        return {TerminalOutcome::terminal_failure, {}};
    }
    if (result.value.capacity() < required_capacity) {
        wipe_pending();
        return {TerminalOutcome::terminal_failure, {}};
    }

    std::size_t input_events = 0;
    for (;;) {
        const TerminalOutcome prompt = write_prompt(request.prompt);
        if (prompt != TerminalOutcome::completed) {
            wipe_pending();
            return {prompt, {}};
        }

        result.outcome = TerminalOutcome::invalid;
        result.value=request.initial_value;
        if(!result.value.empty()) {
            const auto prefill=write_all(result.value);
            if(prefill!=TerminalOutcome::completed){erase_input_value(result);wipe_pending();return {prefill,{}};}
        }
        for (;;) {
            const NextByteResult next = next_prompt_byte(input_events);
            if (!next.byte.has_value()) {
                erase_input_value(result);
                if (next.outcome != TerminalOutcome::completed) wipe_pending();
                return {next.outcome, {}};
            }

            const std::uint8_t byte = *next.byte;
            if (byte == kCancel || (request.escape_cancels && byte==27)) {
                erase_input_value(result);
                wipe_pending();
                return {TerminalOutcome::cancelled, {}};
            }
            if (byte == kCarriageReturn || byte == kLineFeed) {
                if (byte == kCarriageReturn) {
                    discard_one_lf_after_cr_ = true;
                }
                auto newline = system_style();
                if (newline == TerminalOutcome::completed) newline = write_all("\r\n");
                if (newline != TerminalOutcome::completed) {
                    erase_input_value(result);
                    wipe_pending();
                    return {newline, {}};
                }
                if (!result.value.empty()) {
                    result.outcome = TerminalOutcome::completed;
                    return result;
                }
                if (request.empty_policy == EmptyInputPolicy::use_default) {
                    return {TerminalOutcome::empty, {}};
                }
                break;
            }
            if (byte == kBackspace || byte == kDelete) {
                if (!result.value.empty()) {
                    detail::explicitly_erase_bytes(
                        result.value.data() + result.value.size() - 1U, 1U);
                    result.value.pop_back();
                    if (!secret ||
                        options_.secret_input_echo == SecretInputEchoMode::masked) {
                        const TerminalOutcome erase_echo = write_all("\b \b");
                        if (erase_echo != TerminalOutcome::completed) {
                            erase_input_value(result);
                            wipe_pending();
                            return {erase_echo, {}};
                        }
                    }
                }
                continue;
            }
            if (printable_input(byte)) {
                if (result.value.size() == request.maximum_characters) {
                    erase_input_value(result);
                    TerminalOutcome drained =
                        discard_through_logical_enter(input_events);
                    if (drained == TerminalOutcome::completed) {
                        drained = write_all("\r\n");
                    }
                    if (drained != TerminalOutcome::completed) {
                        wipe_pending();
                        return {drained, {}};
                    }
                    return {TerminalOutcome::overlength, {}};
                }
                result.value.push_back(static_cast<char>(byte));
                if (!secret ||
                    options_.secret_input_echo == SecretInputEchoMode::masked) {
                    const char echo = secret ? '*' : static_cast<char>(byte);
                    const TerminalOutcome echoed =
                        write_all(std::string_view(&echo, 1U));
                    if (echoed != TerminalOutcome::completed) {
                        erase_input_value(result);
                        wipe_pending();
                        return {echoed, {}};
                    }
                }
                continue;
            }

            erase_input_value(result);
            TerminalOutcome drained =
                discard_through_logical_enter(input_events);
            if (drained == TerminalOutcome::completed) {
                drained = write_all("\r\n");
            }
            if (drained != TerminalOutcome::completed) {
                wipe_pending();
                return {drained, {}};
            }
            return {TerminalOutcome::invalid, {}};
        }
    }
}

TerminalKeyResult StreamCallerTerminal::read_key(
    const TerminalKeyRequest& request) {
    if (!valid_terminal_key_request(request)) {
        return {TerminalOutcome::invalid, std::nullopt};
    }
    const TerminalOutcome prompt = request.prompt_visible ? TerminalOutcome::completed : write_prompt(request.prompt);
    if (prompt != TerminalOutcome::completed) {
        wipe_pending();
        return {prompt, std::nullopt};
    }

    std::size_t input_events = 0;
    for (;;) {
        const NextByteResult next = request.navigation_keys?next_byte():next_prompt_byte(input_events);
        if(request.navigation_keys&&next.byte&&!increment_event_count(input_events)){wipe_pending();return {TerminalOutcome::terminal_failure,{}};}
        if (!next.byte.has_value()) {
            if (next.outcome != TerminalOutcome::completed) wipe_pending();
            return {next.outcome, std::nullopt};
        }
        const std::uint8_t byte = *next.byte;
        if(request.navigation_keys && byte==27){
            const auto prefix=next_byte(true);
            if(!prefix.byte){if(prefix.outcome==TerminalOutcome::timed_out || prefix.outcome==TerminalOutcome::empty)return {TerminalOutcome::cancelled,{}};return {prefix.outcome,{}};}
            if(*prefix.byte!='[' && *prefix.byte!='O')return {TerminalOutcome::cancelled,{}};
            for(unsigned n=0;n<16;++n){
                // After CSI/SS3 begins, normal input lifetime rules apply.
                // Only the first Escape tail uses a transport poll boundary.
                const auto end=next_byte();if(!end.byte)return {end.outcome,{}};
                if(*end.byte>=0x40 && *end.byte<=0x7e){
                    const auto nav=*end.byte=='A'?TerminalNavigationKey::up:*end.byte=='B'?TerminalNavigationKey::down:*end.byte=='C'?TerminalNavigationKey::right:*end.byte=='D'?TerminalNavigationKey::left:TerminalNavigationKey::none;
                    if(nav!=TerminalNavigationKey::none)return {TerminalOutcome::completed,{},nav};
                    break;
                }
                if(!((*end.byte>='0'&&*end.byte<='9')||*end.byte==';'))return {TerminalOutcome::cancelled,{}};
            }
            continue;
        }
        if (byte == kCancel || (request.escape_cancels && byte==27)) {
            wipe_pending();
            return {TerminalOutcome::cancelled, std::nullopt};
        }
        if (byte == kCarriageReturn || byte == kLineFeed) {
            if (byte == kCarriageReturn) discard_one_lf_after_cr_ = true;
            auto newline = system_style();
            if (newline == TerminalOutcome::completed && request.advance_line) newline = write_all("\r\n");
            if (newline != TerminalOutcome::completed) {
                wipe_pending();
                return {newline, std::nullopt};
            }
            if (request.default_key.has_value()) {
                return {TerminalOutcome::empty, request.default_key};
            }
            return {TerminalOutcome::invalid, std::nullopt};
        }
        if (printable_input(byte)) {
            const auto canonical =
                canonical_allowed_key(request.allowed_keys, byte);
            if (!canonical.has_value()) {
                if (request.report_invalid_key) {
                    const char entered = static_cast<char>(byte);
                    auto outcome = write_all(std::string_view(&entered, 1));
                    if (outcome == TerminalOutcome::completed) outcome = system_style();
                    if (outcome == TerminalOutcome::completed && request.advance_line) outcome = write_all("\r\n");
                    if (outcome != TerminalOutcome::completed) wipe_pending();
                    else discard_coalesced_logical_enter_after_key();
                    return {outcome == TerminalOutcome::completed ? TerminalOutcome::invalid : outcome, {}};
                }
                const TerminalOutcome bell = write_all("\a");
                if (bell != TerminalOutcome::completed) {
                    wipe_pending();
                    return {bell, std::nullopt};
                }
                continue;
            }
            const char output = *canonical;
            auto echoed = write_all(std::string_view(&output, 1));
            if (echoed == TerminalOutcome::completed) echoed = system_style();
            if (echoed == TerminalOutcome::completed && request.advance_line) echoed = write_all("\r\n");
            if (echoed != TerminalOutcome::completed) {
                wipe_pending();
                return {echoed, std::nullopt};
            }
            discard_coalesced_logical_enter_after_key();
            return {TerminalOutcome::completed, canonical};
        }

        TerminalOutcome drained = discard_through_logical_enter(input_events);
        if (drained == TerminalOutcome::completed && request.advance_line) {
            drained = write_all("\r\n");
        }
        if (drained != TerminalOutcome::completed) {
            wipe_pending();
            return {drained, std::nullopt};
        }
        return {TerminalOutcome::invalid, std::nullopt};
    }
}

TerminalOutcome StreamCallerTerminal::present(
    const TerminalPresentationRequest& request) {
    if (!valid_terminal_presentation_request(request)) {
        return TerminalOutcome::output_failure;
    }
    const bool notification=request.identity=="caller.failed_login_notice";
    auto outcome = TerminalOutcome::completed;
    if(request.role==TerminalPresentationRole::caller_value || notification){
        outcome=notification && text_style_==TerminalTextStyle::ogsf_ansi ? write_all(menu_text_sgr(15)) : system_style(true);
        if(outcome==TerminalOutcome::completed)outcome=write_logical_text(request.non_secret_text);
    }else outcome=write_system_text(request.non_secret_text);
    if (outcome != TerminalOutcome::completed) wipe_pending();
    return outcome;
}

TerminalOutcome StreamCallerTerminal::present_display(
    const TerminalDisplayPresentationRequest& request) {
    if (!valid_terminal_display_presentation_request(request)) {
        return TerminalOutcome::output_failure;
    }
    if(request.system_ui && system_style()!=TerminalOutcome::completed)return TerminalOutcome::output_failure;
    bool output_started = false;
    bool paging_enabled = true;
    bool nonstop = false;
    bool abort_enabled = true;
    std::size_t emitted_rows = 0;
    std::size_t paging_input_events = 0;
    const std::size_t row_limit = request.paging.more_prompt_enabled
        ? static_cast<std::size_t>(*request.paging.page_length) - 1U
        : 0U;
    const auto prompt_sgr=text_style_==TerminalTextStyle::ogsf_ansi?menu_text_sgr(7):std::string{};
    DisplayRendition rendition;
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    std::uint32_t playback_bps = 0;
    Clock::time_point next_output;
    const auto write_display = [&](std::string_view bytes, bool split_text = false) {
        if (playback_bps == 0) return write_all(bytes);
        // A serial byte occupies ten bits with 8N1 framing. Use small text
        // batches and leave ANSI sequences and CRLF pairs in one write.
        const auto batch = std::max<std::size_t>(1, playback_bps / 400);
        while (!bytes.empty()) {
            const auto piece = split_text ? bytes.substr(0, batch) : bytes;
            auto now = Clock::now();
            if (now - next_output > 50ms) next_output = now;
            // Keep the cumulative deadline across small ANSI operations.
            // Check the session regularly even after a long sequence at 300 bps.
            while (next_output - now >= 15ms) {
                const auto alive = stream_.poll_output();
                if (alive != InteractiveStreamOutcome::completed) return output_outcome(alive);
                std::this_thread::sleep_until(std::min(next_output, now + 40ms));
                now = Clock::now();
            }
            const auto outcome = write_all(piece);
            if (outcome != TerminalOutcome::completed) return outcome;
            next_output += std::chrono::nanoseconds(10000000000ULL * piece.size() / playback_bps);
            bytes.remove_prefix(piece.size());
        }
        return TerminalOutcome::completed;
    };
    try {
        for (const auto& operation : request.program->operations) {
            if (operation.playback_bps != playback_bps) {
                playback_bps = operation.playback_bps;
                if (playback_bps != 0) next_output = Clock::now();
            }
            TerminalOutcome outcome = TerminalOutcome::completed;
            switch (operation.kind) {
                case TerminalDisplayOperationKind::cp437_text:
                    output_started = true;
                    if(request.system_ui){
                        const auto styled=system_menu_text(operation.bytes,text_style_==TerminalTextStyle::ogsf_ansi);
                        if(styled.size()>kTerminalMaximumDisplayOutputBytes)return TerminalOutcome::output_failure;
                        outcome=write_all(styled);
                    }else outcome = write_display(operation.bytes, true);
                    break;
                case TerminalDisplayOperationKind::logical_newline:
                    output_started = true;
                    outcome = write_display("\r\n");
                    break;
                case TerminalDisplayOperationKind::paging_newline:
                    {
                        output_started = true;
                        bool stopped = false;
                        outcome = write_display("\r\n");
                        if (outcome == TerminalOutcome::completed) {
                            const auto prior_inputs = paging_input_events;
                            outcome = handle_emitted_paging_row(row_limit,
                                emitted_rows, paging_enabled, nonstop,
                                abort_enabled, paging_input_events, stopped,
                                prompt_sgr,rendition.sequence());
                            if (paging_input_events != prior_inputs) playback_bps = 0;
                        }
                        if (outcome == TerminalOutcome::completed && stopped) {
                            return TerminalOutcome::completed;
                        }
                    }
                    break;
                case TerminalDisplayOperationKind::ansi_sequence:
                    output_started = true;
                    outcome = write_display(operation.bytes);
                    if(!prompt_sgr.empty()) rendition.apply(operation.bytes);
                    break;
                case TerminalDisplayOperationKind::clear_screen:
                    output_started = true;
                    outcome = write_display("\x1b[2J\x1b[H");
                    break;
                case TerminalDisplayOperationKind::caller_bell:
                    output_started = true;
                    outcome = write_display("\a");
                    break;
                case TerminalDisplayOperationKind::pause:
                    output_started = true;
                    outcome = pause_internal(
                        {request.identity}, abort_enabled);
                    playback_bps = 0;
                    break;
                case TerminalDisplayOperationKind::paging_disabled:
                    paging_enabled = false;
                    break;
                case TerminalDisplayOperationKind::paging_enabled:
                    if (request.paging.more_prompt_enabled) {
                        paging_enabled = true;
                        nonstop = false;
                        emitted_rows = 0;
                        output_started = true;
                        const PagingPromptResult prompt = paging_prompt(
                            PagingPromptKind::forced, abort_enabled,
                            paging_input_events,prompt_sgr,rendition.sequence());
                        outcome = prompt.outcome;
                        playback_bps = 0;
                    }
                    break;
                case TerminalDisplayOperationKind::abort_disabled:
                    abort_enabled = false;
                    break;
                case TerminalDisplayOperationKind::abort_enabled:
                    abort_enabled = true;
                    break;
            }
            if (outcome != TerminalOutcome::completed) {
                wipe_pending();
                return outcome;
            }
        }
    } catch (...) {
        wipe_pending();
        return output_started ? TerminalOutcome::output_failure
                              : TerminalOutcome::terminal_failure;
    }
    return TerminalOutcome::completed;
}

StreamCallerTerminal::PagingPromptResult StreamCallerTerminal::paging_prompt(
    const PagingPromptKind kind,
    const bool abort_enabled,
    std::size_t& input_events,
    std::string_view prompt_sgr, std::string_view restore_sgr) {
    const std::string_view prompt = kind == PagingPromptKind::automatic
        ? kAutomaticPagingPrompt
        : kForcedPagingPrompt;
    TerminalOutcome outcome = write_system_text(prompt);
    if (outcome != TerminalOutcome::completed) {
        wipe_pending();
        return {outcome, PagingResponse::failed};
    }

    for (;;) {
        const NextByteResult next = next_prompt_byte(input_events);
        if (!next.byte.has_value()) {
            if (next.outcome != TerminalOutcome::completed) wipe_pending();
            return {next.outcome, PagingResponse::failed};
        }

        const std::uint8_t byte = *next.byte;
        if (byte == kCancel) {
            if (!abort_enabled) {
                outcome = write_all("\a");
                if (outcome != TerminalOutcome::completed) {
                    wipe_pending();
                    return {outcome, PagingResponse::failed};
                }
                continue;
            }
            wipe_pending();
            return {TerminalOutcome::cancelled, PagingResponse::failed};
        }

        PagingResponse response = PagingResponse::failed;
        if (byte == kCarriageReturn || byte == kLineFeed || byte == ' ') {
            response = PagingResponse::continued;
        } else if (kind == PagingPromptKind::automatic &&
            (byte == 'N' || byte == 'n')) {
            response = PagingResponse::nonstop;
        } else if (byte == 'S' || byte == 's') {
            response = kind == PagingPromptKind::automatic && abort_enabled
                ? PagingResponse::stopped
                : PagingResponse::continued;
        }

        if (response == PagingResponse::failed) {
            outcome = write_all("\a");
            if (outcome != TerminalOutcome::completed) {
                wipe_pending();
                return {outcome, PagingResponse::failed};
            }
            continue;
        }

        discard_paging_response_tail(byte);
        outcome = write_all("\r\n");
        if(outcome==TerminalOutcome::completed && !prompt_sgr.empty()) outcome=write_all(restore_sgr);
        if (outcome != TerminalOutcome::completed) {
            wipe_pending();
            return {outcome, PagingResponse::failed};
        }
        return {TerminalOutcome::completed, response};
    }
}

TerminalOutcome StreamCallerTerminal::handle_emitted_paging_row(
    const std::size_t row_limit,
    std::size_t& emitted_rows,
    const bool paging_enabled,
    bool& nonstop,
    const bool abort_enabled,
    std::size_t& input_events,
    bool& stopped,
    std::string_view prompt_sgr, std::string_view restore_sgr) {
    stopped = false;
    if (row_limit == 0U || !paging_enabled || nonstop) {
        return TerminalOutcome::completed;
    }

    ++emitted_rows;
    if (emitted_rows != row_limit) return TerminalOutcome::completed;

    const PagingPromptResult prompt = paging_prompt(
        PagingPromptKind::automatic, abort_enabled, input_events,prompt_sgr,restore_sgr);
    if (prompt.outcome != TerminalOutcome::completed) return prompt.outcome;
    emitted_rows = 0;
    if (prompt.response == PagingResponse::nonstop) {
        nonstop = true;
    } else if (prompt.response == PagingResponse::stopped) {
        stopped = true;
    }
    return TerminalOutcome::completed;
}

void StreamCallerTerminal::discard_paging_response_tail(
    const std::uint8_t response) noexcept {
    bool discard_split_lf = false;
    if (pending_offset_ < pending_.size()) {
        if (response == kCarriageReturn &&
            pending_[pending_offset_] == kLineFeed) {
            pending_[pending_offset_] = 0U;
            ++pending_offset_;
        } else if (response == kCarriageReturn) {
            // A different coalesced byte proves there is no split LF that
            // belongs to this CR response.
        } else if (response != kCarriageReturn &&
            response != kLineFeed) {
            const std::uint8_t first = pending_[pending_offset_];
            if (first == kCarriageReturn || first == kLineFeed) {
                pending_[pending_offset_] = 0U;
                ++pending_offset_;
                if (first == kCarriageReturn &&
                    pending_offset_ < pending_.size() &&
                    pending_[pending_offset_] == kLineFeed) {
                    pending_[pending_offset_] = 0U;
                    ++pending_offset_;
                } else if (first == kCarriageReturn &&
                    pending_offset_ == pending_.size()) {
                    discard_split_lf = true;
                }
            }
        }
    } else if (response == kCarriageReturn) {
        discard_split_lf = true;
    }
    wipe_pending(false);
    discard_one_lf_after_cr_ = discard_split_lf;
}

TerminalOutcome StreamCallerTerminal::pause(
    const TerminalPauseRequest& request) {
    return pause_internal(request, true);
}

TerminalOutcome StreamCallerTerminal::pause_internal(
    const TerminalPauseRequest& request,
    const bool cancellation_enabled) {
    if (!valid_terminal_pause_request(request)) {
        return TerminalOutcome::invalid;
    }

    std::size_t input_events = 0;
    for (;;) {
        const NextByteResult next = next_prompt_byte(input_events);
        if (!next.byte.has_value()) {
            if (next.outcome != TerminalOutcome::completed) wipe_pending();
            return next.outcome;
        }
        const std::uint8_t byte = *next.byte;
        if (byte == kCancel) {
            if (!cancellation_enabled) {
                const TerminalOutcome bell = write_all("\a");
                if (bell != TerminalOutcome::completed) {
                    wipe_pending();
                    return bell;
                }
                continue;
            }
            wipe_pending();
            return TerminalOutcome::cancelled;
        }
        if (byte == kCarriageReturn || byte == kLineFeed) {
            if (byte == kCarriageReturn) discard_one_lf_after_cr_ = true;
            const TerminalOutcome newline = write_all("\r\n");
            if (newline != TerminalOutcome::completed) wipe_pending();
            return newline;
        }
        if (byte == kBackspace || byte == kDelete) {
            continue;
        }
        if (printable_input(byte)) {
            const TerminalOutcome bell = write_all("\a");
            if (bell != TerminalOutcome::completed) {
                wipe_pending();
                return bell;
            }
            continue;
        }

        TerminalOutcome drained = discard_through_logical_enter(input_events);
        if (drained == TerminalOutcome::completed) {
            drained = write_all("\r\n");
        }
        if (drained != TerminalOutcome::completed) wipe_pending();
        return drained == TerminalOutcome::completed
            ? TerminalOutcome::invalid
            : drained;
    }
}

StreamCallerTerminal::NextByteResult StreamCallerTerminal::next_prompt_byte(
    std::size_t& input_events) {
    const auto counted = [&](bool tail = false) {
        auto next = next_byte(tail);
        if (next.byte && !increment_event_count(input_events)) {
            wipe_pending();
            return NextByteResult{TerminalOutcome::terminal_failure, std::nullopt};
        }
        return next;
    };
    for (;;) {
        auto next = counted();
        if (!next.byte || *next.byte != 27) return next;
        // A poll timeout denotes bare Escape. Once CSI/SS3 starts, consume its
        // complete body even across stream fragments. Never expose its final
        // A/B/C/D (or other key code) to an ordinary prompt or the next menu.
        auto tail = counted(true);
        if (!tail.byte) {
            if (tail.outcome == TerminalOutcome::timed_out)
                return {TerminalOutcome::completed, std::uint8_t{27}};
            return tail;
        }
        if (*tail.byte != '[' && *tail.byte != 'O')
            return {TerminalOutcome::completed, std::uint8_t{27}};
        for (;;) {
            tail = counted();
            if (!tail.byte) return tail;
            const auto byte = *tail.byte;
            if (byte >= 0x40U && byte <= 0x7eU) break;
            // An actual Enter/cancel still works after an incomplete key.
            if (byte == kCarriageReturn || byte == kLineFeed || byte == kCancel)
                return tail;
        }
    }
}

StreamCallerTerminal::NextByteResult StreamCallerTerminal::next_byte(bool escape_tail) {
    for (;;) {
        if (pending_offset_ < pending_.size()) {
            const std::uint8_t byte = pending_[pending_offset_];
            pending_[pending_offset_] = 0U;
            ++pending_offset_;
            if (pending_offset_ == pending_.size()) wipe_pending(false);

            if (discard_one_lf_after_cr_) {
                discard_one_lf_after_cr_ = false;
                if (byte == kLineFeed) continue;
            }
            return {TerminalOutcome::completed, byte};
        }

        InteractiveStreamReadResult read;
        try {
            read = escape_tail
                ? stream_.read_escape_tail(kInteractiveStreamMaximumReadBytes)
                : stream_.read_some(kInteractiveStreamMaximumReadBytes);
        } catch (...) {
            wipe_pending();
            return {TerminalOutcome::terminal_failure, std::nullopt};
        }
        if (!valid_interactive_stream_read_result(
                kInteractiveStreamMaximumReadBytes, read)) {
            if (!read.bytes.empty()) {
                detail::explicitly_erase_bytes(
                    read.bytes.data(), read.bytes.size());
                read.bytes.clear();
            }
            wipe_pending();
            discard_one_lf_after_cr_ = false;
            return {TerminalOutcome::terminal_failure, std::nullopt};
        }
        if (read.outcome != InteractiveStreamOutcome::completed) {
            wipe_pending();
            discard_one_lf_after_cr_ = false;
            return {input_outcome(read.outcome), std::nullopt};
        }
        pending_ = std::move(read.bytes);
        pending_offset_ = 0;
    }
}

TerminalOutcome StreamCallerTerminal::discard_through_logical_enter(
    std::size_t& input_events) {
    for (;;) {
        const NextByteResult next = next_byte();
        if (!next.byte.has_value()) return next.outcome;
        if (!increment_event_count(input_events)) {
            wipe_pending();
            return TerminalOutcome::invalid;
        }
        const std::uint8_t byte = *next.byte;
        if (byte == kCancel) {
            wipe_pending();
            return TerminalOutcome::cancelled;
        }
        if (byte == kCarriageReturn) {
            discard_one_lf_after_cr_ = true;
            return TerminalOutcome::completed;
        }
        if (byte == kLineFeed) return TerminalOutcome::completed;
    }
}

void StreamCallerTerminal::discard_coalesced_logical_enter_after_key() noexcept {
    if (pending_offset_ >= pending_.size()) return;

    const std::uint8_t first = pending_[pending_offset_];
    if (first != kCarriageReturn && first != kLineFeed) return;

    pending_[pending_offset_] = 0U;
    ++pending_offset_;
    if (first == kCarriageReturn) {
        if (pending_offset_ < pending_.size() &&
            pending_[pending_offset_] == kLineFeed) {
            pending_[pending_offset_] = 0U;
            ++pending_offset_;
        } else if (pending_offset_ == pending_.size()) {
            discard_one_lf_after_cr_ = true;
        }
    }
    if (pending_offset_ == pending_.size()) wipe_pending(false);
}

TerminalOutcome StreamCallerTerminal::write_system_text(std::string_view text) {
    if(!ordinary_logical_text(text))return TerminalOutcome::output_failure;
    try {
    std::string normalized;
    for(std::size_t i=0;i<text.size();++i){
        if(text[i]=='\r'||text[i]=='\n'){
            if(text[i]=='\r'&&i+1<text.size()&&text[i+1]=='\n')++i;
            normalized+="\r\n";
        }else normalized+=text[i];
        if(normalized.size()>kTerminalMaximumOutputBytes)return TerminalOutcome::output_failure;
    }
    const auto output=system_menu_text(normalized,text_style_==TerminalTextStyle::ogsf_ansi);
    if(output.size()>kTerminalMaximumOutputBytes)return TerminalOutcome::output_failure;
    return write_all(output);
    } catch(const std::bad_alloc&) {return TerminalOutcome::terminal_failure;}
}
TerminalOutcome StreamCallerTerminal::write_logical_text(
    const std::string_view text) {
    if (!ordinary_logical_text(text)) {
        return TerminalOutcome::output_failure;
    }

    std::string normalized;
    try {
        normalized.reserve(std::min(text.size(), kTerminalMaximumOutputBytes));
    } catch (const std::bad_alloc&) {
        return TerminalOutcome::terminal_failure;
    }

    try {
        for (std::size_t index = 0; index < text.size(); ++index) {
            const char value = text[index];
            if (value == '\r' || value == '\n') {
                if (value == '\r' && index + 1U < text.size() &&
                    text[index + 1U] == '\n') {
                    ++index;
                }
                if (normalized.size() > kTerminalMaximumOutputBytes - 2U) {
                    return TerminalOutcome::output_failure;
                }
                normalized.push_back('\r');
                normalized.push_back('\n');
            } else {
                if (normalized.size() == kTerminalMaximumOutputBytes) {
                    return TerminalOutcome::output_failure;
                }
                normalized.push_back(value);
            }
        }
    } catch (const std::bad_alloc&) {
        return TerminalOutcome::terminal_failure;
    }
    return write_all(normalized);
}

TerminalOutcome StreamCallerTerminal::write_all(
    const std::string_view bytes) {
    if (bytes.empty()) return TerminalOutcome::completed;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::string_view remaining = bytes.substr(offset);
        InteractiveStreamWriteResult write;
        try {
            write = stream_.write_some(remaining);
        } catch (...) {
            return TerminalOutcome::output_failure;
        }
        if (!valid_interactive_stream_write_result(
                remaining.size(), write)) {
            return TerminalOutcome::output_failure;
        }
        if (write.outcome != InteractiveStreamOutcome::completed) {
            if (offset != 0 || write.bytes_written != 0) {
                return TerminalOutcome::output_failure;
            }
            return output_outcome(write.outcome);
        }
        offset += write.bytes_written;
    }
    return TerminalOutcome::completed;
}

void StreamCallerTerminal::wipe_pending(
    const bool reset_newline_state) noexcept {
    if (!pending_.empty()) {
        detail::explicitly_erase_bytes(
            pending_.data(), pending_.size());
        pending_.clear();
    }
    pending_offset_ = 0;
    if (reset_newline_state) discard_one_lf_after_cr_ = false;
}

}  // namespace sfx::application

