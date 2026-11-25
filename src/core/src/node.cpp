#include "sfx/core/node.hpp"

namespace sfx::core {

namespace {

bool is_allowed(const NodeState current, const NodeState next) noexcept {
    if (current == next) {
        return true;
    }
    switch (current) {
    case NodeState::offline:
        return next == NodeState::ready;
    case NodeState::ready:
        return next == NodeState::offline || next == NodeState::connecting || next == NodeState::draining;
    case NodeState::connecting:
        return next == NodeState::online || next == NodeState::ready || next == NodeState::draining;
    case NodeState::online:
        return next == NodeState::ready || next == NodeState::door || next == NodeState::transfer ||
               next == NodeState::chat || next == NodeState::draining;
    case NodeState::door:
    case NodeState::transfer:
    case NodeState::chat:
        return next == NodeState::online || next == NodeState::ready || next == NodeState::draining;
    case NodeState::draining:
        return next == NodeState::ready || next == NodeState::offline;
    }
    return false;
}

}  // namespace

NodeNumber::NodeNumber(const std::uint16_t value) noexcept : value_(value) {}

std::optional<NodeNumber> NodeNumber::create(const std::uint16_t value) noexcept {
    if (value == 0 || value > kMaximumLogicalNodes) {
        return std::nullopt;
    }
    return NodeNumber(value);
}

std::uint16_t NodeNumber::value() const noexcept { return value_; }
bool operator==(const NodeNumber left, const NodeNumber right) noexcept { return left.value_ == right.value_; }

LogicalNode::LogicalNode(const NodeNumber number) : number_(number) {}
NodeNumber LogicalNode::number() const noexcept { return number_; }
NodeState LogicalNode::state() const noexcept { return state_; }
const std::optional<std::string>& LogicalNode::session_identity() const noexcept { return session_identity_; }

bool LogicalNode::transition(const NodeState next) noexcept {
    if (!is_allowed(state_, next)) {
        return false;
    }
    state_ = next;
    if (state_ == NodeState::offline || state_ == NodeState::ready) {
        clear_session();
    }
    return true;
}

bool LogicalNode::bind_session(std::string session_identity) {
    if (session_identity.empty() || session_identity_.has_value() || state_ != NodeState::connecting) {
        return false;
    }
    session_identity_ = std::move(session_identity);
    return true;
}

void LogicalNode::clear_session() noexcept { session_identity_.reset(); }

}  // namespace sfx::core

