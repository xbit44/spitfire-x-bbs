#pragma once
#include "sfx/application/telgate.hpp"
#include <functional>

namespace sfx::apps::runtime {
// Native outbound socket only. Caller transport stays owned by its existing
// session thread. No process/helper, inbound listener, credential or raw-I/O log.
application::RloginBridgeResult run_alpha_rlogin(const application::RloginTarget&,
    const application::RloginCallerIdentity&,application::RloginSecret credential,application::RloginSecret pepper,
    application::RloginBridgePorts,std::vector<std::uint8_t> caller_prefix={},bool discard_lf=false,
    std::function<void(bool)> gateway_sound={});
application::RloginBridgeResult run_alpha_telnet(const application::TelnetTarget&,
    application::RloginBridgePorts,const application::TelgateLocalPorts&,
    std::vector<std::uint8_t> caller_prefix={},bool discard_lf=false);
unsigned alpha_rlogin_live_sockets() noexcept;
unsigned alpha_rlogin_resolver_unavailable_slots() noexcept;
void close_alpha_rlogin_process() noexcept;
unsigned alpha_rlogin_resolver_worker_count() noexcept;
} // namespace sfx::apps::runtime
