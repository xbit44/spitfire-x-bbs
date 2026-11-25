#pragma once
#include "sfx/application/terminal.hpp"

namespace sfx::application {
// Empty Enter is a cancellation gesture here only. Authentication and new-
// password policy remain in the existing host action after a nonempty result.
inline TerminalInputResult read_password_change_current(ICallerTerminal& terminal,
                                                       std::size_t maximum) {
    for (unsigned empty = 0; empty != 2; ++empty) {
        auto result = terminal.read_secret({"menu.password.current", "Current password: ",
                                           maximum, EmptyInputPolicy::use_default});
        if (result.outcome != TerminalOutcome::empty &&
            !(result.outcome == TerminalOutcome::completed && result.value.empty()))
            return result;
        explicitly_erase_terminal_secret(result.value);
    }
    return {TerminalOutcome::cancelled, {}};
}
} // namespace sfx::application
