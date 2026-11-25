#include "linux_caller_session.hpp"
#include "alpha_telnet_stream.hpp"
#include "sfx/application/admission_notices.hpp"
#include "sfx/application/login_inactivity_stream.hpp"
#include "sfx/platform/network.hpp"
#include <array>
#include <cerrno>

namespace sfx::apps::runtime {
namespace {
bool negotiate(int socket, const std::atomic<bool>& stopping) {
    constexpr std::array<std::uint8_t,12> bytes{
        255,251,1, 255,251,3, 255,253,3, 255,253,24};
    std::size_t offset=0;
    while(offset<bytes.size() && !stopping.load()) {
        const auto count=::send(socket,bytes.data()+offset,bytes.size()-offset,MSG_NOSIGNAL);
        if(count<0 && errno==EINTR) continue;
        if(count<=0) return false;
        offset+=static_cast<std::size_t>(count);
    }
    return offset==bytes.size();
}
bool reset_ansi(application::IInteractiveSessionByteStream& stream) {
    std::string_view remaining="\x1b[0m";
    while(!remaining.empty()) {
        const auto result=stream.write_some(remaining);
        if(!application::valid_interactive_stream_write_result(remaining.size(),result) ||
            result.outcome!=application::InteractiveStreamOutcome::completed || !result.bytes_written)
            return false;
        remaining.remove_prefix(result.bytes_written);
    }
    return true;
}
}
LinuxCallerSessionResult run_linux_caller_session(int socket, bool ssh,
    LinuxCallerSessionPorts ports) {
    using namespace application;
    LinuxCallerSessionResult outcome;
    if(socket<0 || !ports.admission_gate || !ports.menus || !ports.authorize_ready ||
        !ports.request.correlation_id.valid() || !ports.login_idle_seconds ||
        (ssh && !ports.ssh_backend)) {
        outcome.diagnostic="caller_context_invalid";
        return outcome;
    }
    // Bound negotiation before the stream switches this descriptor to polling.
    if(!platform::network::set_timeout(socket,SO_SNDTIMEO,1000) ||
        !platform::network::set_timeout(socket,SO_RCVTIMEO,250) ||
        (!ssh && !negotiate(socket,ports.stopping))) {
        outcome.diagnostic="caller_negotiation_failed";
        return outcome;
    }
    std::unique_ptr<IInteractiveSessionByteStream> transport;
    if(ssh) {
        auto stream=std::make_unique<AlphaSshStream>(ports.ssh_backend,
            static_cast<std::uintptr_t>(socket),ports.stopping,ports.output,
            ports.resize,ports.payload_activity,ports.encrypted);
        stream->enable_menu_polling();
        transport=std::move(stream);
    } else {
        auto stream=std::make_unique<AlphaTelnetStream>(socket,ports.stopping,
            ports.output,ports.payload_activity);
        stream->enable_menu_polling();
        transport=std::move(stream);
    }
    auto& stream=*transport;
    TerminalTextStyle style=TerminalTextStyle::plain;
    ports.terminal_options.effective_style=&style;
    ports.terminal_options.secret_input_echo=SecretInputEchoMode::masked;
    LoginInactivityStream login_stream(stream,ports.login_idle_seconds,
        [&]{return ports.application.clock.monotonic_millis();});
    Phase3aNonNetworkComposition composition(ports.application);
    Phase3aWorkflowResult login_result;
    try {
        if(ssh) {
            auto& d=ports.application;
            StreamCallerTerminal terminal(login_stream,ports.terminal_options);
            Phase3aTransportPasswordLogin login({terminal,d.names,d.credentials,
                d.accounts,d.authentication_storage,d.phase3a_storage,d.admission,
                d.session_commit,d.displays,d.rates,d.local_time,d.welcome_statistics,
                d.clock,d.identifiers,d.stages,d.cleanup_health,d.new_names,d.caller_password},
                ports.configuration,ports.session,ports.request,ports.cancellation);
            const auto banner=ports.configuration.allow_new_users()?std::string{}:
                closed_registration_ssh_banner(ports.configuration.prelogin_identity().bbs_name);
            if(!static_cast<AlphaSshStream&>(stream).establish(login,outcome.diagnostic,banner))
                return outcome;
            if(login.new_registration_pending() && !ports.admission_gate(stream)) {
                outcome.diagnostic="caller_admission_ended";
                return outcome;
            }
            login_result=login.continue_on_shell();
        } else {
            if(!ports.admission_gate(stream)) {
                outcome.diagnostic="caller_admission_ended";
                return outcome;
            }
            login_result=composition.run_existing(login_stream,ports.configuration,
                ports.session,ports.request,ports.cancellation,ports.terminal_options);
        }
        if(login_result.code==Phase3aWorkflowCode::registration_requested && login_result.registration)
            login_result=composition.run_registration(login_stream,ports.configuration,
                ports.session,{*login_result.registration,ports.request.source_ip,
                ports.request.correlation_id},ports.cancellation,ports.terminal_options);
        outcome.login=login_result.code;
        // Acquire the existing durable cleanup/admission guards before output
        // or menu callbacks can fail. Their destructors retain the shared policy.
        outcome.ready=login_result.ready?login_result.ready->take():
            std::optional<AuthenticatedCallerReady>{};
        if(style==TerminalTextStyle::ogsf_ansi && !reset_ansi(stream)) {
            outcome.diagnostic="caller_terminal_ended";
            return outcome;
        }
        if(!outcome.ready || !valid_authenticated_caller_ready(*outcome.ready)) {
            outcome.diagnostic="caller_login_ended";
            return outcome;
        }
        if(ports.stopping.load() || ports.cancellation.is_cancelled()) {
            outcome.diagnostic="caller_shutdown";
            return outcome;
        }
        if(!ports.authorize_ready(*outcome.ready)) {
            outcome.diagnostic="caller_access_revoked";
            return outcome;
        }
        outcome.caller_ready=true;
        outcome.normal_logoff=ports.menus(*outcome.ready,stream);
        if(ssh) static_cast<AlphaSshStream&>(stream).finish_shell(outcome.normal_logoff);
        outcome.diagnostic="caller_session_ended";
        return outcome;
    } catch(...) {
        if(style==TerminalTextStyle::ogsf_ansi) (void)reset_ansi(stream);
        outcome.diagnostic="caller_session_failed";
        return outcome;
    }
}
} // namespace sfx::apps::runtime
