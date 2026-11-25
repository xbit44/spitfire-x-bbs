#pragma once
#include "sfx/application/message.hpp"
#include <deque>
#include <functional>

namespace sfx::application {
// Bounded inert display value; names cannot inject terminal control sequences.
std::string message_arrival_notice(std::string_view sender);
struct MessageArrival {
    MessageId message;
    AccountId recipient{};
    std::int64_t ordinal{};
};
// One session-owned queue, protected by its Runtime owner. It carries identities
// only, not Subjects/bodies. The dispatcher feeds committed arrivals in ordinal
// order, and new sessions start at the current durable ordinal (no toast backlog).
class MessageArrivalQueue final {
public:
    static constexpr std::size_t capacity=64;
    MessageArrivalQueue(AccountId recipient,std::int64_t entry_fence) noexcept
        :recipient_(recipient),last_ordinal_(entry_fence){}
    bool enqueue(const MessageArrival&) noexcept;
    // Returns false only on accepted output failure; retain pending work. Never
    // call at editor, password, Door, transfer or partial-input boundaries.
    bool present(bool safe_boundary,const std::function<bool(MessageId)>& eligible,
        const std::function<bool()>& addressed_unread,const std::function<bool(std::string_view)>& output);
    // Unknown eligibility is a storage failure, not a negative witness. Keep
    // that identity/coalesced work pending without producing a false notice.
    bool present_checked(bool safe_boundary,const std::function<std::optional<bool>(MessageId)>& eligible,
        const std::function<std::optional<bool>()>& addressed_unread,const std::function<bool(std::string_view)>& output);
    std::size_t pending() const noexcept {return pending_.size();}
    bool coalesced() const noexcept {return coalesced_;}
    std::int64_t dispatch_fence() const noexcept {return last_ordinal_;}
private:
    AccountId recipient_{};
    std::int64_t last_ordinal_{};
    std::deque<MessageArrival> pending_;
    bool coalesced_{};
};
} // namespace sfx::application
