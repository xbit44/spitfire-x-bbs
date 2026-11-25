#pragma once

#include "sfx/application/presentation.hpp"
#include "sfx/core/security.hpp"
#include "sfx/core/session.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sfx::application {

enum class CommandStatus : std::uint8_t { completed, unavailable, unauthorized, failed };

struct CommandResult {
    CommandStatus status{CommandStatus::failed};
    std::string diagnostic_code;
};

struct CommandContext {
    core::Session& session;
    ISessionPresentation& presentation;
    const core::SecurityPolicy& security;
};

using NativeCommandHandler = std::function<CommandResult(CommandContext&)>;

struct NativeCommand {
    std::string identity;
    core::SecurityRule security_rule;
    NativeCommandHandler handler;
};

class NativeCommandRegistry final {
public:
    bool register_native(NativeCommand command);
    CommandResult execute(std::string_view identity, CommandContext& context) const;
    std::size_t size() const noexcept;

private:
    std::unordered_map<std::string, NativeCommand> commands_;
};

}  // namespace sfx::application

