#pragma once
#include "sfx/application/rlogin.hpp"

namespace sfx::application {
// A remote Telnet endpoint. Incoming caller Telnet has its own independent codec.
class TelgateCodec {
public:
    explicit TelgateCodec(std::string terminal);
    std::string begin();
    bool receive(std::string_view bytes, std::string& display, std::string& reply);
    std::string send(std::string_view bytes);
    std::string flush_cr();
    std::string dimensions(std::uint16_t rows, std::uint16_t columns);
    bool binary() const noexcept { return us_[0] || peer_[0]; }
    bool remote_echo() const noexcept { return peer_[1]; }
private:
    void negotiate(unsigned command, unsigned option, std::string& reply);
    void payload(unsigned char byte, std::string& display);
    std::string window() const;
    std::string terminal_, sub_;
    std::array<bool,256> us_{},peer_{},us_pending_{},peer_pending_{},us_refused_{},peer_refused_{};
    unsigned state_{}, command_{};
    bool incoming_cr_{},outgoing_cr_{};
    std::uint16_t rows_{25},columns_{80};
};
struct TelgateLocalPorts {
    bool ansi{};
    // These callbacks perform one bounded hub operation, never a terminal read.
    std::function<std::string()> callers;
    // Select a stable session identity before composing, never a bare reused node.
    std::function<bool(unsigned)> select_recipient;
    std::function<std::string(std::string_view)> private_text;
};
RloginBridgeResult run_telgate_bridge(const RloginBridgePorts&, const TelnetTarget&,
    const TelgateLocalPorts&, std::vector<std::uint8_t> prefix={}, bool discard_lf=false);
}
