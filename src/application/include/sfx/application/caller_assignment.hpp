#pragma once

#include "sfx/application/caller_lifetime.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/node_pool.hpp"
#include "sfx/application/menu.hpp"

#include <mutex>

namespace sfx::application {

enum class CallerDiagnostic { none, transport_failure, authentication_ended,
    workflow_failure, session_failure, cleanup_failure };

// Lifetime-stable, per-assignment publication and cancellation state. All
// callback publications validate the exact lease while holding the pool gate.
// Retained weak callbacks can neither keep a connection alive nor reach reuse.
class CallerAssignment final : public ICallerStageObserver {
public:
    CallerAssignment(NodeLease lease, FourNodePool& pool, IClock& clock,
        bool ssh, std::string source);
    void stage(CallerStage value) noexcept override;
    void output(std::string_view bytes);
    void payload_activity(bool received) noexcept;
    void encrypted_transport_established() noexcept;
    void transport_closed() noexcept;
    void resize(std::uint16_t columns, std::uint16_t rows);
    void authenticated(std::string display_name, AccountId account_id);
    void menu_entered(MenuId) noexcept;
    std::uint64_t begin_door(DoorId,std::string_view name,DoorExecutionKind=DoorExecutionKind::rlogin) noexcept;
    void end_door(std::uint64_t launch) noexcept;
    bool request_door_termination(std::uint64_t launch) noexcept;
    bool door_termination_requested(std::uint64_t launch) const noexcept;
    void hostname(const NodeAssignmentIdentity& identity, std::string_view source,
        std::string_view hint) noexcept;
    void diagnose(CallerDiagnostic value) noexcept;
    void cancel();
    // Invoke only AFTER transport joins, terminal erasure and checked session cleanup.
    bool retire(bool clean);
    AlphaNodeStatus snapshot() const;
    CallerDiagnostic diagnostic() const;
    NodeAssignmentIdentity identity() const noexcept { return identity_; }
    const CancellationToken cancellation;
    std::atomic<bool> stopping{false};
    const std::shared_ptr<CallerCleanupHealth> cleanup_health;
private:
    NodeLease lease_;
    const NodeAssignmentIdentity identity_;
    NodeCallbackGate callbacks_;
    FourNodePool& pool_;
    IClock& clock_;
    const std::uint64_t started_;
    mutable std::mutex mutex_;
    AlphaNodeStatus node_;
    ControlPanelTerminalMirror mirror_;
    CallerDiagnostic diagnostic_{CallerDiagnostic::none};
    bool retired_{false};
    std::uint64_t door_sequence_{},terminated_door_{};
};

} // namespace sfx::application
