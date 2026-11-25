#include "sfx/application/events.hpp"

#include <stdexcept>

namespace sfx::application {

BoundedEventQueue::BoundedEventQueue(const std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("event queue capacity must be nonzero");
    }
}

bool BoundedEventQueue::publish(ApplicationEvent event) {
    if (events_.size() >= capacity_) {
        ++rejected_count_;
        return false;
    }
    event.sequence = next_sequence_++;
    events_.push_back(std::move(event));
    return true;
}

std::optional<ApplicationEvent> BoundedEventQueue::pop() {
    if (events_.empty()) {
        return std::nullopt;
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

std::size_t BoundedEventQueue::size() const noexcept { return events_.size(); }
std::size_t BoundedEventQueue::capacity() const noexcept { return capacity_; }
std::uint64_t BoundedEventQueue::rejected_count() const noexcept { return rejected_count_; }

}  // namespace sfx::application

