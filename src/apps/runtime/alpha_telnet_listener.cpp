#include <winsock2.h>
#include <ws2tcpip.h>

#include "alpha_telnet_listener.hpp"
#include "alpha_telnet_stream.hpp"
#include "caller_configuration.hpp"
#include "sfx/application/password_change_prompt.hpp"
#include "sfx/platform/win32/door_bulletin_sources.hpp"
#include "caller_runtime_helpers.hpp"
#include "caller_runtime_accounting.hpp"
#include "sfx/application/external_events.hpp"
#include "alpha_ftn_engine.hpp"
#include "sysop_work.hpp"
#include "newsletter_file.hpp"
#include "sysop_log_reader.hpp"
#include "sysop_ftn_reader.hpp"
#include "sfx/application/sysop_session.hpp"
#include "sfx/platform/win32/file_administration.hpp"
#include "message_receive_deadline.hpp"
#include "alpha_hostname_lookup.hpp"
#include "alpha_rlogin.hpp"
#include "alpha_local_door.hpp"
#include "sfx/application/door_session.hpp"
#include "sfx/application/chat_session.hpp"
#include "sfx/application/caller_login_secret.hpp"
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include "alpha_verified_domain.hpp"
#include "alpha_ssh_stream.hpp"
#include "alpha_message_receive.hpp"
#include "alpha_qwk_transfer.hpp"
#include "alpha_caller_files.hpp"
#include "alpha_file_viewer.hpp"
#include "sfx/platform/win32/file_scanner.hpp"
#include "sfx/platform/win32/clamav_resources.hpp"
#include "sfx/platform/win32/clamav_installation.hpp"
#include "qwk_local_time.hpp"
#include "qwk_packet_notices.hpp"
#include "qwk_runtime_authority.hpp"
#include "qwk_preparation_activity.hpp"
#include "qwk_download_preparation.hpp"
#include "sfx/archive/qwk_packet_builder.hpp"
#include "sfx/application/qwk_session.hpp"
#include "alpha_durable_journal.hpp"
#include "alpha_botgate_feed.hpp"
#include "alpha_botgate_rules.hpp"
#include <ctime>
#include "../product_version.h"

#include "sfx/adapters/authentication/phase3a_authentication_adapters.hpp"
#include "sfx/adapters/storage/control_panel_statistics.hpp"
#include "sfx/application/phase3a_private_adapters.hpp"
#include "sfx/application/terminal_stream.hpp"
#include "sfx/application/caller_assignment.hpp"
#include "sfx/application/runtime_statistics.hpp"
#include "sfx/application/classic_information.hpp"
#include "sfx/application/admission_notices.hpp"
#include "sfx/application/caller_menu_notice.hpp"
#include "sfx/application/caller_inactivity.hpp"
#include "sfx/application/login_inactivity_stream.hpp"
#include "sfx/application/offline_notice.hpp"
#include "sfx/application/menu_session.hpp"
#include "sfx/application/message.hpp"
#include "sfx/application/message_session.hpp"
#include "sfx/application/message_presentation.hpp"
#include "sfx/application/session_revocation.hpp"
#include "sfx/core/session.hpp"
#include "runtime_journal_platform.hpp"
#include "sfx/application/caller_utilities.hpp"
#include "sfx/application/calling_time.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "alpha_trusted_displays.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <deque>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <functional>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sfx::apps::runtime {
namespace {

constexpr std::uint8_t kTelnetIac = 255;
constexpr std::uint8_t kTelnetDo = 253;
constexpr std::uint8_t kTelnetDont = 254;
constexpr std::uint8_t kTelnetWill = 251;
constexpr std::uint8_t kTelnetWont = 252;
constexpr std::uint8_t kTelnetSubnegotiation = 250;
constexpr std::uint8_t kTelnetSubnegotiationEnd = 240;
constexpr std::uint8_t kTelnetEcho = 1;
constexpr std::uint8_t kTelnetSuppressGoAhead = 3;
std::atomic<std::uint32_t> live_node_workers{0}, live_contexts{0}, live_owned_sockets{0};

// The accepted socket has exactly one close owner, including allocation failures.
// Cancellation only shuts it down under this lock; a recycled handle is never used.
class OwnedSocket final {
public:
    explicit OwnedSocket(SOCKET value = INVALID_SOCKET) noexcept : value_(value) {
        if (value_ != INVALID_SOCKET) ++live_owned_sockets;
    }
    OwnedSocket(OwnedSocket&& other) noexcept : value_(std::exchange(other.value_, INVALID_SOCKET)) {}
    OwnedSocket(const OwnedSocket&) = delete;
    ~OwnedSocket() { close(); }
    SOCKET get() const noexcept { return value_; } // owner-thread only
    void interrupt() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (value_ != INVALID_SOCKET) shutdown(value_, SD_BOTH);
    }
    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (value_ != INVALID_SOCKET) {
            shutdown(value_, SD_BOTH);
            closesocket(value_);
            --live_owned_sockets;
            value_ = INVALID_SOCKET;
        }
    }
private:
    SOCKET value_;
    std::mutex mutex_;
};

class SystemClock final : public application::IClock {
public:
    std::int64_t unix_seconds() const noexcept override {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::uint64_t monotonic_millis() const noexcept override {
        return GetTickCount64();
    }
};


bool send_raw(const SOCKET socket, const std::uint8_t* bytes, const std::size_t count) noexcept {
    std::size_t offset = 0;
    while (offset < count) {
        const int result = send(socket, reinterpret_cast<const char*>(bytes + offset),
            static_cast<int>(count - offset), 0);
        if (result == SOCKET_ERROR || result == 0) return false;
        offset += static_cast<std::size_t>(result);
    }
    return true;
}

bool write_all(application::IInteractiveSessionByteStream& stream, const std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto result = stream.write_some(text.substr(offset));
        if (result.outcome != application::InteractiveStreamOutcome::completed ||
            result.bytes_written == 0 || result.bytes_written > text.size() - offset) {
            return false;
        }
        offset += result.bytes_written;
    }
    return true;
}

std::optional<application::MenuInput> read_menu_input(application::IInteractiveSessionByteStream& stream,
    application::InteractiveStreamOutcome& last_outcome,
    application::CallerMenuNotice& notice, const std::atomic<std::uint64_t>& deadline,
    application::MenuInputDecoder& decoder, const std::function<bool()>& redraw,
    application::CallerInactivity& inactivity, const std::function<bool()>& refresh_access,
    const std::function<bool()>& menu_changed, const std::function<void()>& before_inactivity_disconnect,
    const std::function<bool()>& inactivity_warning,const std::function<bool()>& message_boundary={},
    const std::function<bool()>& accepted_keyboard={}) {
    for (;;) {
        if (!refresh_access()) return std::nullopt;
        const auto idle = inactivity.poll(GetTickCount64());
        if (idle == application::CallerInactivity::Action::disconnect) {
            before_inactivity_disconnect();
            last_outcome = application::InteractiveStreamOutcome::timed_out;
            return std::nullopt;
        }
        if (idle == application::CallerInactivity::Action::warn &&
            !inactivity_warning()) {
            last_outcome = application::InteractiveStreamOutcome::failed;
            return std::nullopt;
        }
        const auto change = notice.update(deadline.load(), GetTickCount64());
        if (change != application::CallerMenuNotice::Change::none) {
            const bool shown = redraw();
            if (!shown) {
                last_outcome = application::InteractiveStreamOutcome::failed;
                return std::nullopt;
            }
        }
        // Read one decoded key so Enter cannot consume or execute later input.
        auto input = stream.read_some(1);
        if (input.outcome == application::InteractiveStreamOutcome::timed_out) {
            const auto pending=decoder.idle();
            if(!notice.waiting() && pending.kind!=application::MenuInputKind::pending) return pending;
            if(!notice.waiting() && !decoder.selection_in_progress() && menu_changed())
                return application::MenuInput{application::MenuInputKind::redraw};
            if(!notice.waiting() && !decoder.selection_in_progress() && message_boundary && !message_boundary())return std::nullopt;
            continue;
        }
        last_outcome = input.outcome;
        if (input.outcome != application::InteractiveStreamOutcome::completed) return std::nullopt;
        if (!input.bytes.empty()) {
            inactivity.keyboard(GetTickCount64());
            // Keep this same key for the decoder, including partial commands.
            if(accepted_keyboard && accepted_keyboard() && !redraw()) {
                last_outcome=application::InteractiveStreamOutcome::failed;
                return std::nullopt;
            }
        }
        for (const std::uint8_t byte : input.bytes) {
            if (notice.waiting()) {
                if (notice.acknowledge(byte)) {
                    if (!redraw()) {
                        last_outcome = application::InteractiveStreamOutcome::failed;
                        return std::nullopt;
                    }
                }
                // This notice is an Enter-only overlay. Do not queue its keys as
                // menu commands for Enter to execute. Existing menu location
                // is unchanged; login/editor/secret input is never intercepted.
                continue;
            }
            const auto before=std::string(decoder.pending_text());
            const auto decoded=decoder.feed(byte);
            const auto after=std::string(decoder.pending_text());
            const auto echo=application::menu_input_echo(byte,before,decoded,after);
            if(!echo.empty()&&!write_all(stream,echo))return std::nullopt;
            if(decoded.kind!=application::MenuInputKind::pending) {
                if (!refresh_access()) return std::nullopt;
                return decoded;
            }
        }
    }
}

// Reuses the authenticated lifetime/AFK guard for bounded artwork paging and
// secret prompts. Notice overlays stay at ordinary menus, never inside secrets.


std::string peer_ipv4(const sockaddr_in& peer) {
    std::array<char, INET_ADDRSTRLEN> text{};
    return InetNtopA(AF_INET, const_cast<IN_ADDR*>(&peer.sin_addr),
        text.data(), static_cast<DWORD>(text.size())) != nullptr
        ? std::string(text.data()) : std::string{};
}

application::SessionOutcomeCode outcome_for_socket(
    const bool normal, const bool stopping) noexcept {
    if (normal) return application::SessionOutcomeCode::normal;
    return stopping ? application::SessionOutcomeCode::shutdown
                    : application::SessionOutcomeCode::disconnected;
}



application::OperationalOutcome workflow_reason(const application::Phase3aWorkflowCode code) {
    switch (code) {
        case application::Phase3aWorkflowCode::caller_goodbye: return application::OperationalOutcome::normal;
        case application::Phase3aWorkflowCode::cancelled: return application::OperationalOutcome::cancelled;
        case application::Phase3aWorkflowCode::timed_out: return application::OperationalOutcome::timed_out;
        case application::Phase3aWorkflowCode::disconnected: return application::OperationalOutcome::disconnected;
        case application::Phase3aWorkflowCode::shutdown_requested: return application::OperationalOutcome::shutdown;
        case application::Phase3aWorkflowCode::kdf_backpressure: return application::OperationalOutcome::unavailable;
        case application::Phase3aWorkflowCode::storage_failure: return application::OperationalOutcome::storage_failure;
        case application::Phase3aWorkflowCode::temporarily_unavailable:
            return application::OperationalOutcome::unavailable;
        case application::Phase3aWorkflowCode::terminal_failure: return application::OperationalOutcome::terminal_failure;
        case application::Phase3aWorkflowCode::cleanup_failure: return application::OperationalOutcome::internal_failure;
        case application::Phase3aWorkflowCode::bootstrap_incomplete:
        case application::Phase3aWorkflowCode::invalid_context:
        case application::Phase3aWorkflowCode::internal_failure: return application::OperationalOutcome::internal_failure;
        case application::Phase3aWorkflowCode::registration_requested:
        case application::Phase3aWorkflowCode::registration_unavailable:
            return application::OperationalOutcome::registration_closed;
        case application::Phase3aWorkflowCode::authentication_denied:
            return application::OperationalOutcome::denied;
        case application::Phase3aWorkflowCode::authenticated_caller_ready:
            return application::OperationalOutcome::none;
    }
    return application::OperationalOutcome::internal_failure;
}

}  // namespace

std::vector<application::AlphaOperationalLogEntry> read_alpha_operational_log(
    const std::filesystem::path& path) noexcept {
    std::uint8_t health = 0;
    return AlphaOperationalJournal::read(path, health);
}

