#pragma once
#include "sfx/application/message_receive.hpp"
#include <algorithm>

namespace sfx::apps::runtime::message_receive_detail {
struct ReceiveDeadlines { std::uint64_t hard{},transport{}; };
// The receiver may retain a hard deadline, but must never retain a sliding
// inactivity deadline as a fixed transaction limit. Transport I/O still gets
// the current earlier idle limit on every poll; traffic does not extend AFK.
inline ReceiveDeadlines receive_deadlines(std::uint64_t hard,
    std::uint64_t binary_start,std::uint64_t last_inbound) noexcept {
    const auto total=binary_start+sfx::application::kMessageReceiveTotalMillis;
    hard=hard?std::min(hard,total):total;
    return {hard,std::min(hard,last_inbound+sfx::application::kMessageReceiveIdleMillis)};
}
}
