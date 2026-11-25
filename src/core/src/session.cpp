#include "sfx/core/session.hpp"

#include <algorithm>
#include <stdexcept>

namespace sfx::core {

Session::Session(std::string identity, const NodeNumber node, TransportMetadata transport)
    : identity_(std::move(identity)), node_(node), transport_(std::move(transport)) {
    if (identity_.empty()) {
        throw std::invalid_argument("session identity must not be empty");
    }
}

const std::string& Session::identity() const noexcept { return identity_; }
NodeNumber Session::node() const noexcept { return node_; }
const TransportMetadata& Session::transport() const noexcept { return transport_; }
SessionState Session::state() const noexcept { return state_; }
const std::optional<CallerContext>& Session::caller() const noexcept { return caller_; }
const ActivityContext& Session::activity() const noexcept { return activity_; }
std::uint32_t Session::session_limit_seconds() const noexcept { return session_limit_seconds_; }
std::uint32_t Session::elapsed_seconds() const noexcept { return elapsed_seconds_; }

std::uint32_t Session::remaining_seconds() const noexcept {
    return elapsed_seconds_ >= session_limit_seconds_ ? 0 : session_limit_seconds_ - elapsed_seconds_;
}

bool Session::authenticate(CallerContext caller) {
    if (state_ != SessionState::establishing || caller.caller_identity.empty() ||
        caller.account_id <= 0) {
        return false;
    }
    caller_ = std::move(caller);
    state_ = SessionState::authenticated;
    return true;
}

bool Session::set_activity(ActivityContext activity) {
    if (state_ != SessionState::authenticated) {
        return false;
    }
    activity_ = std::move(activity);
    return true;
}

void Session::update_elapsed(const std::uint32_t elapsed_seconds) noexcept { elapsed_seconds_ = elapsed_seconds; }
void Session::set_session_limit(const std::uint32_t seconds) noexcept { session_limit_seconds_ = seconds; }

bool Session::request_disconnect() noexcept {
    if (state_ == SessionState::disconnected || state_ == SessionState::disconnecting) {
        return false;
    }
    state_ = SessionState::disconnecting;
    return true;
}

bool Session::finalize_disconnect() noexcept {
    if (state_ != SessionState::disconnecting) {
        return false;
    }
    state_ = SessionState::disconnected;
    caller_.reset();
    activity_ = {};
    return true;
}

}  // namespace sfx::core
