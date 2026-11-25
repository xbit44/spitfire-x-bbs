#include "sfx/application/commands.hpp"

namespace sfx::application {

bool NativeCommandRegistry::register_native(NativeCommand command) {
    if (command.identity.empty() || !command.handler) {
        return false;
    }
    return commands_.emplace(command.identity, std::move(command)).second;
}

CommandResult NativeCommandRegistry::execute(
    const std::string_view identity,
    CommandContext& context) const {
    const auto found = commands_.find(std::string(identity));
    if (found == commands_.end()) {
        return {CommandStatus::unavailable, "command_not_registered"};
    }
    if (!context.session.caller().has_value()) {
        return {CommandStatus::unauthorized, "caller_context_required"};
    }
    if (!context.security.authorizes(
            context.session.caller()->security_level,
            found->second.security_rule)) {
        return {CommandStatus::unauthorized, "operation_not_authorized"};
    }
    return found->second.handler(context);
}

std::size_t NativeCommandRegistry::size() const noexcept { return commands_.size(); }

}  // namespace sfx::application

