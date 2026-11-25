#pragma once

#include "sfx/core/identity.hpp"
#include "sfx/core/node.hpp"
#include "sfx/core/security.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace sfx::core {

enum class TransportKind : std::uint8_t { unknown, telnet, ssh, local };

struct TransportMetadata {
    TransportKind kind{TransportKind::unknown};
    std::string untrusted_peer_address;
    std::string untrusted_peer_name;
    std::string untrusted_terminal_type;
};

struct CallerContext {
    std::string caller_identity;
    std::string display_name;
    SecurityLevel security_level;
    AccountId account_id{0};
};

enum class ActivityKind : std::uint8_t { none, menu, message, file, door, chat };

struct ActivityContext {
    ActivityKind kind{ActivityKind::none};
    std::string identity;
};

enum class SessionState : std::uint8_t { establishing, authenticated, disconnecting, disconnected };

class Session final {
public:
    Session(std::string identity, NodeNumber node, TransportMetadata transport);

    const std::string& identity() const noexcept;
    NodeNumber node() const noexcept;
    const TransportMetadata& transport() const noexcept;
    SessionState state() const noexcept;
    const std::optional<CallerContext>& caller() const noexcept;
    const ActivityContext& activity() const noexcept;
    std::uint32_t session_limit_seconds() const noexcept;
    std::uint32_t elapsed_seconds() const noexcept;
    std::uint32_t remaining_seconds() const noexcept;

    bool authenticate(CallerContext caller);
    bool set_activity(ActivityContext activity);
    void update_elapsed(std::uint32_t elapsed_seconds) noexcept;
    void set_session_limit(std::uint32_t seconds) noexcept;
    bool request_disconnect() noexcept;
    bool finalize_disconnect() noexcept;

private:
    std::string identity_;
    NodeNumber node_;
    TransportMetadata transport_;
    SessionState state_{SessionState::establishing};
    std::optional<CallerContext> caller_;
    ActivityContext activity_;
    std::uint32_t session_limit_seconds_{0};
    std::uint32_t elapsed_seconds_{0};
};

}  // namespace sfx::core
