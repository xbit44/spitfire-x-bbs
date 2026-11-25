#pragma once

#include "sfx/application/lifecycle.hpp"
#include "sfx/core/node.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace sfx::application {

enum class EventKind : std::uint8_t {
    runtime_state_changed,
    node_state_changed,
    safe_boundary_shutdown_warning,
    security_alert,
    operational_alert
};

struct ApplicationEvent {
    EventKind kind{EventKind::operational_alert};
    std::uint64_t sequence{0};
    std::optional<core::NodeNumber> node;
    std::string session_identity;
    std::string detail_code;
};

class IEventObserver {
public:
    virtual ~IEventObserver() = default;
    virtual void observe(const ApplicationEvent& event) = 0;
};

class BoundedEventQueue final {
public:
    explicit BoundedEventQueue(std::size_t capacity);
    bool publish(ApplicationEvent event);
    std::optional<ApplicationEvent> pop();
    std::size_t size() const noexcept;
    std::size_t capacity() const noexcept;
    std::uint64_t rejected_count() const noexcept;

private:
    std::size_t capacity_;
    std::uint64_t next_sequence_{1};
    std::uint64_t rejected_count_{0};
    std::deque<ApplicationEvent> events_;
};

}  // namespace sfx::application

