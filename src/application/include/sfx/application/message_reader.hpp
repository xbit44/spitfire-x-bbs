#pragma once
#include "sfx/application/message_selection.hpp"
#include "sfx/application/message_compose.hpp"

namespace sfx::application {
struct MessageReaderPorts {
    IMessageStorage& storage;
    ICallerTerminal& terminal;
    std::function<std::optional<MessageActor>()> actor;
    std::function<bool(const OpenedMessage&)> reply;
    std::function<std::string(std::int64_t)> date;
    std::function<bool()> safe_boundary;
    std::uint16_t page_length{24};
    bool more{true};
    std::optional<MessageQueryContext> entry_context{};
    std::function<Identifier128()> operation{};
    std::function<MessageEditorDimensions()> dimensions{};
    std::function<std::uint8_t()> text_color{};
    bool sysop_review_only{false}; // Section14A: read/delete; no reply or Protect changes
    bool allow_skip{};
    bool return_to_list{};
    bool adjacent_previous{}, adjacent_next{}, start_at_end{};
    std::function<StreamCallerTerminal::EditorByteResult()> byte{};
    std::function<bool(std::string_view)> write{};
    std::function<void(bool)> accepted_key{};
    std::function<std::optional<bool>(unsigned footer_row)> preview{};
    std::function<bool()> lightbars_enabled{};
    std::string list_summary{};
    std::function<bool()> legacy_pipe_colors{};
};
enum class MessageReaderOutcome : std::uint8_t { completed, cancelled, failed, skipped, previous_conference, next_conference };
// One entry fence, bounded storage pages and current visibility on every open.
// A supplied ordinary number is explicit; absence starts the configured scan.
MessageReaderOutcome read_local_messages(const MessageReaderPorts&,const MessageQueryOptions&,
    bool headers_only=false,std::optional<std::int64_t> number={});
struct MessageScanSettingsPorts {
    MessageSelectionPorts selection;
    std::function<Identifier128()> operation;
    std::function<MessageEditorDimensions()> dimensions;
    std::function<StreamCallerTerminal::EditorByteResult()> byte;
    std::function<bool(std::string_view)> write;
    std::function<void(bool)> accepted_key;
    std::function<bool()> lightbars_enabled{};
};
MessageReaderOutcome edit_message_scan_settings(const MessageScanSettingsPorts&);
} // namespace sfx::application
