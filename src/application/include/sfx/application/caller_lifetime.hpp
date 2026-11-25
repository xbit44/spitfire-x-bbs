#pragma once

#include "sfx/application/account_storage.hpp"
#include "sfx/application/time.hpp"
#include "sfx/application/caller_journal.hpp"

#include <atomic>
#include <memory>
#include <functional>

namespace sfx::application {

// Observations carry only reviewed stages, never prompt text or authentication input.
enum class CallerStage { transport, graphics, identity, authentication,
    registration, system_password, caller_information, main_menu, cleanup };
enum class CallerWorkflowObservation { registration_committed, duplicate_denied, durable_session_started };
class ICallerStageObserver {
public:
    virtual ~ICallerStageObserver() = default;
    virtual void stage(CallerStage value) noexcept = 0;
    // Outcome-only observations: never accept prompt text or credential input.
    virtual void observation(CallerWorkflowObservation, AccountId, Identifier128 = {}) noexcept {}
    // Narrow journal observations; claimed names are validated, never credentials
    // or raw input. They do not grant or update authenticated identity.
    virtual void attention(AttentionKind, AccountId, std::string_view = {}, std::uint16_t = 0) noexcept {}
    virtual SessionAccountingSnapshot accounting_snapshot() {return {};}
};

struct CallerCleanupHealth {
    std::atomic<bool> failed{false};
};

// One connection owns this guard, from BEFORE durable commit through the ready
// handoff. Its dependencies must outlive it. Destruction is a checked fallback,
// not a replacement for explicit finalization. A failed cleanup is sticky and
// must quarantine the assignment; it must never be advertised as READY.
class DurableSessionCleanup final {
public:
    DurableSessionCleanup(IAccountStorage& accounts, IClock& clock,
        Identifier128 session, Identifier128 audit, std::int64_t started,
        std::shared_ptr<CallerCleanupHealth> health = {},
        std::function<SessionAccountingSnapshot()> accounting = {});
    ~DurableSessionCleanup();
    DurableSessionCleanup(DurableSessionCleanup&& other) noexcept;
    DurableSessionCleanup& operator=(DurableSessionCleanup&& other) noexcept;
    DurableSessionCleanup(const DurableSessionCleanup&) = delete;
    DurableSessionCleanup& operator=(const DurableSessionCleanup&) = delete;
    void confirm_started() noexcept { confirmed_ = true; }
    void commit_failed(StorageErrorCode code) noexcept;
    bool finalize(std::int64_t seconds, SessionOutcomeCode outcome) noexcept;
private:
    IAccountStorage* accounts_;
    IClock* clock_;
    FinalizeSessionRequest request_;
    std::int64_t started_;
    std::shared_ptr<CallerCleanupHealth> health_;
    bool confirmed_{false};
    bool attempted_{false};
    bool succeeded_{false};
    std::function<SessionAccountingSnapshot()> accounting_;
};

} // namespace sfx::application
