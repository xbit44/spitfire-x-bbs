#include "sfx/application/session_revocation.hpp"
#include <limits>

namespace sfx::application {
bool SessionRevocationLedger::matches(const SessionRevocationCommand& command,
    AccountId account, bool sysop, const Identifier128& session) noexcept {
    switch(command.kind) {
        case SessionRevocationKind::account_all:
            return command.account_id && *command.account_id == account;
        case SessionRevocationKind::account_except_current:
            return command.account_id && *command.account_id == account &&
                (!command.current_session_id || *command.current_session_id != session);
        case SessionRevocationKind::sysop_level_all: return sysop;
    }
    return true; // unknown command must not authorize a session
}

bool SessionRevocationLedger::record(const SessionRevocationCommand& command) noexcept {
    if (exhausted_) return false;
    switch(command.kind) {
        case SessionRevocationKind::account_all:
            if (!command.account_id || *command.account_id <= 0 || command.current_session_id) return false;
            break;
        case SessionRevocationKind::account_except_current:
            if (!command.account_id || *command.account_id <= 0 ||
                !command.current_session_id || !command.current_session_id->valid()) return false;
            break;
        case SessionRevocationKind::sysop_level_all:
            if (command.account_id || command.current_session_id) return false;
            break;
        default: return false;
    }
    if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        exhausted_ = true; return false;
    }
    ++sequence_;
    entries_[(sequence_ - 1U) % capacity] = {sequence_, command};
    return true;
}

bool SessionRevocationLedger::permits(std::uint64_t checkpoint, AccountId account,
    bool sysop, const Identifier128& session) const noexcept {
    if (exhausted_ || account <= 0 || !session.valid() || checkpoint > sequence_ ||
        sequence_ - checkpoint > capacity) return false;
    for (auto sequence = checkpoint; sequence < sequence_;) {
        ++sequence;
        const auto& entry = entries_[(sequence - 1U) % capacity];
        if (entry.sequence != sequence || matches(entry.command,account,sysop,session)) return false;
    }
    return true;
}
} // namespace sfx::application
