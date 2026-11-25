#pragma once
#include "sfx/application/hostname_lookup.hpp"
#include <cstdint>
#include <string_view>

namespace sfx::apps::runtime {
// One process-wide coordinator. Calls below only exchange fixed mailbox values;
// no caller/cleanup/status path invokes a native resolver or waits for DNS.
std::uint64_t start_alpha_hostname_lookup(bool enabled) noexcept;
void stop_alpha_hostname_lookup(std::uint64_t lifetime) noexcept;
application::HostnameSubscription subscribe_alpha_hostname(std::uint64_t lifetime,
    application::NodeAssignmentIdentity assignment, std::string_view source) noexcept;
void unsubscribe_alpha_hostname(const application::HostnameSubscription& subscription) noexcept;
application::HostnameHint read_alpha_hostname(const application::HostnameSubscription& subscription) noexcept;
bool alpha_hostname_exhausted() noexcept;
bool take_alpha_hostname_exhaustion_warning() noexcept;
// Final process close only, never listener/node toggle. Joins the one coordinator.
void close_alpha_hostname_process() noexcept;
unsigned alpha_hostname_worker_count() noexcept;
#ifdef SFX_ALPHA_TEST_AUTOMATION
// Private engineering builds only; absent from the owner/shipping candidate.
void alpha_hostname_test_uncertain(bool enabled) noexcept;
struct HostnameTestStatistics { unsigned submissions{}, completions{}, ignored{}; };
HostnameTestStatistics alpha_hostname_test_statistics() noexcept;
#endif
} // namespace sfx::apps::runtime