class AlphaTelnetListener::Impl final
    : public application::ICallerWelcomeStatisticsPort, public application::ISessionRevocationSink {
public:
    struct TimeContext final {
        std::unique_ptr<NativeBbsTimeZoneRuleProvider> zone;
        std::unique_ptr<application::BbsLocalTimeAdapters> local;
    };

    std::shared_ptr<TimeContext> make_time_context(
        const std::string_view configured_zone) {
        auto result = std::make_shared<TimeContext>();
        result->zone = NativeBbsTimeZoneRuleProvider::create(
            configured_zone);
        if (!result->zone) return {};
        result->local = std::make_unique<application::BbsLocalTimeAdapters>(
            result->zone->configuration(), *result->zone, clock);
        return result;
    }

    std::shared_ptr<const application::Phase3aConfigurationSnapshot>
    make_phase3a_configuration(
        const application::OperationalConfiguration& candidate) {
        return make_caller_configuration(storage, candidate, sysop_display_name(),
            SFX_PRODUCT_RUNTIME_VERSION);
    }

    struct Connection;
    struct LocalReservation final {
        application::NodeLease lease;
        OwnedSocket listener;
        std::uint64_t expires_at{0};
        std::uint64_t attempt{0};
        std::uint16_t port{0};

        LocalReservation(application::NodeLease value, OwnedSocket socket,
            const std::uint64_t expiry, const std::uint64_t attempt_value,
            const std::uint16_t port_value)
            : lease(std::move(value)), listener(std::move(socket)),
              expires_at(expiry), attempt(attempt_value), port(port_value) {}
    };
    Impl(adapters::storage::SqliteStorage& storage_value,
        application::AuthenticationSecurityService& authentication_value,
        application::IPasswordKdf& kdf_value,
        application::IIdentifierGenerator& identifiers_value,
        application::OperationalConfiguration configuration_value,
        application::AlphaRuntimeStatistics initial_statistics,
        std::filesystem::path installation_root_value,
        std::wstring installation_owner_sid_value,
        std::shared_ptr<const AlphaSshBackend> ssh_backend_value, AlphaCallerInterlock interlock,
        const application::NodeEnablement& enabled, application::Identifier128 runtime_epoch,
        const std::array<std::uint64_t, application::kAlphaNodePoolCapacity>& previous_generations,
        std::shared_ptr<AlphaOperationalJournal> journal_value,
        std::shared_ptr<AlphaDurableJournal> durable_value,
        std::shared_ptr<AlphaBotGateFeed> feed_value)
        : storage(storage_value), authentication(authentication_value), kdf(kdf_value),
          identifiers(identifiers_value), configuration(std::move(configuration_value)),
          statistics(std::move(initial_statistics)),
          database_path(installation_root_value / L"Data" / L"SFX.DB"),
          installation_owner_sid(std::move(installation_owner_sid_value)),
          ssh_backend(std::move(ssh_backend_value)),
          admission(configuration.logical_nodes), caller_interlock(std::move(interlock)),
          pool(runtime_epoch.valid() ? runtime_epoch : identifiers.generate(), configuration.logical_nodes,
              configuration.maximum_connections_per_source, UINT64_MAX, previous_generations,
              configuration.source_connection_exceptions) {
        feed=std::move(feed_value);
        rules=std::make_shared<AlphaBotGateRules>(installation_root_value,installation_owner_sid,configuration.botgate,
            [weak=std::weak_ptr<AlphaOperationalJournal>(journal)]{if(const auto j=weak.lock())j->sound(application::SoundEvent::security_alert);});
        for (std::uint16_t index = 0; index < configuration.logical_nodes; ++index) {
            auto& node = nodes[index];
            node.number = index + 1;
            node.assignment_sequence = previous_generations[index];
            node.available = true;
            node.enabled = enabled[index];
            node.state = enabled[index] ? core::NodeState::ready : core::NodeState::offline;
            node.activity = enabled[index] ? "Idle" : "Disabled";
            node.details = enabled[index] ? "Ready for connection" : "Disabled by Sysop";
            (void)pool.set_enabled(index + 1, enabled[index]);
        }
        journal = journal_value ? std::move(journal_value) : std::make_shared<AlphaOperationalJournal>(
            installation_root_value / L"Logs" / L"SFXRuntime.log", installation_owner_sid,
            pool.snapshot().slots[0].identity.runtime_epoch);
        owns_durable = !durable_value;
        durable = durable_value ? std::move(durable_value) : std::make_shared<AlphaDurableJournal>(
            storage, identifiers, installation_root_value, installation_owner_sid, configuration.bbs_time_zone, journal);
        (void)journal->configure(configuration.logging);
        (void)durable->configure_logging(configuration.logging);
    }

    // Serialize the complete read-and-publication operation. Never increment a
    // cache independently of committed rows; no pool/status lock spans database I/O.
    application::StorageResult<std::uint64_t>
    record_successful_call_and_count_today() override {
        return statistics.refresh([this] {
            std::shared_ptr<TimeContext> captured;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                captured = time_context;
            }
            const auto range = captured && captured->local
                ? bbs_today_range(*captured->local, clock.unix_seconds())
                : std::nullopt;
            if (!range) return application::StorageResult<application::AlphaRuntimeStatistics>{
                {application::StorageErrorCode::io, "runtime local date unavailable"}, {}};
            return adapters::storage::ControlPanelStatisticsReader::read_checked(
                database_path, (*range)[0], (*range)[1]);
        });
    }

    bool refresh_statistics_after_session() noexcept {
        try {
            return static_cast<bool>(record_successful_call_and_count_today());
        } catch (...) { return false; }
    }

    void add_log(application::OperationalKind kind, application::OperationalContext context = {},
        application::OperationalOutcome outcome = application::OperationalOutcome::none) noexcept {
        journal->emit({kind, outcome, std::move(context)});
    }

    bool initialize_dependencies() {
        application::CancellationSource cancellation;
        credential_bridge = adapters::authentication::Phase3aCredentialOperationsBridge::create(
            authentication, kdf, clock.unix_seconds(), cancellation.token());
        if (!credential_bridge) {
            set_diagnostic("credential_bridge_unavailable");
            return false;
        }
        constexpr std::size_t maximum_rate_subjects = 4095;
        const auto stored_rates = storage.active_rate_limit_snapshots(
            clock.unix_seconds(), maximum_rate_subjects + 1U);
        if (!stored_rates || !stored_rates.value) {
            set_diagnostic("rate_storage_" + stored_rates.status.operation);
            return false;
        }
        rates = std::make_unique<adapters::authentication::Phase3aRateControlCoordinator>(
            storage, clock.unix_seconds(), maximum_rate_subjects,
            [weak=std::weak_ptr<AlphaOperationalJournal>(journal)]{if(const auto j=weak.lock())j->sound(application::SoundEvent::security_alert);});
        if (!rates->ready()) {
            set_diagnostic("rate_control_unavailable");
            return false;
        }
        time_context = make_time_context(configuration.bbs_time_zone);
        if (!time_context || !time_context->local) {
            set_diagnostic("bbs_time_zone_unavailable");
            return false;
        }
        const auto local_now = time_context->local->format_utc_seconds(
            clock.unix_seconds());
        if (!local_now || local_now->size() < 10U) {
            set_diagnostic("bbs_local_date_unavailable");
            return false;
        }
        if (!refresh_statistics_after_session()) {
            set_diagnostic("statistics_unavailable");
            return false;
        }

        phase3a_configuration = make_phase3a_configuration(configuration);
        if (!phase3a_configuration) {
            set_diagnostic("phase3a_configuration_invalid");
            return false;
        }
        return true;
    }

    std::string sysop_display_name() {
        const auto assignment=storage.message_configuration({true,{}},{},{});
        const auto policy=storage.security_policy();
        if(assignment && assignment.value && assignment.value->system_sysop && policy && policy.value){
            const auto account = storage.find_account_by_id(
                *assignment.value->system_sysop);
            if (account && account.value && account.value->state==application::AccountState::active &&
                account.value->security_level && *account.value->security_level>=policy.value->sysop_threshold && account.value->display_name) {
                return *account.value->display_name;
            }
        }
        return "Not configured";
    }

    void set_diagnostic(std::string value) {
        std::lock_guard<std::mutex> lock(state_mutex);
        diagnostic_code = std::move(value);
    }

    bool start_socket(const bool ssh = false) {
        const std::string protocol = ssh ? "SSH" : "Telnet";
        const std::string prefix = ssh ? "ssh_" : "telnet_";
        const auto port = ssh ? configuration.ssh.port : configuration.telnet.port;
        const auto& bind_address = ssh ? configuration.ssh.bind_address : configuration.telnet.bind_address;
        auto& listener_socket = ssh ? ssh_listen_socket : listen_socket;
        auto& listener_active = ssh ? ssh_listening : listening;
        auto& listener_worker = ssh ? ssh_worker : worker;
        auto& listener_cancellation = ssh ? ssh_acceptor_cancellation
                                          : acceptor_cancellation;
        WSADATA data{};
        if (!winsock_started && WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            set_diagnostic("winsock_startup_failed");
            return false;
        }
        winsock_started = true;
        SOCKET candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (candidate == INVALID_SOCKET) {
            set_diagnostic(prefix + "socket_creation_failed");
            return false;
        }
        if (ssh && !AlphaSshStream::prepare_listener_socket(static_cast<std::uintptr_t>(candidate))) {
            closesocket(candidate);
            set_diagnostic("ssh_receive_backpressure_policy_failed");
            return false;
        }
        BOOL exclusive = TRUE;
        if (setsockopt(candidate, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
            closesocket(candidate);
            set_diagnostic(prefix + "exclusive_bind_policy_failed");
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (InetPtonA(AF_INET, bind_address.c_str(),
                &address.sin_addr) != 1) {
            closesocket(candidate);
            set_diagnostic(prefix + "bind_address_invalid");
            return false;
        }
        if (bind(candidate, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
                SOCKET_ERROR ||
            listen(candidate, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(candidate);
            set_diagnostic(prefix + "bind_or_listen_failed");
            return false;
        }
        u_long nonblocking = 1;
        if (ioctlsocket(candidate, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            closesocket(candidate);
            set_diagnostic(prefix + "nonblocking_accept_failed");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            listener_socket = candidate;
        }
        listener_active.store(true);
        set_diagnostic("listening");
        auto cancellation = std::make_shared<std::atomic<bool>>(false);
        try {
            listener_cancellation = cancellation;
            listener_worker = std::thread([this, candidate, ssh, cancellation] {
                accept_loop(candidate, ssh, cancellation);
            });
        } catch (...) {
            listener_active.store(false);
            {
                std::lock_guard<std::mutex> lock(socket_mutex);
                if (listener_socket == candidate) {
                    closesocket(listener_socket);
                    listener_socket = INVALID_SOCKET;
                    listener_cancellation.reset();
                }
            }
            set_diagnostic(prefix + "worker_start_failed");
            return false;
        }
        application::OperationalContext context;
        context.transport = ssh ? application::OperationalTransport::ssh : application::OperationalTransport::telnet;
        context.source_ip = bind_address; context.local_port = port;
        add_log(application::OperationalKind::listener_ready, std::move(context));
        return true;
    }

    SOCKET prepare_replacement_socket(const bool ssh,
        const application::OperationalConfiguration& candidate,
        std::string& diagnostic) noexcept {
        const auto& bind_address = ssh ? candidate.ssh.bind_address
                                       : candidate.telnet.bind_address;
        const auto port = ssh ? candidate.ssh.port : candidate.telnet.port;
        const std::string prefix = ssh ? "ssh_" : "telnet_";
        SOCKET prepared = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (prepared == INVALID_SOCKET) {
            diagnostic = prefix + "socket_creation_failed";
            return INVALID_SOCKET;
        }
        if (ssh && !AlphaSshStream::prepare_listener_socket(static_cast<std::uintptr_t>(prepared))) {
            closesocket(prepared);
            diagnostic = "ssh_receive_backpressure_policy_failed_prior_endpoint_retained";
            return INVALID_SOCKET;
        }
        BOOL exclusive = TRUE;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (setsockopt(prepared, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR ||
            InetPtonA(AF_INET, bind_address.c_str(), &address.sin_addr) != 1 ||
            bind(prepared, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(prepared, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(prepared);
            diagnostic = prefix + "bind_or_listen_failed_prior_endpoint_retained";
            return INVALID_SOCKET;
        }
        u_long nonblocking = 1;
        if (ioctlsocket(prepared, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            closesocket(prepared);
            diagnostic = prefix + "nonblocking_accept_failed_prior_endpoint_retained";
            return INVALID_SOCKET;
        }
        return prepared;
    }

    bool replace_acceptor(const bool ssh, const SOCKET prepared,
        const bool enabled, std::string& diagnostic) noexcept {
        auto& listener_socket = ssh ? ssh_listen_socket : listen_socket;
        auto& listener_active = ssh ? ssh_listening : listening;
        auto& listener_worker = ssh ? ssh_worker : worker;
        auto& listener_cancellation = ssh ? ssh_acceptor_cancellation
                                          : acceptor_cancellation;
        SOCKET prior_socket = INVALID_SOCKET;
        std::shared_ptr<std::atomic<bool>> prior_cancellation;
        auto replacement_cancellation = enabled
            ? std::make_shared<std::atomic<bool>>(false)
            : std::shared_ptr<std::atomic<bool>>{};
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            prior_socket = listener_socket;
            prior_cancellation = listener_cancellation;
            listener_socket = enabled ? prepared : INVALID_SOCKET;
            listener_cancellation = replacement_cancellation;
        }
        const bool prior_active = listener_active.exchange(enabled);
        std::thread replacement;
        if (enabled) {
            try {
                replacement = std::thread([this, prepared, ssh,
                                              replacement_cancellation] {
                    accept_loop(prepared, ssh, replacement_cancellation);
                });
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(socket_mutex);
                    listener_socket = prior_socket;
                    listener_cancellation = prior_cancellation;
                }
                listener_active.store(prior_active);
                closesocket(prepared);
                diagnostic = ssh ? "ssh_worker_start_failed_prior_endpoint_retained"
                                 : "telnet_worker_start_failed_prior_endpoint_retained";
                return false;
            }
        }
        std::thread prior_worker = std::move(listener_worker);
        listener_worker = std::move(replacement);
        if (prior_cancellation) prior_cancellation->store(true);
        if (prior_socket != INVALID_SOCKET) shutdown(prior_socket, SD_BOTH);
        if (prior_worker.joinable()) prior_worker.join();
        if (prior_socket != INVALID_SOCKET) closesocket(prior_socket);
        diagnostic = enabled ? "listener_endpoint_applied" : "listener_disabled_applied";
        return true;
    }

    bool reconfigure_listener_endpoints(
        const application::OperationalConfiguration& candidate,
        std::string& diagnostic) noexcept {
        const bool telnet_changed = configuration.telnet.enabled != candidate.telnet.enabled ||
            configuration.telnet.bind_address != candidate.telnet.bind_address ||
            configuration.telnet.port != candidate.telnet.port;
        const bool ssh_changed = configuration.ssh.enabled != candidate.ssh.enabled ||
            configuration.ssh.bind_address != candidate.ssh.bind_address ||
            configuration.ssh.port != candidate.ssh.port;
        if (!telnet_changed && !ssh_changed) {
            diagnostic = "listener_endpoints_unchanged";
            return true;
        }
        if (telnet_changed && ssh_changed) {
            diagnostic = "multiple_listener_swap_requires_zero_active_callers";
            return false;
        }
        const bool ssh = ssh_changed;
        const bool enabled = ssh ? candidate.ssh.enabled : candidate.telnet.enabled;
        SOCKET prepared = INVALID_SOCKET;
        if (enabled) {
            prepared = prepare_replacement_socket(ssh, candidate, diagnostic);
            if (prepared == INVALID_SOCKET) return false;
        }
        if (!replace_acceptor(ssh, prepared, enabled, diagnostic)) return false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (ssh) configuration.ssh = candidate.ssh;
            else configuration.telnet = candidate.telnet;
        }
        application::OperationalContext context;
        context.transport = ssh ? application::OperationalTransport::ssh
                                : application::OperationalTransport::telnet;
        context.source_ip = ssh ? candidate.ssh.bind_address
                                : candidate.telnet.bind_address;
        context.local_port = ssh ? candidate.ssh.port : candidate.telnet.port;
        add_log(application::OperationalKind::listener_ready, std::move(context));
        return true;
    }

    bool reconfigure_live_settings(
        const application::OperationalConfiguration& candidate,
        std::string& diagnostic) noexcept {
        try {
            if (candidate.logical_nodes != configuration.logical_nodes ||
                candidate.password_mode != configuration.password_mode ||
                candidate.maximum_connections_per_source != configuration.maximum_connections_per_source ||
                candidate.maximum_sessions_per_account != configuration.maximum_sessions_per_account ||
                candidate.source_connection_exceptions != configuration.source_connection_exceptions ||
                candidate.telnet.enabled != configuration.telnet.enabled ||
                candidate.telnet.bind_address != configuration.telnet.bind_address ||
                candidate.telnet.port != configuration.telnet.port ||
                candidate.ssh.enabled != configuration.ssh.enabled ||
                candidate.ssh.bind_address != configuration.ssh.bind_address ||
                candidate.ssh.port != configuration.ssh.port) {
                diagnostic = "live_settings_scope_invalid";
                return false;
            }
            auto replacement_time = make_time_context(candidate.bbs_time_zone);
            auto replacement_configuration = make_phase3a_configuration(candidate);
            if (!replacement_time || !replacement_time->local ||
                !replacement_configuration) {
                diagnostic = "live_settings_dependencies_invalid";
                return false;
            }
            std::lock_guard<std::mutex> lock(state_mutex);
            const bool hostname_changed =
                configuration.hostname_lookup != candidate.hostname_lookup;
            if (hostname_changed && std::any_of(active_contexts.begin(),
                    active_contexts.end(), [](const auto& value) {
                        return static_cast<bool>(value);
                    })) {
                diagnostic = "hostname_lookup_waits_for_active_connections";
                return false;
            }
            const bool time_zone_changed =
                configuration.bbs_time_zone != candidate.bbs_time_zone;
            if (time_zone_changed &&
                (!durable || !durable->reconfigure_time_zone(candidate.bbs_time_zone))) {
                diagnostic = "durable_journal_time_zone_reconfiguration_failed";
                return false;
            }
            if (hostname_changed) {
                const auto replacement_lifetime =
                    start_alpha_hostname_lookup(candidate.hostname_lookup);
                if (candidate.hostname_lookup && replacement_lifetime == 0) {
                    if (time_zone_changed)
                        (void)durable->reconfigure_time_zone(configuration.bbs_time_zone);
                    diagnostic = "hostname_lookup_reconfiguration_failed";
                    return false;
                }
                hostname_lifetime = replacement_lifetime;
            }
            configuration.bbs_name = candidate.bbs_name;
            configuration.bbs_time_zone = candidate.bbs_time_zone;
            configuration.allow_new_users = candidate.allow_new_users;
            configuration.hostname_lookup = candidate.hostname_lookup;
            configuration.botgate = candidate.botgate;
            configuration.telnet.bot_gate_enabled = candidate.telnet.bot_gate_enabled;
            configuration.logging = candidate.logging;
            configuration.menus = candidate.menus;
            (void)journal->configure(candidate.logging);
            (void)durable->configure_logging(candidate.logging);
            configuration.caller_afk_minutes = candidate.caller_afk_minutes;
            configuration.login_afk_seconds = candidate.login_afk_seconds;
            configuration.configuration_revision = candidate.configuration_revision;
            time_context = std::move(replacement_time);
            phase3a_configuration = std::move(replacement_configuration);
            diagnostic = "live_settings_applied";
            return true;
        } catch (...) {
            diagnostic = "live_settings_reconfiguration_failed";
            return false;
        }
    }

    struct Connection final : application::ICallerStageObserver {
        Impl& owner;
        OwnedSocket socket;
        const std::shared_ptr<application::CallerAssignment> state;
        const std::string source_ip;
        const bool ssh;
        const application::BotGateConfiguration botgate;
        const bool botgate_participating;
        const std::shared_ptr<const application::Phase3aConfigurationSnapshot>
            configuration_snapshot;
        const std::shared_ptr<TimeContext> time_context;
        const std::shared_ptr<AlphaDurableJournal> durable;
        AlphaTrustedDisplays alpha_displays;
        std::uint8_t caller_text_color{0};
        application::TerminalTextStyle login_text_style{application::TerminalTextStyle::plain};
        std::unique_ptr<core::Session> caller_session;
        application::Phase3aExistingCallerRequest request;
        std::unique_ptr<application::IInteractiveSessionByteStream> transport;
        std::unique_ptr<MenuModalStream> accounted_stream;
        std::unique_ptr<application::LoginInactivityStream> login_stream;
        std::unique_ptr<application::StreamCallerTerminal> caller_terminal;
        std::unique_ptr<application::Phase3aTransportPasswordLogin> login;
        application::Phase3aWorkflowResult result;
        std::optional<application::AuthenticatedCallerReady> ready;
        application::CallerLoginSecret caller_login_secret;
        const std::uint64_t revocation_checkpoint;
        // Only accessed under owner.state_mutex. Never inspect worker-owned
        // ready/log_context from an administration thread.
        application::AccountId published_account{};
        application::Identifier128 published_session;
        bool published_sysop{};
        bool authenticated_as_sysop{}; // only set after the full login/SY path
        std::atomic<std::uint16_t> live_security_level{0};
        std::atomic_bool live_sysop{false};
        std::uint16_t authenticated_afk_minutes{5};
        const std::uint16_t login_afk_seconds;
        std::uint64_t menu_started{0};
        std::unique_ptr<CallerRuntimeAccounting> calling_time;
        std::unique_ptr<application::CallerInactivity> post_login_inactivity;
        std::optional<std::int64_t> remaining_calling_seconds;
        std::int64_t calling_time_snapshot_utc{};
        std::int64_t last_time_checkpoint{-1};
        bool menus_active{},calling_time_expired{},post_login_asleep{};
        std::atomic_bool message_binary_owned{false};
        std::atomic_bool door_active{false};
        bool normal{false};
        application::InteractiveStreamOutcome last_menu_outcome{application::InteractiveStreamOutcome::completed};
        bool cleaned{false};
        bool ended_recorded{false};
        bool registration_started{false}, registration_abandoned{false};
        std::int64_t main_menu_utc{0};
        application::OperationalContext log_context;
        std::optional<application::CallerStage> last_log_stage;
        const application::HostnameSubscription hostname_subscription;

        Connection(Impl& service, OwnedSocket accepted,
            std::shared_ptr<application::CallerAssignment> assignment, std::string source, bool is_ssh,
            std::uint64_t attempt, std::uint16_t peer_port, std::uint16_t local_port)
            : owner(service), socket(std::move(accepted)), state(std::move(assignment)),
              source_ip(std::move(source)), ssh(is_ssh),
              botgate(service.configuration.botgate),
              botgate_participating(is_ssh ? botgate.ssh_enabled : service.configuration.telnet.bot_gate_enabled),
              // Capture current authoritative identity for each new caller;
              // a System Sysop reassignment is not a SFX.conf change.
              configuration_snapshot(service.make_phase3a_configuration(service.configuration)),
              time_context(service.time_context), durable(service.durable), alpha_displays(service.database_path.parent_path().parent_path()),
              revocation_checkpoint(service.revocations.checkpoint()),
              login_afk_seconds(service.configuration.login_afk_seconds),
              hostname_subscription(subscribe_alpha_hostname(service.hostname_lifetime,
                  state->identity(), source_ip)) {
            const auto id = state->identity();
            log_context.epoch = id.runtime_epoch; log_context.node = id.node;
            log_context.generation = id.generation; log_context.attempt = attempt;
            log_context.transport = ssh ? application::OperationalTransport::ssh : application::OperationalTransport::telnet;
            log_context.source_ip = source_ip; log_context.peer_port = peer_port; log_context.local_port = local_port;
            ++live_contexts;
        }
        ~Connection() { unsubscribe_alpha_hostname(hostname_subscription); (void)cleanup(); --live_contexts; }
        void refresh_hostname() const noexcept {
            const auto hint = read_alpha_hostname(hostname_subscription);
            if (hint.size) state->hostname(state->identity(), source_ip, hint.view());
        }
        void log(application::OperationalKind kind,
            application::OperationalOutcome outcome = application::OperationalOutcome::none) noexcept {
            try {
                if (outcome == application::OperationalOutcome::shutdown && !owner.stopping.load())
                    outcome = application::OperationalOutcome::cancelled;
                refresh_hostname();
                const auto snapshot = state->snapshot();
                if (snapshot.screen_active) {
                    log_context.hostname = snapshot.hostname;
                    log_context.duration_seconds = snapshot.connected_seconds;
                    log_context.columns = snapshot.terminal_columns; log_context.rows = snapshot.terminal_rows;
                }
                if (kind == application::OperationalKind::connection_ended || kind == application::OperationalKind::ssh_setup_ended ||
                    kind == application::OperationalKind::authentication_denied || kind == application::OperationalKind::session_failed)
                    ended_recorded = true;
                owner.add_log(kind, log_context, outcome);
            } catch (...) { owner.journal->mark_event_failed(); }
        }
        void log_preauthentication_end(
            const application::Phase3aPreAuthenticationEnd& ending) noexcept {
            if (ending.reason != application::Phase3aPreAuthenticationEndReason::joker_name_rejected)
                return;
            log_context.attempted_name = ending.attempted_name;
            log(application::OperationalKind::connection_ended,
                application::OperationalOutcome::registration_closed);
            log_context.attempted_name.clear();
        }
        void stage(application::CallerStage value) noexcept override {
            state->stage(value);
            if (last_log_stage == value) return;
            last_log_stage = value; log_context.stage = value;
            if (value == application::CallerStage::registration && !registration_started) {
                registration_started = true;
                log(application::OperationalKind::registration_started);
            } else if (value != application::CallerStage::cleanup)
                log(application::OperationalKind::stage_changed);
        }
        void observation(application::CallerWorkflowObservation what, application::AccountId account,
            application::Identifier128 session = {}) noexcept override {
            if (what == application::CallerWorkflowObservation::registration_committed) {
                log_context.committed_account_id = account;
                log(application::OperationalKind::registration_committed);
            } else if (what == application::CallerWorkflowObservation::durable_session_started) {
                log_context.verified_account_id = account;
                log_context.durable_session_id = session;
                try {
                    menu_started=owner.clock.monotonic_millis();
                    calling_time=std::make_unique<CallerRuntimeAccounting>(menu_started,
                        [&](std::int64_t utc)->std::optional<std::string> {
                            std::shared_ptr<TimeContext> local;
                            {std::lock_guard<std::mutex> lock(owner.state_mutex);local=owner.time_context;}
                            return local && local->local?local->local->format_utc_seconds(utc):std::nullopt;
                        },owner.clock,owner.chat_hub,[&](application::Identifier128 session,
                            std::int64_t seconds,const std::vector<std::pair<std::string,std::int64_t>>& days){
                            return owner.storage.persist_private_chat_usage(session,seconds,days);
                        });
                    const auto caller=owner.storage.find_account_by_id(account);
                    const auto afk_policy=owner.storage.security_policy();
                    if(!afk_policy||!afk_policy.value)throw std::runtime_error("caller_afk_policy_unavailable");
                    std::uint16_t minutes;
                    {std::lock_guard<std::mutex> lock(owner.state_mutex);minutes=owner.configuration.caller_afk_minutes;}
                    post_login_inactivity=std::make_unique<application::CallerInactivity>(menu_started,minutes,
                        caller && caller.value && caller.value->security_level && *caller.value->security_level>=afk_policy.value->sysop_threshold);
                    const auto saved=owner.storage.menu_preferences(account);
                    if(saved && saved.value) caller_text_color=saved.value->preferences.color;
                } catch(...) {state->cleanup_health->failed.store(true);state->cancel();}
                log(application::OperationalKind::session_persisted);
            } else {
                log_context.verified_account_id = account;
                log(application::OperationalKind::duplicate_denied);
            }
        }
        void attention(application::AttentionKind kind, application::AccountId account,
            std::string_view name = {}, std::uint16_t existing_node = 0) noexcept override {
            try {
                application::AttentionRecord event;
                event.kind = kind; event.utc_seconds = owner.clock.unix_seconds();
                event.node = state->identity().node; event.existing_node = existing_node;
                event.transport = ssh ? "SSH" : "Telnet"; event.source_ip = source_ip;
                event.identity = kind == application::AttentionKind::duplicate_login ?
                    application::AttentionIdentity::verified : application::AttentionIdentity::claimed;
                if (!name.empty()) event.account_name = name;
                else if (account) {
                    const auto known = owner.storage.find_account_by_id(account);
                    if (!known || !known.value || !known.value->display_name) { owner.journal->mark_durable_journal_failed(); return; }
                    event.account_name = *known.value->display_name;
                }
                // No change to caller/name/screen authority in the normal status.
                durable->attention(std::move(event));
            } catch (...) { owner.journal->mark_durable_journal_failed(); }
        }
        void journal_main_menu() noexcept {
            if (!ready || !main_menu_utc) return;
            try {
                refresh_hostname();
                const auto snapshot = state->snapshot();
                durable->activity(ready->durable_session_id,
                    {application::CallerActivityKind::main_menu, main_menu_utc, {}}, snapshot.hostname);
            } catch (...) { owner.journal->mark_durable_journal_failed(); }
        }
        void cancel() {
            state->cancel();
            socket.interrupt();
        }
        application::SessionAccountingSnapshot accounting_snapshot() override {
            return calling_time?calling_time->snapshot(
                {published_session,published_account,state->snapshot().number},
                calling_time_expired||post_login_asleep):application::SessionAccountingSnapshot{};
        }
        bool calling_time_tick() {
            using namespace application;
            if(state->stopping.load() || calling_time_expired || post_login_asleep) return false;
            if(!calling_time) return !state->cleanup_health->failed.load();
            const auto elapsed=static_cast<std::int64_t>((owner.clock.monotonic_millis()-menu_started)/1000U);
            if(elapsed!=last_time_checkpoint) {
                const auto checkpoint_utc=owner.clock.unix_seconds();
                const auto accounting=accounting_snapshot();
                const auto account=owner.storage.find_account_by_id(log_context.verified_account_id);
                if(!account || !account.value || account.value->state!=AccountState::active || !account.value->security_level) return false;
                const auto level=owner.storage.security_level(*account.value->security_level);
                const auto progress=owner.storage.persist_session_progress({log_context.durable_session_id,
                    accounting.total_seconds,checkpoint_utc,accounting.daily_seconds});
                std::shared_ptr<TimeContext> local;
                {std::lock_guard<std::mutex> lock(owner.state_mutex);local=owner.time_context;}
                const auto date=local && local->local?local->local->format_utc_seconds(checkpoint_utc):std::nullopt;
                const auto used=date && date->size()>=10?owner.storage.caller_day_seconds(account.value->account_id,date->substr(0,10)):
                    StorageResult<std::int64_t>{{StorageErrorCode::internal,"caller date unavailable"},{}};
                const auto afk_policy=owner.storage.security_policy();
                if(!level || !level.value || !progress || !used || !used.value || !afk_policy || !afk_policy.value) {state->cleanup_health->failed.store(true);return false;}
                last_time_checkpoint=elapsed;
                remaining_calling_seconds=caller_remaining_seconds(*level.value,accounting.total_seconds,*used.value);
                calling_time_snapshot_utc=checkpoint_utc;
                post_login_inactivity->set_sysop_exemption(level.value->level>=afk_policy.value->sysop_threshold);
                if(remaining_calling_seconds && *remaining_calling_seconds==0) {
                    calling_time_expired=true;last_menu_outcome=InteractiveStreamOutcome::timed_out;
                    const bool ansi=ready?ready->graphics==GraphicsMode::ansi:login_text_style==TerminalTextStyle::ogsf_ansi;
                    if(!message_binary_owned.load()&&!door_active.load())(void)write_all(*transport,"\r\n"+(ansi?menu_text_sgr(15):std::string{})+"Your calling time has expired.\r\n");
                    return false;
                }
            }
            if(!menus_active && post_login_inactivity) {
                const auto action=post_login_inactivity->poll(owner.clock.monotonic_millis());
                const auto graphics=login_text_style==TerminalTextStyle::ogsf_ansi?GraphicsMode::ansi:GraphicsMode::ascii;
                if(action==CallerInactivity::Action::disconnect) {
                    post_login_asleep=true;last_menu_outcome=InteractiveStreamOutcome::timed_out;
                    show_inactivity_display(*transport,graphics);return false;
                }
                if(action==CallerInactivity::Action::warn) {
                    const auto preference=owner.storage.menu_preferences(log_context.verified_account_id);
                    OperationalConfiguration config;
                    {std::lock_guard<std::mutex> lock(owner.state_mutex);config=owner.configuration;}
                    if(!preference || !preference.value || !write_all(*transport,caller_inactivity_warning(graphics,
                        preference.value->preferences,config.menus.text_color,config.menus.input_color))) return false;
                    const auto music=caller_inactivity_music(graphics);
                    if(!music.empty() && !write_all(*transport,music))return false;
                }
            }
            return true;
        }
        bool cleanup() noexcept {
            if (cleaned) return !state->cleanup_health->failed.load();
            cleaned = true;
            caller_login_secret.clear();
            stage(application::CallerStage::cleanup);
            if (registration_started && !log_context.committed_account_id && !registration_abandoned) {
                registration_abandoned = true;
                log(application::OperationalKind::registration_abandoned, workflow_reason(result.code));
            }
            if (ready) {
                journal_main_menu(); // same idempotent transition, latest exactly gated PTR hint
                // The accounting provider owns charged time, including operator
                // pauses. A raw elapsed fallback would reintroduce paused time
                // and disagree with its per-day totals during finalization.
                const auto elapsed = calling_time ? 0 : menu_started ? (owner.clock.monotonic_millis() - menu_started) / 1000 : 0;
                if (!ready->durable_cleanup || !ready->durable_cleanup->finalize(
                    static_cast<std::int64_t>(std::min<std::uint64_t>(elapsed,
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))),
                    state->diagnostic() == application::CallerDiagnostic::session_failure
                        ? application::SessionOutcomeCode::runtime_error
                        : calling_time_expired || post_login_asleep || last_menu_outcome == application::InteractiveStreamOutcome::timed_out
                        ? application::SessionOutcomeCode::timed_out
                        : last_menu_outcome == application::InteractiveStreamOutcome::failed
                        ? application::SessionOutcomeCode::runtime_error
                        : outcome_for_socket(normal, state->stopping.load())))
                    state->cleanup_health->failed.store(true);
                if (!ready->admission_lease || !ready->admission_lease->release())
                    state->cleanup_health->failed.store(true);
                // Retain the immutable failed request in the quarantined context.
                // It is not silently discarded or retried with a fresh identity.
                if (!state->cleanup_health->failed.load()) ready.reset();
                durable->wake();
            }
            // Discarded workflow handoffs own the same durable-before-account guard.
            result.ready.reset();
            login.reset();
            caller_terminal.reset(); // erases pending terminal secrets/bytes
            login_stream.reset();
            accounted_stream.reset();
            transport.reset(); // closes channel and joins demux; callback state still lives
            if (caller_session) {
                (void)caller_session->request_disconnect();
                (void)caller_session->finalize_disconnect();
                caller_session.reset();
            }
            socket.close(); // last raw-socket operation, only after transport cleanup
            state->transport_closed();
            if (!owner.refresh_statistics_after_session())
                owner.add_log(application::OperationalKind::statistics_unavailable);
            return !state->cleanup_health->failed.load();
        }

        bool challenge() {
            if (!botgate.enabled || !botgate_participating) return true;
            struct Active {AlphaOperationalJournal& journal;explicit Active(AlphaOperationalJournal& j):journal(j){journal.gate_active(true);}~Active(){journal.gate_active(false);}} active(*owner.journal);
            const auto ip=application::parse_botgate_ipv4(source_ip);
            if (!ip) return false;
            // OG exemptions bypass filtering, never the human challenge.
            std::string artwork;
            if (const auto reader=open_native_display_reader(
                    owner.database_path.parent_path().parent_path()/"Display")) {
                auto asset=reader->read_asset("bg-prelog.ans",16384);
                if (asset.code==application::TrustedDisplayAssetReadCode::available) artwork=std::move(asset.bytes);
            }
            application::BotGateChallenge gate(GetTickCount64(),botgate.timeout_seconds);
            unsigned last_seconds=UINT_MAX;
            bool last_ansi=false;
            while (!state->stopping.load()) {
                const auto now=GetTickCount64();
                const auto gate_result=gate.poll(now);
                if (gate_result==application::BotGateChallenge::Result::passed) {
                    log(application::OperationalKind::botgate_passed);
                    return write_all(*transport,last_ansi ? "\x1b[0m\x1b[2J\x1b[H" : "\r\n\r\n");
                }
                if (gate_result==application::BotGateChallenge::Result::timed_out) {
                    log(application::OperationalKind::botgate_timed_out,application::OperationalOutcome::timed_out);
                    return false;
                }
                const bool ansi=ssh ? static_cast<AlphaSshStream&>(*transport).ansi_terminal() :
                    static_cast<AlphaTelnetStream&>(*transport).ansi_terminal();
                const auto seconds=gate.remaining(now);
                if (seconds!=last_seconds || ansi!=last_ansi) {
                    const auto presentation=!ansi && last_seconds!=UINT_MAX
                        ? "\rYou have "+std::to_string(seconds)+" seconds.    "
                        : application::render_botgate_art(artwork,seconds,ansi,botgate.timeout_seconds>=100 ? 3 : 2);
                    if (!write_all(*transport,presentation)) return false;
                    last_seconds=seconds; last_ansi=ansi;
                }
                const auto read=transport->read_some(application::kInteractiveStreamMaximumReadBytes);
                if (read.outcome==application::InteractiveStreamOutcome::timed_out) continue;
                if (read.outcome!=application::InteractiveStreamOutcome::completed) return false;
                const auto input=std::string_view(reinterpret_cast<const char*>(read.bytes.data()),read.bytes.size());
                if (gate.input(input,GetTickCount64())==application::BotGateChallenge::Result::invalid) {
                    log(application::OperationalKind::botgate_failed,application::OperationalOutcome::denied);
                    return false;
                }
            }
            return false;
        }

        void show_inactivity_display(application::IInteractiveSessionByteStream& stream, application::GraphicsMode graphics) {
            using namespace application;
            TrustedDisplaySelectionRequest ending_request;
            ending_request.identity=TrustedDisplayIdentity::sfasleep;ending_request.graphics=graphics;
            auto selected=alpha_displays.select(ending_request);
            if(!selected.available()) return;
            // ASC needs a native boundary; authored ANS layout is unchanged.
            // Selection canonicalizes the logical filename to uppercase.
            if(selected.program->selected_logical_asset=="SFASLEEP.ASC" &&
                !write_all(stream,"\r\n\r\n"))return;
            auto program=selected.program->terminal_program;
            // Disconnect notices cannot extend AFK or enter an active Door.
            for(auto& operation:program.operations) {
                operation.playback_bps=0;
                if(operation.kind==TerminalDisplayOperationKind::pause) program.output_byte_count-=2U;
            }
            program.operations.erase(std::remove_if(program.operations.begin(),program.operations.end(),
                [](const auto& operation) {return operation.kind==TerminalDisplayOperationKind::pause ||
                    operation.kind==TerminalDisplayOperationKind::paging_enabled;}),program.operations.end());
            // This noninteractive path bypasses the menu/display style setup.
            // Select the admitted caller's current preference (or board default)
            // so ASC fallback cannot inherit the last input color. Authored ANS
            // operations still override this base rendition; ASCII gets no SGR.
            if(graphics==GraphicsMode::ansi) {
                auto color=caller_text_color;
                if(log_context.verified_account_id>0) {
                    const auto saved=owner.storage.menu_preferences(log_context.verified_account_id);
                    if(saved && saved.value) color=saved.value->preferences.color;
                }
                if(color==0 || color>15) {
                    std::lock_guard<std::mutex> lock(owner.state_mutex);
                    color=owner.configuration.menus.text_color;
                }
                if(!write_all(stream,menu_text_sgr(color))) return;
            }
            StreamCallerTerminal ending(stream,{SecretInputEchoMode::masked,7,11,&caller_text_color});
            (void)ending.present_display({"caller.sfasleep",&program,{false,24}});
        }

        bool run_menus(application::IInteractiveSessionByteStream& stream) {
            using namespace application;
            CallerMenuNotice notice;
            CallerInactivity inactivity(GetTickCount64(),authenticated_afk_minutes,live_sysop.load());
            const auto refresh=[&] {
                if(state->stopping.load()) return false;
                ready->security_level=live_security_level.load();
                inactivity.set_sysop_exemption(live_sysop.load());
                return ready->security_level!=0;
            };
            bool asleep_shown=false;
            const auto asleep=[&] {
                if(asleep_shown || !refresh()) return;
                asleep_shown=true;
                show_inactivity_display(stream,ready->graphics);
            };
            bool afk_notice=false,menu_notice_surface=false,afk_music_sent=false;
            std::vector<MenuTextRow> caller_notifications;
            std::function<bool()> active_menu_redraw;
            const auto afk_music=[&] {
                if(!afk_notice || afk_music_sent || !menu_notice_surface)return true;
                const auto music=caller_inactivity_music(ready->graphics);
                if(!music.empty() && !write_all(stream,music))return false;
                afk_music_sent=true;return true;
            };
            const auto dismiss_notifications=[&] {
                const bool displayed=afk_notice || !caller_notifications.empty();
                caller_notifications.clear();afk_notice=false;
                return displayed;
            };
            const auto warn=[&] {
                afk_notice=true;afk_music_sent=false;
                if(menu_notice_surface && active_menu_redraw)return active_menu_redraw() && afk_music();
                // Protected/modal screens keep their input and cursor intact.
                // Queue the warning for the next safe menu; the existing AFK
                // deadline continues to run, including inside an editor.
                return true;
            };
            const auto modal_tick=[&] {
                if(!refresh()) return false;
                const auto action=inactivity.poll(GetTickCount64());
                if(action==CallerInactivity::Action::disconnect) {
                    asleep();
                    last_menu_outcome=InteractiveStreamOutcome::timed_out; return false;
                }
                return action!=CallerInactivity::Action::warn || warn();
            };
            MenuModalStream modal_stream(stream,modal_tick,[&]{inactivity.keyboard(GetTickCount64());dismiss_notifications();});
            StreamCallerTerminal terminal(modal_stream,{SecretInputEchoMode::masked,7,11,&caller_text_color});
            const auto message_actor=[&]() -> std::optional<MessageActor> {
                if(!refresh())return {};
                const auto account=owner.storage.find_account_by_id(ready->account_id);const auto policy=owner.storage.security_policy();
                if(!account || !account.value || account.value->state!=AccountState::active || !policy || !policy.value)return {};
                return MessageActor{ready->durable_session_id,ready->account_id,account.value->revision,policy.value->revision};
            };
            const auto own_name=[&]{const auto account=owner.storage.find_account_by_id(ready->account_id);return account && account.value?account.value->display_name.value_or("Removed caller"):std::string("Removed caller");};
#include "chat_runtime_session.inc"
            const auto message_date=[&](std::int64_t utc){std::shared_ptr<TimeContext> local;{std::lock_guard<std::mutex> lock(owner.state_mutex);local=owner.time_context;}const auto text=local && local->local?local->local->format_utc_seconds(utc):std::nullopt;return text.value_or("Unavailable");};
            MessageQuestionTerminal message_terminal(terminal,[&]{return ready->graphics==GraphicsMode::ansi;},
                [&](std::string_view text){return modal_tick() && write_all(modal_stream,text);},
                [&]{return state->snapshot().terminal_columns;},
                [&]{return state->snapshot().terminal_rows;});
            MessageComposePorts compose{owner.storage,message_terminal,message_actor,[&]{return owner.identifiers.generate();},[&]{const auto node=state->snapshot();return MessageEditorDimensions{ready->graphics==GraphicsMode::ansi,node.terminal_columns,node.terminal_rows};},[&]{
                struct Restore {MenuModalStream& stream;~Restore(){stream.preserve_poll_timeouts(false);}} restore{modal_stream};
                modal_stream.preserve_poll_timeouts(true);return terminal.read_editor_byte();
            },[&](std::string_view text){return modal_tick() && write_all(modal_stream,text);},[&]() -> std::optional<std::pair<MessageTarget,std::string>>{
                const auto actor=message_actor();if(!actor)return {};const auto target=owner.storage.system_sysop_target(*actor);if(!target || !target.value)return {};
                const auto recipient=owner.storage.resolve_message_recipient(*actor,std::to_string(*target.value->recipient));if(!recipient || !recipient.value || recipient.value->entries.size()!=1)return {};return std::make_pair(*target.value,recipient.value->entries.front().name);
            }};
            compose.accepted_key=[&](bool carriage_return){terminal.finish_editor_key(carriage_return);};
            compose.text_color=[&]{
                if(caller_text_color>0 && caller_text_color<=15)return caller_text_color;
                std::lock_guard<std::mutex> lock(owner.state_mutex);return owner.configuration.menus.text_color;
            };
            compose.unavailable_notice=[&](std::string_view text){
                // Leave the last cell clear so the terminal cannot auto-wrap a notice.
                const auto width=std::clamp<std::size_t>(state->snapshot().terminal_columns,2,4096)-1;
                for(const auto& row:wrap_message_line(text,width,false)){
                    const auto first=row.text.find_first_not_of(' ');
                    if(first==std::string::npos)continue;
                    const auto last=row.text.find_last_not_of(' ');
                    caller_notifications.push_back({MenuTextRole::notification,row.text.substr(first,last-first+1)});
                }
                return true;
            };
            compose.real_name_editable=!configuration_snapshot->real_names_required();
            StreamCallerTerminal::BinaryInputPrefix binary_prefix;
            std::uint64_t binary_start{},binary_last_inbound{};bool file_binary_owned{},qwk_binary_owned{};
            DecodedMessageReceiveChannel receive_channel;
            receive_channel.now=[] {return GetTickCount64();};
            receive_channel.absolute_deadline=[&] {
                auto deadline=inactivity.absolute_deadline();
                const auto take=[&](std::uint64_t value){if(value)deadline=deadline?std::min(deadline,value):value;};
                take(owner.drain_deadline.load());
                if(remaining_calling_seconds)take(GetTickCount64()+static_cast<std::uint64_t>(*remaining_calling_seconds)*1000);
                if(message_binary_owned.load()){
                    const auto limits=message_receive_detail::receive_deadlines(deadline,binary_start,binary_last_inbound);
                    deadline=limits.hard;
                    // File host owns its bounded idle cancellation/recovery.
                    // Do not let the SSH demultiplexer kill the whole session
                    // at the same idle boundary before that recovery can run.
                    // Hard AFK/calling-time/drain/total limits still terminate.
                    if(ssh)static_cast<AlphaSshStream&>(*transport).message_binary_deadline((file_binary_owned||qwk_binary_owned)?limits.hard:limits.transport);
                }
                return deadline;
            };
            receive_channel.authorized=[&](const MessageReceiveRequest& request){
                if(!calling_time_tick() || !refresh())return false;
                const auto actor=message_actor();
                if(!actor || actor->session!=request.actor.session || actor->account!=request.actor.account || actor->account_revision!=request.actor.account_revision || actor->policy_revision!=request.actor.policy_revision)return false;
                const auto current=owner.storage.begin_message(*actor,request.context.target,request.context.parent);
                return current && current.value && same_message_receive_context(*current.value,request.context);
            };
            receive_channel.acquire=[&]{
                if(message_binary_owned.exchange(true))return false;
                binary_start=binary_last_inbound=GetTickCount64();
                if(ssh && !static_cast<AlphaSshStream&>(*transport).message_binary_mode(true)){message_binary_owned.store(false);shutdown(socket.get(),SD_BOTH);return false;}
                if(!ssh && !static_cast<AlphaTelnetStream&>(*transport).message_binary_mode(true)){message_binary_owned.store(false);shutdown(socket.get(),SD_BOTH);return false;}
                binary_prefix=terminal.take_binary_input_prefix();return true;
            };
            receive_channel.release=[&](bool synchronized){
                bool restored=true;
                if(ssh)restored=static_cast<AlphaSshStream&>(*transport).message_binary_mode(false);
                else restored=static_cast<AlphaTelnetStream&>(*transport).message_binary_mode(false);
                binary_prefix={};message_binary_owned.store(false);
                if(!synchronized || !restored){last_menu_outcome=InteractiveStreamOutcome::disconnected;shutdown(socket.get(),SD_BOTH);}
            };
            receive_channel.read=[&](std::size_t maximum){
                if(!message_binary_owned.load())return InteractiveStreamReadResult{InteractiveStreamOutcome::failed,{}};
                if(!binary_prefix.bytes.empty()){
                    const auto count=std::min(maximum,binary_prefix.bytes.size());std::vector<std::uint8_t> bytes(binary_prefix.bytes.begin(),binary_prefix.bytes.begin()+count);
                    binary_prefix.bytes.erase(binary_prefix.bytes.begin(),binary_prefix.bytes.begin()+count);
                    binary_last_inbound=GetTickCount64();
                    return InteractiveStreamReadResult{InteractiveStreamOutcome::completed,std::move(bytes)};
                }
                auto result=(file_binary_owned||qwk_binary_owned)
                    ? (ssh?static_cast<AlphaSshStream&>(*transport).read_ready(maximum):static_cast<AlphaTelnetStream&>(*transport).read_ready(maximum))
                    : transport->read_some(maximum);
                if(result.outcome==InteractiveStreamOutcome::completed && binary_prefix.discard_delimiter_lf && !result.bytes.empty()){
                    binary_prefix.discard_delimiter_lf=false;if(result.bytes.front()=='\n')result.bytes.erase(result.bytes.begin());
                    if(result.bytes.empty())result.outcome=InteractiveStreamOutcome::timed_out;
                }
                if(result.outcome==InteractiveStreamOutcome::completed && !result.bytes.empty())binary_last_inbound=GetTickCount64();
                return result;
            };
            receive_channel.write=[&](std::string_view bytes){
                if(!message_binary_owned.load())return InteractiveStreamWriteResult{InteractiveStreamOutcome::failed,0};
                return (file_binary_owned||qwk_binary_owned)&&ssh?static_cast<AlphaSshStream&>(*transport).write_ready(bytes):transport->write_some(bytes);
            };
            DecodedQwkTransferChannel packet_channel{receive_channel.acquire,receive_channel.release,receive_channel.read,receive_channel.write,receive_channel.now,receive_channel.absolute_deadline,{}};
            packet_channel.acquire=[&,acquire=receive_channel.acquire]{qwk_binary_owned=true;if(acquire())return true;qwk_binary_owned=false;return false;};
            packet_channel.release=[&,release=receive_channel.release](bool synchronized){release(synchronized);qwk_binary_owned=false;};
            DecodedFileTransferChannel file_channel{receive_channel.acquire,{},receive_channel.release,receive_channel.read,receive_channel.write,receive_channel.now,receive_channel.absolute_deadline};
            file_channel.acquire=[&,acquire=receive_channel.acquire]{file_binary_owned=true;if(acquire())return true;file_binary_owned=false;return false;};
            file_channel.release=[&,release=receive_channel.release](bool synchronized){release(synchronized);file_binary_owned=false;};
            file_channel.begin_recovery=[&,absolute_deadline=receive_channel.absolute_deadline]{
                if(!message_binary_owned.load()||!calling_time_tick()||!refresh())return false;
                const auto now=GetTickCount64();if(now<binary_start||now-binary_start>=kMessageReceiveTotalMillis)return false;
                binary_last_inbound=now;return absolute_deadline()>now;
            };
            AlphaMessageSourceReceiver source_receiver(owner.database_path.parent_path().parent_path(),owner.installation_owner_sid,std::move(receive_channel),{},[weak=std::weak_ptr<AlphaOperationalJournal>(owner.journal)](std::string resource,std::string diagnostic){if(const auto journal=weak.lock())journal->problem(AttentionKind::work_preserved,std::move(resource),std::move(diagnostic));});
            compose.source_receiver=&source_receiver;
            const auto initial_actor=message_actor();if(!initial_actor)return false;
            const auto initial_counts=owner.storage.message_caller_statistics(*initial_actor,{});if(!initial_counts || !initial_counts.value)return false;
            std::int64_t dispatch_fence=initial_counts.value->last_arrival;
            MessageArrivalQueue arrivals(ready->account_id,dispatch_fence);
            std::uint64_t arrival_presentations{};
            bool gathering_menu_arrivals=false;
            const auto message_boundary=[&] {
                // Only called at a menu/reader command boundary, never by the
                // modal password/editor/help/quote stream or a partial menu key.
                const auto actor=message_actor();if(!actor)return false;
                for(unsigned page=0;page<4;++page){
                    const auto work=owner.storage.message_arrival_page(*actor,dispatch_fence,100);if(!work || !work.value)return true; // retain fence; retry later, no false notice
                    for(const auto& item:work.value->arrivals)if(!arrivals.enqueue(item))return false;
                    dispatch_fence=work.value->next_after.value_or(work.value->fence);if(!work.value->next_after)break;
                }
                // A modal reader owns its viewport until the menu is redrawn.
                if(!gathering_menu_arrivals)return true;
                bool storage_failed=false;std::string arrival_sender;
                const auto accepted=arrivals.present_checked(true,[&](MessageId id)->std::optional<bool>{const auto live=message_actor();if(!live){storage_failed=true;return {};}const auto result=owner.storage.message_arrival_sender(*live,id);if(!result || !result.value){storage_failed=true;return {};}arrival_sender.clear();if(!result.value->has_value())return false;arrival_sender=**result.value;return true;},[&]()->std::optional<bool>{const auto live=message_actor();if(!live){storage_failed=true;return {};}arrival_sender.clear();const auto result=owner.storage.has_addressed_unread(*live,false);if(!result || !result.value){storage_failed=true;return {};}return *result.value;},[&](std::string_view text){
                    const auto rendered=arrival_sender.empty()?std::string(text):message_arrival_notice(arrival_sender);
                    caller_notifications.push_back({MenuTextRole::notification,rendered});
                    ++arrival_presentations;
                    return true;
                });
                return accepted || storage_failed;
            };
            const auto menu_message_boundary=[&]{
                if(!menu_notice_surface)return true;
                if(!chat_refresh())return false;
                const auto count=caller_notifications.size();
                const auto chat_before=chat.presentations();
                if(!chat.safe_events([&](std::string text){caller_notifications.push_back({MenuTextRole::feedback,std::move(text)});} ))return false;
                if((caller_notifications.size()!=count||chat_before!=chat.presentations())&&active_menu_redraw&&!active_menu_redraw())return false;
                const auto before=arrival_presentations;
                gathering_menu_arrivals=true;const auto ok=message_boundary();gathering_menu_arrivals=false;
                if(!ok)return false;
                return before==arrival_presentations || !active_menu_redraw || active_menu_redraw();
            };
            MessageCallerSession messages({std::move(compose),own_name,message_date,message_boundary,
                []{return TerminalDisplayPagingPolicy{true,std::uint16_t{24}};},
                [&]{std::lock_guard<std::mutex> lock(owner.state_mutex);return owner.configuration.menus.lightbars_enabled;},
                [&]{std::lock_guard<std::mutex> lock(owner.state_mutex);return owner.configuration.menus.legacy_message_pipe_colors;}});
            if(!messages.initialize())return false;
#include "qwk_runtime_session.inc"
#include "file_runtime_session.inc"
            MenuSessionPorts ports;
            CallerTimeWarnings calling_time_warnings;
            const auto effective_seconds = [&] {
                return event_limited_calling_seconds(remaining_calling_seconds, owner.event_cutoff_utc.load(), owner.clock.unix_seconds());
            };
            ports.calling_time_warning = [&] { return calling_time_warnings.take(effective_seconds()); };
            ports.snapshot=[&]() -> std::optional<MenuSessionSnapshot> {
                if(!refresh() || !calling_time_tick()) return std::nullopt;
                const auto account=owner.storage.find_account_by_id(ready->account_id);
                const auto preferences=owner.storage.menu_preferences(ready->account_id);
                if(!account || !account.value || account.value->state!=AccountState::active ||
                    !account.value->security_level || !preferences || !preferences.value) return std::nullopt;
                OperationalConfiguration configuration;
                std::shared_ptr<TimeContext> local_time;
                unsigned online=0;
                {
                    std::lock_guard<std::mutex> lock(owner.state_mutex);
                    configuration=owner.configuration;local_time=owner.time_context;
                    for(const auto& caller:owner.active_contexts) if(caller && caller->published_account>0 &&
                        !caller->state->stopping.load()) ++online;
                }
                if(!local_time || !local_time->local) return std::nullopt;
                const auto now=owner.clock.unix_seconds();
                const auto local=local_time->local->format_utc_seconds(now);
                if(!local || local->size()<19) return std::nullopt;
                const auto node=state->snapshot();
                MenuSessionSnapshot result;
                result.access={*account.value->security_level,live_sysop.load(),static_cast<std::uint64_t>(account.value->revision)};
                result.configuration_revision=configuration.configuration_revision;
                result.preferences=*preferences.value;result.graphics=ready->graphics;
                result.columns=node.terminal_columns;
                result.rows=node.terminal_rows;
                result.default_color=configuration.menus.text_color;
                result.input_color=configuration.menus.input_color;
                result.lightbars_enabled=configuration.menus.lightbars_enabled;
                result.selected_foreground=configuration.menus.selected_foreground;
                result.selected_background=configuration.menus.selected_background;
                result.compact_footer=configuration.menus.compact_footer;
                caller_text_color=result.preferences.preferences.color;
                (void)terminal.set_colors(result.default_color,result.input_color);
                (void)terminal.set_text_style(ready->graphics==GraphicsMode::ansi?TerminalTextStyle::ogsf_ansi:TerminalTextStyle::plain);
                auto& context=result.context;
                context.bbs_name=configuration.bbs_name;context.sysop=owner.sysop_display_name();
                context.version=SFX_PRODUCT_DISPLAY_VERSION;
                context.date=local->substr(0,10);context.time=local->substr(11,8);context.datetime=local->substr(0,19);
                context.node=std::to_string(ready->node_id);context.nodes=std::to_string(configuration.logical_nodes);
                context.online=std::to_string(online);context.transport=ssh?"SSH":"Telnet";
                if(!chat_refresh())return std::nullopt;
                const auto selected_chat_room=chat.selected_room();
                context.chat_room=selected_chat_room?selected_chat_room->name:std::string("None");
                context.columns=std::to_string(node.terminal_columns);context.rows=std::to_string(node.terminal_rows);
                context.alias=account.value->display_name.value_or(ready->display_name);
                context.name=context.alias;
                context.first_name=context.name->substr(0,context.name->find(' '));
                context.city_state=account.value->profile.location;context.phone_number=account.value->profile.phone;
                context.email=account.value->profile.email;context.security_level=std::to_string(result.access.security_level);
                context.logons=std::to_string(account.value->statistics.successful_login_count);
                if(!messages.refresh_selection())return std::nullopt;
                const auto actor=message_actor();if(!actor)return std::nullopt;
                const auto counts=owner.storage.message_caller_statistics(*actor,messages.selection()?std::optional<LocalEchoId>{messages.selection()->echo.id}:std::nullopt);
                if(!counts || !counts.value)return std::nullopt;
                context.posts=std::to_string(counts.value->messages_entered);context.mail_total=std::to_string(counts.value->mail_total);context.mail_unread=std::to_string(counts.value->mail_unread);
                if(messages.selection()){
                    result.message_group=messages.selection()->group.name;result.message_echo=messages.selection()->echo.name;
                    context.message_conference=result.message_echo;context.message_conference_number=std::to_string(messages.selection()->echo.conference_number);
                    if(counts.value->echo_count)context.message_count=std::to_string(*counts.value->echo_count);
                    if(counts.value->new_messages)context.new_messages=std::to_string(*counts.value->new_messages);
                }
                const auto file_context=files.context();if(!file_context)return std::nullopt;
                result.file_group=file_terminal_text(file_context->group,64);result.file_area=file_terminal_text(file_context->area,64);result.file_area_number=file_context->number;
                if(file_context->id.value){context.file_area=result.file_area;context.file_area_number=std::to_string(result.file_area_number);}
                const auto file_state=owner.storage.file_caller_state(*actor);const auto file_stats=owner.storage.file_caller_statistics(*actor,file_local_date());
                if(!file_state||!file_state.value||!file_stats||!file_stats.value)return std::nullopt;
                context.uploads=std::to_string(file_stats.value->counts.successful_uploads);context.downloads=std::to_string(file_stats.value->counts.successful_downloads);
                context.uploaded_kb=std::to_string(file_stats.value->upload_bytes/1024);context.downloaded_kb=std::to_string(file_stats.value->download_bytes/1024);
                if(file_context->id.value){FileQuery query;query.area=file_context->id;
                    const auto catalog=owner.storage.file_page(*actor,query,{},1);if(catalog&&catalog.value)context.file_count=std::to_string(catalog.value->total_files);
                    query.new_only=query.unseen_only=true;query.since_utc=file_state.value->since_utc;const auto unread=owner.storage.file_page(*actor,query,{},1);if(unread&&unread.value)context.new_files=std::to_string(unread.value->total_files);
                }
                if(ready->previous_login_utc) {
                    const auto previous=local_time->local->format_utc_seconds(*ready->previous_login_utc);
                    if(previous && previous->size()>=19) context.last_call=previous->substr(0,19);
                }
                const auto credential=owner.storage.load_credential(ready->account_id);
                if(credential && credential.value && now>=credential.value->created_utc_seconds)
                    context.password_age=std::to_string((now-credential.value->created_utc_seconds)/86400);
                context.uptime=std::to_string((GetTickCount64()-owner.started_at)/1000U);
                context.time_on=std::to_string((owner.clock.monotonic_millis()-menu_started)/1000U);
                const auto remaining = effective_seconds();
                context.time_left=remaining?std::to_string(*remaining):"Unlimited";
                context.logon_minutes=remaining?std::to_string(*remaining/60):"Unlimited";
                context.ip_address=source_ip;
                if(!node.hostname.empty()) context.hostname=node.hostname;
                context.hotkey=preferences.value->preferences.hotkey?"On":"Off";
                const auto transfer=owner.storage.transfer_preference(ready->account_id);
                if(transfer && transfer.value) context.protocol=std::string(storage_code(transfer.value->preference));
                return result;
            };
            ports.file_action=[&](MenuAction action,MenuId menu)->std::optional<FileSessionEffect>{if(!file_live()||!write_all(modal_stream,"\r\n"))return {};return files.action(action,menu);};
            ports.file_navigation=[&](MenuId from,MenuId to){return files.leave(from,to);};
            ports.menu_entered=[&](MenuId menu){state->menu_entered(menu);chat_refresh();};
            ports.chat=[&](MenuAction action)->std::optional<ChatExit>{return chat.menu_action(action);};
            ports.login_file_scan=[&]{return files.login_scan();};
            ports.artwork=[&](const TrustedDisplaySelectionRequest& request) {
                const auto reader=open_native_display_reader(
                    owner.database_path.parent_path().parent_path()/"Display");
                if(!reader) return TrustedDisplaySelectionResult{TrustedDisplaySelectionCode::unavailable,std::nullopt};
                TrustedDisplaySelector selector(*reader);return selector.select(request);
            };
            ports.write=[&](std::string_view bytes) {return refresh() && write_all(stream,bytes);};
            ports.display=[&](const TerminalDisplayProgram& program) {
                const auto height=static_cast<std::uint16_t>(std::clamp<unsigned>(state->snapshot().terminal_rows-1U,10,24));
                return refresh() && terminal.present_display({"menu.artwork",&program,{true,height}})==TerminalOutcome::completed;
            };
            ports.read=[&](MenuInputDecoder& decoder,const std::function<bool()>& redraw) {
                owner.chat_hub.safe(chat_identity,true);
                if(!afk_music())return std::optional<MenuInput>{};
                MenuDelimiterStream menu_stream(stream,terminal,decoder);
                active_menu_redraw=[&]{return redraw() && (notice.waiting() || decoder.pending_text().empty() || write_all(stream,decoder.pending_text()));};
                struct RestoreNoticeSurface {std::function<bool()>& redraw;bool& surface;~RestoreNoticeSurface(){redraw={};surface=false;}} restore_notice_surface{active_menu_redraw,menu_notice_surface};
                std::uint64_t revision;
                {std::lock_guard<std::mutex> lock(owner.state_mutex);revision=owner.configuration.configuration_revision;}
                const auto level=live_security_level.load();const auto sysop=live_sysop.load();
                const auto changed=[&] {
                    if(menu_notice_surface && calling_time_warnings.pending(effective_seconds())) return true;
                    std::lock_guard<std::mutex> lock(owner.state_mutex);
                    return revision!=owner.configuration.configuration_revision || level!=live_security_level.load() || sysop!=live_sysop.load();
                };
                return read_menu_input(menu_stream,last_menu_outcome,notice,owner.drain_deadline,decoder,active_menu_redraw,inactivity,refresh,changed,asleep,warn,menu_message_boundary,dismiss_notifications);
            };
            ports.save_preferences=[&](const UpdateMenuPreferenceRequest& request) {
                return refresh() && request.account_id==ready->account_id && owner.storage.update_menu_preferences(request).succeeded();
            };
            ports.transfer_preference=[&]() -> std::optional<TransferPreferenceRecord> {
                if(!refresh()) return std::nullopt;
                const auto preference=owner.storage.transfer_preference(ready->account_id);
                return preference && preference.value?std::optional<TransferPreferenceRecord>{*preference.value}:std::nullopt;
            };
            ports.save_transfer_preference=[&](const UpdateTransferPreferenceRequest& request) {
                return refresh() && request.account_id==ready->account_id && owner.storage.update_transfer_preference(request).succeeded();
            };
            ports.utc_now=[&]{return owner.clock.unix_seconds();};
#include "sysop_runtime_session.inc"
            ports.action=[&](MenuAction action) -> std::optional<std::string> {
                owner.chat_hub.safe(chat_identity,false);
                caller_notifications.clear();afk_notice=false;menu_notice_surface=false;
                if(!refresh()) return std::nullopt;
                if(action==MenuAction::sysop_utilities)return sysop_menu();
                if(action>=MenuAction::message_change && action<=MenuAction::comment_sysop){message_terminal.menu_boundary();return messages.action(action);}
                if(action>=MenuAction::qwk_download && action<=MenuAction::qwk_pointers){message_terminal.menu_boundary();return offline.action(action);}
                if(action==MenuAction::graphics) {
                    if(ready->graphics==GraphicsMode::ansi && !write_all(stream,"\x1b[0m")) return std::nullopt;
                    ready->graphics=ready->graphics==GraphicsMode::ansi?GraphicsMode::ascii:GraphicsMode::ansi;
                    return ready->graphics==GraphicsMode::ansi?"Graphics: ANSI.":"Graphics: ASCII.";
                }
                if(action==MenuAction::other_boards) {
                    return run_other_board_session(ports,terminal,[&](std::int64_t after) {
                        if(!refresh())return StorageResult<OtherBoardPage>{{StorageErrorCode::internal,"caller unavailable"},{}};
                        return owner.storage.other_boards(after);
                    },[&](const OtherBoardValues& values) {
                        if(!refresh())return false;
                        const auto account=owner.storage.find_account_by_id(ready->account_id);
                        if(!account || !account.value || account.value->state!=AccountState::active)return false;
                        ClassicInformationChange change;change.values=values;
                        return owner.storage.change_classic_information(change,{false,ready->account_id,account.value->revision,
                            owner.clock.unix_seconds(),owner.identifiers.generate()}).succeeded();
                    },[&](const OtherBoardRecord& shown) {
                        if(!refresh() || live_security_level.load()!=999 || !live_sysop.load())return false;
                        const auto account=owner.storage.find_account_by_id(ready->account_id);
                        if(!account || !account.value || account.value->state!=AccountState::active ||
                            account.value->security_level!=999)return false;
                        ClassicInformationChange change;change.action=ClassicInformationAction::remove_board;
                        change.id=shown.id;change.expected_revision=shown.revision;
                        return owner.storage.change_classic_information(change,{false,ready->account_id,account.value->revision,
                            owner.clock.unix_seconds(),owner.identifiers.generate()}).succeeded();
                    });
                }
                if(action==MenuAction::system_information) {
                    std::shared_ptr<TimeContext> local;
                    {std::lock_guard<std::mutex> lock(owner.state_mutex);local=owner.time_context;}
                    const auto range=local && local->local?bbs_today_range(*local->local,owner.clock.unix_seconds()):std::nullopt;
                    if(!range)return "System information is unavailable. Please try again later.";
                    const auto counts=owner.storage.system_information_counters((*range)[0],(*range)[1]);
                    const auto opening=owner.storage.opening_date();
                    if(!counts || !counts.value || !opening || !opening.value)return "System information is unavailable. Please try again later.";
                    auto date=opening.value->configured_date;
                    if(!date) {
                        const auto initialized=local->local->format_utc_seconds(opening.value->initialization_utc);
                        if(initialized && initialized->size()>=10)date=initialized->substr(0,10);
                    }
                    return show_system_information(ports,*counts.value,date.value_or("Unavailable"))?std::optional<std::string>{""}:std::nullopt;
                }
                if(action==MenuAction::caller_profile || action==MenuAction::caller_directory || action==MenuAction::locate_caller) {
                    CallerUtilityPorts utilities(terminal,ready->account_id);
                    utilities.rules={!configuration_snapshot->real_names_required(),
                        configuration_snapshot->location_mode()==RegistrationFieldMode::required,
                        configuration_snapshot->email_mode()==EmailRegistrationMode::required,
                        configuration_snapshot->sex_mode()==RegistrationFieldMode::required};
                    utilities.page_rows=std::clamp<unsigned>(state->snapshot().terminal_rows,12,25)-5;
                    utilities.account=[&]() -> std::optional<AccountRecord> {
                        if(!refresh()) return std::nullopt;
                        const auto account=owner.storage.find_account_by_id(ready->account_id);
                        return account && account.value?account.value:std::nullopt;
                    };
                    utilities.save_profile=[&](const AccountRecord& shown,const AccountProfile& replacement) {
                        if(!refresh() || shown.account_id!=ready->account_id ||
                            !valid_caller_profile_change(shown.profile,replacement,utilities.rules)) return false;
                        const auto authoritative=owner.storage.find_account_by_id(ready->account_id);
                        if(!authoritative || !authoritative.value || authoritative.value->state!=AccountState::active ||
                            authoritative.value->revision!=shown.revision ||
                            !valid_caller_profile_change(authoritative.value->profile,replacement,utilities.rules)) return false;
                        return owner.storage.update_profile({ready->account_id,shown.revision,replacement,
                            {owner.identifiers.generate(),ready->correlation_id,ready->account_id,
                                owner.clock.unix_seconds(),AuditReasonCode::caller_action}}).succeeded();
                    };
                    utilities.history=[&](const HistoryQuery& query) {
                        if(!refresh() || query.account_id!=ready->account_id)
                            return StorageResult<std::vector<SessionRecord>>{{StorageErrorCode::invalid_argument,"caller identity mismatch"},{}};
                        return owner.storage.session_history(query);
                    };
                    utilities.directory=[&](const CallerDirectoryQuery& query) {
                        if(!refresh()) return StorageResult<std::vector<CallerDirectoryEntry>>{{StorageErrorCode::internal,"caller unavailable"},{}};
                        return owner.storage.caller_directory(query);
                    };
                    utilities.local_time=[&](std::int64_t utc) -> std::optional<std::string> {
                        std::shared_ptr<TimeContext> local;
                        {std::lock_guard<std::mutex> lock(owner.state_mutex);local=owner.time_context;}
                        return local && local->local?local->local->format_utc_seconds(utc):std::nullopt;
                    };
                    utilities.ghost=[&]()->std::optional<bool>{if(!live_sysop.load()||!chat_refresh())return {};const auto me=owner.chat_hub.participant(chat_identity);return me?std::optional<bool>(me->preferences.ghost):std::nullopt;};
                    utilities.toggle_ghost=[&]{if(!live_sysop.load())return false;const auto actor=message_actor();if(!actor)return false;const auto old=owner.storage.chat_preferences(*actor);if(!old||!old.value)return false;auto next=*old.value;next.ghost=!next.ghost;return owner.storage.save_chat_preferences(*actor,next).succeeded()&&chat_refresh();};
                    utilities.utc_now=[&]{return owner.clock.unix_seconds();};
                    return run_caller_utility(action,utilities);
                }
                if(action==MenuAction::chat_dnd||action==MenuAction::chat_notices)return chat_toggle(action);
                if(action==MenuAction::page_sysop){
                    const auto comment=chat.page_sysop();owner.chat_hub.safe(chat_identity,false);
                    if(!comment)return {};if(*comment){message_terminal.menu_boundary();return messages.action(MenuAction::comment_sysop);}
                    return std::string{};
                }
                if(action==MenuAction::online) {
                    owner.chat_hub.safe(chat_identity,true);const bool ok=chat.online();owner.chat_hub.safe(chat_identity,false);
                    return ok?std::optional<std::string>(std::string{}):std::nullopt;
                }
                if(action==MenuAction::password) {
                    auto old_password=read_password_change_current(terminal,kPasswordMaximum);
                    struct Erase { std::string& value;~Erase(){explicitly_erase_terminal_secret(value);} } erase_old{old_password.value};
                    if(!old_password.has_value()) return old_password.outcome==TerminalOutcome::cancelled ? std::optional<std::string>{"Password change cancelled."}:std::nullopt;
                    auto replacement=terminal.read_secret({"menu.password.new","New password: ",kPasswordMaximum});
                    Erase erase_new{replacement.value};if(!replacement.has_value()) return replacement.outcome==TerminalOutcome::cancelled ? std::optional<std::string>{"Password change cancelled."}:std::nullopt;
                    auto confirmation=terminal.read_secret({"menu.password.confirm","Confirm new password: ",kPasswordMaximum});
                    Erase erase_confirm{confirmation.value};if(!confirmation.has_value()) return confirmation.outcome==TerminalOutcome::cancelled ? std::optional<std::string>{"Password change cancelled."}:std::nullopt;
                    if(replacement.value!=confirmation.value) return "Passwords did not match. Nothing changed.";
                    const auto account=owner.storage.find_account_by_id(ready->account_id);
                    if(!account || !account.value || !account.value->display_name) return std::nullopt;
                    PasswordPolicyContext policy;
                    {std::lock_guard<std::mutex> lock(owner.state_mutex);policy.mode=owner.configuration.password_mode;policy.bbs_name=owner.configuration.bbs_name;}
                    policy.account_name=*account.value->display_name;
                    const auto changed=owner.authentication.change_account_password({ready->account_id,account.value->revision,
                        old_password.value,replacement.value,policy,ready->durable_session_id,
                        {owner.identifiers.generate(),ready->correlation_id,ready->account_id,owner.clock.unix_seconds()}},state->cancellation);
                    if(changed.succeeded()&&changed.committed_credential)
                        caller_login_secret.capture(ready->account_id,*changed.committed_credential,replacement.value);
                    else if(changed.credential_committed)caller_login_secret.clear();
                    return changed.succeeded()?"Password changed.":"Password not changed. Check your current password and password rules.";
                }
                return std::string(unavailable_menu_feature);
            };
            ports.previous_login_utc=ready->previous_login_utc;
            ports.door_bulletin_updates=[&](std::int64_t previous)->std::optional<std::vector<DoorBulletinNotice>>{const auto actor=message_actor();if(!actor)return {};const auto result=owner.storage.door_bulletin_changes(*actor,previous);return result&&result.value?result.value:std::nullopt;};
            ports.login_message_scan=[&]{return messages.login_scan();};
            ports.operational_notifications=[&]{
                std::vector<MenuTextRow> rows;
                if(notice.waiting()){
                    const auto text=notice.text(GetTickCount64());std::string line;
                    for(const auto c:text){if(c=='\r')continue;if(c=='\n'){if(!line.empty())rows.push_back({MenuTextRole::notification,line});line.clear();}else line+=c;}
                }
                return rows;
            };
            ports.notifications=[&]{
                menu_notice_surface=true;auto rows=caller_notifications;
                if(afk_notice)rows.push_back({MenuTextRole::inactivity_warning,"You have another minute to execute a command or you will be logged off."});
                const auto required=ports.operational_notifications();rows.insert(rows.end(),required.begin(),required.end());
                return rows;
            };
            ports.safe_boundary=[&](std::string_view){return true;};
#include "alpha_caller_doors.inc"
            return run_menu_session(ports,MenuId::main,true);
        }

        void serve() {
            const auto socket_value = socket.get();
            try {
                if (owner.caller_interlock) owner.caller_interlock(AlphaCallerBoundary::accepted);
                constexpr std::array<std::uint8_t, 12> negotiation{
                    kTelnetIac, kTelnetWill, kTelnetEcho,
                    kTelnetIac, kTelnetWill, kTelnetSuppressGoAhead,
                    kTelnetIac, kTelnetDo, kTelnetSuppressGoAhead,
                    kTelnetIac, kTelnetDo, 24};
                if (!ssh && !send_raw(socket_value, negotiation.data(), negotiation.size())) return;
                const std::weak_ptr<application::CallerAssignment> weak = state;
                const auto observe = [weak,this](const std::string_view bytes) {
                    if(message_binary_owned.load())return;
                    if (const auto assignment = weak.lock()) assignment->output(bytes);
                };
                const auto activity = [weak](bool received) {
                    if (const auto assignment = weak.lock()) assignment->payload_activity(received);
                };
                if (ssh) {
                    transport = std::make_unique<AlphaSshStream>(owner.ssh_backend, socket_value, state->stopping, observe,
                        [weak](SshTerminalDimensions dimensions) {
                            if (const auto assignment = weak.lock())
                                assignment->resize(dimensions.columns, dimensions.rows);
                        }, activity, [weak] {
                            if (const auto assignment = weak.lock())
                                assignment->encrypted_transport_established();
                        });
                } else {
                    transport = std::make_unique<AlphaTelnetStream>(socket_value, state->stopping, observe, activity);
                }
                auto& stream = *transport;
                if (ssh) static_cast<AlphaSshStream&>(stream).enable_menu_polling();
                else static_cast<AlphaTelnetStream&>(stream).enable_menu_polling();
                accounted_stream=std::make_unique<MenuModalStream>(stream,[&]{return calling_time_tick();},[&] {
                    if(post_login_inactivity) post_login_inactivity->keyboard(owner.clock.monotonic_millis());
                },true);
                login_stream = std::make_unique<application::LoginInactivityStream>(
                    *accounted_stream, login_afk_seconds, [] { return GetTickCount64(); },[&] {
                        show_inactivity_display(stream,login_text_style==application::TerminalTextStyle::ogsf_ansi
                            ? application::GraphicsMode::ansi : application::GraphicsMode::ascii);
                    },[&]{return calling_time!=nullptr;});
                const auto logical_node = core::NodeNumber::create(state->identity().node);
                if (!logical_node || !configuration_snapshot || !owner.credential_bridge ||
                    !owner.rates || !time_context || !time_context->local || !durable) {
                    return;
                }
                caller_session = std::make_unique<core::Session>(
                    std::string(ssh ? "alpha-ssh-" : "alpha-telnet-") +
                    std::to_string(state->identity().generation), *logical_node,
                    core::TransportMetadata{ssh ? core::TransportKind::ssh : core::TransportKind::telnet, source_ip, {}, "ansi"});
                auto& session = *caller_session;
                const application::CallerPasswordObserver capture_password=[this](application::AccountId id,
                    const application::CredentialEnvelope& proof,std::string_view password){caller_login_secret.capture(id,proof,password);};
                application::Phase3aNonNetworkComposition composition({owner.storage,
                    *owner.credential_bridge, owner.storage, owner.storage, owner.storage, owner.storage, owner.admission,
                    owner.storage, alpha_displays, *owner.rates, *time_context->local,
                    owner, *time_context->local, owner.clock, owner.identifiers, this,
                    state->cleanup_health, &durable->new_names(),capture_password});
                request.source_ip = source_ip;
                request.correlation_id = owner.identifiers.generate();
                if (!request.correlation_id.valid()) return;
                log_context.correlation_id = request.correlation_id;
                application::StreamCallerTerminalOptions terminal_options;
                terminal_options.secret_input_echo=application::SecretInputEchoMode::masked;
                terminal_options.caller_text_color=&caller_text_color;
                terminal_options.effective_style=&login_text_style;
                {
                    std::lock_guard<std::mutex> lock(owner.state_mutex);
                    terminal_options.text_color=owner.configuration.menus.text_color;
                    terminal_options.input_color=owner.configuration.menus.input_color;
                }
                try {
                    if (ssh) {
                        caller_terminal = std::make_unique<application::StreamCallerTerminal>(*login_stream,
                            terminal_options);
                        login = std::make_unique<application::Phase3aTransportPasswordLogin>(
                            application::Phase3aExistingCallerDependencies{*caller_terminal, owner.storage,
                            *owner.credential_bridge, owner.storage, owner.storage, owner.storage, owner.admission, owner.storage,
                            alpha_displays, *owner.rates, *time_context->local, owner,
                            owner.clock, owner.identifiers, this, state->cleanup_health,
                            &durable->new_names(),capture_password}, *configuration_snapshot, session,
                            request, state->cancellation);
                        std::string ssh_diagnostic;
                        const auto banner = configuration_snapshot->allow_new_users()
                            ? std::string{}
                            : application::closed_registration_ssh_banner(
                                configuration_snapshot->prelogin_identity().bbs_name);
                        if (!static_cast<AlphaSshStream&>(stream).establish(*login, ssh_diagnostic, banner)) {
                            state->diagnose(application::CallerDiagnostic::transport_failure);
                            if (const auto* ending = login->preauthentication_end()) {
                                log_preauthentication_end(*ending);
                                return;
                            }
                            // This is the transport's closed set of internal codes,
                            // never library exception text or authentication input.
                            log_context.diagnostic_code = ssh_diagnostic;
                            log(application::OperationalKind::ssh_setup_ended,
                                state->stopping.load() ? application::OperationalOutcome::shutdown :
                                application::OperationalOutcome::unavailable);
                            return;
                        }
                        if (owner.caller_interlock) owner.caller_interlock(AlphaCallerBoundary::shell_ready);
                        // Only the authoritative provisional-registration branch
                        // may challenge. Verified existing SSH accounts never do.
                        // No caller/account authority is conferred by this gate.
                        if (login->new_registration_pending() && !challenge()) return;
                        result = login->continue_on_shell();
                    } else {
                        if (!challenge()) return;
                        result = composition.run_existing(*login_stream, *configuration_snapshot,
                            session, request, state->cancellation,
                            terminal_options);
                    }
                    if (result.code == application::Phase3aWorkflowCode::registration_requested &&
                        result.registration) {
                        // Provisional SSH transport auth and Telnet's New Caller
                        // choice converge here. Neither has authenticated authority.
                        result = composition.run_registration(*login_stream, *configuration_snapshot,
                            session, {*result.registration, source_ip, request.correlation_id},
                            state->cancellation, terminal_options);
                    }
                } catch (...) {
                    if (alpha_displays.ansi_selected()) {
                        (void)write_all(stream, "\x1b[0m");
                    }
                    throw;
                }
                // Take ownership before any further output can fail. An abandoned
                // result also owns its durable guard, including failed handoff paths.
                if (result.ready) ready = result.ready->take();
                if (ready && owner.caller_interlock) owner.caller_interlock(AlphaCallerBoundary::ready_handoff);
                const bool reset_written = !alpha_displays.ansi_selected() ||
                    write_all(stream, "\x1b[0m");
                if (!ready) {
                    if (result.code == application::Phase3aWorkflowCode::cleanup_failure)
                        state->cleanup_health->failed.store(true);
                    state->diagnose(application::CallerDiagnostic::authentication_ended);
                    if (result.code == application::Phase3aWorkflowCode::authentication_denied) {
                        log(application::OperationalKind::authentication_denied);
                    } else if (result.preauthentication_end) {
                        log_preauthentication_end(*result.preauthentication_end);
                    } else {
                        log(application::OperationalKind::connection_ended, workflow_reason(result.code));
                    }
                    return;
                }
                if (!reset_written) return;
                if (!ready->admission_lease || !ready->durable_cleanup) {
                    state->cleanup_health->failed.store(true);
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock(owner.state_mutex);
                    const bool sysop = ready->security_level >=
                        configuration_snapshot->security_policy().sysop_security_threshold;
                    if (state->stopping.load() || !owner.revocations.permits(
                            revocation_checkpoint, ready->account_id, sysop,
                            ready->durable_session_id)) {
                        cancel();
                        return;
                    }
                    published_account = ready->account_id;
                    published_session = ready->durable_session_id;
                    published_sysop = sysop;
                    authenticated_as_sysop = sysop;
                    live_security_level.store(ready->security_level);
                    live_sysop.store(sysop);
                    if (!owner.refresh_connection_access_locked(*this) || state->stopping.load()) { cancel(); return; }
                    authenticated_afk_minutes = owner.configuration.caller_afk_minutes;
                    state->authenticated(ready->display_name,ready->account_id);
                }
                log_context.caller = ready->display_name; log_context.account_id = ready->account_id;
                log_context.durable_session_id = ready->durable_session_id;
                log_context.correlation_id = ready->correlation_id;
                log(application::OperationalKind::authenticated);
                stage(application::CallerStage::main_menu);
                main_menu_utc = owner.clock.unix_seconds();
                journal_main_menu();

                menus_active=true;
                if (owner.caller_interlock) owner.caller_interlock(AlphaCallerBoundary::menu);
                try {
                    (void)session.set_activity({core::ActivityKind::menu, "alpha.main_menu"});
                    if (ssh) static_cast<AlphaSshStream&>(stream).enable_menu_polling();
                    else static_cast<AlphaTelnetStream&>(stream).enable_menu_polling();
                    normal=run_menus(*accounted_stream);
                } catch (...) {
                    state->diagnose(application::CallerDiagnostic::session_failure);
                }

                if (ssh) static_cast<AlphaSshStream&>(stream).finish_shell(normal);
                log(application::OperationalKind::connection_ended, normal ? application::OperationalOutcome::normal :
                    state->stopping.load() ? application::OperationalOutcome::shutdown : application::OperationalOutcome::disconnected);
            } catch (...) {
                state->diagnose(application::CallerDiagnostic::session_failure);
                log(application::OperationalKind::session_failed);
            }
        }


    };

    // Two fixed denial workers (one per transport). Telnet retains at most four
    // waiting notices so a peer's bounded close-drain cannot silence normal retries.
    // SSH retains its one-job/no-queue and handshake-rate bounds. Neither path
    // constructs a Connection, allocates a node or calls login.
    // Slow/scanning refused peers cannot occupy an acceptor or caller worker.
    struct Denial final {
        Denial(OwnedSocket accepted, std::string name)
            : socket(std::move(accepted)), bbs_name(std::move(name)) {}
        OwnedSocket socket;
        const std::string bbs_name;
    };

    void deny(OwnedSocket accepted, const bool ssh) {
        const std::size_t index = ssh ? 1 : 0;
        std::lock_guard<std::mutex> lock(state_mutex);
        if (stopping.load()) return;
        if (denials[index]) {
            if (!ssh && pending_telnet_denials.size() < 4)
                pending_telnet_denials.push_back(std::make_shared<Denial>(
                    std::move(accepted), configuration.bbs_name));
            return; // excess beyond the fixed budget still closes without work
        }
        const auto now = GetTickCount64();
        // In addition to concurrency/byte/time bounds, at most four expensive
        // denial handshakes per second globally. Excess is closed without work.
        if (ssh && now < next_ssh_denial) return;
        denials[index] = std::make_shared<Denial>(std::move(accepted), configuration.bbs_name);
        if (ssh) next_ssh_denial = now + 250;
        work_changed.notify_all();
    }

    void denial_loop(const std::size_t index) noexcept {
        ++live_node_workers; // resource counter includes bounded denial workers
        struct WorkerExit { ~WorkerExit() { --live_node_workers; } } worker_exit;
        for (;;) {
            std::shared_ptr<Denial> denied;
            {
                std::unique_lock<std::mutex> lock(state_mutex);
                work_changed.wait(lock, [&] { return stopping.load() || denials[index]; });
                denied = denials[index];
                if (!denied) return;
            }
            try {
                DWORD timeout = 200;
                const auto peer = denied->socket.get();
                if (setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != SOCKET_ERROR &&
                    setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != SOCKET_ERROR) {
                    // No graphics preference exists before admission. Never infer ANSI
                    // from arbitrary negotiation, nor ask an interactive refusal question.
                    auto reader = open_native_display_reader(
                        database_path.parent_path().parent_path()/"Display");
                    auto notice = application::offline_notice(denied->bbs_name, reader.get());
                    if (index == 1) {
                        // SSH disconnect text is UTF-8, not CP437. Fall back for an
                        // incompatible/oversize custom ASCII body; no shell is granted.
                        if (notice.size() > 200 || std::any_of(notice.begin(), notice.end(),
                                [](unsigned char byte) { return byte >= 0x7f; }))
                            notice = application::offline_notice(denied->bbs_name, nullptr);
                        AlphaSshStream refusal(ssh_backend, peer, stopping, {}, {});
                        (void)refusal.deny_capacity(notice);
                    } else {
                        // Telnet escapes literal IAC even in a custom CP437 display.
                        // Leave one leading blank row for clients that scroll once
                        // to insert their disconnected status bar (SyncTERM without
                        // a reserved status row). Plain CRLF needs no ANSI inference.
                        std::string encoded{"\r\n"};
                        for (const unsigned char byte : notice) {
                            encoded.push_back(static_cast<char>(byte));
                            if (byte == kTelnetIac) encoded.push_back(static_cast<char>(byte));
                        }
                        notice = std::move(encoded);
                        std::size_t sent = 0;
                        const auto deadline = GetTickCount64() + 1000;
                        while (sent < notice.size() && !stopping.load() && GetTickCount64() < deadline) {
                            const int written = send(peer, notice.data() + sent,
                                static_cast<int>(notice.size() - sent), 0);
                            if (written <= 0) break;
                            sent += static_cast<std::size_t>(written);
                        }
                        // Some clients start their terminal renderer only after their
                        // connection workers are running. An immediate FIN can win that
                        // race even when all notice bytes reached the socket. Keep the
                        // send half alive briefly BEFORE FIN, not just during teardown.
                        // No caller/input authority, negotiation or interactive prompt:
                        // discard at most 64 KiB, never extend the absolute deadline,
                        // and interrupt promptly for stop or a peer that has closed.
                        if (sent == notice.size()) {
                            const auto render_deadline = GetTickCount64() + 250;
                            std::size_t budget = 65536;
                            std::array<char, 1024> discard{};
                            while (!stopping.load() && budget) {
                                const auto now = GetTickCount64();
                                if (now >= render_deadline) break;
                                fd_set readable; FD_ZERO(&readable); FD_SET(peer, &readable);
                                timeval wait{};
                                wait.tv_usec = static_cast<long>(std::min<ULONGLONG>(25, render_deadline - now) * 1000);
                                const int ready = select(0, &readable, nullptr, nullptr, &wait);
                                if (ready == SOCKET_ERROR) break;
                                if (!ready) continue;
                                const int received = recv(peer, discard.data(),
                                    static_cast<int>(std::min(budget, discard.size())), 0);
                                SecureZeroMemory(discard.data(), discard.size());
                                if (received <= 0) break;
                                budget -= static_cast<std::size_t>(received);
                            }
                        }
                    }
                    // Complete queued refusal bytes before close even if the
                    // peer has concurrently sent negotiation/service data.
                    // Drain is bounded, never interpreted, retained or logged.
                    shutdown(peer, SD_SEND);
                    const auto deadline = GetTickCount64() + 250;
                    std::size_t remaining = 65536;
                    std::array<char, 1024> discard{};
                    while (!stopping.load() && remaining && GetTickCount64() < deadline) {
                        const int received = recv(peer, discard.data(), static_cast<int>(discard.size()), 0);
                        SecureZeroMemory(discard.data(), discard.size());
                        if (received <= 0) break;
                        remaining -= std::min(remaining, static_cast<std::size_t>(received));
                    }
                }
            } catch (...) { /* bounded refusal owns no caller authority */ }
            denied->socket.close(); // SSH session cleanup completed first
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                denials[index].reset();
                if (index == 0 && !stopping.load() && !pending_telnet_denials.empty()) {
                    denials[index] = std::move(pending_telnet_denials.front());
                    pending_telnet_denials.pop_front();
                }
            }
        }
    }

    void accept_loop(const SOCKET server_socket, const bool ssh,
        const std::shared_ptr<std::atomic<bool>>& cancellation) noexcept {
        try {
            const std::string protocol = ssh ? "SSH" : "Telnet";
            const std::string prefix = ssh ? "ssh_" : "telnet_";
            const auto cancelled = [&] {
                return cancellation && cancellation->load();
            };
            while (!stopping.load() && !cancelled()) {
                fd_set readable; FD_ZERO(&readable); FD_SET(server_socket, &readable);
                timeval timeout{}; timeout.tv_usec = 100000;
                const int ready = select(0, &readable, nullptr, nullptr, &timeout);
                if (ready == 0) continue;
                if (ready == SOCKET_ERROR) {
                    if (!stopping.load() && !cancelled())
                        set_diagnostic(prefix + "accept_wait_failed");
                    break;
                }
                sockaddr_in peer{}; int peer_size = sizeof(peer);
                OwnedSocket accepted(accept(server_socket, reinterpret_cast<sockaddr*>(&peer), &peer_size));
                if (accepted.get() == INVALID_SOCKET) {
                    if (!stopping.load() && !cancelled())
                        set_diagnostic(prefix + "accept_failed");
                    break;
                }
                const auto attempt = journal->next_attempt();
                application::OperationalContext refused;
                refused.attempt = attempt;
                refused.transport = ssh ? application::OperationalTransport::ssh : application::OperationalTransport::telnet;
                refused.source_ip = peer_ipv4(peer); refused.peer_port = ntohs(peer.sin_port);
                refused.local_port = ssh ? configuration.ssh.port : configuration.telnet.port;
                u_long blocking = 0;
                DWORD receive_timeout = 250, send_timeout = 5000;
                if (ioctlsocket(accepted.get(), FIONBIO, &blocking) == SOCKET_ERROR ||
                    setsockopt(accepted.get(), SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout)) == SOCKET_ERROR ||
                    setsockopt(accepted.get(), SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<const char*>(&send_timeout), sizeof(send_timeout)) == SOCKET_ERROR) {
                    add_log(application::OperationalKind::socket_failed, refused);
                    continue;
                }
                const auto source = peer_ipv4(peer);
                if (source.empty()) {
                    add_log(application::OperationalKind::socket_failed, refused);
                    continue;
                }
                std::shared_ptr<Connection> connection;
                auto refusal = application::OperationalKind::runtime_refused;
                bool security_refusal=false;
                application::BotGateConfiguration admission_policy;
                bool admission_enabled{};
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    admission_policy=configuration.botgate;
                    admission_enabled=application::botgate_filters_enabled(configuration.botgate,configuration.telnet.bot_gate_enabled,ssh);
                }
                VerifiedDomain verified{true,{}};
                const auto numeric_peer=application::parse_botgate_ipv4(source);
                const auto rule_snapshot=rules->snapshot();
                const auto rule_now=static_cast<std::int64_t>(std::time(nullptr));
                const bool exempt=admission_enabled && rule_snapshot->exempt(source,rule_now);
                if(admission_enabled) journal->gate_count(0);
                const bool arrival_refused=admission_enabled && !exempt && rule_snapshot->refusal(source,"",rule_now).empty() &&
                    !rules->arrive(source,GetTickCount64()/1000,rule_now);
                if (admission_enabled && !exempt && !arrival_refused && !rule_snapshot->lists[3].empty() && numeric_peer)
                    verified=verify_peer_domain(*numeric_peer,stopping);
                if (caller_interlock) caller_interlock(AlphaCallerBoundary::before_assignment);
                {
                    // Admission and publication linearize with disable/stop.
                    // A slot has one worker and no queue behind its assignment.
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (stopping.load()) break;
                    const auto ip=application::parse_botgate_ipv4(source);
                    const auto downloaded=feed ? feed->snapshot() : nullptr;
                    static const std::vector<std::uint32_t> empty_feed;
                    const bool enabled=application::botgate_filters_enabled(configuration.botgate,configuration.telnet.bot_gate_enabled,ssh);
                    auto reason=enabled ? rule_snapshot->refusal(source,verified.completed ? verified.name : "",rule_now) : std::string{};
                    if(enabled && !rule_snapshot->ready) reason="rules_unavailable";
                    if(enabled && !exempt && reason.empty() && arrival_refused) reason="temporary_rate_ban";
                    if(enabled && !exempt && reason.empty() && ip && downloaded &&
                        !application::botgate_patterns_match(rule_snapshot->lists[5],source,rule_now) &&
                        std::binary_search(downloaded->begin(),downloaded->end(),*ip)) reason="downloaded_feed";
                    security_refusal=!reason.empty();
                    if(security_refusal) refused.diagnostic_code=reason=="ip.can" ? "manual_ip_rule" :
                        reason=="ip-silent.can" ? "silent_ip_rule" : reason=="host.can" ? "verified_domain_rule" :
                        reason=="temp_ip.can" ? "temporary_ban" : reason.compare(0,4,"geo/")==0 ? "geo_country_rule" : reason;
                    if(enabled && exempt) {auto exempt_context=refused;exempt_context.diagnostic_code="filter_exemption";
                        add_log(application::OperationalKind::botgate_bypassed,std::move(exempt_context));}
                    if (!security_refusal && !cleanup_failed && !draining && !event_held) {
                        auto allocated = pool.allocate(source,enabled && exempt);
                        refusal = allocated.code == application::NodeAllocationCode::source_limit ?
                            application::OperationalKind::source_refused : application::OperationalKind::capacity_refused;
                        if (allocated.lease) {
                            try {
                                auto state = std::make_shared<application::CallerAssignment>(
                                    std::move(*allocated.lease), pool, clock, ssh, source);
                                try { connection = std::make_shared<Connection>(*this,
                                    std::move(accepted), state, source, ssh, attempt, refused.peer_port, refused.local_port); }
                                catch (...) { (void)state->retire(true); throw; }
                                active_contexts[state->identity().node - 1] = connection;
                            } catch (...) {
                                allocated.lease.reset();
                                for (const auto& slot : pool.snapshot().slots)
                                    cleanup_failed = cleanup_failed || slot.quarantined;
                                throw;
                            }
                        }
                    }
                }
                if (!connection) {
                    if (security_refusal) {
                        refusal=application::OperationalKind::botgate_blocked;
                    } else deny(std::move(accepted), ssh);
                    if(security_refusal && refused.diagnostic_code=="silent_ip_rule") journal->gate_count(4);
                    else add_log(refusal, std::move(refused));
                    continue;
                }
                work_changed.notify_all();
            }
        } catch (...) {
            try { set_diagnostic(ssh ? "ssh_acceptor_failure" : "telnet_acceptor_failure"); } catch (...) {}
        }
        bool owned_current_socket = false;
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            auto& listener_socket = ssh ? ssh_listen_socket : listen_socket;
            auto& listener_cancellation = ssh ? ssh_acceptor_cancellation
                                              : acceptor_cancellation;
            if (listener_socket == server_socket) {
                closesocket(listener_socket);
                listener_socket = INVALID_SOCKET;
                listener_cancellation.reset();
                owned_current_socket = true;
            }
        }
        if (owned_current_socket)
            (ssh ? ssh_listening : listening).store(false);
    }

    void run_connection(const std::shared_ptr<Connection>& connection) noexcept {
        try {
            connection->log(application::OperationalKind::connection_assigned);
            connection->serve();
        } catch (...) {
            connection->state->diagnose(application::CallerDiagnostic::session_failure);
        }
        if (!connection->ended_recorded)
            connection->log(application::OperationalKind::connection_ended,
                connection->state->stopping.load() ? application::OperationalOutcome::shutdown :
                application::OperationalOutcome::none);
        if(connection->log_context.account_id>0 && journal)
            journal->sound(connection->ssh?application::SoundEvent::ssh_logout:application::SoundEvent::telnet_logout);
        const bool clean = connection->cleanup();
        unsubscribe_alpha_hostname(connection->hostname_subscription);
        bool retired = false;
        try {
            std::lock_guard<std::mutex> lock(state_mutex);
            retired = connection->state->retire(clean);
            const auto index = connection->state->identity().node - 1;
            nodes[index] = connection->state->snapshot();
            cleanup_failed = cleanup_failed || !retired;
            if (!retired) quarantined_contexts[index] = connection;
            if (active_contexts[index] == connection) active_contexts[index].reset();
            if (cleanup_failed) diagnostic_code = "caller_cleanup_failed";
        } catch (...) {
            // Never return an unverified assignment to the allocator.
            std::lock_guard<std::mutex> lock(state_mutex);
            cleanup_failed = true;
            diagnostic_code = "caller_cleanup_failed";
        }
        // No success record before actual node-lease release; no file I/O under
        // service/pool locks. The owned connection retains its historical context.
        connection->log(retired ? application::OperationalKind::assignment_released :
            application::OperationalKind::assignment_quarantined);
        work_changed.notify_all();
    }

    application::OperationalContext local_context(const LocalReservation& reservation) const {
        application::OperationalContext context;
        const auto identity = reservation.lease.identity();
        context.epoch = identity.runtime_epoch;
        context.node = identity.node;
        context.generation = identity.generation;
        context.attempt = reservation.attempt;
        context.transport = application::OperationalTransport::telnet;
        context.source_ip = "127.0.0.1";
        context.local_port = reservation.port;
        return context;
    }

    void reset_local_node_locked(const std::size_t index,
        const std::uint64_t generation) noexcept {
        application::AlphaNodeStatus reset;
        reset.number = static_cast<std::uint16_t>(index + 1U);
        reset.assignment_sequence = generation;
        reset.enabled = pool.snapshot().slots[index].enabled;
        reset.available = reset.enabled;
        reset.state = reset.enabled ? core::NodeState::ready : core::NodeState::offline;
        reset.activity = reset.enabled ? "Idle" : "Disabled";
        reset.details = reset.enabled ? "Ready for connection" : "Disabled by Sysop";
        nodes[index] = std::move(reset);
    }

    bool release_local_locked(const std::size_t index,
        const application::OperationalKind kind,
        const application::OperationalOutcome outcome) noexcept {
        auto reservation = std::move(local_reservations[index]);
        if (!reservation) return false;
        const auto identity = reservation->lease.identity();
        const auto context = local_context(*reservation);
        reservation->listener.close();
        const bool released = reservation->lease.release_after_cleanup();
        reset_local_node_locked(index, identity.generation);
        if (!released) {
            cleanup_failed = true;
            diagnostic_code = "caller_cleanup_failed";
            nodes[index].available = false;
            nodes[index].state = core::NodeState::offline;
            nodes[index].activity = "Unavailable";
            nodes[index].details = "Cleanup failed; node unavailable";
        }
        add_log(kind, context, outcome);
        work_changed.notify_all();
        return released;
    }

    application::AlphaLocalSessionStartResult start_local_session(
        const std::uint16_t number) {
        try {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (number == 0 || number > configuration.logical_nodes || stopping.load() ||
                cleanup_failed || draining || event_held) return {{application::LocalControlResponseCode::invalid_request,
                    "node_not_ready"}};
            const auto index = static_cast<std::size_t>(number - 1U);
            if (!nodes[index].enabled || nodes[index].state != core::NodeState::ready ||
                active_contexts[index] || local_reservations[index]) {
                return {{application::LocalControlResponseCode::invalid_request,
                    "node_not_ready"}};
            }
            const bool exempt=configuration.botgate.enabled && configuration.telnet.bot_gate_enabled && rules->snapshot()->exempt("127.0.0.1",static_cast<std::int64_t>(std::time(nullptr)));
            auto allocated = pool.allocate_exact(number, "127.0.0.1",exempt);
            if (!allocated.lease) return {{application::LocalControlResponseCode::invalid_request,
                allocated.code == application::NodeAllocationCode::source_limit ?
                    "local_source_limit" : "node_not_ready"}};

            SOCKET candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (candidate == INVALID_SOCKET) {
                (void)allocated.lease->release_after_cleanup();
                return {{application::LocalControlResponseCode::internal_failure,
                    "local_socket_creation_failed"}};
            }
            OwnedSocket listener(candidate);
            BOOL exclusive = TRUE;
            u_long nonblocking = 1;
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = 0;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int address_size = sizeof(address);
            if (setsockopt(candidate, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                    reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR ||
                bind(candidate, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
                listen(candidate, 1) == SOCKET_ERROR ||
                ioctlsocket(candidate, FIONBIO, &nonblocking) == SOCKET_ERROR ||
                getsockname(candidate, reinterpret_cast<sockaddr*>(&address), &address_size) == SOCKET_ERROR ||
                address.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || ntohs(address.sin_port) == 0) {
                listener.close();
                (void)allocated.lease->release_after_cleanup();
                return {{application::LocalControlResponseCode::internal_failure,
                    "local_bind_or_listen_failed"}};
            }
            const auto identity = allocated.lease->identity();
            const auto attempt = journal->next_attempt();
            const auto port = ntohs(address.sin_port);
            std::unique_ptr<LocalReservation> reservation;
            try {
                reservation = std::make_unique<LocalReservation>(std::move(*allocated.lease),
                    std::move(listener), GetTickCount64() +
                        application::kAlphaLocalSessionTimeoutMilliseconds,
                    attempt, port);
            } catch (...) {
                listener.close();
                if (allocated.lease) (void)allocated.lease->release_after_cleanup();
                return {{application::LocalControlResponseCode::internal_failure,
                    "local_session_start_failed"}};
            }
            const auto context = local_context(*reservation);
            auto& node = nodes[index];
            node.number = number;
            node.assignment_sequence = identity.generation;
            node.screen_owner = identity;
            node.available = node.enabled = node.screen_active = true;
            node.state = core::NodeState::connecting;
            node.connection = "Telnet";
            node.source_ip = "127.0.0.1";
            node.activity = "Waiting for local Telnet client";
            node.details = "One-use loopback reservation";
            local_reservations[index] = std::move(reservation);
            add_log(application::OperationalKind::local_session_requested, context);
            add_log(application::OperationalKind::local_session_reserved, context);
            work_changed.notify_all();
            return {{application::LocalControlResponseCode::ok, "local_session_reserved"},
                "127.0.0.1", port, identity,
                application::kAlphaLocalSessionTimeoutMilliseconds};
        } catch (...) {
            return {{application::LocalControlResponseCode::internal_failure,
                "local_session_start_failed"}};
        }
    }

    application::AlphaControlResponse cancel_local_session(
        const application::AlphaLocalSessionCancelRequest& request) {
        try {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!request.reservation.valid() ||
                request.reservation.node > configuration.logical_nodes) {
                return {application::LocalControlResponseCode::invalid_request,
                    "local_reservation_invalid"};
            }
            const auto index = static_cast<std::size_t>(request.reservation.node - 1U);
            if (!local_reservations[index] ||
                !(local_reservations[index]->lease.identity() == request.reservation)) {
                return {application::LocalControlResponseCode::invalid_request,
                    "local_reservation_stale"};
            }
            const auto kind = request.reason == application::AlphaLocalSessionCancelReason::client_launch_failed
                ? application::OperationalKind::local_session_launch_failed
                : application::OperationalKind::local_session_cancelled;
            const auto outcome = request.reason == application::AlphaLocalSessionCancelReason::client_launch_failed
                ? application::OperationalOutcome::unavailable
                : application::OperationalOutcome::cancelled;
            if (!release_local_locked(index, kind, outcome)) {
                return {application::LocalControlResponseCode::storage_failure,
                    "caller_cleanup_failed"};
            }
            return {application::LocalControlResponseCode::ok, "local_session_cancelled"};
        } catch (...) {
            return {application::LocalControlResponseCode::internal_failure,
                "local_session_cancel_failed"};
        }
    }

    void local_session_loop() noexcept {
        ++live_node_workers;
        struct WorkerExit { ~WorkerExit() { --live_node_workers; } } worker_exit;
        try {
            for (;;) {
                std::unique_lock<std::mutex> lock(state_mutex);
                work_changed.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    return stopping.load() || std::any_of(local_reservations.begin(),
                        local_reservations.end(), [](const auto& item) { return item != nullptr; });
                });
                bool any = false;
                for (std::size_t index = 0; index < local_reservations.size(); ++index) {
                    auto& reservation = local_reservations[index];
                    if (!reservation) continue;
                    any = true;
                    if (stopping.load()) {
                        (void)release_local_locked(index,
                            application::OperationalKind::local_session_cancelled,
                            application::OperationalOutcome::shutdown);
                        continue;
                    }
                    if (GetTickCount64() >= reservation->expires_at) {
                        (void)release_local_locked(index,
                            application::OperationalKind::local_session_timed_out,
                            application::OperationalOutcome::timed_out);
                        continue;
                    }
                    fd_set readable;
                    FD_ZERO(&readable);
                    FD_SET(reservation->listener.get(), &readable);
                    timeval timeout{};
                    timeout.tv_usec = 25000;
                    const int ready = select(0, &readable, nullptr, nullptr, &timeout);
                    if (ready != 1) continue;

                    sockaddr_in peer{};
                    int peer_size = sizeof(peer);
                    OwnedSocket accepted(accept(reservation->listener.get(),
                        reinterpret_cast<sockaddr*>(&peer), &peer_size));
                    const auto identity = reservation->lease.identity();
                    const auto attempt = reservation->attempt;
                    const auto local_port = reservation->port;
                    reservation->listener.close(); // the first accept consumes the endpoint
                    if (accepted.get() == INVALID_SOCKET ||
                        peer.sin_family != AF_INET ||
                        peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
                        (void)release_local_locked(index,
                            application::OperationalKind::socket_failed,
                            application::OperationalOutcome::unavailable);
                        continue;
                    }
                    u_long blocking = 0;
                    DWORD receive_timeout = 250, send_timeout = 5000;
                    if (ioctlsocket(accepted.get(), FIONBIO, &blocking) == SOCKET_ERROR ||
                        setsockopt(accepted.get(), SOL_SOCKET, SO_RCVTIMEO,
                            reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout)) == SOCKET_ERROR ||
                        setsockopt(accepted.get(), SOL_SOCKET, SO_SNDTIMEO,
                            reinterpret_cast<const char*>(&send_timeout), sizeof(send_timeout)) == SOCKET_ERROR) {
                        (void)release_local_locked(index,
                            application::OperationalKind::socket_failed,
                            application::OperationalOutcome::unavailable);
                        continue;
                    }
                    std::shared_ptr<application::CallerAssignment> state;
                    const auto local_rules=rules->snapshot();
                    const auto local_now=static_cast<std::int64_t>(std::time(nullptr));
                    VerifiedDomain local_domain{true,{}};
                    if (configuration.botgate.enabled && configuration.telnet.bot_gate_enabled &&
                        !local_rules->exempt("127.0.0.1",local_now) && !local_rules->lists[3].empty()) {
                        lock.unlock(); local_domain=verify_peer_domain(0x7f000001U,stopping); lock.lock();
                        if (!reservation || reservation->lease.identity().generation!=identity.generation) continue;
                    }
                    const auto downloaded=feed ? feed->snapshot() : nullptr;
                    static const std::vector<std::uint32_t> empty_feed;
                    const bool gate_enabled=configuration.botgate.enabled && configuration.telnet.bot_gate_enabled;
                    if(gate_enabled) journal->gate_count(0);
                    if(gate_enabled && local_rules->exempt("127.0.0.1",local_now)) {
                        application::OperationalContext exempt_context;exempt_context.attempt=attempt;exempt_context.source_ip="127.0.0.1";
                        exempt_context.transport=application::OperationalTransport::telnet;exempt_context.diagnostic_code="filter_exemption";
                        add_log(application::OperationalKind::botgate_bypassed,std::move(exempt_context));
                    }
                    auto reason=gate_enabled ? local_rules->refusal("127.0.0.1",local_domain.completed ? local_domain.name : "",local_now) : std::string{};
                    if(gate_enabled && !local_rules->ready) reason="rules_unavailable";
                    if(gate_enabled && !local_rules->exempt("127.0.0.1",local_now) && reason.empty() && downloaded &&
                        !application::botgate_patterns_match(local_rules->lists[5],"127.0.0.1",local_now) &&
                        std::binary_search(downloaded->begin(),downloaded->end(),0x7f000001U)) reason="downloaded_feed";
                    if (!reason.empty()) {
                        application::OperationalContext blocked;
                        blocked.attempt=attempt; blocked.transport=application::OperationalTransport::telnet;
                        blocked.source_ip="127.0.0.1"; blocked.diagnostic_code=reason=="downloaded_feed" ? reason :
                            reason=="rules_unavailable" ? reason : "local_botgate_rule";
                        if(reason=="ip-silent.can") journal->gate_count(4);
                        else add_log(application::OperationalKind::botgate_blocked,std::move(blocked));
                        (void)release_local_locked(index,application::OperationalKind::local_session_cancelled,
                            application::OperationalOutcome::denied);
                        continue;
                    }
                    try {
                        state = std::make_shared<application::CallerAssignment>(
                            std::move(reservation->lease), pool, clock, false, "127.0.0.1");
                        auto connection = std::make_shared<Connection>(*this, std::move(accepted),
                            state, "127.0.0.1", false, attempt, ntohs(peer.sin_port), local_port);
                        local_reservations[index].reset();
                        active_contexts[index] = std::move(connection);
                        work_changed.notify_all();
                    } catch (...) {
                        local_reservations[index].reset();
                        const bool retired = state && state->retire(true);
                        reset_local_node_locked(index, identity.generation);
                        if (!retired) {
                            cleanup_failed = true;
                            diagnostic_code = "caller_cleanup_failed";
                            nodes[index].available = false;
                            nodes[index].state = core::NodeState::offline;
                            nodes[index].activity = "Unavailable";
                            nodes[index].details = "Cleanup failed; node unavailable";
                        }
                    }
                }
                if (stopping.load() && !any) return;
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(state_mutex);
            cleanup_failed = true;
            diagnostic_code = "caller_cleanup_failed";
        }
    }

    void node_loop(const std::size_t index) noexcept {
        ++live_node_workers;
        struct WorkerExit { ~WorkerExit() { --live_node_workers; } } worker_exit;
        for (;;) {
            std::shared_ptr<Connection> connection;
            {
                std::unique_lock<std::mutex> lock(state_mutex);
                work_changed.wait(lock, [&] { return stopping.load() || active_contexts[index]; });
                connection = active_contexts[index];
                if (!connection) return;
            }
            run_connection(connection);
            // All protocol resources have gone before the assignment is retired.
            connection.reset();
            std::lock_guard<std::mutex> lock(state_mutex);
            if (cleanup_failed) return;
        }
    }

    bool start_workers() {
        try {
            local_worker = std::thread([this] { local_session_loop(); });
            for (std::size_t index = 0; index < denial_workers.size(); ++index)
                denial_workers[index] = std::thread([this, index] { denial_loop(index); });
            for (std::size_t index = 0; index < configuration.logical_nodes; ++index) {
                if (caller_interlock) caller_interlock(AlphaCallerBoundary::worker_start);
                node_workers[index] = std::thread([this, index] { node_loop(index); });
            }
            return true;
        } catch (...) {
            set_diagnostic("node_worker_start_failed");
            return false; // caller stop joins even a partially created worker set
        }
    }

    bool refresh_connection_access_locked(Connection& connection) {
        const auto account = storage.find_account_by_id(connection.published_account);
        const auto policy = storage.security_policy();
        if (!account || !account.value || !policy || !policy.value ||
            account.value->state != application::AccountState::active || !account.value->security_level) {
            connection.cancel(); return account && policy;
        }
        const auto level = *account.value->security_level;
        const bool sysop = level >= policy.value->sysop_threshold;
        if(level!=999) {
            std::size_t sessions=0;
            for(const auto& peer:active_contexts)
                if(peer && peer->published_account==connection.published_account && !peer->state->stopping.load()) ++sessions;
            if(sessions>1) {
                // Losing the 999-only concurrency permission revokes all of
                // this account's existing parallel sessions. Do not invent a
                // preferred survivor or let a downgrade retain extra callers.
                for(const auto& peer:active_contexts)
                    if(peer && peer->published_account==connection.published_account) peer->cancel();
                return true;
            }
        }
        if (sysop && !connection.authenticated_as_sysop) {
            // A promotion must not manufacture completion of the global SY
            // challenge. Reauthentication is limited to this affected caller.
            connection.cancel(); return true;
        }
        connection.live_security_level.store(level);
        connection.live_sysop.store(sysop);
        if(!sysop) connection.authenticated_as_sysop=false;
        connection.published_sysop = sysop;
        return true;
    }

    bool refresh_account_access(application::AccountId account_id) {
        std::lock_guard<std::mutex> lock(state_mutex);
        bool ok = !cleanup_failed;
        for (const auto& connection : active_contexts)
            if (connection && connection->published_account == account_id)
                ok = refresh_connection_access_locked(*connection) && ok;
        return ok;
    }

    bool account_online(application::AccountId account_id) const {
        std::lock_guard<std::mutex> lock(state_mutex);
        for (const auto& connection : active_contexts)
            if (connection && connection->published_account == account_id && !connection->state->stopping.load()) return true;
        return false;
    }

    std::optional<application::MessageActor> sysop_actor(const application::ChatIdentity& identity) {
        std::lock_guard<std::mutex> lock(state_mutex);
        if(stopping.load()||cleanup_failed||!identity.node||identity.node>active_contexts.size())return {};
        const auto& connection=active_contexts[identity.node-1];
        if(!connection||connection->state->stopping.load()||!connection->authenticated_as_sysop||
            !connection->live_sysop.load()||connection->published_account!=identity.account||
            connection->published_session!=identity.session)return {};
        const auto account=storage.find_account_by_id(identity.account);const auto policy=storage.security_policy();
        if(!account||!account.value||!policy||!policy.value||account.value->state!=application::AccountState::active||
            !account.value->security_level||*account.value->security_level<policy.value->sysop_threshold)return {};
        application::MessageActor actor{identity.session,identity.account,account.value->revision,policy.value->revision};
        const auto authorized=storage.message_delete_authority(actor);
        return authorized&&authorized.value&&*authorized.value?std::optional<application::MessageActor>{actor}:std::nullopt;
    }
    bool revoke(const application::SessionRevocationCommand& command) override {return revoke_sessions(command);}

    bool refresh_security_policy(const application::OperationalConfiguration& candidate) {
        auto snapshot = make_phase3a_configuration(candidate);
        if (!snapshot) return false;
        std::lock_guard<std::mutex> lock(state_mutex);
        configuration.security_policy_revision=candidate.security_policy_revision;
        configuration.security_policy_hash=candidate.security_policy_hash;
        configuration.configuration_revision=candidate.configuration_revision;
        phase3a_configuration=std::move(snapshot);
        bool ok = !cleanup_failed;
        for (const auto& connection : active_contexts)
            if (connection && connection->published_account)
                ok=refresh_connection_access_locked(*connection) && ok;
        return ok;
    }

    bool revoke_sessions(const application::SessionRevocationCommand& command) {
        std::unique_lock<std::mutex> lock(state_mutex);
        if (cleanup_failed || !revocations.record(command)) return false;
        std::array<std::shared_ptr<Connection>, application::kAlphaNodePoolCapacity> affected{};
        for (std::size_t index = 0; index < active_contexts.size(); ++index) {
            const auto& connection = active_contexts[index];
            if (connection && connection->published_account &&
                application::SessionRevocationLedger::matches(command,
                    connection->published_account, connection->published_sysop,
                    connection->published_session)) {
                affected[index] = connection;
                connection->cancel();
            }
        }
        // The identity comparison does not wait for a subsequent caller that
        // legitimately reuses the node. A timeout is never reported as success.
        const bool retired = work_changed.wait_for(lock, std::chrono::seconds(10), [&] {
            if (cleanup_failed) return true;
            for (std::size_t index = 0; index < affected.size(); ++index)
                if (affected[index] && active_contexts[index] == affected[index]) return false;
            return true;
        });
        if (!retired) {
            pool.stop(); // do not admit more work over uncertain revocation
            cleanup_failed = true;
            diagnostic_code = "caller_cleanup_failed";
        }
        return retired && !cleanup_failed;
    }

    bool set_node_enabled_locked(std::unique_lock<std::mutex>& lock, const std::uint16_t number, const bool enabled) {
        if (number == 0 || number > configuration.logical_nodes || stopping.load() || cleanup_failed)
            return false;
        const auto index = number - 1;
        if (!pool.set_enabled(number, enabled)) return false;
        if (!enabled && local_reservations[index])
            (void)release_local_locked(index, application::OperationalKind::local_session_cancelled,
                application::OperationalOutcome::cancelled);
        if (!enabled && active_contexts[index]) active_contexts[index]->cancel();
        if (!enabled && !work_changed.wait_for(lock, std::chrono::seconds(10), [&] {
                return !active_contexts[index] || cleanup_failed;
            })) {
            pool.stop();
            cleanup_failed = true;
            diagnostic_code = "caller_cleanup_failed";
        }
        if (cleanup_failed) return false;
        nodes[index].enabled = enabled;
        nodes[index].state = enabled ? core::NodeState::ready : core::NodeState::offline;
        nodes[index].activity = enabled ? "Idle" : "Disabled";
        nodes[index].details = enabled ? "Ready for connection" : "Disabled by Sysop";
        return true;
    }

    bool set_node_enabled(const std::uint16_t number, const bool enabled) {
        std::unique_lock<std::mutex> lock(state_mutex);
        return set_node_enabled_locked(lock, number, enabled);
    }

    application::AlphaControlResponse disable_observed_node(const std::uint16_t number,
        const std::uint64_t expected_generation, const std::function<application::StorageStatus()>& persist) {
        using Code = application::LocalControlResponseCode;
        std::unique_lock<std::mutex> lock(state_mutex);
        if (!number || number > configuration.logical_nodes || stopping.load() || cleanup_failed)
            return {Code::invalid_request, "node_not_controllable"};
        const auto index = number - 1;
        const auto& connection = active_contexts[index];
        const auto observed = connection ? connection->state->snapshot() : application::AlphaNodeStatus{};
        const auto generation = observed.caller_connected ? observed.assignment_sequence : 0;
        if (generation != expected_generation)
            return {Code::invalid_request, "node_caller_changed"};
        // Both public admission and local-reservation handoff use state_mutex.
        // Keep the reviewed assignment stable until admission closes for this node.
        const auto stored = persist();
        if (!stored) return {Code::storage_failure, stored.operation};
        if (!set_node_enabled_locked(lock, number, false))
            return {Code::storage_failure, "caller_cleanup_failed"};
        return {Code::ok, "node_disabled"};
    }

    bool set_draining(const bool value, const std::uint64_t deadline) {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (stopping.load() || cleanup_failed) return false;
        draining = value;
        drain_deadline.store(value ? deadline : 0);
        if (value) for (std::size_t index = 0; index < local_reservations.size(); ++index)
            if (local_reservations[index])
                (void)release_local_locked(index, application::OperationalKind::local_session_cancelled,
                    application::OperationalOutcome::shutdown);
        return !cleanup_failed;
    }

    application::AlphaControlResponse disconnect_caller(
        const application::NodeAssignmentIdentity identity) {
        std::unique_lock<std::mutex> lock(state_mutex);
        if (!identity.valid() || identity.node > configuration.logical_nodes ||
            stopping.load() || cleanup_failed)
            return {application::LocalControlResponseCode::invalid_request, "caller_not_controllable"};
        const auto index = identity.node - 1U;
        if (local_reservations[index] && local_reservations[index]->lease.identity() == identity) {
            const bool released = release_local_locked(index,
                application::OperationalKind::local_session_cancelled,
                application::OperationalOutcome::cancelled);
            return {released ? application::LocalControlResponseCode::ok :
                application::LocalControlResponseCode::storage_failure,
                released ? "local_reservation_cancelled" : "caller_cleanup_failed"};
        }
        const auto affected = active_contexts[index];
        if (!affected || !(affected->state->identity() == identity))
            return {application::LocalControlResponseCode::invalid_request, "caller_assignment_changed"};
        affected->cancel();
        // Wait only for this captured assignment, not a replacement caller.
        const bool retired = work_changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return active_contexts[index] != affected || cleanup_failed;
        });
        if (!retired) {
            pool.stop();
            cleanup_failed = true;
            diagnostic_code = "caller_cleanup_failed";
        }
        return {retired && !cleanup_failed ? application::LocalControlResponseCode::ok :
            application::LocalControlResponseCode::storage_failure,
            retired && !cleanup_failed ? "caller_disconnected" : "caller_cleanup_failed"};
    }

    void stop() noexcept {
        if (stopping.exchange(true)) return;
        stop_alpha_hostname_lookup(hostname_lifetime);
        const bool was_listening = listening.load();
        const bool was_ssh_listening = ssh_listening.load();
        application::OperationalContext telnet_log, ssh_log;
        telnet_log.transport = application::OperationalTransport::telnet;
        ssh_log.transport = application::OperationalTransport::ssh;
        if (was_listening) add_log(application::OperationalKind::listener_stopping, telnet_log);
        if (was_ssh_listening) add_log(application::OperationalKind::listener_stopping, ssh_log);
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            pool.stop();
            for (std::size_t index = 0; index < local_reservations.size(); ++index)
                if (local_reservations[index])
                    (void)release_local_locked(index,
                        application::OperationalKind::local_session_cancelled,
                        application::OperationalOutcome::shutdown);
            for (const auto& connection : active_contexts)
                if (connection) connection->cancel();
            for (const auto& denial : denials)
                if (denial) denial->socket.interrupt();
            pending_telnet_denials.clear(); // close only these owned, unadmitted sockets
        }
        work_changed.notify_all();
        listening.store(false);
        ssh_listening.store(false);
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            if (listen_socket != INVALID_SOCKET) {
                shutdown(listen_socket, SD_BOTH);
            }
            if (ssh_listen_socket != INVALID_SOCKET) {
                shutdown(ssh_listen_socket, SD_BOTH);
            }
        }
        // The acceptors own final close. Never let a closed/recycled listener
        // handle be used by an accept/select that was already in flight.
        if (worker.joinable()) worker.join();
        if (ssh_worker.joinable()) ssh_worker.join();
        if (local_worker.joinable()) local_worker.join();
        for (auto& denial_worker : denial_workers)
            if (denial_worker.joinable()) denial_worker.join();
        for (auto& node_worker : node_workers)
            if (node_worker.joinable()) node_worker.join();

        // A successful stop is an ownership assertion, not just joined threads.
        // Failed authoritative cleanup keeps its assignment and faults Runtime.
        if (pool.snapshot().active != 0 || admission.active_leases() != 0) {
            std::lock_guard<std::mutex> lock(state_mutex);
            cleanup_failed = true;
        }

        if (winsock_started) {
            WSACleanup();
            winsock_started = false;
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            diagnostic_code = cleanup_failed ? "caller_cleanup_failed" : "stopped";
            if (!cleanup_failed) {
                for (auto& node : nodes) {
                    node.state = core::NodeState::offline;
                    node.activity = "Stopped";
                    node.details = "Caller listeners stopped";
                }
            }
        }
        sysop_work.stop();
        if (was_listening) add_log(application::OperationalKind::listener_stopped, telnet_log);
        if (was_ssh_listening) add_log(application::OperationalKind::listener_stopped, ssh_log);
    }

    adapters::storage::SqliteStorage& storage;
    SysopWork sysop_work;
    std::weak_ptr<AlphaFtnEngine> sysop_ftn;
    application::AuthenticationSecurityService& authentication;
    application::IPasswordKdf& kdf;
    application::IIdentifierGenerator& identifiers;
    application::OperationalConfiguration configuration;
    application::RuntimeStatisticsPublisher statistics;
    std::filesystem::path database_path;
    std::wstring installation_owner_sid;
    const std::shared_ptr<const AlphaSshBackend> ssh_backend;
    SystemClock clock;
    std::shared_ptr<AlphaOperationalJournal> journal;
    std::shared_ptr<AlphaDurableJournal> durable;
    std::shared_ptr<AlphaBotGateFeed> feed;
    std::shared_ptr<AlphaBotGateRules> rules;
    bool owns_durable{false}, durable_started{false};
    application::HostIndependentAccountSessionAdmissionCoordinator admission;
    std::unique_ptr<adapters::authentication::Phase3aCredentialOperationsBridge>
        credential_bridge;
    std::unique_ptr<adapters::authentication::Phase3aRateControlCoordinator> rates;
    std::shared_ptr<TimeContext> time_context;
    std::shared_ptr<const application::Phase3aConfigurationSnapshot>
        phase3a_configuration;

    const AlphaCallerInterlock caller_interlock;
    application::FourNodePool pool;
    application::SessionRevocationLedger revocations;
    application::DoorReservations door_reservations;
    application::ChatHub chat_hub;
    std::array<std::shared_ptr<Connection>, application::kAlphaNodePoolCapacity> active_contexts;
    std::array<std::unique_ptr<LocalReservation>, application::kAlphaNodePoolCapacity> local_reservations;
    std::array<std::shared_ptr<Connection>, application::kAlphaNodePoolCapacity> quarantined_contexts;
    std::array<std::thread, application::kAlphaNodePoolCapacity> node_workers;
    std::array<std::thread, 2> denial_workers;
    std::array<std::shared_ptr<Denial>, 2> denials;
    std::deque<std::shared_ptr<Denial>> pending_telnet_denials;
    std::uint64_t next_ssh_denial{0};
    std::condition_variable work_changed;
    bool cleanup_failed{false};
    bool draining{false}; // guarded by state_mutex with admission publication
    std::atomic<std::uint64_t> drain_deadline{0};
    bool event_held{};
    std::atomic<std::int64_t> event_cutoff_utc{};
    mutable std::mutex state_mutex;
    mutable std::mutex socket_mutex;
    std::string diagnostic_code{"not_started"};
    SOCKET listen_socket{INVALID_SOCKET};
    SOCKET ssh_listen_socket{INVALID_SOCKET};
    std::thread worker;
    std::thread ssh_worker;
    std::thread local_worker;
    std::shared_ptr<std::atomic<bool>> acceptor_cancellation;
    std::shared_ptr<std::atomic<bool>> ssh_acceptor_cancellation;
    std::atomic<bool> stopping{false};
    std::atomic<bool> listening{false};
    std::atomic<bool> ssh_listening{false};
    std::array<application::AlphaNodeStatus, application::kAlphaNodePoolCapacity> nodes;
    bool winsock_started{false};
    const std::uint64_t started_at{GetTickCount64()};
    std::uint64_t hostname_lifetime{};
};

