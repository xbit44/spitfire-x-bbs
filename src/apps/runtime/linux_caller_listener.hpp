#pragma once
#include "sfx/application/caller_assignment.hpp"
#include "sfx/application/operational_configuration.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <functional>
#include <memory>

namespace sfx::apps::runtime {
struct LinuxConnectionAdmission {
    bool allowed{};
    bool source_limit_exempt{};
};
struct LinuxCallerListenerPorts {
    application::IClock& clock;
    // Runs before either Telnet login or SSH transport authentication. The
    // separate registration human challenge belongs to the session workflow.
    std::function<LinuxConnectionAdmission(std::string_view,bool,const std::atomic<bool>&)> admit;
    // Must return only after all session/child/transport resources are joined
    // and durable/account cleanup is complete. False quarantines the assignment.
    std::function<bool(int,bool,const std::shared_ptr<application::CallerAssignment>&)> serve;
    std::function<void(application::OperationalEvent)> observe{};
};
struct LinuxCallerListenerStatus {
    bool listening{};
    bool draining{};
    bool event_held{};
    std::uint16_t telnet_port{},ssh_port{};
    std::string diagnostic;
    application::NodePoolSnapshot pool;
    std::vector<application::AlphaNodeStatus> callers;
};
struct LinuxBrowserNodeStart {
    application::AlphaControlResponse response;
    application::NodeAssignmentIdentity identity;
    // Connected private transport. No TCP reservation address reaches a browser.
    platform::linux_host::FileDescriptor client;
};
class LinuxCallerListener final {
public:
    LinuxCallerListener(application::Identifier128 epoch,
        application::OperationalConfiguration configuration, LinuxCallerListenerPorts ports,
        application::NodeEnablement enabled=application::default_node_enablement(),
        std::array<std::uint64_t,application::kAlphaNodePoolCapacity> previous_generations={});
    ~LinuxCallerListener();
    LinuxCallerListener(const LinuxCallerListener&)=delete;
    LinuxCallerListener& operator=(const LinuxCallerListener&)=delete;
    bool start();
    void stop() noexcept;
    bool disconnect(application::NodeAssignmentIdentity);
    bool terminate_door(application::NodeAssignmentIdentity,std::uint64_t launch);
    application::AlphaLocalSessionStartResult reserve_local_node(std::uint16_t node);
    bool cancel_local_node(application::NodeAssignmentIdentity);
    LinuxBrowserNodeStart open_browser_node(std::uint16_t node);
    // Success means the exact worker joined and released its lease, not merely
    // that cancellation was requested. A later caller reusing the slot is not
    // waited on; foreign/future identities and cleanup quarantine refuse.
    bool wait_for_retirement(application::NodeAssignmentIdentity, std::uint32_t timeout_millis);
    bool set_enabled(std::uint16_t node,bool enabled);
    bool set_enabled(std::uint16_t,bool,std::optional<application::NodeAssignmentIdentity>,const std::function<void()>& publish);
    bool set_draining(bool draining);
    void set_external_event_hold(bool held);
    // Bind candidates before publishing. Existing callers keep their sockets;
    // failed binding/publication leaves the prior listeners authoritative.
    bool reconfigure_endpoints(const application::OperationalConfiguration&,
        const std::function<bool()>& publish);
    LinuxCallerListenerStatus status() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sfx::apps::runtime
