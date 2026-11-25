#pragma once
#include "sfx/application/message_storage.hpp"
#include "sfx/application/terminal.hpp"
#include <functional>

namespace sfx::application {
struct MessageAreaSelection { MessageGroup group; LocalEcho echo; std::int64_t configuration_revision{}; };
struct MessageSelectionPorts {
    IMessageStorage& storage;
    ICallerTerminal& terminal;
    std::function<std::optional<MessageActor>()> actor;
    std::size_t page_size{18};
    std::function<unsigned()> columns{};
    std::function<unsigned()> rows{};
};
enum class MessageSelectionOutcome : std::uint8_t { selected, cancelled, failed };
struct MessageAreaSelectionResult { MessageSelectionOutcome outcome{MessageSelectionOutcome::cancelled}; std::optional<MessageAreaSelection> selection; };
MessageAreaSelectionResult select_local_message_echo(const MessageSelectionPorts&);
StorageResult<std::optional<MessageAreaSelection>> first_local_message_echo(IMessageStorage&,const MessageActor&);
// Cancelling a recipient confirmation never silently means All Callers.
struct MessageRecipientSelectionResult { MessageSelectionOutcome outcome{MessageSelectionOutcome::cancelled}; std::optional<MessageRecipient> recipient; };
MessageRecipientSelectionResult select_local_message_recipient(const MessageSelectionPorts&,bool complete_name=false);
} // namespace sfx::application