AlphaTelnetListener::AlphaTelnetListener(
    adapters::storage::SqliteStorage& storage,
    application::AuthenticationSecurityService& authentication,
    application::IPasswordKdf& kdf,
    application::IIdentifierGenerator& identifiers,
    application::OperationalConfiguration configuration,
    application::AlphaRuntimeStatistics initial_statistics,
    std::filesystem::path installation_root,
    std::wstring installation_owner_sid,
    std::shared_ptr<const AlphaSshBackend> ssh_backend, AlphaCallerInterlock interlock,
    application::NodeEnablement enabled, application::Identifier128 runtime_epoch,
    std::array<std::uint64_t, application::kAlphaNodePoolCapacity> previous_generations,
    std::shared_ptr<AlphaOperationalJournal> journal, std::shared_ptr<AlphaDurableJournal> durable,
    std::shared_ptr<AlphaBotGateFeed> feed)
        : impl_(std::make_unique<Impl>(storage, authentication, kdf, identifiers,
          std::move(configuration), std::move(initial_statistics), std::move(installation_root),
          std::move(installation_owner_sid), std::move(ssh_backend), std::move(interlock), enabled,
          runtime_epoch, previous_generations, std::move(journal), std::move(durable),std::move(feed))) {}

void AlphaTelnetListener::attach_sysop_ftn(std::shared_ptr<AlphaFtnEngine> engine){std::lock_guard<std::mutex> lock(impl_->state_mutex);impl_->sysop_ftn=std::move(engine);}
AlphaTelnetListener::~AlphaTelnetListener() {
    stop();
    if (impl_->owns_durable) impl_->durable->stop();
}

