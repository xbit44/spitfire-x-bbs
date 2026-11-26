#include "sfx/application/phase3a_workflow.hpp"
#include "sfx/application/admission_notices.hpp"

#include <cctype>
#include <algorithm>
#include <utility>

#include "sfx/core/node.hpp"
#include "sfx/core/security.hpp"

namespace sfx::application {
namespace {

constexpr std::size_t kRequiredFirstNameMaximum = kAccountNameMaximum - 2U;
struct WorkflowSecretGuard{std::string& value;~WorkflowSecretGuard(){explicitly_erase_terminal_secret(value);}};

bool valid_local_timestamp(const std::string& value) noexcept {
    if (value.empty() || value.size() > 64U) return false;
    for (const unsigned char character : value) {
        if (character < 0x20U || character > 0x7eU) return false;
    }
    return true;
}

AuthenticationProtocol protocol_for(const core::TransportKind kind) {
    return kind == core::TransportKind::ssh
        ? AuthenticationProtocol::ssh
        : AuthenticationProtocol::telnet;
}

std::string transport_label(const core::TransportKind kind) {
    switch (kind) {
        case core::TransportKind::telnet: return "TELNET";
        case core::TransportKind::ssh: return "SSH";
        case core::TransportKind::local: return "LOCAL";
        case core::TransportKind::unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::string transfer_preference_label(
    const TransferProtocolPreference preference) {
    switch (preference) {
        case TransferProtocolPreference::xmodem: return "XMODEM";
        case TransferProtocolPreference::ymodem: return "YMODEM";
        case TransferProtocolPreference::ymodem_g: return "YMODEM-G";
        case TransferProtocolPreference::zmodem: return "ZMODEM";
        case TransferProtocolPreference::select_at_transfer:
            return "Select at time of transfer";
    }
    return {};
}

Phase3aWorkflowCode code_for_terminal(const TerminalOutcome outcome) noexcept {
    switch (outcome) {
        case TerminalOutcome::cancelled: return Phase3aWorkflowCode::cancelled;
        case TerminalOutcome::timed_out: return Phase3aWorkflowCode::timed_out;
        case TerminalOutcome::disconnected: return Phase3aWorkflowCode::disconnected;
        case TerminalOutcome::shutdown_requested:
            return Phase3aWorkflowCode::shutdown_requested;
        case TerminalOutcome::terminal_failure:
        case TerminalOutcome::output_failure: return Phase3aWorkflowCode::terminal_failure;
        case TerminalOutcome::invalid:
        case TerminalOutcome::overlength:
        case TerminalOutcome::empty:
        case TerminalOutcome::completed:
            return Phase3aWorkflowCode::internal_failure;
    }
    return Phase3aWorkflowCode::internal_failure;
}

Phase3aWorkflowCode code_for_rate_gate(const Phase3aRateGateCode code) noexcept {
    switch (code) {
        case Phase3aRateGateCode::admitted: return Phase3aWorkflowCode::internal_failure;
        case Phase3aRateGateCode::cancelled: return Phase3aWorkflowCode::cancelled;
        case Phase3aRateGateCode::timed_out: return Phase3aWorkflowCode::timed_out;
        case Phase3aRateGateCode::disconnected: return Phase3aWorkflowCode::disconnected;
        case Phase3aRateGateCode::shutdown_requested:
            return Phase3aWorkflowCode::shutdown_requested;
        case Phase3aRateGateCode::unavailable:
            return Phase3aWorkflowCode::temporarily_unavailable;
    }
    return Phase3aWorkflowCode::internal_failure;
}

Phase3aWorkflowCode code_for_authentication(
    const AuthenticationOperationCode code) noexcept {
    switch (code) {
        case AuthenticationOperationCode::cancelled:
            return Phase3aWorkflowCode::cancelled;
        case AuthenticationOperationCode::backpressure:
            return Phase3aWorkflowCode::kdf_backpressure;
        case AuthenticationOperationCode::storage_failure:
            return Phase3aWorkflowCode::storage_failure;
        case AuthenticationOperationCode::throttled:
        case AuthenticationOperationCode::unavailable:
            return Phase3aWorkflowCode::temporarily_unavailable;
        case AuthenticationOperationCode::completed:
        case AuthenticationOperationCode::rejected:
        case AuthenticationOperationCode::password_change_required:
        case AuthenticationOperationCode::policy_rejected:
        case AuthenticationOperationCode::malformed_record:
        case AuthenticationOperationCode::unsupported_record:
        case AuthenticationOperationCode::conflict:
        case AuthenticationOperationCode::revocation_failed:
            return Phase3aWorkflowCode::authentication_denied;
    }
    return Phase3aWorkflowCode::internal_failure;
}

SessionOutcomeCode session_outcome_for(const Phase3aWorkflowCode code) noexcept {
    switch (code) {
        case Phase3aWorkflowCode::disconnected: return SessionOutcomeCode::disconnected;
        case Phase3aWorkflowCode::timed_out: return SessionOutcomeCode::timed_out;
        case Phase3aWorkflowCode::shutdown_requested: return SessionOutcomeCode::shutdown;
        default: return SessionOutcomeCode::runtime_error;
    }
}

struct NameInputResult {
    std::optional<std::string> name;
    std::optional<Phase3aWorkflowCode> stop;
};

enum class AttemptAction : std::uint8_t { retry, authenticated, stop };

struct AttemptResult {
    AttemptAction action{AttemptAction::stop};
    Phase3aWorkflowCode code{Phase3aWorkflowCode::internal_failure};
    std::optional<AccountRecord> account;
    bool system_password_verified{false};
    std::optional<std::string> expected_name_key;
    std::optional<std::uint16_t> classified_security_level;

    AttemptResult() = default;
    AttemptResult(
        const AttemptAction result_action,
        const Phase3aWorkflowCode result_code,
        std::optional<AccountRecord> result_account,
        const bool result_system_password_verified,
        std::optional<std::string> result_expected_name_key = std::nullopt,
        const std::optional<std::uint16_t> result_classified_security_level =
            std::nullopt)
        : action(result_action),
          code(result_code),
          account(std::move(result_account)),
          system_password_verified(result_system_password_verified),
          expected_name_key(std::move(result_expected_name_key)),
          classified_security_level(result_classified_security_level) {}
};

struct AdmissionSnapshot {
    AccountRecord account;
    SecurityPolicyRecord policy;
    std::string account_name_key;
};

class ExistingCallerExecution final {
public:
    ExistingCallerExecution(
        Phase3aExistingCallerDependencies dependencies,
        const Phase3aConfigurationSnapshot& configuration,
        core::Session& session,
        const Phase3aExistingCallerRequest& request,
        const CancellationToken& cancellation)
        : dependencies_(dependencies),
          configuration_(configuration),
          session_(session),
          request_(request),
          cancellation_(cancellation) {}

    Phase3aWorkflowResult execute_safely(bool transport = false) noexcept {
        try { return transport ? continue_transport_password() : execute(); }
        catch (...) {
            try { return finish(Phase3aWorkflowCode::internal_failure); }
            catch (...) {
                if (dependencies_.cleanup_health) dependencies_.cleanup_health->failed.store(true);
                return {Phase3aWorkflowCode::cleanup_failure, {}, {}, {}};
            }
        }
    }

    ~ExistingCallerExecution() {
        explicitly_erase_terminal_secret(transport_temporary_password_);
    }

    TransportPasswordResult authenticate_transport_password(
        const std::string_view username, const std::string_view password) {
        if (transport_closed_ || transport_verified_.has_value() || transport_registration_name_ ||
            cancellation_.is_cancelled()) return TransportPasswordResult::disconnect;
        protocol_authentication_ = true;
        if (!transport_initialized_) {
            transport_initialized_ = true;
            if (!valid_start_context() || request_.resume.has_value() ||
                session_.transport().kind != core::TransportKind::ssh ||
                !configuration_.caller_entry_allowed() ||
                !authoritative_policy_matches_configuration()) {
                transport_closed_ = true;
                return TransportPasswordResult::disconnect;
            }
            durable_session_id_ = dependencies_.identifiers.generate();
            if (!durable_session_id_.valid()) {
                transport_closed_ = true;
                return TransportPasswordResult::disconnect;
            }
        }
        supplied_password_ = password;
        const auto normalized = username.size() <= kAccountNameMaximum
            ? normalize_account_name(std::string(username)) : std::nullopt;
        AttemptResult attempt;
        if (normalized.has_value()) {
            const auto classified = dependencies_.names.classify(*normalized);
            if (!classified || !classified.value.has_value() ||
                !classified.value->valid() ||
                classified.value->normalized_name != *normalized) {
                transport_closed_ = true;
                supplied_password_.reset();
                return TransportPasswordResult::disconnect;
            }
            attempt = classified.value->classification == CallerNameClassification::existing
                ? authenticate_existing(*classified.value)
                : authenticate_unavailable(classified.value->classification ==
                    CallerNameClassification::registerable && configuration_.allow_new_users()
                        ? normalized : std::nullopt);
        } else {
            attempt = authenticate_unavailable();
        }
        supplied_password_.reset();
        if (transport_registration_name_) return TransportPasswordResult::accepted;
        if (attempt.action == AttemptAction::authenticated && attempt.account &&
            attempt.expected_name_key && attempt.classified_security_level) {
            transport_verified_ = std::move(attempt);
            return TransportPasswordResult::accepted;
        }
        transport_closed_ = attempt.action != AttemptAction::retry;
        return transport_closed_ ? TransportPasswordResult::disconnect
                                 : TransportPasswordResult::rejected;
    }

    bool new_registration_pending() const noexcept {
        return !transport_closed_ && transport_registration_name_.has_value() &&
            !transport_verified_.has_value() && configuration_.allow_new_users() &&
            !cancellation_.is_cancelled();
    }

    void abort_transport_password() noexcept {
        transport_closed_ = true;
        supplied_password_.reset();
        transport_registration_name_.reset();
        explicitly_erase_terminal_secret(transport_temporary_password_);
    }

    std::string_view registration_refusal_notice() const noexcept { return transport_name_refusal_; }
    const Phase3aPreAuthenticationEnd* preauthentication_end() const noexcept {
        return preauthentication_end_ ? &*preauthentication_end_ : nullptr;
    }

    Phase3aWorkflowResult continue_transport_password() {
        if (transport_closed_ || (!transport_verified_ && !transport_registration_name_)) {
            return {Phase3aWorkflowCode::invalid_context, {}, {}, {}};
        }
        transport_closed_ = true; // single consumption, including failed continuations
        protocol_authentication_ = false;
        if (cancellation_.is_cancelled()) return finish(Phase3aWorkflowCode::cancelled);
        if (transport_registration_name_) {
            const std::string name = std::move(*transport_registration_name_);
            transport_registration_name_.reset();
            const auto current = dependencies_.names.classify(name);
            if (!configuration_.allow_new_users() || !configuration_.caller_entry_allowed() ||
                !authoritative_policy_matches_configuration() || !current || !current.value ||
                !current.value->valid() || current.value->normalized_name != name ||
                current.value->classification != CallerNameClassification::registerable) {
                return finish(Phase3aWorkflowCode::registration_unavailable);
            }
            if (const auto stop = common_presentation(); stop) return finish(*stop);
            return registration_continuation(name);
        }
        AttemptResult verified = std::move(*transport_verified_);
        transport_verified_.reset();
        auto current = dependencies_.accounts.find_account_by_id(verified.account->account_id);
        if (!current || !current.value ||
            current.value->revision != verified.account->revision ||
            !account_matches_classification(*current.value, verified.account->account_id,
                *verified.expected_name_key, *verified.classified_security_level) ||
            !authoritative_policy_matches_configuration()) {
            return finish(Phase3aWorkflowCode::authentication_denied);
        }
        if (const auto stop = common_presentation(); stop) return finish(*stop);
        if (!transport_temporary_password_.empty()) {
            const auto replacement = replace_temporary_password(
                *current.value, transport_temporary_password_);
            explicitly_erase_terminal_secret(transport_temporary_password_);
            if (replacement != Phase3aWorkflowCode::authenticated_caller_ready) {
                return finish(replacement);
            }
            current = dependencies_.accounts.find_account_by_id(verified.account->account_id);
            if (!current || !current.value ||
                !account_matches_classification(*current.value, verified.account->account_id,
                    *verified.expected_name_key, *verified.classified_security_level)) {
                return finish(Phase3aWorkflowCode::storage_failure);
            }
        }
        bool system_verified = false;
        if (next_sysop_sequence_action(true, *current.value->security_level,
                configuration_.security_policy().sysop_security_threshold) ==
                SysopSequenceAction::request_system_password) {
            for (;;) {
                const auto system = authenticate_system_password(*current.value);
                if (system.action == AttemptAction::authenticated) {
                    system_verified = true;
                    break;
                }
                if (system.action == AttemptAction::stop) return finish(system.code);
                // The transport already verified the ordinary account password.
                // Retry only SY:, within the original shared three-failure budget.
            }
        }
        return complete_authenticated_session(std::move(*current.value), system_verified,
            std::move(*verified.expected_name_key), *verified.classified_security_level);
    }

    Phase3aWorkflowResult execute() {
        if (cancellation_.is_cancelled()) return finish(Phase3aWorkflowCode::cancelled);
        if (!valid_start_context()) return finish(Phase3aWorkflowCode::invalid_context);
        if (!configuration_.caller_entry_allowed()) {
            return finish(Phase3aWorkflowCode::bootstrap_incomplete);
        }
        if (!authoritative_policy_matches_configuration()) {
            return finish(Phase3aWorkflowCode::storage_failure);
        }
        durable_session_id_ = dependencies_.identifiers.generate();
        if (!durable_session_id_.valid()) {
            return finish(Phase3aWorkflowCode::internal_failure);
        }
        if (request_.resume.has_value()) {
            if (!request_.resume->valid() ||
                request_.resume->failed_authentication_attempts >=
                    configuration_.maximum_authentication_attempts()) {
                return finish(Phase3aWorkflowCode::invalid_context);
            }
            graphics_ = request_.resume->graphics;
            attempts_.failed_account_attempts =
                request_.resume->failed_authentication_attempts;
        } else if (const auto stop = common_presentation(); stop.has_value()) {
            return finish(*stop);
        }

        for (;;) {
            const NameInputResult entered = read_caller_name();
            if (entered.stop.has_value()) return finish(*entered.stop);
            if (!entered.name.has_value()) return finish(Phase3aWorkflowCode::internal_failure);

            if (const auto stop = present("caller.name.searching",
                    "Searching for " + *entered.name + ", please stand by...\n"); stop)
                return finish(*stop);
            const auto classified = dependencies_.names.classify(*entered.name);
            if (!classified || !classified.value.has_value() ||
                !classified.value->valid() ||
                classified.value->normalized_name != *entered.name) {
                return finish(Phase3aWorkflowCode::storage_failure);
            }
            if (classified.value->classification ==
                    CallerNameClassification::registerable &&
                !request_.resume.has_value()) {
                Phase3aWorkflowResult branch = registration_branch(*entered.name);
                if (branch.code == Phase3aWorkflowCode::internal_failure) continue;
                return branch;
            }

            if (classified.value->classification == CallerNameClassification::existing) {
                // The independent name-entry guard has done its job once a usable
                // returning identity is found. Password retries retain their own
                // established three-attempt budget.
                substantive_name_attempts_ = 0;
            }

            AttemptResult attempt = classified.value->classification ==
                    CallerNameClassification::existing
                ? authenticate_existing(*classified.value)
                : authenticate_unavailable();
            if (attempt.action == AttemptAction::retry) continue;
            if (attempt.action == AttemptAction::stop) return finish(attempt.code);
            if (!attempt.account.has_value() ||
                !attempt.expected_name_key.has_value() ||
                !attempt.classified_security_level.has_value()) {
                return finish(Phase3aWorkflowCode::internal_failure);
            }
            return complete_authenticated_session(
                std::move(*attempt.account), attempt.system_password_verified,
                std::move(*attempt.expected_name_key),
                *attempt.classified_security_level);
        }
    }

private:
    bool valid_start_context() {
        const auto source = canonicalize_source_ip(request_.source_ip);
        if (!source.has_value() || !request_.correlation_id.valid() ||
            session_.state() != core::SessionState::establishing ||
            (session_.transport().kind != core::TransportKind::telnet &&
                session_.transport().kind != core::TransportKind::ssh)) {
            return false;
        }
        source_ip_ = source->text;
        protocol_ = protocol_for(session_.transport().kind);
        return true;
    }

    bool policy_matches_configuration(
        const SecurityPolicyRecord& policy) const noexcept {
        const auto& configured = configuration_.security_policy();
        return policy.revision == configured.revision &&
            policy.policy_hash == configured.policy_hash &&
            policy.new_user_level == configured.new_user_security_level &&
            policy.sysop_threshold == configured.sysop_security_threshold;
    }

    bool authoritative_policy_matches_configuration() {
        const auto policy = dependencies_.accounts.security_policy();
        return policy && policy.value.has_value() &&
            policy_matches_configuration(*policy.value);
    }

    std::optional<AdmissionSnapshot> revalidate_for_admission(
        const AccountRecord& authenticated,
        const bool system_password_verified,
        const std::string& expected_name_key,
        const std::uint16_t classified_security_level) {
        const auto policy = dependencies_.accounts.security_policy();
        if (!policy || !policy.value.has_value() ||
            !policy_matches_configuration(*policy.value)) {
            return std::nullopt;
        }
        auto current = dependencies_.accounts.find_account_by_id(
            authenticated.account_id);
        if (!current || !current.value.has_value() ||
            !valid_current_account(*current.value, authenticated.account_id)) {
            return std::nullopt;
        }
        if (!account_matches_classification(authenticated,
                authenticated.account_id, expected_name_key,
                classified_security_level) ||
            !account_matches_classification(*current.value,
                authenticated.account_id, expected_name_key,
                classified_security_level)) {
            return std::nullopt;
        }
        const bool sysop = *current.value->security_level >= policy.value->sysop_threshold;
        if (sysop != system_password_verified) return std::nullopt;
        return AdmissionSnapshot{std::move(*current.value),
            std::move(*policy.value), expected_name_key};
    }

    std::optional<Phase3aWorkflowCode> present(
        const std::string& identity,
        const std::string& text) {
        if (protocol_authentication_) return std::nullopt;
        const TerminalOutcome outcome = dependencies_.terminal.present({identity, text});
        if (outcome == TerminalOutcome::completed) return std::nullopt;
        return code_for_terminal(outcome);
    }

    TrustedDisplayRenderContext display_context(
        const AccountRecord* const account = nullptr) {
        TrustedDisplayRenderContext context;
        const auto& identity = configuration_.prelogin_identity();
        context.bbs_name = identity.bbs_name;
        context.sysop = identity.sysop_name;
        context.version = identity.version_label;
        context.node = std::to_string(session_.node().value());
        context.nodes =
            std::to_string(configuration_.foundation().logical_nodes);
        context.transport = transport_label(session_.transport().kind);

        const auto local = dependencies_.local_time.format_utc_seconds(
            dependencies_.clock.unix_seconds());
        if (local.has_value() && valid_local_timestamp(*local)) {
            context.datetime = *local;
            const auto separator = local->find(' ');
            if (separator != std::string::npos && separator > 0U &&
                separator + 1U < local->size()) {
                context.date = local->substr(0, separator);
                context.time = local->substr(separator + 1U);
            }
        }

        if (account != nullptr && account->display_name.has_value() &&
            account->security_level.has_value()) {
            const auto first_separator = account->display_name->find(' ');
            context.first_name = account->display_name->substr(
                0, first_separator);
            context.name = *account->display_name;
            context.alias = *account->display_name;
            context.city_state = account->profile.location;
            context.phone_number = account->profile.phone;
            context.email = account->profile.email;
            context.security_level =
                std::to_string(*account->security_level);
            context.logons = std::to_string(
                account->statistics.successful_login_count);
            context.time_on = std::to_string(session_.elapsed_seconds());
            context.ip_address = source_ip_;
            const auto preference =
                dependencies_.phase3a_storage.transfer_preference(
                    account->account_id);
            if (preference && preference.value.has_value() &&
                preference.value->account_id == account->account_id) {
                const auto label = transfer_preference_label(
                    preference.value->preference);
                if (!label.empty()) context.protocol = label;
            }
            if (account->statistics.last_successful_login_utc_seconds
                    .has_value()) {
                const auto last = dependencies_.local_time.format_utc_seconds(
                    *account->statistics.last_successful_login_utc_seconds);
                if (last.has_value() && valid_local_timestamp(*last)) {
                    context.last_call = *last;
                }
            }
        }
        return context;
    }

    std::optional<Phase3aWorkflowCode> present_display_program(
        const std::string& terminal_identity,
        const RenderedTrustedDisplayProgram& program) {
        const TerminalOutcome outcome = dependencies_.terminal.present_display(
            {terminal_identity, &program.terminal_program});
        if (outcome == TerminalOutcome::completed) return std::nullopt;
        return code_for_terminal(outcome);
    }

    std::optional<Phase3aWorkflowCode> present_optional_display(
        const TrustedDisplayIdentity identity,
        const GraphicsMode graphics,
        const std::string& terminal_identity,
        const std::optional<std::uint16_t> security_level = std::nullopt,
        const AccountRecord* const account = nullptr) {
        const TrustedDisplaySelectionResult selected = dependencies_.displays.select(
            {identity, graphics, security_level, display_context(account)});
        if (!selected.available()) return std::nullopt;
        if (!selected.program.has_value() ||
            selected.program->identity != identity ||
            selected.program->graphics != graphics) {
            return std::nullopt;
        }
        return present_display_program(terminal_identity, *selected.program);
    }

    std::optional<Phase3aWorkflowCode> common_presentation() {
        if (dependencies_.stages) dependencies_.stages->stage(CallerStage::graphics);
        const auto plain = dependencies_.terminal.set_text_style(TerminalTextStyle::plain);
        if (plain != TerminalOutcome::completed) return code_for_terminal(plain);
        const auto& identity = configuration_.prelogin_identity();
        const std::string built_in = "Your Sysop is " + identity.sysop_name +
            "\nSPITFIRE-X Bulletin Board System - Version " +
            identity.version_label +
            "\nIn memory of Mike Woltz and Spitfire BBS"
            "\nCopyright (C) 2026 Spitfire-X BBS Project\n\n";
        const TrustedDisplaySelectionResult selected = dependencies_.displays.select(
            {TrustedDisplayIdentity::sfprelog, GraphicsMode::ascii,
                std::nullopt, display_context()});
        const bool valid_custom = selected.available() &&
            selected.program.has_value() &&
            selected.program->identity == TrustedDisplayIdentity::sfprelog &&
            selected.program->graphics == GraphicsMode::ascii &&
            valid_rendered_trusted_display_program(*selected.program);
        if (valid_custom) {
            auto prelog=*selected.program;
            auto& program=prelog.terminal_program;
            // Keep authored content, but own the boundary: exactly one empty
            // row before the always-present hardcoded system message.
            while(!program.operations.empty()) {
                auto& last=program.operations.back();
                if(last.kind==TerminalDisplayOperationKind::logical_newline ||
                    last.kind==TerminalDisplayOperationKind::paging_newline) {
                    program.output_byte_count-=2;program.operations.pop_back();continue;
                }
                if(last.kind==TerminalDisplayOperationKind::cp437_text) {
                    while(!last.bytes.empty() && last.bytes.back()==' ') {last.bytes.pop_back();--program.output_byte_count;}
                    if(last.bytes.empty()) {program.operations.pop_back();continue;}
                }
                break;
            }
            for(unsigned n=0;n<2;++n) program.operations.push_back({TerminalDisplayOperationKind::logical_newline,{}});
            program.output_byte_count+=4;
            if (valid_rendered_trusted_display_program(prelog)) {
              if (const auto stop = present_display_program(
                    "prelogin.sfprelog", prelog);
                stop.has_value()) {
                return stop;
              }
            }
        }
        if (const auto stop = present("prelogin.builtin", built_in);
            stop.has_value()) {
            return stop;
        }
        const TerminalKeyRequest request{"prelogin.graphics",
            "Do you want ANSI COLOR GRAPHICS? [Y/n] ",
            {'Y', 'y', 'N', 'n'}, 'Y'};
        const TerminalKeyResult graphics = dependencies_.terminal.read_key(request);
        if (!valid_terminal_key_result(request, graphics) || !graphics.has_key())
            return code_for_terminal(graphics.outcome);
        graphics_ = (*graphics.key == 'Y' || *graphics.key == 'y')
            ? GraphicsMode::ansi : GraphicsMode::ascii;
        const auto styled = dependencies_.terminal.set_text_style(graphics_ == GraphicsMode::ansi
            ? TerminalTextStyle::ogsf_ansi : TerminalTextStyle::plain);
        if (styled != TerminalOutcome::completed) return code_for_terminal(styled);
        TerminalDisplayProgram identification;
        using Op=TerminalDisplayOperationKind;
        const auto add=[&](Op kind,std::string bytes={}) {
            identification.output_byte_count+=kind==Op::logical_newline ? 2U : bytes.size();
            identification.operations.push_back({kind,std::move(bytes)});
        };
        add(Op::logical_newline);
        if(graphics_==GraphicsMode::ansi) {
            for(const auto* sgr:{"\x1b[0;1;30;40m","\x1b[0;37;40m","\x1b[0;1;37;40m"}) {
                add(Op::ansi_sequence,sgr);add(Op::cp437_text,std::string(1,static_cast<char>(0xfe)));
            }
            add(Op::ansi_sequence,"\x1b[0;37;40m");
            add(Op::cp437_text," Identification required ");
            for(const auto* sgr:{"\x1b[0;1;37;40m","\x1b[0;37;40m","\x1b[0;1;30;40m"}) {
                add(Op::ansi_sequence,sgr);add(Op::cp437_text,std::string(1,static_cast<char>(0xfe)));
            }
            add(Op::ansi_sequence,"\x1b[0;37;40m");
        } else add(Op::cp437_text,"Identification required:");
        add(Op::logical_newline);add(Op::logical_newline);
        const auto heading=dependencies_.terminal.present_display({"prelogin.identification",&identification});
        if(heading!=TerminalOutcome::completed) return code_for_terminal(heading);
        return std::nullopt;
    }

    NameInputResult read_caller_name() {
        if (dependencies_.stages) dependencies_.stages->stage(CallerStage::identity);
        for (;;) {
            const bool components_required = configuration_.real_names_required();
            const std::size_t first_maximum = components_required
                ? kRequiredFirstNameMaximum
                : kAccountNameMaximum;
            const TerminalInputRequest first_request{"caller.name.first", "Enter your first name: ",
                first_maximum, EmptyInputPolicy::use_default};
            TerminalInputResult first = dependencies_.terminal.read_visible(first_request);
            if (!valid_terminal_input_result(first_request, first) ||
                (!first.has_value() && first.outcome != TerminalOutcome::empty)) {
                if (first.outcome == TerminalOutcome::invalid ||
                    first.outcome == TerminalOutcome::overlength) {
                    blank_name_submissions_ = 0;
                    ++substantive_name_attempts_;
                    if (const auto stop = present_name_rejection(); stop.has_value()) {
                        return {{}, stop};
                    }
                    if (const auto stop = failed_substantive_name_attempt(); stop.has_value())
                        return {{}, stop};
                    continue;
                }
                return {{}, code_for_terminal(first.outcome)};
            }
            if (first.outcome == TerminalOutcome::empty) {
                ++blank_name_submissions_;
                if (blank_name_submissions_ >= 6U) {
                    if (const auto stop = present("caller.name.blank.disconnect",
                            "No caller name was entered. Disconnecting.\n");
                        stop.has_value()) return {{}, stop};
                    return {{}, Phase3aWorkflowCode::caller_goodbye};
                }
                continue;
            }
            blank_name_submissions_ = 0;
            ++substantive_name_attempts_;
            const auto normalized_first = normalize_account_name(first.value);
            if (!normalized_first.has_value() || *normalized_first != first.value ||
                normalized_first->size() > first_maximum) {
                if (const auto stop = present_name_rejection(); stop.has_value()) {
                    return {{}, stop};
                }
                if (const auto stop = failed_substantive_name_attempt(); stop.has_value())
                    return {{}, stop};
                continue;
            }

            if (!components_required &&
                normalized_first->size() >= kAccountNameMaximum - 1U) {
                return {normalized_first, std::nullopt};
            }

            // A complete saved login name needs no artificial last-name entry.
            // This lookup grants no authority: normal classification, password,
            // account-state and System Password checks still follow unchanged.
            const auto exact=dependencies_.accounts.find_account_by_name(*normalized_first);
            if(!exact && exact.status.code!=StorageErrorCode::not_found) return {{},Phase3aWorkflowCode::storage_failure};
            if(exact.value && exact.value->display_name && exact.value->state==AccountState::active &&
                account_name_comparison_key(*exact.value->display_name)==account_name_comparison_key(*normalized_first))
                return {normalized_first,std::nullopt};

            const std::size_t last_maximum =
                kAccountNameMaximum - normalized_first->size() - 1U;
            const TerminalInputRequest last_request{"caller.name.last", "Enter your last name: ",
                last_maximum, components_required
                    ? EmptyInputPolicy::reject
                    : EmptyInputPolicy::use_default};
            TerminalInputResult last = dependencies_.terminal.read_visible(last_request);
            if (!valid_terminal_input_result(last_request, last) ||
                (!last.has_value() && last.outcome != TerminalOutcome::empty)) {
                if (last.outcome == TerminalOutcome::invalid ||
                    last.outcome == TerminalOutcome::overlength) {
                    if (const auto stop = present_name_rejection(); stop.has_value()) {
                        return {{}, stop};
                    }
                    if (const auto stop = failed_substantive_name_attempt(); stop.has_value())
                        return {{}, stop};
                    continue;
                }
                return {{}, code_for_terminal(last.outcome)};
            }
            if (last.outcome == TerminalOutcome::empty) {
                return {normalized_first, std::nullopt};
            }
            const auto normalized_last = normalize_account_name(last.value);
            if (!normalized_last.has_value() || *normalized_last != last.value) {
                if (const auto stop = present_name_rejection(); stop.has_value()) {
                    return {{}, stop};
                }
                if (const auto stop = failed_substantive_name_attempt(); stop.has_value())
                    return {{}, stop};
                continue;
            }
            const auto complete = normalize_account_name(
                *normalized_first + " " + *normalized_last);
            if (!complete.has_value() || complete->size() > kAccountNameMaximum) {
                if (const auto stop = present_name_rejection(); stop.has_value()) {
                    return {{}, stop};
                }
                if (const auto stop = failed_substantive_name_attempt(); stop.has_value())
                    return {{}, stop};
                continue;
            }
            return {complete, std::nullopt};
        }
    }

    std::optional<Phase3aWorkflowCode> present_name_rejection() {
        return present("caller.name.invalid",
            "Caller name must be a normalized printable name of no more than 30 characters.\n");
    }

    std::optional<Phase3aWorkflowCode> failed_substantive_name_attempt() {
        if (substantive_name_attempts_ >= 3U) {
            if (const auto stop = present("caller.name.attempts.disconnect",
                    "A usable caller name was not entered. Disconnecting.\n");
                stop.has_value()) return stop;
            return Phase3aWorkflowCode::caller_goodbye;
        }
        if (substantive_name_attempts_ == 2U)
            return present("caller.name.one_more_try", "One more try.\n");
        return std::nullopt;
    }

    std::optional<Phase3aWorkflowCode> present_generic_authentication_failure() {
        return present("authentication.failure", "Login unsuccessful.\n");
    }

    AttemptResult stop_after_generic(const Phase3aWorkflowCode code) {
        const auto presentation = present_generic_authentication_failure();
        return {AttemptAction::stop,
            presentation.has_value() ? *presentation : code, {}, false};
    }

    Phase3aWorkflowResult finish_after_generic(const Phase3aWorkflowCode code) {
        const auto presentation = present_generic_authentication_failure();
        return finish(presentation.has_value() ? *presentation : code);
    }

    Phase3aWorkflowResult registration_branch(const std::string& name) {
        if (const auto refusal = configuration_.allow_new_users() ? reject_new_name(name) : std::string_view{}; !refusal.empty()) {
            if (preauthentication_end_) {
                if (const auto stop=present_optional_display(TrustedDisplayIdentity::lockout,graphics_,"login.lockout");stop)
                    return finish(*stop);
            }
            const auto stop = present("registration.name.unavailable", std::string(refusal));
            return finish(stop.value_or(Phase3aWorkflowCode::registration_unavailable));
        }
        if (const auto stop = present("caller.name.available",
                "Unable to find " + name + "....\n"); stop.has_value()) {
            return finish(*stop);
        }
        const TerminalKeyRequest request{"caller.name.available.action",
            "<R>e-Enter Name, <N>ew Caller, <G>oodbye? ",
            {'R', 'r', 'N', 'n', 'G', 'g'}, std::nullopt};
        const TerminalKeyResult action = dependencies_.terminal.read_key(request);
        if (!valid_terminal_key_result(request, action) || !action.has_key()) {
            return finish(code_for_terminal(action.outcome));
        }
        const char selected = static_cast<char>(
            std::toupper(static_cast<unsigned char>(*action.key)));
        if (selected == 'R') {
            const auto stopped = failed_substantive_name_attempt();
            return stopped.has_value() ? finish(*stopped)
                : Phase3aWorkflowResult{Phase3aWorkflowCode::internal_failure, {}, {}, {}};
        }
        if (selected == 'G') return finish(Phase3aWorkflowCode::caller_goodbye);
        if (!configuration_.allow_new_users()) {
            if (const auto stop = present("registration.closed",
                    closed_registration_notice(configuration_.prelogin_identity().bbs_name)); stop) {
                return finish(*stop);
            }
            return finish(Phase3aWorkflowCode::registration_unavailable);
        }
        return registration_continuation(name);
    }

    Phase3aWorkflowResult registration_continuation(const std::string& name) {
        Phase3aWorkflowResult result;
        result.code = Phase3aWorkflowCode::registration_requested;
        result.registration = Phase3aRegistrationContinuation{
            name, graphics_, attempts_.failed_account_attempts};
        if (!result.registration->valid()) {
            return finish(Phase3aWorkflowCode::internal_failure);
        }
        successful_continuation_ = true;
        return result;
    }

    std::string_view reject_new_name(const std::string& name) {
        if (!dependencies_.new_names) return {};
        const auto decision = dependencies_.new_names->evaluate(name);
        if (decision == NewCallerNameDecision::allowed) return {};
        if (decision == NewCallerNameDecision::temporarily_unavailable)
            return "New registration is temporarily unavailable. Please try again later.\r\n";
        preauthentication_end_ = Phase3aPreAuthenticationEnd{
            Phase3aPreAuthenticationEndReason::joker_name_rejected, name};
        if (dependencies_.stages) dependencies_.stages->attention(AttentionKind::new_name_denied, 0, name);
        return "That name is unavailable. Please choose another.\r\n";
    }

    Phase3aRateScopeSet ordinary_scopes(
        const std::optional<AccountId> account_id) const {
        Phase3aRateScopeSet scopes;
        scopes.account_id = account_id;
        scopes.source_ip = source_ip_;
        scopes.source = true;
        scopes.global = true;
        return scopes;
    }

    Phase3aRateScopeSet system_password_scopes() const {
        Phase3aRateScopeSet scopes;
        scopes.source_ip = source_ip_;
        scopes.source = true;
        scopes.global = true;
        scopes.system_password = true;
        return scopes;
    }

    std::optional<Phase3aWorkflowCode> await_rate(
        const Phase3aRateScopeSet& scopes) {
        const Phase3aRateGateResult gate = dependencies_.rates.await_admission(
            scopes, dependencies_.clock.unix_seconds(), cancellation_);
        if (gate.code == Phase3aRateGateCode::admitted) return std::nullopt;
        return code_for_rate_gate(gate.code);
    }

    TerminalInputResult read_password(
        const std::string& identity,
        const std::string& prompt) {
        if (protocol_authentication_) {
            if (!supplied_password_ || supplied_password_->empty() ||
                supplied_password_->size() > kPasswordMaximum ||
                !std::all_of(supplied_password_->begin(), supplied_password_->end(),
                    [](const unsigned char c) { return c >= 0x20U && c <= 0x7eU; })) {
                return {TerminalOutcome::invalid, {}};
            }
            return {TerminalOutcome::completed, std::string(*supplied_password_)};
        }
        const TerminalInputRequest request{
            identity, prompt, kPasswordMaximum, EmptyInputPolicy::reject};
        TerminalInputResult result = dependencies_.terminal.read_secret(request);
        if (!valid_terminal_input_result(request, result)) {
            explicitly_erase_terminal_secret(result.value);
        }
        return result;
    }

    AttemptResult authenticate_unavailable(
        const std::optional<std::string>& provisional_name = std::nullopt) {
        const Phase3aRateScopeSet scopes = ordinary_scopes(std::nullopt);
        if (const auto stop = await_rate(scopes); stop.has_value()) {
            return *stop == Phase3aWorkflowCode::cancelled ||
                    *stop == Phase3aWorkflowCode::timed_out ||
                    *stop == Phase3aWorkflowCode::disconnected ||
                    *stop == Phase3aWorkflowCode::shutdown_requested
                ? AttemptResult{AttemptAction::stop, *stop, {}, false}
                : stop_after_generic(*stop);
        }
        TerminalInputResult secret = read_password(
            "authentication.password", "Enter your password: ");
        const WorkflowSecretGuard secret_guard{secret.value};
        if (!secret.has_value()) {
            if (secret.outcome == TerminalOutcome::invalid ||
                secret.outcome == TerminalOutcome::overlength) {
                return failed_attempt(scopes, std::nullopt, false);
            }
            return {AttemptAction::stop, code_for_terminal(secret.outcome), {}, false};
        }
        PasswordAuthenticationResult dummy;
        try {
            dummy = dependencies_.credentials.perform_dummy_verification(
                secret.value, dependencies_.clock.unix_seconds(), cancellation_);
        } catch (...) {
            explicitly_erase_terminal_secret(secret.value);
            throw;
        }
        explicitly_erase_terminal_secret(secret.value);
        const Phase3aWorkflowCode mapped = code_for_authentication(dummy.code);
        if (mapped == Phase3aWorkflowCode::cancelled ||
            mapped == Phase3aWorkflowCode::kdf_backpressure ||
            mapped == Phase3aWorkflowCode::storage_failure ||
            mapped == Phase3aWorkflowCode::temporarily_unavailable) {
            return mapped == Phase3aWorkflowCode::cancelled
                ? AttemptResult{AttemptAction::stop, mapped, {}, false}
                : stop_after_generic(mapped);
        }
        if (provisional_name && dummy.code == AuthenticationOperationCode::rejected &&
            !dummy.verified && !cancellation_.is_cancelled()) {
            // Reclassify after rate/KDF work: a concurrently claimed or banned
            // name must never acquire provisional authority. Do not reset the
            // source/global defenses or count this as a successful account login.
            const auto current = dependencies_.names.classify(*provisional_name);
            if (!current || !current.value || !current.value->valid() ||
                current.value->normalized_name != *provisional_name) {
                return stop_after_generic(Phase3aWorkflowCode::storage_failure);
            }
            if (current.value->classification == CallerNameClassification::registerable &&
                configuration_.allow_new_users() && authoritative_policy_matches_configuration()) {
                const auto refusal = reject_new_name(*provisional_name);
                if (!refusal.empty()) {
                    transport_name_refusal_ = refusal;
                    return {AttemptAction::stop, Phase3aWorkflowCode::registration_unavailable, {}, false};
                }
                transport_registration_name_ = *provisional_name;
                return {AttemptAction::retry, Phase3aWorkflowCode::registration_requested, {}, false};
            }
        }
        return failed_attempt(scopes, std::nullopt, false);
    }

    AttemptResult authenticate_existing(const CallerNameResolution& resolution) {
        claimed_account_name_ = resolution.normalized_name;
        if (dependencies_.stages) dependencies_.stages->stage(CallerStage::authentication);
        const auto expected_name_key =
            account_name_comparison_key(resolution.normalized_name);
        if (!expected_name_key.has_value() ||
            !resolution.account_id.has_value() ||
            !resolution.security_level.has_value()) {
            return stop_after_generic(Phase3aWorkflowCode::internal_failure);
        }
        const Phase3aRateScopeSet scopes = ordinary_scopes(resolution.account_id);
        if (const auto stop = await_rate(scopes); stop.has_value()) {
            return *stop == Phase3aWorkflowCode::cancelled ||
                    *stop == Phase3aWorkflowCode::timed_out ||
                    *stop == Phase3aWorkflowCode::disconnected ||
                    *stop == Phase3aWorkflowCode::shutdown_requested
                ? AttemptResult{AttemptAction::stop, *stop, {}, false}
                : stop_after_generic(*stop);
        }

        TerminalInputResult secret = read_password(
            "authentication.password", "Enter your password: ");
        const WorkflowSecretGuard secret_guard{secret.value};
        if (!secret.has_value()) {
            if (secret.outcome == TerminalOutcome::invalid ||
                secret.outcome == TerminalOutcome::overlength) {
                return failed_attempt(scopes, resolution.account_id, false);
            }
            return {AttemptAction::stop, code_for_terminal(secret.outcome), {}, false};
        }

        auto current = dependencies_.accounts.find_account_by_id(*resolution.account_id);
        if (!current || !current.value.has_value()) {
            const PasswordAuthenticationResult dummy =
                dependencies_.credentials.perform_dummy_verification(
                    secret.value, dependencies_.clock.unix_seconds(), cancellation_);
            explicitly_erase_terminal_secret(secret.value);
            const Phase3aWorkflowCode mapped = code_for_authentication(dummy.code);
            if (mapped == Phase3aWorkflowCode::cancelled ||
                mapped == Phase3aWorkflowCode::kdf_backpressure ||
                mapped == Phase3aWorkflowCode::temporarily_unavailable) {
                return {AttemptAction::stop, mapped, {}, false};
            }
            if (!current.status.succeeded() &&
                current.status.code != StorageErrorCode::not_found) {
                return stop_after_generic(Phase3aWorkflowCode::storage_failure);
            }
            return failed_attempt(scopes, std::nullopt, false);
        }
        if (!account_matches_classification(*current.value,
                *resolution.account_id, *expected_name_key,
                *resolution.security_level)) {
            const PasswordAuthenticationResult dummy =
                dependencies_.credentials.perform_dummy_verification(
                    secret.value, dependencies_.clock.unix_seconds(), cancellation_);
            explicitly_erase_terminal_secret(secret.value);
            const Phase3aWorkflowCode mapped = code_for_authentication(dummy.code);
            if (mapped == Phase3aWorkflowCode::cancelled ||
                mapped == Phase3aWorkflowCode::kdf_backpressure ||
                mapped == Phase3aWorkflowCode::temporarily_unavailable) {
                return mapped == Phase3aWorkflowCode::cancelled
                    ? AttemptResult{AttemptAction::stop, mapped, {}, false}
                    : stop_after_generic(mapped);
            }
            // Password work/rate defenses are unchanged. SSH password rejection
            // has no interactive shell in which to show a display file.
            if (!protocol_authentication_ && current.value->state==AccountState::locked) {
                if (const auto stop=present_optional_display(TrustedDisplayIdentity::lockout,graphics_,"login.lockout");stop)
                    return {AttemptAction::stop,*stop,{},false};
            }
            return failed_attempt(scopes, std::nullopt, false);
        }

        const AuthenticationAuditContext maintenance = audit_context(
            *resolution.account_id, dependencies_.clock.unix_seconds());
        if (!valid_authentication_audit_context(maintenance)) {
            explicitly_erase_terminal_secret(secret.value);
            return stop_after_generic(Phase3aWorkflowCode::internal_failure);
        }
        const PasswordAuthenticationResult verified =
            dependencies_.credentials.verify_existing_account(
                *resolution.account_id, secret.value,
                dependencies_.clock.unix_seconds(), maintenance, cancellation_);
        if (!verified.authenticated()) {
            const Phase3aWorkflowCode mapped = code_for_authentication(verified.code);
            explicitly_erase_terminal_secret(secret.value);
            if (mapped == Phase3aWorkflowCode::cancelled ||
                mapped == Phase3aWorkflowCode::kdf_backpressure ||
                mapped == Phase3aWorkflowCode::storage_failure ||
                mapped == Phase3aWorkflowCode::temporarily_unavailable) {
                return mapped == Phase3aWorkflowCode::cancelled
                    ? AttemptResult{AttemptAction::stop, mapped, {}, false}
                    : stop_after_generic(mapped);
            }
            const bool wrong_password =
                verified.code == AuthenticationOperationCode::rejected;
            return failed_attempt(scopes,
                wrong_password ? resolution.account_id : std::nullopt,
                wrong_password);
        }

        current = dependencies_.accounts.find_account_by_id(*resolution.account_id);
        if (!current || !current.value.has_value() ||
            !account_matches_classification(*current.value,
                *resolution.account_id, *expected_name_key,
                *resolution.security_level)) {
            explicitly_erase_terminal_secret(secret.value);
            return stop_after_generic(Phase3aWorkflowCode::storage_failure);
        }

        if (protocol_authentication_) {
            if (verified.password_change_required) {
                transport_temporary_password_ = secret.value;
            }
            else if(dependencies_.caller_password && verified.verified_credential)
                dependencies_.caller_password(*resolution.account_id,*verified.verified_credential,secret.value);
            explicitly_erase_terminal_secret(secret.value);
            return {AttemptAction::authenticated,
                Phase3aWorkflowCode::authenticated_caller_ready,
                std::move(current.value), false, std::move(expected_name_key),
                resolution.security_level};
        }

        if (verified.password_change_required) {
            const Phase3aWorkflowCode replacement = replace_temporary_password(
                *current.value, secret.value);
            explicitly_erase_terminal_secret(secret.value);
            if (replacement != Phase3aWorkflowCode::authenticated_caller_ready) {
                if (replacement == Phase3aWorkflowCode::authentication_denied) {
                    return failed_attempt(scopes, std::nullopt, false);
                }
                return replacement == Phase3aWorkflowCode::cancelled ||
                        replacement == Phase3aWorkflowCode::timed_out ||
                        replacement == Phase3aWorkflowCode::disconnected ||
                        replacement == Phase3aWorkflowCode::shutdown_requested
                    ? AttemptResult{AttemptAction::stop, replacement, {}, false}
                    : stop_after_generic(replacement);
            }
            current = dependencies_.accounts.find_account_by_id(*resolution.account_id);
            if (!current || !current.value.has_value() ||
                !account_matches_classification(*current.value,
                    *resolution.account_id, *expected_name_key,
                    *resolution.security_level)) {
                return stop_after_generic(Phase3aWorkflowCode::storage_failure);
            }
        } else {
            if(dependencies_.caller_password && verified.verified_credential)
                dependencies_.caller_password(*resolution.account_id,*verified.verified_credential,secret.value);
            explicitly_erase_terminal_secret(secret.value);
        }

        bool system_verified = false;
        const SysopSequenceAction sysop_action = next_sysop_sequence_action(
            true, *current.value->security_level,
            configuration_.security_policy().sysop_security_threshold);
        if (sysop_action == SysopSequenceAction::request_system_password) {
            AttemptResult system = authenticate_system_password(*current.value);
            if (system.action != AttemptAction::authenticated) return system;
            system_verified = true;
        }
        return {AttemptAction::authenticated,
            Phase3aWorkflowCode::authenticated_caller_ready,
            std::move(current.value), system_verified,
            std::move(expected_name_key), resolution.security_level};
    }

    bool valid_current_account(
        const AccountRecord& account,
        const AccountId expected) const {
        if (account.account_id != expected || account.state != AccountState::active ||
            account.revision < 1 || !account.display_name.has_value() ||
            !account.security_level.has_value()) {
            return false;
        }
        const auto normalized = normalize_account_name(*account.display_name);
        const auto name_key = account_name_comparison_key(*account.display_name);
        const auto level = core::SecurityLevel::create(*account.security_level);
        return normalized.has_value() && *normalized == *account.display_name &&
            name_key.has_value() && account.name_key.has_value() &&
            *account.name_key == *name_key && level.has_value() &&
            configuration_.foundation().security.is_configured(*level);
    }

    bool account_matches_classification(
        const AccountRecord& account,
        const AccountId expected_account_id,
        const std::string& expected_name_key,
        const std::uint16_t classified_security_level) const {
        return valid_current_account(account, expected_account_id) &&
            account.name_key == expected_name_key &&
            account.security_level == classified_security_level;
    }

    Phase3aWorkflowCode replace_temporary_password(
        const AccountRecord& account,
        const std::string& temporary_password) {
        const auto reset = dependencies_.authentication_storage.temporary_password(
            account.account_id);
        if (!reset || !reset.value.has_value() ||
            reset.value->state != TemporaryPasswordState::active ||
            reset.value->revision < 1) {
            return Phase3aWorkflowCode::storage_failure;
        }
        for (;;) {
            TerminalInputResult permanent = read_password(
                "authentication.replacement", "New permanent password: ");
            const WorkflowSecretGuard permanent_guard{permanent.value};
            if (!permanent.has_value()) return code_for_terminal(permanent.outcome);
            TerminalInputResult confirmation = read_password(
                "authentication.replacement.confirm", "Confirm permanent password: ");
            const WorkflowSecretGuard confirmation_guard{confirmation.value};
            if (!confirmation.has_value()) {
                explicitly_erase_terminal_secret(permanent.value);
                return code_for_terminal(confirmation.outcome);
            }
            if (permanent.value != confirmation.value) {
                explicitly_erase_terminal_secret(permanent.value);
                explicitly_erase_terminal_secret(confirmation.value);
                if (const auto stop = present("authentication.replacement.mismatch",
                        "Passwords do not match.\n"); stop.has_value()) {
                    return *stop;
                }
                continue;
            }

            const std::int64_t now = dependencies_.clock.unix_seconds();
            TemporaryPasswordReplacementOperation operation;
            operation.account_id = account.account_id;
            operation.expected_account_revision = account.revision;
            operation.expected_reset_revision = reset.value->revision;
            operation.temporary_password = temporary_password;
            operation.permanent_password = permanent.value;
            operation.policy = {configuration_.password_mode(),
                *account.display_name, configuration_.prelogin_identity().bbs_name};
            operation.current_session_id = durable_session_id_;
            operation.consumed_audit = audit_context(account.account_id, now);
            operation.changed_audit = audit_context(account.account_id, now);
            if (!valid_authentication_audit_context(operation.consumed_audit) ||
                !valid_authentication_audit_context(operation.changed_audit)) {
                explicitly_erase_terminal_secret(permanent.value);
                explicitly_erase_terminal_secret(confirmation.value);
                return Phase3aWorkflowCode::internal_failure;
            }
            const PasswordMutationResult replaced =
                dependencies_.credentials.replace_temporary_password(
                    operation, cancellation_);
            if(replaced.succeeded() && replaced.committed_credential && dependencies_.caller_password)
                dependencies_.caller_password(account.account_id,*replaced.committed_credential,permanent.value);
            explicitly_erase_terminal_secret(permanent.value);
            explicitly_erase_terminal_secret(confirmation.value);
            if (replaced.succeeded()) {
                return Phase3aWorkflowCode::authenticated_caller_ready;
            }
            if (replaced.code == AuthenticationOperationCode::policy_rejected) {
                if (const auto stop = present("authentication.replacement.policy",
                        "The new password does not meet the configured policy.\n");
                    stop.has_value()) {
                    return *stop;
                }
                continue;
            }
            return code_for_authentication(replaced.code);
        }
    }

    AttemptResult authenticate_system_password(const AccountRecord& account) {
        if (dependencies_.stages) dependencies_.stages->stage(CallerStage::system_password);
        const Phase3aRateScopeSet scopes = system_password_scopes();
        if (const auto stop = await_rate(scopes); stop.has_value()) {
            return *stop == Phase3aWorkflowCode::cancelled ||
                    *stop == Phase3aWorkflowCode::timed_out ||
                    *stop == Phase3aWorkflowCode::disconnected ||
                    *stop == Phase3aWorkflowCode::shutdown_requested
                ? AttemptResult{AttemptAction::stop, *stop, {}, false}
                : stop_after_generic(*stop);
        }
        TerminalInputResult secret = read_password(
            "authentication.system_password", "su password: ");
        if (!secret.has_value()) {
            if (secret.outcome == TerminalOutcome::invalid ||
                secret.outcome == TerminalOutcome::overlength) {
                return failed_attempt(scopes, account.account_id, true);
            }
            return {AttemptAction::stop, code_for_terminal(secret.outcome), {}, false};
        }
        const AuthenticationAuditContext maintenance = audit_context(
            account.account_id, dependencies_.clock.unix_seconds());
        if (!valid_authentication_audit_context(maintenance)) {
            explicitly_erase_terminal_secret(secret.value);
            return stop_after_generic(Phase3aWorkflowCode::internal_failure);
        }
        const PasswordAuthenticationResult verified =
            dependencies_.credentials.verify_system_password(
                secret.value, dependencies_.clock.unix_seconds(),
                maintenance, cancellation_);
        explicitly_erase_terminal_secret(secret.value);
        if (verified.authenticated() &&
            sysop_authority_granted(
                SysopSequenceAction::request_system_password, verified.verified)) {
            return {AttemptAction::authenticated,
                Phase3aWorkflowCode::authenticated_caller_ready, account, true};
        }
        const Phase3aWorkflowCode mapped = code_for_authentication(verified.code);
        if (mapped == Phase3aWorkflowCode::cancelled ||
            mapped == Phase3aWorkflowCode::kdf_backpressure ||
            mapped == Phase3aWorkflowCode::storage_failure ||
            mapped == Phase3aWorkflowCode::temporarily_unavailable) {
            return mapped == Phase3aWorkflowCode::cancelled
                ? AttemptResult{AttemptAction::stop, mapped, {}, false}
                : stop_after_generic(mapped);
        }
        const bool wrong_system_password =
            verified.code == AuthenticationOperationCode::rejected;
        return failed_attempt(scopes,
            wrong_system_password
                ? std::optional<AccountId>(account.account_id)
                : std::nullopt,
            wrong_system_password);
    }

    AttemptResult failed_attempt(
        const Phase3aRateScopeSet& scopes,
        const std::optional<AccountId> notice_account,
        const bool enqueue_notice) {
        const std::int64_t now = dependencies_.clock.unix_seconds();
        if (!dependencies_.rates.record_failure(scopes, now).succeeded()) {
            return stop_after_generic(Phase3aWorkflowCode::storage_failure);
        }
        if (enqueue_notice && notice_account.has_value()) {
            EnqueueFailedLoginNoticeRequest notice;
            notice.notice_id = dependencies_.identifiers.generate();
            notice.account_id = *notice_account;
            notice.source_ip = source_ip_;
            notice.event_utc_seconds = now;
            if (!notice.notice_id.valid() ||
                !dependencies_.phase3a_storage
                     .enqueue_failed_login_notice(notice)
                     .succeeded()) {
                return stop_after_generic(Phase3aWorkflowCode::storage_failure);
            }
        }
        if (const auto stop = present(
                "authentication.failure", "Login unsuccessful.\n");
            stop.has_value()) {
            return {AttemptAction::stop, *stop, {}, false};
        }
        const ConnectionAttemptAction action =
            record_failed_account_attempt(attempts_);
        if (action == ConnectionAttemptAction::disconnect) {
            if (notice_account && dependencies_.stages && !scopes.system_password)
                dependencies_.stages->attention(AttentionKind::password_attempts_exhausted, *notice_account, claimed_account_name_);
            return {AttemptAction::stop,
                Phase3aWorkflowCode::authentication_denied, {}, false};
        }
        if (attempts_.failed_account_attempts ==
            configuration_.maximum_authentication_attempts() - 1U) {
            if (const auto stop = present("authentication.final_attempt_warning",
                    "Warning: this is your final login attempt.\n");
                stop.has_value()) {
                return {AttemptAction::stop, *stop, {}, false};
            }
        }
        return {AttemptAction::retry,
            Phase3aWorkflowCode::authentication_denied, {}, false};
    }

    AuthenticationAuditContext audit_context(
        const AccountId actor,
        const std::int64_t now) {
        return {dependencies_.identifiers.generate(),
            request_.correlation_id, actor, now};
    }

    Phase3aWorkflowResult complete_authenticated_session(
        AccountRecord account,
        const bool system_password_verified,
        std::string expected_name_key,
        const std::uint16_t classified_security_level) {
        auto admission_snapshot = revalidate_for_admission(
            account, system_password_verified, expected_name_key,
            classified_security_level);
        if (!admission_snapshot.has_value()) {
            return finish_after_generic(Phase3aWorkflowCode::storage_failure);
        }
        account = admission_snapshot->account;
        const AccountStatistics pre_commit_statistics = account.statistics;

        AccountSessionAdmissionRequest admission_request;
        admission_request.lease_id = dependencies_.identifiers.generate();
        admission_request.account_id = account.account_id;
        admission_request.intended_session_id = durable_session_id_;
        admission_request.diagnostic_node = session_.node().value();
        admission_request.maximum_sessions_per_account =
            account.security_level==999 && system_password_verified ? configuration_.foundation().logical_nodes : 1;
        if (!admission_request.lease_id.valid()) {
            return finish_after_generic(Phase3aWorkflowCode::internal_failure);
        }
        const AccountSessionAdmissionResult admitted = dependencies_.admission.acquire(
            admission_request, cancellation_);
        if (!admitted.acquired() || !admitted.lease.has_value()) {
            switch (admitted.code) {
                case AccountSessionAdmissionCode::cancelled:
                    return finish(Phase3aWorkflowCode::cancelled);
                case AccountSessionAdmissionCode::account_limit:
                    if (dependencies_.stages) dependencies_.stages->observation(
                        CallerWorkflowObservation::duplicate_denied, account.account_id);
                    if (dependencies_.stages) dependencies_.stages->attention(
                        AttentionKind::duplicate_login, account.account_id, *account.display_name, admitted.existing_node);
                    // Reached only after ordinary authentication and authoritative
                    // account/policy revalidation. Never disclose before password proof.
                    if (const auto stop = present("authentication.already_online",
                            std::string(kDuplicateCallerNotice)); stop) return finish(*stop);
                    return finish(Phase3aWorkflowCode::temporarily_unavailable);
                case AccountSessionAdmissionCode::unavailable:
                case AccountSessionAdmissionCode::acquired:
                    return finish_after_generic(Phase3aWorkflowCode::internal_failure);
            }
        }
        if (admitted.lease->lease_id != admission_request.lease_id ||
            admitted.lease->account_id != admission_request.account_id ||
            admitted.lease->intended_session_id !=
                admission_request.intended_session_id) {
            const bool released = dependencies_.admission.release(*admitted.lease);
            Phase3aWorkflowResult result =
                finish_after_generic(Phase3aWorkflowCode::internal_failure);
            if (!released) result.code = Phase3aWorkflowCode::cleanup_failure;
            return result;
        }
        lease_owner_ = AccountSessionAdmissionLeaseOwner::create(
            *admitted.lease, dependencies_.admission, dependencies_.cleanup_health);
        if (!lease_owner_.has_value()) {
            return finish_after_generic(Phase3aWorkflowCode::internal_failure);
        }

        const std::int64_t now = dependencies_.clock.unix_seconds();
        AuthenticatedSessionCommitRequest commit;
        commit.session = {durable_session_id_, request_.correlation_id,
            account.account_id, session_.node().value(), now};
        commit.session.journal_transport = protocol_ == AuthenticationProtocol::ssh ? "SSH" : "Telnet";
        commit.session.journal_source_ip = source_ip_;
        commit.session.journal_ansi = graphics_ == GraphicsMode::ansi ? 1 : 0;
        commit.successful_source = {dependencies_.identifiers.generate(),
            account.account_id, source_ip_, protocol_, now};
        commit.expected_account_revision = account.revision;
        commit.expected_account_name_key = expected_name_key;
        commit.expected_security_level = classified_security_level;
        commit.expected_security_policy_revision = admission_snapshot->policy.revision;
        commit.expected_security_policy_hash = admission_snapshot->policy.policy_hash;
        if (!valid_authenticated_session_commit_request(commit)) {
            return finish_after_generic(Phase3aWorkflowCode::internal_failure);
        }
        durable_cleanup_.emplace(dependencies_.accounts, dependencies_.clock,
            durable_session_id_, dependencies_.identifiers.generate(), now, dependencies_.cleanup_health,
            [observer=dependencies_.stages] {return observer?observer->accounting_snapshot():SessionAccountingSnapshot{};});
        const MutationResult committed =
            dependencies_.session_commit.commit_authenticated_session(commit);
        if (!committed.succeeded()) {
            durable_cleanup_->commit_failed(committed.status.code);
            return finish_after_generic(Phase3aWorkflowCode::storage_failure);
        }
        durable_cleanup_->confirm_started();
        if (dependencies_.stages) dependencies_.stages->observation(
            CallerWorkflowObservation::durable_session_started, account.account_id, durable_session_id_);

        const auto authoritative_policy = dependencies_.accounts.security_policy();
        auto authoritative = dependencies_.accounts.find_account_by_id(
            account.account_id);
        if (!authoritative_policy || !authoritative_policy.value.has_value() ||
            !policy_matches_configuration(*authoritative_policy.value) ||
            authoritative_policy.value->revision != admission_snapshot->policy.revision ||
            authoritative_policy.value->policy_hash !=
                admission_snapshot->policy.policy_hash ||
            !authoritative || !authoritative.value.has_value() ||
            !valid_current_account(*authoritative.value, account.account_id)) {
            return finish_after_generic(Phase3aWorkflowCode::storage_failure);
        }
        const auto authoritative_identity = account_name_comparison_key(
            *authoritative.value->display_name);
        if (!authoritative_identity.has_value() ||
            *authoritative_identity != expected_name_key ||
            authoritative.value->name_key != expected_name_key ||
            authoritative.value->security_level != classified_security_level ||
            authoritative.value->revision < account.revision) {
            return finish_after_generic(Phase3aWorkflowCode::storage_failure);
        }
        account = std::move(*authoritative.value);

        const auto level = core::SecurityLevel::create(*account.security_level);
        if (!level.has_value() ||
            !session_.authenticate({*authoritative_identity, *account.display_name,
                *level, account.account_id})) {
            return finish_after_generic(Phase3aWorkflowCode::internal_failure);
        }

        Phase3aRateScopeSet successful_account;
        successful_account.account_id = account.account_id;
        if (!dependencies_.rates.record_success(successful_account,
                dependencies_.clock.unix_seconds()).succeeded()) {
            return finish_after_generic(Phase3aWorkflowCode::storage_failure);
        }
        if (system_password_verified) {
            Phase3aRateScopeSet successful_system;
            successful_system.system_password = true;
            if (!dependencies_.rates.record_success(successful_system,
                    dependencies_.clock.unix_seconds()).succeeded()) {
                return finish_after_generic(Phase3aWorkflowCode::storage_failure);
            }
        }

        if (const auto stop = present_caller_information(
                account, pre_commit_statistics.last_successful_login_utc_seconds);
            stop.has_value()) {
            return *stop == Phase3aWorkflowCode::internal_failure ||
                    *stop == Phase3aWorkflowCode::storage_failure
                ? finish_after_generic(*stop)
                : finish(*stop);
        }
        if (const auto stop = present_failed_login_notices(account.account_id);
            stop.has_value()) {
            return *stop == Phase3aWorkflowCode::internal_failure ||
                    *stop == Phase3aWorkflowCode::storage_failure
                ? finish_after_generic(*stop)
                : finish(*stop);
        }
        if (const auto stop = present(
                "caller.continue.prompt", "Press <ENTER> to continue.");
            stop.has_value()) {
            return finish(*stop);
        }
        const TerminalOutcome paused = dependencies_.terminal.pause(
            {"caller.continue"});
        if (paused != TerminalOutcome::completed) {
            return finish(code_for_terminal(paused));
        }

        // Numbered welcomes are post-authentication screens, after the caller
        // has read and acknowledged the greeting/statistics, before the menu.
        if (const auto stop = present_optional_display(
                TrustedDisplayIdentity::welcome1, graphics_, "caller.welcomes");
            stop.has_value()) return finish(*stop);
        if (const auto stop = present_optional_display(
                TrustedDisplayIdentity::all, graphics_, "caller.all",std::nullopt,&account);
            stop.has_value()) return finish(*stop);

        AuthenticatedCallerReady ready;
        ready.account_id = account.account_id;
        ready.durable_session_id = durable_session_id_;
        ready.correlation_id = request_.correlation_id;
        ready.node_id = session_.node().value();
        ready.security_level = *account.security_level;
        ready.graphics = graphics_;
        ready.display_name = *account.display_name;
        ready.previous_login_utc=pre_commit_statistics.last_successful_login_utc_seconds;
        ready.durable_cleanup = std::move(durable_cleanup_);
        ready.admission_lease.emplace(std::move(*lease_owner_));
        lease_owner_.reset();
        auto handoff = AuthenticatedCallerReadyHandoff::create(std::move(ready));
        if (!handoff.has_value()) {
            return finish(Phase3aWorkflowCode::internal_failure);
        }

        successful_continuation_ = true;
        Phase3aWorkflowResult result;
        result.code = Phase3aWorkflowCode::authenticated_caller_ready;
        result.ready.emplace(std::move(*handoff));
        return result;
    }

    std::optional<Phase3aWorkflowCode> present_caller_information(
        const AccountRecord& account,
        const std::optional<std::int64_t> previous_successful_login) {
        const auto current = dependencies_.local_time.format_utc_seconds(
            dependencies_.clock.unix_seconds());
        if (dependencies_.stages) dependencies_.stages->stage(CallerStage::caller_information);
        const auto today = dependencies_.welcome_statistics
            .record_successful_call_and_count_today();
        if (!today) return Phase3aWorkflowCode::storage_failure;
        if (!current.has_value() || current->size() < 16U ||
            !valid_local_timestamp(*current) || !today.value.has_value() ||
            (*current)[4] != '-' || (*current)[7] != '-' || (*current)[10] != ' ' ||
            (*current)[13] != ':') {
            return Phase3aWorkflowCode::internal_failure;
        }
        const int hour = ((*current)[11] - '0') * 10 + ((*current)[12] - '0');
        if (hour < 0 || hour > 23 || (*current)[11] < '0' || (*current)[11] > '9' ||
            (*current)[12] < '0' || (*current)[12] > '9') {
            return Phase3aWorkflowCode::internal_failure;
        }
        const char* period = hour < 12 ? "morning" : hour < 18 ? "afternoon" : "evening";
        const bool returning = previous_successful_login.has_value();
        std::string greeting = "\nGood " + std::string(period) + ", " +
            *account.display_name + (returning ? ", welcome back.\n" :
                ", welcome to " + configuration_.prelogin_identity().bbs_name + ".\n");
        if (const auto stop = present("caller.greeting",
                greeting);
            stop.has_value()) {
            return stop;
        }
        std::string information = "Your IP address is: " + source_ip_ + "\n";
        if (previous_successful_login.has_value()) {
            const auto formatted = dependencies_.local_time.format_utc_seconds(
                *previous_successful_login);
            if (!formatted.has_value() || formatted->size() < 16U ||
                !valid_local_timestamp(*formatted) || (*formatted)[4] != '-' ||
                (*formatted)[7] != '-' || (*formatted)[10] != ' ' ||
                (*formatted)[13] != ':') {
                return Phase3aWorkflowCode::internal_failure;
            }
            information += "You last called on " + formatted->substr(5, 2) + "-" +
                formatted->substr(8, 2) + "-" + formatted->substr(2, 2) +
                " at " + formatted->substr(11, 5) + ".\n";
        }
        information += *today.value == 1U
            ? "There has been 1 call today.\n"
            : "There have been " + std::to_string(*today.value) + " calls today.\n";
        information += account.statistics.successful_login_count == 1U
            ? "You have called 1 time.\n"
            : "You have called " +
                std::to_string(account.statistics.successful_login_count) + " times.\n";
        const std::int64_t minutes = account.statistics.authenticated_session_seconds / 60;
        information += minutes < 1
            ? "You have spent less than one minute online.\n"
            : "You have spent " + std::to_string(minutes) +
                (minutes == 1 ? " minute online.\n" : " minutes online.\n");
        information += "Your security level is " +
            std::to_string(*account.security_level) + ".\n\n";
        return present("caller.information", information);
    }

    std::optional<Phase3aWorkflowCode> present_failed_login_notices(
        const AccountId account_id) {
        ClaimFailedLoginNoticesRequest request;
        request.account_id = account_id;
        request.claim_id = dependencies_.identifiers.generate();
        request.session_id = durable_session_id_;
        request.claimed_utc_seconds = dependencies_.clock.unix_seconds();
        request.limit = kFailedLoginNoticeMaximum;
        if (!request.claim_id.valid()) return Phase3aWorkflowCode::internal_failure;

        const auto claimed =
            dependencies_.phase3a_storage.claim_failed_login_notices(request);
        if (!claimed) {
            if (claimed.status.code == StorageErrorCode::conflict) return std::nullopt;
            return Phase3aWorkflowCode::storage_failure;
        }
        if (!claimed.value.has_value() ||
            claimed.value->claim_id != request.claim_id ||
            claimed.value->session_id != request.session_id ||
            claimed.value->notices.size() > kFailedLoginNoticeMaximum) {
            return Phase3aWorkflowCode::storage_failure;
        }
        if (claimed.value->notices.empty()) return std::nullopt;

        for (const FailedLoginNoticeRecord& notice : claimed.value->notices) {
            if (notice.account_id != account_id || !notice.notice_id.valid() ||
                !canonicalize_source_ip(notice.source_ip).has_value()) {
                return Phase3aWorkflowCode::storage_failure;
            }
            const auto formatted = dependencies_.local_time.format_utc_seconds(
                notice.event_utc_seconds);
            if (!formatted.has_value() || !valid_local_timestamp(*formatted)) {
                return Phase3aWorkflowCode::internal_failure;
            }
            if (const auto stop = present("caller.failed_login_notice",
                    "Failed login attempt - " + notice.source_ip + " - " +
                        *formatted + "\n");
                stop.has_value()) {
                return stop;
            }
        }
        if (!dependencies_.phase3a_storage.acknowledge_failed_login_notices(
                {account_id, request.claim_id, durable_session_id_}).succeeded()) {
            return Phase3aWorkflowCode::storage_failure;
        }
        return std::nullopt;
    }

    Phase3aWorkflowResult finish(Phase3aWorkflowCode code) {
        if (successful_continuation_) return {code, {}, {}, {}};
        bool cleanup_ok = true;
        if (dependencies_.stages) dependencies_.stages->stage(CallerStage::cleanup);
        if (durable_cleanup_ && !durable_cleanup_->finalize(0, session_outcome_for(code)))
            cleanup_ok = false;
        if (lease_owner_.has_value() && lease_owner_->active()) {
            if (!lease_owner_->release()) cleanup_ok = false;
            lease_owner_.reset();
        }
        if (session_.state() != core::SessionState::disconnected) {
            if (session_.state() != core::SessionState::disconnecting) {
                session_.request_disconnect();
            }
            if (session_.state() == core::SessionState::disconnecting) {
                session_.finalize_disconnect();
            }
        }
        if (!cleanup_ok && dependencies_.cleanup_health) dependencies_.cleanup_health->failed.store(true);
        Phase3aWorkflowResult result{
            cleanup_ok ? code : Phase3aWorkflowCode::cleanup_failure, {}, {}, {}};
        result.preauthentication_end = preauthentication_end_;
        return result;
    }

    Phase3aExistingCallerDependencies dependencies_;
    const Phase3aConfigurationSnapshot& configuration_;
    core::Session& session_;
    const Phase3aExistingCallerRequest& request_;
    const CancellationToken& cancellation_;
    std::string source_ip_;
    AuthenticationProtocol protocol_{AuthenticationProtocol::telnet};
    GraphicsMode graphics_{GraphicsMode::ascii};
    ConnectionAttemptState attempts_;
    Identifier128 durable_session_id_;
    std::optional<AccountSessionAdmissionLeaseOwner> lease_owner_;
    std::optional<DurableSessionCleanup> durable_cleanup_;
    bool successful_continuation_{false};
    std::uint8_t blank_name_submissions_{0};
    std::uint8_t substantive_name_attempts_{0};
    bool protocol_authentication_{false};
    bool transport_initialized_{false};
    bool transport_closed_{false};
    std::optional<std::string_view> supplied_password_;
    std::optional<AttemptResult> transport_verified_;
    std::optional<std::string> transport_registration_name_;
    std::string transport_temporary_password_;
    std::string_view transport_name_refusal_; // static reviewed wording only
    std::string claimed_account_name_; // normalized classification, explicitly unverified
    std::optional<Phase3aPreAuthenticationEnd> preauthentication_end_;
};

}  // namespace

struct Phase3aTransportPasswordLogin::Impl {
    Phase3aExistingCallerRequest request;
    CancellationToken cancellation;
    ExistingCallerExecution execution;
    Impl(Phase3aExistingCallerDependencies dependencies,
        const Phase3aConfigurationSnapshot& configuration, core::Session& session,
        Phase3aExistingCallerRequest input_request, CancellationToken input_cancellation)
        : request(std::move(input_request)), cancellation(std::move(input_cancellation)),
          execution(dependencies, configuration, session, request, cancellation) {}
};

Phase3aTransportPasswordLogin::Phase3aTransportPasswordLogin(
    Phase3aExistingCallerDependencies dependencies,
    const Phase3aConfigurationSnapshot& configuration, core::Session& session,
    Phase3aExistingCallerRequest request, CancellationToken cancellation)
    : impl_(std::make_unique<Impl>(dependencies, configuration, session,
          std::move(request), std::move(cancellation))) {}

Phase3aTransportPasswordLogin::~Phase3aTransportPasswordLogin() = default;

TransportPasswordResult Phase3aTransportPasswordLogin::authenticate(
    const std::string_view username, const std::string_view password) noexcept {
    try {
        return impl_->execution.authenticate_transport_password(username, password);
    } catch (...) {
        impl_->execution.abort_transport_password();
        return TransportPasswordResult::disconnect;
    }
}

bool Phase3aTransportPasswordLogin::new_registration_pending() const noexcept {
    return impl_->execution.new_registration_pending();
}

Phase3aWorkflowResult Phase3aTransportPasswordLogin::continue_on_shell() {
    return impl_->execution.execute_safely(true);
}

std::string_view Phase3aTransportPasswordLogin::registration_refusal_notice() const noexcept {
    return impl_->execution.registration_refusal_notice();
}

const Phase3aPreAuthenticationEnd*
Phase3aTransportPasswordLogin::preauthentication_end() const noexcept {
    return impl_->execution.preauthentication_end();
}

bool Phase3aRegistrationContinuation::valid() const noexcept {
    const auto normalized = normalize_account_name(normalized_name);
    return normalized.has_value() && *normalized == normalized_name &&
        failed_authentication_attempts < kFailedAttemptsPerConnection &&
        (graphics == GraphicsMode::ascii || graphics == GraphicsMode::ansi);
}

bool Phase3aExistingCallerRequest::ResumeState::valid() const noexcept {
    return (graphics == GraphicsMode::ascii || graphics == GraphicsMode::ansi) &&
        failed_authentication_attempts > 0 &&
        failed_authentication_attempts < kFailedAttemptsPerConnection &&
        newuser_entitled_account_id != 0;
}

Phase3aExistingCallerWorkflow::Phase3aExistingCallerWorkflow(
    Phase3aExistingCallerDependencies dependencies) noexcept
    : dependencies_(dependencies) {}

Phase3aWorkflowResult Phase3aExistingCallerWorkflow::run(
    const Phase3aConfigurationSnapshot& configuration,
    core::Session& session,
    const Phase3aExistingCallerRequest& request,
    const CancellationToken& cancellation) {
    ExistingCallerExecution execution(
        dependencies_, configuration, session, request, cancellation);
    return execution.execute_safely();
}

}  // namespace sfx::application
