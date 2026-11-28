#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sfx::application {

inline constexpr std::size_t kTerminalMaximumInputCharacters = 4096;
inline constexpr std::size_t kTerminalMaximumDisplayOperations = 65536;
inline constexpr std::size_t kTerminalMaximumDisplayOutputBytes =
    1024U * 1024U;
inline constexpr std::uint16_t kTerminalMinimumPageLength = 10U;
inline constexpr std::uint16_t kTerminalMaximumPageLength = 24U;

enum class TerminalOutcome : std::uint8_t {
    completed,
    empty,
    cancelled,
    timed_out,
    disconnected,
    shutdown_requested,
    invalid,
    overlength,
    terminal_failure,
    output_failure
};

enum class EmptyInputPolicy : std::uint8_t { reject, use_default };
enum class TerminalTextStyle : std::uint8_t { plain, ogsf_ansi };

struct TerminalInputRequest {
    std::string identity;
    std::string prompt;
    std::size_t maximum_characters{0};
    EmptyInputPolicy empty_policy{EmptyInputPolicy::reject};
    // Visible editing only. Secret input rejects nonempty initial values.
    std::string initial_value{};
    bool escape_cancels{false};
};

struct TerminalKeyRequest {
    std::string identity;
    std::string prompt;
    std::vector<char> allowed_keys;
    std::optional<char> default_key;
    bool report_invalid_key{false};
    bool escape_cancels{false};
    bool navigation_keys{false};
    // A positioned prompt can reclaim its row after a key is accepted.
    bool advance_line{true};
    // A retained viewport may already have this prompt and cursor in place.
    bool prompt_visible{false};
};

// Host-selected presentation role. Caller values are inert data, never ANSI.
enum class TerminalPresentationRole : std::uint8_t { system, caller_value };
struct TerminalPresentationRequest {
    std::string identity;
    std::string non_secret_text;
    TerminalPresentationRole role{TerminalPresentationRole::system};
};

enum class TerminalDisplayOperationKind : std::uint8_t {
    cp437_text,
    logical_newline,
    paging_newline,
    ansi_sequence,
    clear_screen,
    caller_bell,
    pause,
    paging_disabled,
    paging_enabled,
    abort_disabled,
    abort_enabled
};

struct TerminalDisplayOperation {
    TerminalDisplayOperationKind kind{TerminalDisplayOperationKind::cp437_text};
    std::string bytes;
    // Source-specific rate for authored artwork. Zero uses the normal speed.
    std::uint32_t playback_bps{};
};

struct TerminalDisplayProgram {
    std::vector<TerminalDisplayOperation> operations;
    std::size_t output_byte_count{0};
    // Trusted host framing only; never selected by display-file tokens.
    std::optional<std::uint8_t> paging_prompt_color;
};

struct TerminalDisplayPagingPolicy {
    bool more_prompt_enabled{false};
    std::optional<std::uint16_t> page_length;
};

struct TerminalDisplayPresentationRequest {
    std::string identity;
    const TerminalDisplayProgram* program{nullptr};
    TerminalDisplayPagingPolicy paging;
    // Generated UI only. Authored artwork/message programs retain their styles.
    bool system_ui{false};

    TerminalDisplayPresentationRequest(
        std::string request_identity,
        const TerminalDisplayProgram* request_program,
        TerminalDisplayPagingPolicy request_paging = {})
        : identity(std::move(request_identity)),
          program(request_program),
          paging(std::move(request_paging)) {}
};

struct TerminalPauseRequest {
    std::string identity;
};

struct TerminalInputResult {
    TerminalOutcome outcome{TerminalOutcome::invalid};
    std::string value;

    bool has_value() const noexcept {
        return outcome == TerminalOutcome::completed && !value.empty();
    }
};

enum class TerminalNavigationKey : std::uint8_t { none, up, down, left, right };

struct TerminalKeyResult {
    TerminalOutcome outcome{TerminalOutcome::invalid};
    std::optional<char> key;
    TerminalNavigationKey navigation{TerminalNavigationKey::none};

    bool has_key() const noexcept {
        return (outcome == TerminalOutcome::completed ||
                   outcome == TerminalOutcome::empty) &&
            key.has_value();
    }
};

bool valid_terminal_input_request(const TerminalInputRequest& request) noexcept;
bool valid_terminal_key_request(const TerminalKeyRequest& request) noexcept;
bool valid_terminal_input_result(
    const TerminalInputRequest& request,
    const TerminalInputResult& result) noexcept;
bool valid_terminal_key_result(
    const TerminalKeyRequest& request,
    const TerminalKeyResult& result) noexcept;
bool valid_terminal_presentation_request(
    const TerminalPresentationRequest& request) noexcept;
bool valid_terminal_ansi_sequence(std::string_view sequence) noexcept;
bool valid_terminal_display_program(
    const TerminalDisplayProgram& program) noexcept;
bool valid_terminal_display_paging_policy(
    const TerminalDisplayPagingPolicy& policy) noexcept;
bool valid_terminal_display_presentation_request(
    const TerminalDisplayPresentationRequest& request) noexcept;
bool valid_terminal_pause_request(const TerminalPauseRequest& request) noexcept;
bool terminal_outcome_has_no_input(TerminalOutcome outcome) noexcept;

namespace detail {

void explicitly_erase_bytes(void* data, std::size_t byte_count) noexcept;

}  // namespace detail

void explicitly_erase_terminal_secret(std::string& value) noexcept;

class ICallerTerminal {
public:
    virtual ~ICallerTerminal() = default;

    virtual TerminalOutcome set_text_style(TerminalTextStyle style) = 0;
    virtual TerminalOutcome clear_screen() = 0;

    virtual TerminalInputResult read_visible(const TerminalInputRequest& request) = 0;
    virtual TerminalInputResult read_secret(const TerminalInputRequest& request) = 0;
    virtual TerminalKeyResult read_key(const TerminalKeyRequest& request) = 0;
    virtual TerminalOutcome present(const TerminalPresentationRequest& request) = 0;
    virtual TerminalOutcome present_display(
        const TerminalDisplayPresentationRequest& request) = 0;
    virtual TerminalOutcome pause(const TerminalPauseRequest& request) = 0;
};

}  // namespace sfx::application
