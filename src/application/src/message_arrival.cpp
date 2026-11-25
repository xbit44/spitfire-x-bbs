#include "sfx/application/message_arrival.hpp"

namespace sfx::application {
std::string message_arrival_notice(std::string_view sender){
    std::string name;name.reserve(64);for(const unsigned char byte:sender){if(name.size()==64)break;name+=byte>=32&&byte<=126?static_cast<char>(byte):'?';}
    return name.empty()?"You have Mail":"You have Mail [from: "+name+"]";
}
bool MessageArrivalQueue::enqueue(const MessageArrival& arrival) noexcept {
    if(recipient_<=0 || arrival.recipient!=recipient_ || arrival.message.value<=0 || arrival.ordinal<=last_ordinal_)return false;
    // No unbounded dedup set: committed dispatch is monotonic. Recovery repeats
    // an existing identity at or below this session's accepted dispatch fence.
    if(pending_.size()==capacity){last_ordinal_=arrival.ordinal;coalesced_=true;return true;}
    try {pending_.push_back(arrival);}catch(...){return false;}
    last_ordinal_=arrival.ordinal;return true;
}
bool MessageArrivalQueue::present(bool safe,const std::function<bool(MessageId)>& eligible,
    const std::function<bool()>& unread,const std::function<bool(std::string_view)>& output){
    if(!eligible || !unread)return !safe;
    return present_checked(safe,[&](MessageId id){return std::optional<bool>{eligible(id)};},[&]{return std::optional<bool>{unread()};},output);
}
bool MessageArrivalQueue::present_checked(bool safe,const std::function<std::optional<bool>(MessageId)>& eligible,
    const std::function<std::optional<bool>()>& unread,const std::function<bool(std::string_view)>& output){
    if(!safe)return true;
    if(!eligible || !unread || !output)return false;
    while(!pending_.empty()){
        const auto allowed=eligible(pending_.front().message);if(!allowed)return false;
        if(!*allowed){pending_.pop_front();continue;}
        if(!output("You have a new message."))return false;
        pending_.pop_front();
    }
    if(coalesced_){const auto witness=unread();if(!witness)return false;if(*witness && !output("You have a new message."))return false;coalesced_=false;}
    return true;
}
} // namespace sfx::application
