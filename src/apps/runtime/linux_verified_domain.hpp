#pragma once
#include "alpha_verified_domain.hpp"
#include <filesystem>
#include "sfx/application/hostname_lookup.hpp"
#include <functional>
namespace sfx::apps::runtime {
VerifiedDomain verify_linux_peer_domain(const std::filesystem::path& installation,std::uint32_t ipv4,const std::atomic<bool>& stop) noexcept;
// Display-only PTR result. This type is never BotGate domain authority.
std::optional<application::HostnameHint> lookup_linux_hostname(const std::filesystem::path&,std::uint32_t,const std::function<bool()>& cancelled) noexcept;
}