AlphaCallerResourceCounts alpha_caller_resource_counts() noexcept {
    return {live_node_workers.load(), live_contexts.load(), live_owned_sockets.load()};
}

application::ChatConfigurationResult AlphaTelnetListener::change_chat_configuration(const application::ChatConfiguration& c){
    const auto result=impl_->chat_hub.change_configuration(c,[&]{return impl_->storage.change_chat_configuration({true,{}},c).succeeded();});
    return {result,impl_->chat_hub.configuration()};
}
application::ChatOperatorSnapshot AlphaTelnetListener::chat_operator(const application::ChatOperatorRequest& r){
    using namespace application;
    if(encode_chat_operator_request(r).empty()||impl_->stopping.load())return {};
    const auto now=impl_->clock.monotonic_millis();auto& hub=impl_->chat_hub;hub.tick(now);
    if(r.action==ChatOperatorAction::status)return hub.operator_status();
    if(r.action==ChatOperatorAction::availability){hub.operator_available(r.available);return hub.operator_status();}
    if(r.action==ChatOperatorAction::poll)return hub.operator_poll(r.token,r.view,r.sequence,now);
    if(r.action==ChatOperatorAction::end){hub.operator_end(r.token,r.view,now);return hub.operator_poll(r.token,r.view,0,now);}
    if(r.action==ChatOperatorAction::send){ChatOperatorSnapshot out;out.code=hub.operator_send(r.token,r.view,r.sequence,r.text,now);return out;}
    ChatIdentity target;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if(!r.target||r.target->node>impl_->configuration.logical_nodes)return {};
        const auto caller=impl_->active_contexts[r.target->node-1];
        if(!caller||!(caller->state->identity()==*r.target)||caller->state->stopping.load()||!caller->published_session.valid())return {};
        target={caller->published_session,caller->published_account,r.target->node};
    }
    return hub.operator_start(target,r.view,r.page,r.override_dnd,now);
}
application::ChatResult AlphaTelnetListener::send_chat_notice(const application::ChatNoticeRequest& r){
    if(application::encode_chat_notice(r).empty()||impl_->stopping.load())return application::ChatResult::invalid;
    std::optional<application::ChatIdentity> target;
    if(r.target){
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if(r.target->node>impl_->configuration.logical_nodes)return application::ChatResult::unavailable;
        const auto caller=impl_->active_contexts[r.target->node-1];
        if(!caller||!(caller->state->identity()==*r.target)||caller->state->stopping.load()||!caller->published_session.valid())return application::ChatResult::unavailable;
        target=application::ChatIdentity{caller->published_session,caller->published_account,r.target->node};
    }
    if(r.room){const auto config=impl_->chat_hub.configuration();if(std::none_of(config.rooms.begin(),config.rooms.end(),[&](const auto& room){return room.id==r.room&&room.enabled;}))return application::ChatResult::unavailable;}
    return impl_->chat_hub.notice(target,r.room,r.text,r.override_dnd);
}
bool AlphaTelnetListener::start() {
    const auto chat=impl_->storage.chat_configuration({true,{}});
    if(!chat||!chat.value||!impl_->chat_hub.configure(*chat.value)){impl_->set_diagnostic("chat_configuration_unavailable");return false;}
    if (impl_->owns_durable && !impl_->durable_started) {
        if (!impl_->durable->start()) {
            impl_->set_diagnostic("caller_journal_authoritative_recovery_failed"); return false;
        }
        impl_->durable_started = true;
    }
    if (!impl_->configuration.telnet.enabled && !impl_->configuration.ssh.enabled) {
        impl_->set_diagnostic("caller_listeners_disabled");
        return true;
    }
    if (!impl_->initialize_dependencies()) {
        std::string diagnostic;
        {
            std::lock_guard<std::mutex> lock(impl_->state_mutex);
            diagnostic = impl_->diagnostic_code;
        }
        application::OperationalContext context;
        context.diagnostic_code = diagnostic;
        impl_->add_log(application::OperationalKind::runtime_failed, std::move(context));
        return false;
    }
    if (impl_->configuration.ssh.enabled && !impl_->ssh_backend) {
        impl_->set_diagnostic("ssh_backend_required");
        return false;
    }
    impl_->hostname_lifetime = start_alpha_hostname_lookup(impl_->configuration.hostname_lookup);
    if (!impl_->start_workers() ||
        (impl_->configuration.telnet.enabled && !impl_->start_socket()) ||
        (impl_->configuration.ssh.enabled && !impl_->start_socket(true))) {
        std::string diagnostic;
        {
            std::lock_guard<std::mutex> lock(impl_->state_mutex);
            diagnostic = impl_->diagnostic_code;
        }
        application::OperationalContext context;
        context.diagnostic_code = diagnostic;
        impl_->add_log(application::OperationalKind::listener_failed, std::move(context));
        return false;
    }
    return true;
}

