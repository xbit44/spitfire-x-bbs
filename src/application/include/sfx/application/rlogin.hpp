#pragma once
#include "sfx/application/door.hpp"
#include "sfx/application/terminal_stream.hpp"

#include <array>
#include <functional>

namespace sfx::application {
void erase_rlogin_secret(void* memory,std::size_t bytes) noexcept;
class RloginSecret final {
public:
    RloginSecret() = default;
    explicit RloginSecret(std::string_view value);
    RloginSecret(const RloginSecret&) = delete;
    RloginSecret& operator=(const RloginSecret&) = delete;
    RloginSecret(RloginSecret&&) noexcept;
    RloginSecret& operator=(RloginSecret&&) noexcept;
    ~RloginSecret();
    std::string_view view() const noexcept;
    void clear() noexcept;
private:
    std::vector<char> bytes_;
};
struct RloginCallerIdentity {
    std::string alias;
    std::uint32_t account_number{}, firston_date{};
    std::string board_qwk_id;
};
using RloginSha1 = std::function<std::optional<std::array<std::uint8_t,20>>(std::string_view)>;
// The result includes the leading NUL and three NUL-terminated fields. It is
// sensitive, move-only, and erased after the handshake. No wire-frame logging.
std::optional<RloginSecret> rlogin_handshake(const RloginTarget&,const RloginCallerIdentity&,
    std::string_view credential,std::string_view pepper,const RloginSha1&);
std::array<std::uint8_t,12> rlogin_window_size(std::uint16_t rows,std::uint16_t columns) noexcept;

enum class RloginEnd : std::uint8_t {
    remote_closed, gateway_escape, operator_terminated, caller_disconnected,
    shutdown, authority_lost, calling_time, inactivity, connection_failed,
    handshake_failed, transport_failed, backpressure_timeout
};
const char* rlogin_result_code(RloginEnd) noexcept;
struct RloginBridgePorts {
    std::function<InteractiveStreamReadResult(std::size_t)> caller_read, remote_read;
    std::function<InteractiveStreamWriteResult(std::string_view)> caller_write, remote_write;
    std::function<std::optional<RloginEnd>()> stop;
    std::function<std::uint64_t()> now;
    std::function<void()> wait; // Bounded native wait, at most 20ms.
    std::function<void()> keyboard; // Caller payload only, never remote output.
    // Urgent control bytes are consumed separately from ordinary display data.
    std::function<std::optional<std::uint8_t>()> urgent;
    std::function<std::pair<std::uint16_t,std::uint16_t>()> dimensions;
    std::function<bool()> caller_discard; // Synchronize any native framing carry
                                        // before an urgent output flush.
};
struct RloginBridgeResult {
    RloginEnd end{RloginEnd::transport_failed};
    std::uint64_t caller_bytes{}, remote_bytes{};
    std::size_t outbound_high_water{}, inbound_high_water{};
};
inline constexpr std::size_t kRloginBridgeBufferBytes=65536;
// After successful acknowledgment only. All four I/O callbacks are ready-only;
// each direction owns at most 64KiB, and a blocked pending write has a hard 5s
// occupancy bound. Idle sessions are governed by caller time/AFK in stop().
RloginBridgeResult run_rlogin_bridge(const RloginBridgePorts&,
    std::vector<std::uint8_t> caller_prefix={},bool discard_delimiter_lf=false);
} // namespace sfx::application
