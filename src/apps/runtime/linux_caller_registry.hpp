#pragma once
#include "sfx/application/caller_assignment.hpp"
#include "sfx/application/session_revocation.hpp"
#include "sfx/application/chat.hpp"
#include <functional>
#include <mutex>

namespace sfx::apps::runtime {
// The owner registers an assignment BEFORE authentication begins and publishes
// only a completed shared login/SY result. Publication and revocation use the
// same gate. A delayed password check cannot escape a concurrent reset.
class LinuxCallerRegistry final {
public:
    struct Ports {
        // Must wait for the exact listener worker to join, not just its account
        // lease or registry entry to disappear. Called outside the registry lock.
        std::function<bool(application::NodeAssignmentIdentity,std::uint32_t)> retire;
        std::function<void()> stop_admission;
    };
    LinuxCallerRegistry(application::Identifier128 epoch,Ports);
    bool begin(const std::shared_ptr<application::CallerAssignment>&);
    bool publish(application::NodeAssignmentIdentity,application::AccountId,
        application::Identifier128 session,bool sysop);
    // Capture before authentication. Refresh may retain or reduce the initial
    // SY proof; raising authority after demotion requires a new login.
    std::uint64_t authorization_checkpoint() const;
    bool refresh_access(application::NodeAssignmentIdentity,application::AccountId,
        application::Identifier128 session,bool sysop,std::uint64_t checkpoint);
    bool account_online(application::AccountId) const;
    bool enforce_single_session(application::AccountId);
    bool refresh_account_authority(application::AccountId,bool active,bool sysop,bool parallel);
    std::vector<application::AccountId> published_accounts()const;
    std::optional<application::ChatIdentity> chat_identity(application::NodeAssignmentIdentity);
    bool sysop_authorized(const application::ChatIdentity&)const;
    bool sysop_disconnect(const application::ChatIdentity& actor,const application::ChatIdentity& target);
    // Worker calls this after checked durable/account cleanup. The separate
    // retire port still proves the actual thread/transport retirement.
    bool forget(application::NodeAssignmentIdentity,bool clean);
    bool revoke(const application::SessionRevocationCommand&);
    bool healthy() const;
private:
    struct Entry {
        std::shared_ptr<application::CallerAssignment> assignment;
        std::uint64_t checkpoint{};
        application::AccountId account{};
        application::Identifier128 session;
        bool sysop{};
        bool sysop_proof{};
    };
    Entry* find(application::NodeAssignmentIdentity);
    void quarantine();
    const application::Identifier128 epoch_;
    Ports ports_;
    mutable std::mutex mutex_;
    application::SessionRevocationLedger ledger_;
    std::array<Entry,application::kAlphaNodePoolCapacity> entries_{};
    std::array<std::uint64_t,application::kAlphaNodePoolCapacity> generations_{};
    bool healthy_{true};
};
} // namespace sfx::apps::runtime