bool AlphaTelnetListener::probe_bind(
    const application::TelnetOperationalConfiguration& configuration,
    std::string& diagnostic_code) noexcept {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        diagnostic_code = "winsock_startup_failed";
        return false;
    }
    const SOCKET candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (candidate == INVALID_SOCKET) {
        WSACleanup(); diagnostic_code = "telnet_socket_creation_failed"; return false;
    }
    BOOL exclusive = TRUE;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(configuration.port);
    const bool valid = setsockopt(candidate, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != SOCKET_ERROR &&
        InetPtonA(AF_INET, configuration.bind_address.c_str(), &address.sin_addr) == 1 &&
        bind(candidate, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != SOCKET_ERROR;
    closesocket(candidate);
    WSACleanup();
    diagnostic_code = valid ? "bind_available" : "telnet_bind_or_listen_failed";
    return valid;
}

bool AlphaTelnetListener::reconfigure_listener_endpoints(
    const application::OperationalConfiguration& configuration,
    std::string& diagnostic_code) noexcept {
    try {
        return impl_->reconfigure_listener_endpoints(configuration, diagnostic_code);
    } catch (...) {
        diagnostic_code = "listener_reconfiguration_failed_prior_endpoint_retained";
        return false;
    }
}

bool AlphaTelnetListener::reconfigure_live_settings(
    const application::OperationalConfiguration& configuration,
    std::string& diagnostic_code) noexcept {
    return impl_->reconfigure_live_settings(configuration, diagnostic_code);
}

application::StorageStatus AlphaTelnetListener::clear_throttle(
    const application::LocalThrottleClearRequest& request,
    const application::AuthenticationAuditContext& audit) {
    if (impl_->rates) return impl_->rates->clear_locally(request,audit);
    // A ready Runtime may have both public transports disabled and no caller
    // workers. The owning host serializes this against listener activation.
    if (impl_->configuration.telnet.enabled || impl_->configuration.ssh.enabled)
        return {application::StorageErrorCode::internal,"rate coordinator unavailable"};
    adapters::authentication::Phase3aRateControlCoordinator offline_rates(
        impl_->storage,audit.occurred_utc_seconds,4095);
    return offline_rates.clear_locally(request,audit);
}

application::AlphaLocalSessionStartResult AlphaTelnetListener::start_local_session(
    const std::uint16_t node) {
    return impl_->start_local_session(node);
}

application::AlphaControlResponse AlphaTelnetListener::cancel_local_session(
    const application::AlphaLocalSessionCancelRequest& request) {
    return impl_->cancel_local_session(request);
}

void AlphaTelnetListener::record_operational_log(
    application::OperationalKind kind, application::OperationalContext context) noexcept {
    impl_->add_log(kind, std::move(context));
}

void AlphaTelnetListener::stop() noexcept {
    impl_->stop();
    if (impl_->owns_durable) impl_->durable->stop();
}

bool AlphaTelnetListener::set_node_enabled(std::uint16_t node, bool enabled) {
    return impl_->set_node_enabled(node, enabled);
}

application::AlphaControlResponse AlphaTelnetListener::disable_observed_node(std::uint16_t node,
    std::uint64_t expected_generation, const std::function<application::StorageStatus()>& persist) {
    return impl_->disable_observed_node(node, expected_generation, persist);
}

bool AlphaTelnetListener::set_draining(const bool draining, const std::uint64_t deadline) {
    return impl_->set_draining(draining, deadline);
}
void AlphaTelnetListener::set_external_event_gate(bool held, std::int64_t cutoff_utc) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (held && !impl_->event_held) {
        for (std::size_t i = 0; i < impl_->local_reservations.size(); ++i) {
            if (impl_->local_reservations[i])
                (void)impl_->release_local_locked(i, application::OperationalKind::local_session_cancelled,
                    application::OperationalOutcome::shutdown);
        }
    }
    impl_->event_held = held;
    impl_->event_cutoff_utc.store(cutoff_utc);
}
void AlphaTelnetListener::end_calls_for_external_event() {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    for (const auto& caller : impl_->active_contexts) if (caller) caller->cancel();
}
bool AlphaTelnetListener::external_event_callers_empty() const {
    return impl_->pool.snapshot().active == 0 && impl_->admission.active_leases() == 0;
}

