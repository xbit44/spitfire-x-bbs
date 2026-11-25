#pragma once
#include "sfx/application/message_editor.hpp"
#include "sfx/application/message_storage.hpp"
#include "sfx/application/message_receive.hpp"
#include "sfx/application/terminal_stream.hpp"
#include <functional>

namespace sfx::application {
struct MessageEditorDimensions { bool ansi{}; unsigned columns{80},rows{24}; };
struct MessageComposePorts {
    IMessageStorage& storage;
    ICallerTerminal& terminal;
    std::function<std::optional<MessageActor>()> actor;
    std::function<Identifier128()> operation;
    std::function<MessageEditorDimensions()> dimensions;
    std::function<StreamCallerTerminal::EditorByteResult()> byte;
    std::function<bool(std::string_view)> write;
    std::function<std::optional<std::pair<MessageTarget,std::string>>()> current_sysop{};
    std::function<void(bool)> accepted_key{};
    // Pre-editor refusal survives menu redraw; presentation only.
    std::function<bool(std::string_view)> unavailable_notice{};
    IMessageSourceReceiver* source_receiver{}; // session-bound; owner outlives compose
    std::function<std::uint8_t()> text_color{};
    bool real_name_editable{};
};
enum class MessageComposeOutcome : std::uint8_t { saved, cancelled, failed };
MessageComposeOutcome compose_local_message(const MessageComposePorts&,const MessageTarget&,
    std::string to,std::string from,std::optional<MessageId> parent={},std::string subject={});
} // namespace sfx::application
