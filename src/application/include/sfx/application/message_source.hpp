#pragma once
#include "sfx/application/message.hpp"
#include "sfx/application/terminal.hpp"

namespace sfx::application {
inline constexpr std::size_t kMessageSourceBytes=262144;
inline constexpr std::size_t kMessageCanvasRows=1000;
inline constexpr std::size_t kMessageCanvasWork=4000000;
struct MessageCanvasCell { std::uint8_t glyph{32}, attribute{7}; };
struct MessageCanvas {
    std::vector<MessageCanvasCell> cells;
    // Only received rendition v2 allocates these. Stored profile1 keeps its
    // original VGA contract and allocation cost. 0xffffffff means VGA color.
    std::vector<std::uint32_t> foreground_rgb,background_rgb;
    MessageBody text;
    std::uint16_t extent{};
    std::size_t operations{}, work{};
    bool nonblank{};
};
enum class MessageSourceValidation : std::uint8_t {
    valid, invalid_profile, size, control, bounds, budget, blank, line_limit
};
struct ValidatedMessageSource {
    MessageSourceValidation validation{MessageSourceValidation::invalid_profile};
    MessageCanvas off, on;
    std::uint16_t charged_extent{};
    explicit operator bool() const noexcept {return validation==MessageSourceValidation::valid;}
};
// Original bytes are inert data. No tokens, includes or caller controls execute.
// Both policy variants are validated and charged, independent of current policy.
ValidatedMessageSource validate_message_source(const MessageSource&,std::uint32_t maximum_lines,
    std::uint8_t plain_color=7);
// A policy-specific view; original bytes/profile and charged validation remain
// immutable. Disabling ANSI attributes does not remove cursor/layout handling
// or independently enabled pipe colors.
std::optional<MessageCanvas> message_source_rendition(const MessageSource&,bool pipes,
    bool ansi_colors,std::uint8_t plain_color=7);
// Existing normalized bodies retain their 1 MiB contract; this is rendition,
// not an upload or a storage rewrite. Pipe policy cannot add visible rows.
std::optional<MessageCanvas> message_plain_canvas(const MessageBody&,bool pipes,std::uint8_t color=7);
// Render plain text for the terminal only. Supported Renegade-style pipe
// codes end at 23; the caller's base color resumes at each stored newline.
std::optional<MessageCanvas> message_synchronet_pipe_canvas(std::string_view,std::uint8_t color=7);
// Received ANSI rendition v2 is derived presentation, never stored profile1.
// Original controls cannot leave this bounded virtual canvas.
inline constexpr unsigned kReceivedAnsiRenditionVersion=2;
std::optional<MessageCanvas> message_received_ansi_canvas(std::string_view,bool pipes);
// Bounded page of generated rendition/text only. No original CSI leaves parser.
std::optional<TerminalDisplayProgram> message_canvas_page(const MessageCanvas&,
    std::size_t first,std::size_t count,bool ansi);
} // namespace sfx::application
