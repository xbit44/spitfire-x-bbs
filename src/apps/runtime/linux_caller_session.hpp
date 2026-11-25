#pragma once
#include "alpha_ssh_stream.hpp"
#include "sfx/application/phase3a_private_adapters.hpp"
#include <atomic>
#include <functional>

namespace sfx::apps::runtime {
// One admitted native socket. The acceptor retains sole descriptor ownership
// and joins this call before closing it. All ports outlive the session.
struct LinuxCallerSessionPorts {
    application::Phase3aPrivateCompositionDependencies application;
    const application::Phase3aConfigurationSnapshot& configuration;
    core::Session& session;
    application::Phase3aExistingCallerRequest request;
    application::CancellationToken cancellation;
    const std::atomic<bool>& stopping;
    std::shared_ptr<const AlphaSshBackend> ssh_backend;
    application::StreamCallerTerminalOptions terminal_options;
    std::uint16_t login_idle_seconds{60};
    // The host supplies its actual admission gate, including any BotGate policy.
    // Missing admission/menu wiring refuses the session rather than bypassing it.
    std::function<bool(application::IInteractiveSessionByteStream&)> admission_gate;
    std::function<bool(application::AuthenticatedCallerReady&,
        application::IInteractiveSessionByteStream&)> menus;
    std::function<void(std::string_view)> output;
    std::function<void(bool)> payload_activity;
    std::function<void(SshTerminalDimensions)> resize;
    std::function<void()> encrypted;
    // Atomically publish against the host's pre-login revocation checkpoint.
    // Transport authentication alone cannot authorize a concurrently revoked caller.
    std::function<bool(const application::AuthenticatedCallerReady&)> authorize_ready;
};
struct LinuxCallerSessionResult {
    application::Phase3aWorkflowCode login{application::Phase3aWorkflowCode::invalid_context};
    bool caller_ready{};
    bool normal_logoff{};
    std::string diagnostic;
    // The host finalizes accounting before releasing this admission guard.
    // Keeping it in the result also retains failed finalization for recovery.
    std::optional<application::AuthenticatedCallerReady> ready;
};
LinuxCallerSessionResult run_linux_caller_session(int socket, bool ssh,
    LinuxCallerSessionPorts ports);
} // namespace sfx::apps::runtime
