#pragma once

#include "sfx/application/time.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace sfx::application {

enum class RuntimeState : std::uint8_t { stopped, starting, online, draining, stopping, recovery_required };
enum class RuntimeOwnershipMode : std::uint8_t { desktop, service };
enum class ShutdownDisposition : std::uint8_t { now, graceful_drain };

struct ActiveWork {
    std::uint16_t caller_sessions{0};
    std::uint16_t supervised_processes{0};
    bool empty() const noexcept { return caller_sessions == 0 && supervised_processes == 0; }
};

struct ShutdownRequest {
    ShutdownDisposition disposition{ShutdownDisposition::now};
    ActiveWork active_work;
    Deadline drain_deadline;
};

class RuntimeLifecycle final {
public:
    RuntimeState state() const noexcept;
    bool accepts_new_callers() const noexcept;
    const std::optional<Deadline>& drain_deadline() const noexcept;

    bool request_start() noexcept;
    bool report_started() noexcept;
    bool request_shutdown(const ShutdownRequest& request, std::uint64_t now_millis) noexcept;
    bool observe_active_work(ActiveWork active_work, std::uint64_t now_millis) noexcept;
    bool cancel_shutdown() noexcept;
    bool report_stopped() noexcept;
    void report_failure() noexcept;
    bool acknowledge_recovery() noexcept;

private:
    RuntimeState state_{RuntimeState::stopped};
    std::optional<Deadline> drain_deadline_;
};

struct ControlProtocolVersion {
    std::uint16_t major{1};
    std::uint16_t minor{0};
};

enum class ControlAuthority : std::uint8_t { observe, operate_nodes, control_runtime, stop_service };
enum class AttachResult : std::uint8_t { attached, incompatible_version, unauthorized, unavailable };
enum class OwnershipResult : std::uint8_t { acquired, already_owned, stale_owner_recovery_required };

struct RuntimeStatus {
    RuntimeState state{RuntimeState::stopped};
    RuntimeOwnershipMode owner_mode{RuntimeOwnershipMode::desktop};
    std::uint16_t configured_nodes{0};
    ActiveWork active_work;
};

class IRuntimeOwnership {
public:
    virtual ~IRuntimeOwnership() = default;
    virtual OwnershipResult acquire(RuntimeOwnershipMode mode, std::string owner_identity) = 0;
    virtual void release(std::string owner_identity) noexcept = 0;
};

class ILocalControl {
public:
    virtual ~ILocalControl() = default;
    virtual AttachResult attach(ControlProtocolVersion version, ControlAuthority authority) = 0;
    virtual void detach() noexcept = 0;
    virtual RuntimeStatus observe_status() const = 0;
    virtual bool submit_shutdown(ShutdownRequest request) = 0;
    virtual bool cancel_shutdown() = 0;
};

}  // namespace sfx::application

