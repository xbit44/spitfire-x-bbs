#pragma once
#include "linux_runtime_store.hpp"
#include "linux_caller_listener.hpp"
#include "alpha_durable_journal.hpp"
#include "sfx/application/chat_notice.hpp"
#include "sfx/application/phase3a_workflow.hpp"

namespace sfx::apps::runtime {
class LinuxFtnEngine;
// Caller/background ownership, independent of management's process/browser
// lifetime. The store must remain open until stop has joined all workers.
class LinuxCallerEngine final {
public:
    LinuxCallerEngine(std::filesystem::path,LinuxRuntimeStore&,application::IClock&,
        application::Identifier128 epoch,std::shared_ptr<AlphaOperationalJournal>,
        std::shared_ptr<AlphaDurableJournal>,std::array<std::uint64_t,application::kAlphaNodePoolCapacity>,
        application::NodeEnablement enabled=application::default_node_enablement());
    ~LinuxCallerEngine();
    LinuxCallerEngine(const LinuxCallerEngine&)=delete;
    LinuxCallerEngine& operator=(const LinuxCallerEngine&)=delete;
    application::AlphaControlResponse start();
    void set_external_event_gate(bool held, std::int64_t cutoff_utc);
    void end_calls_for_external_event();
    bool stop() noexcept;
    bool set_draining(bool);
    bool revoke(const application::SessionRevocationCommand&);
    application::StorageStatus clear_throttle(const application::LocalThrottleClearRequest&,const application::AuthenticationAuditContext&);
    application::Phase3aRateGateResult web_authentication_admission(const application::Phase3aRateScopeSet&,std::int64_t now);
    application::StorageStatus web_authentication_result(const application::Phase3aRateScopeSet&,std::int64_t now,bool success);
    bool disconnect(application::NodeAssignmentIdentity);
    bool set_enabled(std::uint16_t,bool);
    bool set_enabled(std::uint16_t,bool,std::optional<application::NodeAssignmentIdentity>,const std::function<void()>& publish);
    bool terminate_door(application::NodeAssignmentIdentity,std::uint64_t);
    application::AlphaLocalSessionStartResult reserve_local_node(std::uint16_t);
    bool cancel_local_node(application::NodeAssignmentIdentity);
    LinuxBrowserNodeStart open_browser_node(std::uint16_t);
    bool account_online(application::AccountId)const;
    bool refresh_account_access(application::AccountId);
    bool refresh_security_policy(const application::OperationalConfiguration&);
    bool reconfigure(const application::OperationalConfiguration&,const std::function<bool()>& publish);
    application::ChatConfigurationResult change_chat_configuration(const application::ChatConfiguration&);
    application::ChatOperatorSnapshot chat_operator(const application::ChatOperatorRequest&);
    application::ChatResult send_chat_notice(const application::ChatNoticeRequest&);
    LinuxCallerListenerStatus status() const;
    application::BotGateFeedStatus botgate_status() const;
    bool update_botgate_feed();
    void attach_sysop_ftn(std::shared_ptr<LinuxFtnEngine>);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sfx::apps::runtime