application::AlphaControlResponse AlphaTelnetListener::disconnect_caller(
    const application::NodeAssignmentIdentity identity) {
    return impl_->disconnect_caller(identity);
}

bool AlphaTelnetListener::revoke_sessions(const application::SessionRevocationCommand& command) {
    return impl_->revoke_sessions(command);
}
application::AlphaControlResponse AlphaTelnetListener::terminate_door(application::NodeAssignmentIdentity identity,std::uint64_t launch){
    std::shared_ptr<Impl::Connection> caller;
    {std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if(!identity.valid()||identity.node>impl_->configuration.logical_nodes||!launch||impl_->stopping.load())return {application::LocalControlResponseCode::invalid_request,"door_not_controllable"};
        caller=impl_->active_contexts[identity.node-1];
        if(!caller||!(caller->state->identity()==identity)||!caller->state->request_door_termination(launch))return {application::LocalControlResponseCode::invalid_request,"door_launch_changed"};
    }
    const auto deadline=GetTickCount64()+10000;
    while(GetTickCount64()<deadline){if(caller->state->snapshot().door_launch_sequence!=launch)return {application::LocalControlResponseCode::ok,"door_terminated"};Sleep(10);}
    return {application::LocalControlResponseCode::storage_failure,"door_termination_unconfirmed"};
}

