#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace sfx::apps::runtime {
std::vector<std::array<std::uint8_t,4>> resolve_alpha_rlogin_ipv4(
    std::string_view,const std::function<bool()>& running);
void close_alpha_rlogin_process() noexcept;
unsigned alpha_rlogin_resolver_worker_count() noexcept;
unsigned alpha_rlogin_native_uncertain_requests() noexcept;
} // namespace sfx::apps::runtime
