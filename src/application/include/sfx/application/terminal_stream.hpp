#pragma once

#include "sfx/application/terminal.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace sfx::application {

inline constexpr std::size_t kInteractiveStreamMaximumReadBytes = 4096;
inline constexpr std::size_t kTerminalMaximumOutputBytes = 1024U * 1024U;
inline constexpr std::size_t kTerminalMaximumInputEventsPerRequest = 65536;
inline constexpr std::size_t kTerminalSecretStorageCapacity =
    kTerminalMaximumInputCharacters;

enum class InteractiveStreamOutcome : std::uint8_t {
    completed,
    cancelled,
    timed_out,
    disconnected,
    shutdown_requested,
    failed
};

struct InteractiveStreamReadResult {
    InteractiveStreamOutcome outcome{InteractiveStreamOutcome::failed};
    std::vector<std::uint8_t> bytes;
};

struct InteractiveStreamWriteResult {
    InteractiveStreamOutcome outcome{InteractiveStreamOutcome::failed};
    std::size_t bytes_written{0};
};

enum class SecretInputEchoMode : std::uint8_t {
    hidden,
    masked
};

struct StreamCallerTerminalOptions {
    SecretInputEchoMode secret_input_echo{SecretInputEchoMode::hidden};
    std::uint8_t text_color{7};
    std::uint8_t input_color{11};
    // Optional session-owned preference, set only after successful admission.
    // Zero means use the board default. The owner outlives this terminal.
    const std::uint8_t* caller_text_color{nullptr};
    // Optional session-owned style observation for a noninteractive inactivity
    // notice. It grants no identity or ANSI capability before the choice.
    TerminalTextStyle* effective_style{nullptr};
};

bool valid_interactive_stream_read_result(
    std::size_t requested_maximum,
    const InteractiveStreamReadResult& result) noexcept;
bool valid_interactive_stream_write_result(
    std::size_t offered_bytes,
    const InteractiveStreamWriteResult& result) noexcept;

class IInteractiveSessionByteStream {
public:
    virtual ~IInteractiveSessionByteStream() = default;

    virtual InteractiveStreamReadResult read_some(std::size_t maximum_bytes) = 0;
    // Preserve a transport poll boundary only while distinguishing bare Escape
    // from a terminal key sequence. Lifetime/cancellation checks still apply.
    virtual InteractiveStreamReadResult read_escape_tail(std::size_t maximum_bytes) {
        return read_some(maximum_bytes);
    }
    virtual InteractiveStreamWriteResult write_some(std::string_view bytes) = 0;
};

class StreamCallerTerminal final : public ICallerTerminal {
public:
    struct EditorByteResult { TerminalOutcome outcome{TerminalOutcome::invalid}; std::optional<std::uint8_t> byte; };
    explicit StreamCallerTerminal(
        IInteractiveSessionByteStream& stream,
        StreamCallerTerminalOptions options = {}) noexcept;
    StreamCallerTerminal(const StreamCallerTerminal&) = delete;
    StreamCallerTerminal& operator=(const StreamCallerTerminal&) = delete;
    StreamCallerTerminal(StreamCallerTerminal&&) = delete;
    StreamCallerTerminal& operator=(StreamCallerTerminal&&) = delete;
    ~StreamCallerTerminal() override;

    TerminalOutcome set_text_style(TerminalTextStyle style) override;
    TerminalOutcome set_colors(std::uint8_t text, std::uint8_t input);
    TerminalOutcome clear_screen() override;

    TerminalInputResult read_visible(const TerminalInputRequest& request) override;
    TerminalInputResult read_secret(const TerminalInputRequest& request) override;
    TerminalKeyResult read_key(const TerminalKeyRequest& request) override;
    TerminalOutcome present(const TerminalPresentationRequest& request) override;
    TerminalOutcome present_display(
        const TerminalDisplayPresentationRequest& request) override;
    TerminalOutcome pause(const TerminalPauseRequest& request) override;
    // Shares pending bytes and CRLF state with visible prompts. The transport
    // owner retains deadlines/revocation; this does not bypass lifetime polling.
    EditorByteResult read_editor_byte();
    void finish_editor_key(bool carriage_return) noexcept;
    // Move only CRLF delimiter ownership when the one-byte menu/modal reader
    // changes. This never reads, flushes or discards a later caller command.
    bool take_pending_delimiter_lf() noexcept {const bool value=discard_one_lf_after_cr_;discard_one_lf_after_cr_=false;return value;}
    struct BinaryInputPrefix {std::vector<std::uint8_t> bytes;bool discard_delimiter_lf{};};
    // Move already-decoded protocol bytes into exclusive receive ownership.
    // Only the pending logical CRLF delimiter may be consumed; never flush IO.
    BinaryInputPrefix take_binary_input_prefix();

private:
    enum class PagingPromptKind : std::uint8_t { automatic, forced };
    enum class PagingResponse : std::uint8_t {
        continued,
        nonstop,
        stopped,
        failed
    };

    struct NextByteResult {
        TerminalOutcome outcome{TerminalOutcome::invalid};
        std::optional<std::uint8_t> byte;
    };

    struct PagingPromptResult {
        TerminalOutcome outcome{TerminalOutcome::invalid};
        PagingResponse response{PagingResponse::failed};
    };

    TerminalInputResult read_line(
        const TerminalInputRequest& request,
        bool secret);
    NextByteResult next_byte(bool escape_tail = false);
    NextByteResult next_prompt_byte(std::size_t& input_events);
    TerminalOutcome discard_through_logical_enter(std::size_t& input_events);
    void discard_coalesced_logical_enter_after_key() noexcept;
    TerminalOutcome pause_internal(
        const TerminalPauseRequest& request,
        bool cancellation_enabled);
    PagingPromptResult paging_prompt(
        PagingPromptKind kind,
        bool abort_enabled,
        std::size_t& input_events,
        std::string_view prompt_sgr = {}, std::string_view restore_sgr = {});
    TerminalOutcome handle_emitted_paging_row(
        std::size_t row_limit,
        std::size_t& emitted_rows,
        bool paging_enabled,
        bool& nonstop,
        bool abort_enabled,
        std::size_t& input_events,
        bool& stopped,
        std::string_view prompt_sgr = {}, std::string_view restore_sgr = {});
    void discard_paging_response_tail(std::uint8_t response) noexcept;
    TerminalOutcome write_logical_text(std::string_view text);
    TerminalOutcome write_prompt(std::string_view text);
    TerminalOutcome system_style(bool caller_color = false);
    TerminalOutcome write_system_text(std::string_view text);
    TerminalOutcome write_all(std::string_view bytes);
    void wipe_pending(bool reset_newline_state = true) noexcept;

    IInteractiveSessionByteStream& stream_;
    StreamCallerTerminalOptions options_;
    TerminalTextStyle text_style_{TerminalTextStyle::plain};
    std::vector<std::uint8_t> pending_;
    std::size_t pending_offset_{0};
    bool discard_one_lf_after_cr_{false};
};

}  // namespace sfx::application
