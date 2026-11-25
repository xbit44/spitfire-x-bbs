#include "sfx/application/chat_notice.hpp"
#pragma once

#include "alpha_operational_journal.hpp"

#include "sfx/adapters/authentication/sodium_authentication.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/authentication_service.hpp"
#include "sfx/application/operational_configuration.hpp"
#include "sfx/application/local_control_protocol.hpp"

#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace sfx::apps::runtime {

class AlphaSshBackend;
class AlphaDurableJournal;
class AlphaBotGateFeed;
class AlphaFtnEngine;

// Workers include the configured caller workers (default four, maximum 128), two bounded denial-only workers and
// one bounded one-use local-listener coordinator.
// Contexts count caller authority only; sockets include pending refusal sockets.
struct AlphaCallerResourceCounts { std::uint32_t workers, contexts, sockets; };
AlphaCallerResourceCounts alpha_caller_resource_counts() noexcept;

// Private deterministic boundary interlock. No caller input is exposed and no
// shipping configuration/control command installs one; production leaves empty.
enum class AlphaCallerBoundary { accepted, shell_ready, ready_handoff, menu, worker_start, before_assignment };
using AlphaCallerInterlock = std::function<void(AlphaCallerBoundary)>;

std::vector<sfx::application::AlphaOperationalLogEntry> read_alpha_operational_log(
    const std::filesystem::path& path) noexcept;

struct AlphaTelnetListenerStatus {
    bool listening{false};
    bool ssh_listening{false};
    std::uint16_t port{0};
    std::uint16_t active_connections{0};
    std::string diagnostic_code{"not_started"};
    std::vector<sfx::application::AlphaNodeStatus> nodes;
    std::vector<sfx::application::AlphaOperationalLogEntry> logs;
    std::uint8_t log_health{0};
    sfx::application::AlphaRuntimeStatistics statistics;
};

class AlphaTelnetListener final {
public:
    AlphaTelnetListener(
        adapters::storage::SqliteStorage& storage,
        application::AuthenticationSecurityService& authentication,
        application::IPasswordKdf& kdf,
        application::IIdentifierGenerator& identifiers,
        application::OperationalConfiguration configuration,
        application::AlphaRuntimeStatistics initial_statistics,
        std::filesystem::path installation_root,
        std::wstring installation_owner_sid,
        std::shared_ptr<const AlphaSshBackend> ssh_backend,
        AlphaCallerInterlock interlock = {},
        application::NodeEnablement enabled = application::default_node_enablement(),
        application::Identifier128 runtime_epoch = {},
        std::array<std::uint64_t, application::kAlphaNodePoolCapacity> previous_generations = {},
        std::shared_ptr<AlphaOperationalJournal> journal = {},
        std::shared_ptr<AlphaDurableJournal> durable_journal = {},
        std::shared_ptr<AlphaBotGateFeed> feed = {});
    ~AlphaTelnetListener();

    AlphaTelnetListener(const AlphaTelnetListener&) = delete;
    AlphaTelnetListener& operator=(const AlphaTelnetListener&) = delete;

    bool start();
    void attach_sysop_ftn(std::shared_ptr<AlphaFtnEngine>);
    application::ChatResult send_chat_notice(const application::ChatNoticeRequest&);
    application::ChatOperatorSnapshot chat_operator(const application::ChatOperatorRequest&);
    application::ChatConfigurationResult change_chat_configuration(const application::ChatConfiguration&);
    bool refresh_account_access(application::AccountId account_id);
    bool account_online(application::AccountId account_id) const;
    bool refresh_security_policy(const application::OperationalConfiguration& configuration);
    application::AlphaLocalSessionStartResult start_local_session(std::uint16_t node);
    application::AlphaControlResponse cancel_local_session(
        const application::AlphaLocalSessionCancelRequest& request);
    // Disable closes admission first, cancels only this assignment and waits for
    // its complete cleanup. Neither control recreates listeners or SSH identity.
    bool set_node_enabled(std::uint16_t node, bool enabled);
    application::AlphaControlResponse disable_observed_node(std::uint16_t node,
        std::uint64_t expected_generation, const std::function<application::StorageStatus()>& persist);
    application::AlphaControlResponse disconnect_caller(application::NodeAssignmentIdentity identity);
    application::AlphaControlResponse terminate_door(application::NodeAssignmentIdentity identity,std::uint64_t launch);
    // Linearizes against both public assignment and exact-node reservations.
    // Existing assignments continue until normal completion or explicit stop.
    bool set_draining(bool draining, std::uint64_t deadline = 0);
    void set_external_event_gate(bool held, std::int64_t cutoff_utc);
    bool external_event_callers_empty() const;
    void end_calls_for_external_event();
    // Records an authorization barrier before cancelling exact matching live
    // sessions. Delayed handoffs must cross the same barrier; success requires
    // cleanup of the captured live assignments within the finite wait.
    bool revoke_sessions(const application::SessionRevocationCommand& command);
    application::StorageStatus clear_throttle(const application::LocalThrottleClearRequest& request,
        const application::AuthenticationAuditContext& audit);
    static bool probe_bind(const application::TelnetOperationalConfiguration& configuration,
        std::string& diagnostic_code) noexcept;
    // Replaces one changed public acceptor while established caller workers
    // remain owned by this listener.  Ambiguous/multiple endpoint swaps refuse
    // before changing either working acceptor.
    bool reconfigure_listener_endpoints(
        const application::OperationalConfiguration& configuration,
        std::string& diagnostic_code) noexcept;
    // Applies settings used by newly accepted work while established caller
    // contexts retain their captured immutable configuration/time context.
    bool reconfigure_live_settings(
        const application::OperationalConfiguration& configuration,
        std::string& diagnostic_code) noexcept;
    void record_operational_log(application::OperationalKind kind,
        application::OperationalContext context = {}) noexcept;
    void stop() noexcept;
    AlphaTelnetListenerStatus status() const;
    void botgate_status(application::BotGateFeedStatus&) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sfx::apps::runtime