bool AlphaTelnetListener::refresh_account_access(application::AccountId id) { return impl_->refresh_account_access(id); }
bool AlphaTelnetListener::account_online(application::AccountId id) const { return impl_->account_online(id); }
bool AlphaTelnetListener::refresh_security_policy(const application::OperationalConfiguration& value) {
    return impl_->refresh_security_policy(value);
}

AlphaTelnetListenerStatus AlphaTelnetListener::status() const {
    AlphaTelnetListenerStatus result;
    if (take_alpha_hostname_exhaustion_warning())
        impl_->add_log(application::OperationalKind::hostname_exhausted);
    result.listening = impl_->listening.load();
    result.ssh_listening = impl_->ssh_listening.load();
    result.port = impl_->configuration.telnet.port;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        result.diagnostic_code = impl_->diagnostic_code;
        const auto pool = impl_->pool.snapshot();
        for (std::size_t index = 0; index < impl_->configuration.logical_nodes; ++index) {
            const auto& context = impl_->active_contexts[index];
            if (context || impl_->local_reservations[index]) ++result.active_connections;
            if (context) context->refresh_hostname();
            auto node = context ? context->state->snapshot() : impl_->nodes[index];
            node.enabled = pool.slots[index].enabled;
            if (!context && !node.enabled && node.available) {
                node.state = core::NodeState::offline;
                node.activity = "Disabled";
                node.details = "Disabled by Sysop";
            } else if (!context && impl_->event_held && node.available && node.enabled) {
                node.available = false;
                node.state = core::NodeState::offline;
                node.activity = "External event";
                node.details = "Caller admission paused";
            }
            result.nodes.push_back(std::move(node));
        }
    }
    result.logs = impl_->journal->snapshot();
    result.log_health = impl_->journal->health();
    result.statistics = impl_->statistics.snapshot();
    return result;
}

void AlphaTelnetListener::botgate_status(application::BotGateFeedStatus& status) const {
    if(!impl_->rules) return;
    const auto rules=impl_->rules->snapshot();status.rules_ready=rules->ready;
    status.rule_status=impl_->rules->status();
    if(status.rule_status.size()>128 || std::any_of(status.rule_status.begin(),status.rule_status.end(),[](unsigned char c){return c<32 || c>126;})) status.rule_status="rules_reload_failed";
    for(unsigned i=0;i<rules->lists.size();++i) status.rule_counts[i]=rules->lists[i].size();
    for(const auto& geo:rules->geo) {if(!status.geo_summary.empty()) status.geo_summary+=", ";status.geo_summary+=geo.name;}
    if(status.geo_summary.size()>4096) status.geo_summary.resize(4096);
}
}  // namespace sfx::apps::runtime
