#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace sfx::core {

inline constexpr std::uint16_t kMaximumLogicalNodes = 128;

class NodeNumber final {
public:
    static std::optional<NodeNumber> create(std::uint16_t value) noexcept;
    std::uint16_t value() const noexcept;
    friend bool operator==(NodeNumber left, NodeNumber right) noexcept;

private:
    explicit NodeNumber(std::uint16_t value) noexcept;
    std::uint16_t value_;
};

enum class NodeState : std::uint8_t {
    offline,
    ready,
    connecting,
    online,
    door,
    transfer,
    chat,
    draining
};

class LogicalNode final {
public:
    explicit LogicalNode(NodeNumber number);

    NodeNumber number() const noexcept;
    NodeState state() const noexcept;
    const std::optional<std::string>& session_identity() const noexcept;
    bool transition(NodeState next) noexcept;
    bool bind_session(std::string session_identity);
    void clear_session() noexcept;

private:
    NodeNumber number_;
    NodeState state_{NodeState::offline};
    std::optional<std::string> session_identity_;
};

}  // namespace sfx::core

