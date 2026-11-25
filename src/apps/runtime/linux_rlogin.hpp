#pragma once
#include "sfx/application/telgate.hpp"
#include <filesystem>
namespace sfx::apps::runtime {
application::RloginBridgeResult run_linux_rlogin(const std::filesystem::path&,
    const application::RloginTarget&,const application::RloginCallerIdentity&,
    application::RloginSecret,application::RloginSecret,application::RloginBridgePorts,
    std::vector<std::uint8_t> caller_prefix={},bool discard_lf=false,std::function<void(bool)> sound={});
application::RloginBridgeResult run_linux_telnet(const std::filesystem::path&,
    const application::TelnetTarget&,application::RloginBridgePorts,const application::TelgateLocalPorts&,
    std::vector<std::uint8_t> caller_prefix={},bool discard_lf=false);
unsigned linux_gateway_live_sockets() noexcept;
std::vector<std::array<std::uint8_t,4>> resolve_linux_gateway_ipv4(const std::filesystem::path&,
    std::string_view,const std::function<bool()>&);
}
