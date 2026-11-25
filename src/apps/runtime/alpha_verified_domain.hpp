#pragma once
#include <atomic>
#include <cstdint>
#include <string>
namespace sfx::apps::runtime {
struct VerifiedDomain { bool completed{}; std::string name; };
// Pre-admission only, outside listener locks. One fixed process coordinator,
// four native buffers, two-second total deadline, no detached resolver threads.
VerifiedDomain verify_peer_domain(std::uint32_t ipv4,const std::atomic<bool>& stop) noexcept;
}
