#include "sfx/application/phase3a_workflow.hpp"
#include "sfx/application/admission_notices.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

#include "sfx/core/node.hpp"
#include "sfx/core/security.hpp"

namespace sfx::application {
namespace {

constexpr std::size_t kRequiredFirstNameMaximum = kAccountNameMaximum - 2U;

struct RegistrationSecretGuard {
    std::string& value;
    ~RegistrationSecretGuard() { explicitly_erase_terminal_secret(value); }
};

Phase3aWorkflowCode terminal_code(const TerminalOutcome outcome) noexcept {
    switch (outcome) {
        case TerminalOutcome::cancelled: return Phase3aWorkflowCode::cancelled;
        case TerminalOutcome::timed_out: return Phase3aWorkflowCode::timed_out;
        case TerminalOutcome::disconnected: return Phase3aWorkflowCode::disconnected;
        case TerminalOutcome::shutdown_requested: return Phase3aWorkflowCode::shutdown_requested;
        case TerminalOutcome::terminal_failure:
        case TerminalOutcome::output_failure: return Phase3aWorkflowCode::terminal_failure;
        default: return Phase3aWorkflowCode::internal_failure;
    }
}

SessionOutcomeCode session_outcome(const Phase3aWorkflowCode code) noexcept {
    switch (code) {
        case Phase3aWorkflowCode::disconnected: return SessionOutcomeCode::disconnected;
        case Phase3aWorkflowCode::timed_out: return SessionOutcomeCode::timed_out;
        case Phase3aWorkflowCode::shutdown_requested: return SessionOutcomeCode::shutdown;
        default: return SessionOutcomeCode::runtime_error;
    }
}

AuthenticationProtocol protocol_for(const core::TransportKind kind) noexcept {
    return kind == core::TransportKind::ssh ? AuthenticationProtocol::ssh
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

std::string registration_transfer_label(
    const TransferProtocolPreference preference) {
    switch (preference) {
        case TransferProtocolPreference::xmodem: return "Xmodem";
        case TransferProtocolPreference::ymodem: return "Ymodem";
        case TransferProtocolPreference::ymodem_g: return "Ymodem-G";
        case TransferProtocolPreference::zmodem: return "Zmodem";
        case TransferProtocolPreference::select_at_transfer:
            return "None";
    }
    return {};
}

// Trusted-display token values retain their established display contract;
// registration menus/summaries use the owner-supplied labels above.
std::string transfer_preference_label(const TransferProtocolPreference preference) {
    switch (preference) {
        case TransferProtocolPreference::xmodem: return "XMODEM";
        case TransferProtocolPreference::ymodem: return "YMODEM";
        case TransferProtocolPreference::ymodem_g: return "YMODEM-G";
        case TransferProtocolPreference::zmodem: return "ZMODEM";
        case TransferProtocolPreference::select_at_transfer: return "Select at time of transfer";
    }
    return {};
}

bool valid_local_timestamp(const std::string& value) noexcept {
    return !value.empty() && value.size() <= 64U &&
        std::all_of(value.begin(), value.end(), [](const unsigned char character) {
            return character >= 0x20U && character <= 0x7eU;
        });
}

bool valid_basic_email(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kEmailMaximum || value.front() == '@' ||
        value.back() == '@' || value.front() == '.' || value.back() == '.' ||
        value.find(' ') != std::string_view::npos) return false;
    const auto at = value.find('@');
    if (at == std::string_view::npos || at != value.rfind('@')) return false;
    const auto dot = value.find('.', at + 2U);
    return at > 0U && at + 1U < value.size() && dot != std::string_view::npos &&
        dot + 1U < value.size();
}

std::optional<CivilDate> parse_birth_date(const std::string_view value) noexcept {
    if (value.size() != 10U || value[2] != '/' || value[5] != '/') return std::nullopt;
    for (const std::size_t index : {0U, 1U, 3U, 4U, 6U, 7U, 8U, 9U}) {
        if (value[index] < '0' || value[index] > '9') return std::nullopt;
    }
    CivilDate result;
    result.month = static_cast<std::uint8_t>((value[0] - '0') * 10 + value[1] - '0');
    result.day = static_cast<std::uint8_t>((value[3] - '0') * 10 + value[4] - '0');
    result.year = (value[6] - '0') * 1000 + (value[7] - '0') * 100 +
        (value[8] - '0') * 10 + value[9] - '0';
    return valid_civil_date(result) ? std::optional<CivilDate>(result) : std::nullopt;
}

std::string iso_date(const CivilDate& value) {
    std::string text(10U, '0');
    text[0] = static_cast<char>('0' + (value.year / 1000) % 10);
    text[1] = static_cast<char>('0' + (value.year / 100) % 10);
    text[2] = static_cast<char>('0' + (value.year / 10) % 10);
    text[3] = static_cast<char>('0' + value.year % 10);
    text[4] = '-';
    text[5] = static_cast<char>('0' + value.month / 10U);
    text[6] = static_cast<char>('0' + value.month % 10U);
    text[7] = '-';
    text[8] = static_cast<char>('0' + value.day / 10U);
    text[9] = static_cast<char>('0' + value.day % 10U);
    return text;
}

std::optional<std::uint16_t> age_on(const CivilDate& birth, const CivilDate& today) noexcept {
    if (!valid_civil_date(birth) || !valid_civil_date(today) || birth.year > today.year ||
        (birth.year == today.year && (birth.month > today.month ||
            (birth.month == today.month && birth.day > today.day)))) return std::nullopt;
    std::int32_t age = today.year - birth.year;
    if (today.month < birth.month || (today.month == birth.month && today.day < birth.day)) --age;
    return age >= 0 && age <= 999 ? std::optional<std::uint16_t>(static_cast<std::uint16_t>(age))
                                 : std::nullopt;
}

bool valid_registration_account(const AccountRecord& account, const AccountId expected_id,
    const std::string& expected_key, const std::uint16_t expected_level,
    const Phase3aConfigurationSnapshot& configuration) {
    if (account.account_id != expected_id || account.state != AccountState::active ||
        account.revision < 1 || !account.display_name.has_value() ||
        account.name_key != expected_key || account.security_level != expected_level) return false;
    const auto normalized = normalize_account_name(*account.display_name);
    const auto key = account_name_comparison_key(*account.display_name);
    const auto level = core::SecurityLevel::create(expected_level);
    return normalized.has_value() && *normalized == *account.display_name && key == expected_key &&
        level.has_value() && configuration.foundation().security.is_configured(*level);
}

struct NameResult { std::optional<std::string> name; std::optional<Phase3aWorkflowCode> stop; };
struct Draft {
    std::string display_name;
    AccountProfile profile;
    std::optional<CredentialEnvelope> credential;
    TransferProtocolPreference transfer{TransferProtocolPreference::select_at_transfer};
};

enum class SystemPasswordAction : std::uint8_t { verified, resume_existing, stop };

struct SystemPasswordResult {
    SystemPasswordAction action{SystemPasswordAction::stop};
    Phase3aWorkflowCode code{Phase3aWorkflowCode::internal_failure};
};

class RegistrationExecution final {
public:
    RegistrationExecution(Phase3aRegistrationDependencies dependencies,
        const Phase3aConfigurationSnapshot& configuration, core::Session& session,
        const Phase3aRegistrationRequest& request, const CancellationToken& cancellation)
        : dependencies_(dependencies), configuration_(configuration), session_(session),
          request_(request), cancellation_(cancellation) {}
    ~RegistrationExecution(){explicitly_erase_terminal_secret(caller_password_);}

    Phase3aWorkflowResult execute_safely() noexcept {
        try { return execute(); }
        catch (...) {
            try { return finish(Phase3aWorkflowCode::internal_failure); }
            catch (...) {
                if (dependencies_.cleanup_health) dependencies_.cleanup_health->failed.store(true);
                return {Phase3aWorkflowCode::cleanup_failure, {}, {}, {}};
            }
        }
    }

    Phase3aWorkflowResult execute() {
        if (dependencies_.stages) dependencies_.stages->stage(CallerStage::registration);
        if (cancellation_.is_cancelled()) return finish(Phase3aWorkflowCode::cancelled);
        if (!valid_start_context()) return finish(Phase3aWorkflowCode::invalid_context);
        if (!configuration_.caller_entry_allowed()) return finish(Phase3aWorkflowCode::bootstrap_incomplete);
        if (!configuration_.allow_new_users()) return finish(Phase3aWorkflowCode::registration_unavailable);
        if (!authoritative_policy_matches()) return finish(Phase3aWorkflowCode::storage_failure);
        if (!name_is_registerable(request_.continuation.normalized_name, true))
            return finish(name_check_stop_.value_or(name_check_failed_ ? Phase3aWorkflowCode::storage_failure
                                             : Phase3aWorkflowCode::registration_unavailable));
        durable_session_id_ = dependencies_.identifiers.generate();
        registration_event_id_ = dependencies_.identifiers.generate();
        if (!durable_session_id_.valid() || !registration_event_id_.valid())
            return finish(Phase3aWorkflowCode::internal_failure);
        draft_.display_name = request_.continuation.normalized_name;
        if (configuration_.real_names_required()) draft_.profile.real_name = draft_.display_name;
        if (const auto stop = optional_display(TrustedDisplayIdentity::newuser, graphics_,
                "registration.newuser", std::nullopt); stop.has_value()) return finish(*stop);
        if (const auto stop = optional_display(TrustedDisplayIdentity::newreg, graphics_,
                "registration.newreg", std::nullopt); stop.has_value()) return finish(*stop);
        const auto styled = dependencies_.terminal.set_text_style(graphics_ == GraphicsMode::ansi
            ? TerminalTextStyle::ogsf_ansi : TerminalTextStyle::plain);
        if (styled != TerminalOutcome::completed) return finish(terminal_code(styled));
        if (const auto stop = acquire_credential(protocol_ != AuthenticationProtocol::ssh); stop.has_value()) return finish(*stop);
        if (const auto stop = collect_fields(); stop.has_value()) return finish(*stop);
        if (const auto stop = review_and_edit(); stop.has_value()) return finish(*stop);
        return commit_and_continue();
    }

private:
    bool valid_start_context() {
        const auto source = canonicalize_source_ip(request_.source_ip);
        if (!request_.continuation.valid() || !source.has_value() ||
            !request_.correlation_id.valid() || session_.state() != core::SessionState::establishing ||
            (session_.transport().kind != core::TransportKind::telnet &&
             session_.transport().kind != core::TransportKind::ssh)) return false;
        source_ip_ = source->text;
        protocol_ = protocol_for(session_.transport().kind);
        graphics_ = request_.continuation.graphics;
        attempts_.failed_account_attempts = request_.continuation.failed_authentication_attempts;
        if (attempts_.failed_account_attempts >= configuration_.maximum_authentication_attempts())
            return false;
        return true;
    }

    bool policy_matches(const SecurityPolicyRecord& policy) const noexcept {
        const auto& configured = configuration_.security_policy();
        return policy.revision == configured.revision && policy.policy_hash == configured.policy_hash &&
            policy.new_user_level == configured.new_user_security_level &&
            policy.sysop_threshold == configured.sysop_security_threshold;
    }
    bool authoritative_policy_matches() {
        const auto policy = dependencies_.accounts.security_policy();
        return policy && policy.value.has_value() && policy_matches(*policy.value);
    }
    bool name_is_registerable(const std::string& name,
        const bool ending_on_joker_match = false) {
        name_check_failed_ = false;
        name_notice_sent_ = false;
        name_check_stop_.reset();
        const auto classified = dependencies_.names.classify(name);
        if (!classified || !classified.value.has_value() || !classified.value->valid() ||
            classified.value->normalized_name != name) { name_check_failed_ = true; return false; }
        if (classified.value->classification != CallerNameClassification::registerable) return false;
        if (dependencies_.new_names) {
            const auto decision = dependencies_.new_names->evaluate(name);
            if (decision != NewCallerNameDecision::allowed) {
                if (decision == NewCallerNameDecision::unavailable &&
                    ending_on_joker_match) {
                    preauthentication_end_ = Phase3aPreAuthenticationEnd{
                        Phase3aPreAuthenticationEndReason::joker_name_rejected, name};
                }
                if (decision == NewCallerNameDecision::unavailable && dependencies_.stages)
                    dependencies_.stages->attention(AttentionKind::new_name_denied, 0, name);
                name_notice_sent_ = true;
                name_check_stop_ = present("registration.name.unavailable", decision == NewCallerNameDecision::unavailable
                    ? "That name is unavailable. Please choose another.\n"
                    : "New registration is temporarily unavailable. Please try again later.\n");
                return false;
            }
        }
        return true;
    }
    std::optional<Phase3aWorkflowCode> present(const std::string& identity, const std::string& text) {
        const auto outcome = dependencies_.terminal.present({identity, text});
        return outcome == TerminalOutcome::completed ? std::nullopt
            : std::optional<Phase3aWorkflowCode>(terminal_code(outcome));
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
    std::optional<Phase3aWorkflowCode> optional_display(TrustedDisplayIdentity identity,
        GraphicsMode graphics, const std::string& terminal_identity,
        std::optional<std::uint16_t> security_level,
        const AccountRecord* const account = nullptr) {
        const auto selected = dependencies_.displays.select(
            {identity, graphics, security_level, display_context(account)});
        if (!selected.available()) return std::nullopt;
        if (!selected.program.has_value() ||
            selected.program->identity != identity ||
            selected.program->graphics != graphics)
            return std::nullopt;
        const auto outcome = dependencies_.terminal.present_display(
            {terminal_identity, &selected.program->terminal_program});
        return outcome == TerminalOutcome::completed
            ? std::nullopt
            : std::optional<Phase3aWorkflowCode>(terminal_code(outcome));
    }

    NameResult read_name() {
        for (;;) {
            const bool split = configuration_.real_names_required();
            const std::size_t first_max = split ? kRequiredFirstNameMaximum : kAccountNameMaximum;
            const TerminalInputRequest first_request{"registration.name.first", "\nEnter your first name: ",
                first_max, editing_ ? EmptyInputPolicy::use_default : EmptyInputPolicy::reject};
            auto first = dependencies_.terminal.read_visible(first_request);
            if (valid_terminal_input_result(first_request, first) && first.outcome == TerminalOutcome::empty && editing_)
                return {draft_.display_name, {}};
            if (!valid_terminal_input_result(first_request, first) || !first.has_value()) {
                if (first.outcome == TerminalOutcome::invalid || first.outcome == TerminalOutcome::overlength) {
                    if (const auto stop = present("registration.name.invalid",
                            "Caller name must be a normalized printable name of no more than 30 characters.\n");
                        stop.has_value()) return {{}, stop};
                    continue;
                }
                return {{}, terminal_code(first.outcome)};
            }
            const auto normalized_first = normalize_account_name(first.value);
            if (!normalized_first.has_value() || *normalized_first != first.value ||
                normalized_first->size() > first_max) {
                if (const auto stop = present("registration.name.invalid",
                        "Caller name must be a normalized printable name of no more than 30 characters.\n");
                    stop.has_value()) return {{}, stop};
                continue;
            }
            if (!split && normalized_first->size() >= kAccountNameMaximum - 1U)
                return {normalized_first, std::nullopt};
            const std::size_t last_max = kAccountNameMaximum - normalized_first->size() - 1U;
            const TerminalInputRequest last_request{"registration.name.last", "Enter your last name: ",
                last_max, split ? EmptyInputPolicy::reject : EmptyInputPolicy::use_default};
            auto last = dependencies_.terminal.read_visible(last_request);
            if (!valid_terminal_input_result(last_request, last) ||
                (!last.has_value() && last.outcome != TerminalOutcome::empty)) {
                if (last.outcome == TerminalOutcome::invalid || last.outcome == TerminalOutcome::overlength) {
                    if (const auto stop = present("registration.name.invalid",
                            "Caller name must be a normalized printable name of no more than 30 characters.\n");
                        stop.has_value()) return {{}, stop};
                    continue;
                }
                return {{}, terminal_code(last.outcome)};
            }
            if (last.outcome == TerminalOutcome::empty) return {normalized_first, std::nullopt};
            const auto normalized_last = normalize_account_name(last.value);
            const auto complete = normalized_last.has_value()
                ? normalize_account_name(*normalized_first + " " + *normalized_last) : std::nullopt;
            if (!normalized_last.has_value() || *normalized_last != last.value || !complete.has_value()) {
                if (const auto stop = present("registration.name.invalid",
                        "Caller name must be a normalized printable name of no more than 30 characters.\n");
                    stop.has_value()) return {{}, stop};
                continue;
            }
            return {complete, std::nullopt};
        }
    }

    std::optional<Phase3aWorkflowCode> acquire_credential(bool separate = true) {
        for (;;) {
            const TerminalInputRequest password_request{"registration.password",
                std::string(separate ? "\n" : "") + "Enter new password: ",
                kPasswordMaximum, editing_ && draft_.credential ? EmptyInputPolicy::use_default : EmptyInputPolicy::reject};
            separate = true;
            auto password = dependencies_.terminal.read_secret(password_request);
            const RegistrationSecretGuard password_guard{password.value};
            if (valid_terminal_input_result(password_request, password) &&
                password.outcome == TerminalOutcome::empty && editing_ && draft_.credential) return std::nullopt;
            if (!valid_terminal_input_result(password_request, password) || !password.has_value()) {
                explicitly_erase_terminal_secret(password.value);
                if (password.outcome == TerminalOutcome::invalid ||
                    password.outcome == TerminalOutcome::overlength) {
                    if (const auto stop = present("registration.password.policy",
                            "The password does not meet the configured policy.\n"); stop.has_value()) return stop;
                    continue;
                }
                return terminal_code(password.outcome);
            }
            const TerminalInputRequest confirmation_request{"registration.password.confirm",
                "\nPlease verify password: ", kPasswordMaximum, EmptyInputPolicy::reject};
            auto confirmation = dependencies_.terminal.read_secret(confirmation_request);
            const RegistrationSecretGuard confirmation_guard{confirmation.value};
            if (!valid_terminal_input_result(confirmation_request, confirmation) ||
                !confirmation.has_value()) {
                explicitly_erase_terminal_secret(password.value);
                explicitly_erase_terminal_secret(confirmation.value);
                if (confirmation.outcome == TerminalOutcome::invalid ||
                    confirmation.outcome == TerminalOutcome::overlength) {
                    if (const auto stop = present("registration.password.policy",
                            "The password does not meet the configured policy.\n"); stop.has_value()) return stop;
                    continue;
                }
                return terminal_code(confirmation.outcome);
            }
            if (password.value != confirmation.value) {
                explicitly_erase_terminal_secret(password.value);
                explicitly_erase_terminal_secret(confirmation.value);
                if (const auto stop = present("registration.password.mismatch",
                        "Passwords do not match.\n"); stop.has_value()) return stop;
                continue;
            }
            auto created = dependencies_.credentials.create_registration_credential(password.value,
                {configuration_.password_mode(), draft_.display_name,
                    configuration_.prelogin_identity().bbs_name},
                dependencies_.clock.unix_seconds(), cancellation_);
            if(created.succeeded()&&created.credential&&valid_credential_envelope(*created.credential)&&dependencies_.caller_password){
                explicitly_erase_terminal_secret(caller_password_);caller_password_=password.value;
            }
            explicitly_erase_terminal_secret(password.value);
            explicitly_erase_terminal_secret(confirmation.value);
            if (created.succeeded() && created.credential.has_value() &&
                valid_credential_envelope(*created.credential)) {
                draft_.credential = std::move(created.credential);
                return std::nullopt;
            }
            if (created.code == AuthenticationOperationCode::policy_rejected ||
                created.policy_code != PasswordPolicyCode::accepted) {
                if (const auto stop = present("registration.password.policy",
                        "The password does not meet the configured policy.\n"); stop.has_value()) return stop;
                continue;
            }
            switch (created.code) {
                case AuthenticationOperationCode::cancelled: return Phase3aWorkflowCode::cancelled;
                case AuthenticationOperationCode::backpressure: return Phase3aWorkflowCode::kdf_backpressure;
                case AuthenticationOperationCode::storage_failure: return Phase3aWorkflowCode::storage_failure;
                case AuthenticationOperationCode::throttled:
                case AuthenticationOperationCode::unavailable:
                    return Phase3aWorkflowCode::temporarily_unavailable;
                default: return Phase3aWorkflowCode::internal_failure;
            }
        }
    }

    std::optional<Phase3aWorkflowCode> read_real_name() {
        for (;;) {
            if (const auto stop = present("registration.real_name.explanation",
                    "\nReal Name is optional, but some FTN areas require real names.\n");
                stop.has_value()) return stop;
            const TerminalInputRequest request{"registration.real_name", "Real Name: ",
                kRealNameMaximum, EmptyInputPolicy::use_default};
            auto value = dependencies_.terminal.read_visible(request);
            if (!valid_terminal_input_result(request, value)) {
                if (value.outcome == TerminalOutcome::invalid || value.outcome == TerminalOutcome::overlength)
                    continue;
                return terminal_code(value.outcome);
            }
            if (value.outcome == TerminalOutcome::empty) {
                if (!editing_) draft_.profile.real_name.reset();
                return std::nullopt;
            }
            const auto normalized = normalize_real_name(value.value);
            if (!normalized.has_value() || *normalized != value.value) continue;
            draft_.profile.real_name = *normalized;
            return std::nullopt;
        }
    }

    std::optional<Phase3aWorkflowCode> read_location() {
        for (;;) {
            const TerminalInputRequest request{"registration.location", "\nLocation: ",
                kLocationMaximum, editing_ ? EmptyInputPolicy::use_default : EmptyInputPolicy::reject};
            auto value = dependencies_.terminal.read_visible(request);
            if (valid_terminal_input_result(request, value) && value.outcome == TerminalOutcome::empty && editing_)
                return std::nullopt;
            if (!valid_terminal_input_result(request, value) || !value.has_value()) {
                if (value.outcome == TerminalOutcome::invalid || value.outcome == TerminalOutcome::overlength)
                    continue;
                return terminal_code(value.outcome);
            }
            const auto normalized = normalize_optional_profile_text(value.value, kLocationMaximum);
            if (!normalized.has_value() || *normalized != value.value) continue;
            draft_.profile.location = *normalized;
            return std::nullopt;
        }
    }

    std::optional<Phase3aWorkflowCode> read_birth_date() {
        for (;;) {
            const TerminalInputRequest request{"registration.birth_date", "\nDate of birth (MM/DD/YYYY): ",
                10U, editing_ ? EmptyInputPolicy::use_default : EmptyInputPolicy::reject};
            auto value = dependencies_.terminal.read_visible(request);
            if (valid_terminal_input_result(request, value) && value.outcome == TerminalOutcome::empty && editing_)
                return std::nullopt;
            if (!valid_terminal_input_result(request, value) || !value.has_value()) {
                if (value.outcome == TerminalOutcome::invalid || value.outcome == TerminalOutcome::overlength) {
                    if (const auto stop = present("registration.birth_date.invalid",
                            "Enter a valid date as MM/DD/YYYY.\n"); stop.has_value()) return stop;
                    continue;
                }
                return terminal_code(value.outcome);
            }
            const auto birth = parse_birth_date(value.value);
            if (!birth.has_value()) {
                if (const auto stop = present("registration.birth_date.invalid",
                        "Enter a valid date as MM/DD/YYYY.\n"); stop.has_value()) return stop;
                continue;
            }
            const auto age = age_on(*birth, dependencies_.local_date.today());
            if (!age.has_value()) return Phase3aWorkflowCode::internal_failure;
            if (*age < kPhase3aMinimumPlausibleAge || *age > kPhase3aMaximumPlausibleAge ||
                (configuration_.minimum_age().has_value() && *age < *configuration_.minimum_age())) {
                draft_.profile.birth_date.reset();
                if (const auto stop = present("registration.birth_date.rejected",
                        "Registration is unavailable for the supplied age.\n"); stop.has_value()) return stop;
                return Phase3aWorkflowCode::registration_unavailable;
            }
            draft_.profile.birth_date = iso_date(*birth);
            return std::nullopt;
        }
    }

    std::optional<Phase3aWorkflowCode> read_sex() {
        TerminalKeyRequest request{"registration.sex", configuration_.allow_sex_x()
                ? "\nSex [M/F/X]: " : "\nSex [M/F]: ",
            configuration_.allow_sex_x() ? std::vector<char>{'M','m','F','f','X','x'}
                                         : std::vector<char>{'M','m','F','f'}, editing_ ? draft_.profile.sex : std::nullopt};
        const auto value = dependencies_.terminal.read_key(request);
        if (!valid_terminal_key_result(request, value) || !value.has_key()) return terminal_code(value.outcome);
        draft_.profile.sex = static_cast<char>(std::toupper(static_cast<unsigned char>(*value.key)));
        return std::nullopt;
    }

    std::optional<Phase3aWorkflowCode> read_email() {
        const bool required = configuration_.email_mode() == EmailRegistrationMode::required;
        for (;;) {
            const TerminalInputRequest request{"registration.email", "\nEmail: ", kEmailMaximum,
                required && !editing_ ? EmptyInputPolicy::reject : EmptyInputPolicy::use_default};
            auto value = dependencies_.terminal.read_visible(request);
            if (!valid_terminal_input_result(request, value)) {
                if (value.outcome == TerminalOutcome::invalid || value.outcome == TerminalOutcome::overlength)
                    continue;
                return terminal_code(value.outcome);
            }
            if (value.outcome == TerminalOutcome::empty && (!required || editing_)) {
                if (!editing_) draft_.profile.email.reset();
                return std::nullopt;
            }
            if (!value.has_value()) return terminal_code(value.outcome);
            const auto normalized = normalize_optional_profile_text(value.value, kEmailMaximum);
            if (!normalized.has_value() || *normalized != value.value || !valid_basic_email(*normalized)) {
                if (const auto stop = present("registration.email.invalid", "Enter a valid email address.\n");
                    stop.has_value()) return stop;
                continue;
            }
            draft_.profile.email = *normalized;
            return std::nullopt;
        }
    }

    std::optional<Phase3aWorkflowCode> read_transfer() {
        auto choices = configuration_.enabled_transfer_choices();
        choices.erase(std::remove(choices.begin(), choices.end(), TransferProtocolPreference::select_at_transfer), choices.end());
        if (!configuration_.transfer_protocol_question_enabled() || choices.empty()) {
            draft_.transfer = TransferProtocolPreference::select_at_transfer;
            return std::nullopt;
        }
        std::vector<char> keys;
        std::string prompt = "\nSelect a default transfer protocol:\n\n";
        for (std::size_t index = 0; index < choices.size(); ++index) {
            const char key = static_cast<char>('1' + index);
            keys.push_back(key);
            prompt += "[" + std::string(1, key) + "] " + registration_transfer_label(choices[index]) + "\n";
        }
        const char none = static_cast<char>('1' + choices.size());
        keys.push_back(none);
        prompt += "[" + std::string(1, none) + "] None\n\nSelection: ";
        char default_key = none;
        if (editing_) {
            const auto found = std::find(choices.begin(), choices.end(), draft_.transfer);
            if (found != choices.end()) default_key = static_cast<char>('1' + (found - choices.begin()));
        }
        const TerminalKeyRequest request{"registration.transfer_protocol", prompt, keys, default_key};
        const auto value = dependencies_.terminal.read_key(request);
        if (!valid_terminal_key_result(request, value) || !value.has_key()) return terminal_code(value.outcome);
        if (*value.key == none) draft_.transfer = TransferProtocolPreference::select_at_transfer;
        else {
            const auto index = static_cast<std::size_t>(*value.key - '1');
            if (index >= choices.size()) return Phase3aWorkflowCode::internal_failure;
            draft_.transfer = choices[index];
        }
        return std::nullopt;
    }

    std::optional<Phase3aWorkflowCode> collect_fields() {
        if (configuration_.collect_optional_real_name() && !configuration_.real_names_required())
            if (const auto stop = read_real_name(); stop.has_value()) return stop;
        if (configuration_.location_mode() == RegistrationFieldMode::required)
            if (const auto stop = read_location(); stop.has_value()) return stop;
        if (configuration_.birth_date_mode() == RegistrationFieldMode::required)
            if (const auto stop = read_birth_date(); stop.has_value()) return stop;
        if (configuration_.sex_mode() == RegistrationFieldMode::required)
            if (const auto stop = read_sex(); stop.has_value()) return stop;
        if (configuration_.email_mode() != EmailRegistrationMode::off)
            if (const auto stop = read_email(); stop.has_value()) return stop;
        return read_transfer();
    }

    std::string review_text(bool separate = true, bool editing = false) const {
        std::string text = separate ? "\n" : "";
        const auto field = [&](const char* shortcut, const char* label, const std::string& value) {
            if (editing) text += shortcut;
            text += std::string(label) + ": " + value + "\n";
        };
        field("[N] ", "Caller Name", draft_.display_name);
        field("[P] ", "Password", "set");
        if (configuration_.collect_optional_real_name() && !configuration_.real_names_required())
            field("[R] ", "Real Name", draft_.profile.real_name.value_or("(not supplied)"));
        if (configuration_.location_mode() == RegistrationFieldMode::required)
            field("[L] ", "Location", draft_.profile.location.value_or(""));
        if (configuration_.birth_date_mode() == RegistrationFieldMode::required)
            field("[D] ", editing ? "Date of birth" : "DOB", draft_.profile.birth_date.value_or(""));
        if (configuration_.sex_mode() == RegistrationFieldMode::required)
            field("[X] ", "Sex", draft_.profile.sex ? std::string(1, *draft_.profile.sex) : "");
        if (configuration_.email_mode() != EmailRegistrationMode::off)
            field("[E] ", "Email", draft_.profile.email.value_or("(not supplied)"));
        if (configuration_.transfer_protocol_question_enabled())
            field("[T] ", "Default Transfer Protocol", registration_transfer_label(draft_.transfer));
        return text;
    }

    TerminalKeyRequest review_request() const {
        return {"registration.review.action", "\n[R]eview/Edit your settings\n"
            "[C]onfirm settings\n[Q]uit & disconnect - no account created\n\nSelection: ",
            {'R','r','C','c','Q','q'}, std::nullopt, true};
    }

    TerminalKeyRequest edit_request() const {
        std::vector<char> keys{'S','s','N','n','P','p'};
        if (configuration_.collect_optional_real_name() && !configuration_.real_names_required())
            keys.insert(keys.end(), {'R','r'});
        if (configuration_.location_mode() == RegistrationFieldMode::required) keys.insert(keys.end(), {'L','l'});
        if (configuration_.birth_date_mode() == RegistrationFieldMode::required) keys.insert(keys.end(), {'D','d'});
        if (configuration_.sex_mode() == RegistrationFieldMode::required) keys.insert(keys.end(), {'X','x'});
        if (configuration_.email_mode() != EmailRegistrationMode::off) keys.insert(keys.end(), {'E','e'});
        if (configuration_.transfer_protocol_question_enabled()) keys.insert(keys.end(), {'T','t'});
        std::string prompt = "\n[S] Save and return to confirmation\n\nEnter keeps the current value.\nSelection [S]: ";
        return {"registration.edit.action", std::move(prompt), std::move(keys), 'S', true};
    }

    std::optional<Phase3aWorkflowCode> review_and_edit() {
        for (;;) {
            if (!draft_.credential.has_value() || !valid_profile(draft_.profile))
                return Phase3aWorkflowCode::internal_failure;
            if (const auto stop = present("registration.review", review_text()); stop.has_value()) return stop;
            const auto request = review_request();
            const auto action = dependencies_.terminal.read_key(request);
            if (action.outcome == TerminalOutcome::invalid && !action.key) continue;
            if (!valid_terminal_key_result(request, action) || !action.has_key()) return terminal_code(action.outcome);
            const char key = static_cast<char>(std::toupper(static_cast<unsigned char>(*action.key)));
            if (key == 'C') return std::nullopt;
            if (key == 'Q') {
                if (const auto stop = present("registration.goodbye", "\nGoodbye.\n"); stop.has_value()) return stop;
                return Phase3aWorkflowCode::caller_goodbye;
            }
            if (key != 'R') return Phase3aWorkflowCode::internal_failure;
            editing_ = true;
            const auto stop = edit_draft();
            editing_ = false;
            if (stop) return stop;
        }
    }

    std::optional<Phase3aWorkflowCode> edit_draft() {
        for (;;) {
            const auto cleared = dependencies_.terminal.clear_screen();
            if (cleared != TerminalOutcome::completed) return terminal_code(cleared);
            if (const auto stop = present("registration.review", review_text(false, true)); stop) return stop;
            const auto request = edit_request();
            const auto action = dependencies_.terminal.read_key(request);
            if (action.outcome == TerminalOutcome::invalid && !action.key) continue;
            if (!valid_terminal_key_result(request, action) || !action.has_key()) return terminal_code(action.outcome);
            const char key = static_cast<char>(std::toupper(static_cast<unsigned char>(*action.key)));
            if (key == 'S') return std::nullopt;
            if (key == 'N') {
                const auto entered = read_name();
                if (entered.stop.has_value()) return entered.stop;
                if (!entered.name.has_value()) return Phase3aWorkflowCode::internal_failure;
                if (*entered.name == draft_.display_name) continue;
                if (!name_is_registerable(*entered.name)) {
                    if (name_check_stop_) return name_check_stop_;
                    if (name_check_failed_) return Phase3aWorkflowCode::storage_failure;
                    if (!name_notice_sent_) {
                        if (const auto stop = present("registration.name.unavailable", "That caller name is unavailable.\n");
                            stop.has_value()) return stop;
                    }
                    continue;
                }
                draft_.display_name = *entered.name;
                if (configuration_.real_names_required()) draft_.profile.real_name = draft_.display_name;
                draft_.credential.reset();
                if (const auto stop = acquire_credential(); stop.has_value()) return stop;
                continue;
            }
            if (key == 'P') { if (const auto stop = acquire_credential(); stop.has_value()) return stop; continue; }
            if (key == 'R') { if (const auto stop = read_real_name(); stop.has_value()) return stop; }
            else if (key == 'L') { if (const auto stop = read_location(); stop.has_value()) return stop; }
            else if (key == 'D') { if (const auto stop = read_birth_date(); stop.has_value()) return stop; }
            else if (key == 'X') { if (const auto stop = read_sex(); stop.has_value()) return stop; }
            else if (key == 'E') { if (const auto stop = read_email(); stop.has_value()) return stop; }
            else if (key == 'T') { if (const auto stop = read_transfer(); stop.has_value()) return stop; }
            else return Phase3aWorkflowCode::internal_failure;
        }
    }

    Phase3aWorkflowResult commit_and_continue() {
        if (!configuration_.allow_new_users() || !authoritative_policy_matches())
            return finish(Phase3aWorkflowCode::storage_failure);
        if (!name_is_registerable(draft_.display_name, true))
            return finish(name_check_stop_.value_or(name_check_failed_ ? Phase3aWorkflowCode::storage_failure
                                             : Phase3aWorkflowCode::registration_unavailable));
        if (!draft_.credential.has_value()) return finish(Phase3aWorkflowCode::internal_failure);
        const std::int64_t now = dependencies_.clock.unix_seconds();
        RegistrationCommitRequest commit;
        commit.account.display_name = draft_.display_name;
        commit.account.profile = draft_.profile;
        commit.account.credential = std::move(*draft_.credential);
        draft_.credential.reset();
        commit.account.audit = {registration_event_id_, request_.correlation_id, std::nullopt,
            now, AuditReasonCode::registration};
        commit.security_level = configuration_.security_policy().new_user_security_level;
        commit.transfer_preference = draft_.transfer;
        commit.initial_statistics = {};
        commit.expected_security_policy_revision = configuration_.security_policy().revision;
        commit.expected_security_policy_hash = configuration_.security_policy().policy_hash;
        if (!valid_registration_commit_request(commit))
            return finish(Phase3aWorkflowCode::internal_failure);

        auto committed = dependencies_.registration_commit.commit_registration(commit);
        if (!committed.succeeded() && (committed.status.code == StorageErrorCode::io ||
            committed.status.code == StorageErrorCode::busy ||
            committed.status.code == StorageErrorCode::backpressure))
            committed = dependencies_.registration_commit.commit_registration(commit);
        if (!committed.succeeded())
            return finish(committed.status.code == StorageErrorCode::conflict ||
                    committed.status.code == StorageErrorCode::constraint
                ? Phase3aWorkflowCode::registration_unavailable
                : Phase3aWorkflowCode::storage_failure);
        if (!committed.account_id.has_value() || *committed.account_id == 0 ||
            !committed.revision.has_value() || *committed.revision < 1)
            return finish(Phase3aWorkflowCode::storage_failure);
        committed_account_id_ = *committed.account_id;
        committed_utc_ = now;
        committed_revision_ = *committed.revision;
        if (dependencies_.stages) dependencies_.stages->observation(
            CallerWorkflowObservation::registration_committed, committed_account_id_);
        const auto expected_key = account_name_comparison_key(draft_.display_name);
        if (!expected_key.has_value()) return finish(Phase3aWorkflowCode::internal_failure);
        auto account = dependencies_.accounts.find_account_by_id(committed_account_id_);
        if (!account || !account.value.has_value() || account.value->revision < committed_revision_ ||
            !valid_registration_account(*account.value, committed_account_id_, *expected_key,
                commit.security_level, configuration_)) return finish(Phase3aWorkflowCode::storage_failure);

        bool system_verified = false;
        if (commit.security_level >= configuration_.security_policy().sysop_security_threshold) {
            const auto system = authenticate_system_password(committed_account_id_);
            if (system.action == SystemPasswordAction::resume_existing)
                return resume_existing_caller();
            if (system.action == SystemPasswordAction::stop) return finish(system.code);
            system_verified = true;
        }
        if(dependencies_.caller_password&&!caller_password_.empty())
            dependencies_.caller_password(committed_account_id_,commit.account.credential,caller_password_);
        explicitly_erase_terminal_secret(caller_password_);
        return complete_authenticated_session(std::move(*account.value), *expected_key, system_verified);
    }

    Phase3aRateScopeSet system_password_scopes() const {
        Phase3aRateScopeSet scopes;
        scopes.source_ip = source_ip_;
        scopes.source = true;
        scopes.global = true;
        scopes.system_password = true;
        return scopes;
    }

    SystemPasswordResult authenticate_system_password(AccountId account_id) {
        if (dependencies_.stages) dependencies_.stages->stage(CallerStage::system_password);
        const Phase3aRateScopeSet scopes = system_password_scopes();
        const auto gate = dependencies_.rates.await_admission(scopes,
            dependencies_.clock.unix_seconds(), cancellation_);
        if (gate.code != Phase3aRateGateCode::admitted) {
            switch (gate.code) {
                case Phase3aRateGateCode::cancelled:
                    return {SystemPasswordAction::stop, Phase3aWorkflowCode::cancelled};
                case Phase3aRateGateCode::timed_out:
                    return {SystemPasswordAction::stop, Phase3aWorkflowCode::timed_out};
                case Phase3aRateGateCode::disconnected:
                    return {SystemPasswordAction::stop, Phase3aWorkflowCode::disconnected};
                case Phase3aRateGateCode::shutdown_requested:
                    return {SystemPasswordAction::stop, Phase3aWorkflowCode::shutdown_requested};
                default:
                    return {SystemPasswordAction::stop,
                        Phase3aWorkflowCode::temporarily_unavailable};
            }
        }
        const TerminalInputRequest request{"registration.system_password", "su password: ",
            kPasswordMaximum, EmptyInputPolicy::reject};
        auto secret = dependencies_.terminal.read_secret(request);
        if (!valid_terminal_input_result(request, secret) || !secret.has_value()) {
            explicitly_erase_terminal_secret(secret.value);
            if (secret.outcome != TerminalOutcome::invalid &&
                secret.outcome != TerminalOutcome::overlength)
                return {SystemPasswordAction::stop, terminal_code(secret.outcome)};
            return failed_system_password(account_id, scopes);
        }
        const auto now = dependencies_.clock.unix_seconds();
        const AuthenticationAuditContext audit{dependencies_.identifiers.generate(),
            request_.correlation_id, account_id, now};
        if (!valid_authentication_audit_context(audit)) {
            explicitly_erase_terminal_secret(secret.value);
            return {SystemPasswordAction::stop, Phase3aWorkflowCode::internal_failure};
        }
        const auto verified = dependencies_.credentials.verify_system_password(
            secret.value, now, audit, cancellation_);
        explicitly_erase_terminal_secret(secret.value);
        if (verified.authenticated() && verified.verified)
            return {SystemPasswordAction::verified,
                Phase3aWorkflowCode::authenticated_caller_ready};
        if (verified.code == AuthenticationOperationCode::cancelled)
            return {SystemPasswordAction::stop, Phase3aWorkflowCode::cancelled};
        if (verified.code == AuthenticationOperationCode::backpressure)
            return {SystemPasswordAction::stop, Phase3aWorkflowCode::kdf_backpressure};
        if (verified.code == AuthenticationOperationCode::storage_failure)
            return {SystemPasswordAction::stop, Phase3aWorkflowCode::storage_failure};
        if (verified.code == AuthenticationOperationCode::throttled ||
            verified.code == AuthenticationOperationCode::unavailable)
            return {SystemPasswordAction::stop,
                Phase3aWorkflowCode::temporarily_unavailable};
        return failed_system_password(account_id, scopes);
    }

    SystemPasswordResult failed_system_password(
        AccountId account_id, const Phase3aRateScopeSet& scopes) {
        const auto now = dependencies_.clock.unix_seconds();
        if (!dependencies_.rates.record_failure(scopes, now).succeeded())
            return {SystemPasswordAction::stop, Phase3aWorkflowCode::storage_failure};
        EnqueueFailedLoginNoticeRequest notice{dependencies_.identifiers.generate(),
            account_id, source_ip_, now};
        if (!notice.notice_id.valid() ||
            !dependencies_.phase3a_storage.enqueue_failed_login_notice(notice).succeeded())
            return {SystemPasswordAction::stop, Phase3aWorkflowCode::storage_failure};
        if (const auto stop = present("authentication.failure", "Login unsuccessful.\n"); stop.has_value())
            return {SystemPasswordAction::stop, *stop};
        if (record_failed_account_attempt(attempts_) ==
            ConnectionAttemptAction::disconnect)
            return {SystemPasswordAction::stop,
                Phase3aWorkflowCode::authentication_denied};
        if (attempts_.failed_account_attempts ==
            configuration_.maximum_authentication_attempts() - 1U) {
            if (const auto stop = present("authentication.final_attempt_warning",
                    "Warning: this is your final login attempt.\n"); stop.has_value())
                return {SystemPasswordAction::stop, *stop};
        }
        return {SystemPasswordAction::resume_existing,
            Phase3aWorkflowCode::authentication_denied};
    }

    Phase3aWorkflowResult resume_existing_caller() {
        Phase3aExistingCallerDependencies dependencies{dependencies_.terminal,
            dependencies_.names, dependencies_.credentials, dependencies_.accounts,
            dependencies_.authentication_storage, dependencies_.phase3a_storage,
            dependencies_.admission, dependencies_.session_commit,
            dependencies_.displays, dependencies_.rates, dependencies_.local_time,
            dependencies_.welcome_statistics, dependencies_.clock,
            dependencies_.identifiers, dependencies_.stages, dependencies_.cleanup_health,dependencies_.new_names,dependencies_.caller_password};
        Phase3aExistingCallerWorkflow workflow(dependencies);
        Phase3aExistingCallerRequest request;
        request.source_ip = source_ip_;
        request.correlation_id = request_.correlation_id;
        request.resume = Phase3aExistingCallerRequest::ResumeState{
            graphics_, attempts_.failed_account_attempts, committed_account_id_};
        return workflow.run(configuration_, session_, request, cancellation_);
    }

    Phase3aWorkflowResult complete_authenticated_session(AccountRecord account,
        const std::string& expected_name_key, bool system_password_verified) {
        const auto policy = dependencies_.accounts.security_policy();
        auto current = dependencies_.accounts.find_account_by_id(account.account_id);
        if (!policy || !policy.value.has_value() || !policy_matches(*policy.value) || !current ||
            !current.value.has_value() || !valid_registration_account(*current.value,
                account.account_id, expected_name_key,
                configuration_.security_policy().new_user_security_level, configuration_))
            return finish(Phase3aWorkflowCode::storage_failure);
        const bool sysop = *current.value->security_level >= policy.value->sysop_threshold;
        if (sysop != system_password_verified) return finish(Phase3aWorkflowCode::storage_failure);
        account = std::move(*current.value);
        const AccountStatistics pre_commit_statistics = account.statistics;

        AccountSessionAdmissionRequest admission;
        admission.lease_id = dependencies_.identifiers.generate();
        admission.account_id = account.account_id;
        admission.intended_session_id = durable_session_id_;
        admission.diagnostic_node = session_.node().value();
        admission.maximum_sessions_per_account =
            account.security_level==999 && system_password_verified ? configuration_.foundation().logical_nodes : 1;
        if (!admission.lease_id.valid()) return finish(Phase3aWorkflowCode::internal_failure);
        const auto admitted = dependencies_.admission.acquire(admission, cancellation_);
        if (!admitted.acquired() || !admitted.lease.has_value()) {
            if (admitted.code == AccountSessionAdmissionCode::account_limit) {
                if (dependencies_.stages) dependencies_.stages->observation(
                    CallerWorkflowObservation::duplicate_denied, account.account_id);
                if (dependencies_.stages) dependencies_.stages->attention(
                    AttentionKind::duplicate_login, account.account_id, *account.display_name, admitted.existing_node);
                if (const auto stop = present("authentication.already_online",
                        std::string(kDuplicateCallerNotice)); stop) return finish(*stop);
            }
            return finish(admitted.code == AccountSessionAdmissionCode::cancelled
                ? Phase3aWorkflowCode::cancelled : Phase3aWorkflowCode::temporarily_unavailable);
        }
        if (admitted.lease->lease_id != admission.lease_id ||
            admitted.lease->account_id != admission.account_id ||
            admitted.lease->intended_session_id != admission.intended_session_id) {
            const bool released = dependencies_.admission.release(*admitted.lease);
            return finish(released ? Phase3aWorkflowCode::internal_failure
                                   : Phase3aWorkflowCode::cleanup_failure);
        }
        lease_owner_ = AccountSessionAdmissionLeaseOwner::create(
            std::move(*admitted.lease), dependencies_.admission, dependencies_.cleanup_health);
        if (!lease_owner_.has_value()) return finish(Phase3aWorkflowCode::internal_failure);

        const auto now = dependencies_.clock.unix_seconds();
        AuthenticatedSessionCommitRequest session_commit;
        session_commit.session = {durable_session_id_, request_.correlation_id,
            account.account_id, session_.node().value(), now};
        session_commit.session.journal_transport = protocol_ == AuthenticationProtocol::ssh ? "SSH" : "Telnet";
        session_commit.session.journal_source_ip = source_ip_;
        session_commit.session.journal_ansi = graphics_ == GraphicsMode::ansi ? 1 : 0;
        session_commit.session.journal_new_registration = true;
        session_commit.session.journal_registration_utc = committed_utc_;
        session_commit.successful_source = {dependencies_.identifiers.generate(),
            account.account_id, source_ip_, protocol_, now};
        session_commit.expected_account_revision = account.revision;
        session_commit.expected_account_name_key = expected_name_key;
        session_commit.expected_security_level = *account.security_level;
        session_commit.expected_security_policy_revision = policy.value->revision;
        session_commit.expected_security_policy_hash = policy.value->policy_hash;
        if (!valid_authenticated_session_commit_request(session_commit))
            return finish(Phase3aWorkflowCode::internal_failure);
        durable_cleanup_.emplace(dependencies_.accounts, dependencies_.clock,
            durable_session_id_, dependencies_.identifiers.generate(), now, dependencies_.cleanup_health,
            [observer=dependencies_.stages] {return observer?observer->accounting_snapshot():SessionAccountingSnapshot{};});
        const auto session_committed = dependencies_.session_commit.commit_authenticated_session(session_commit);
        if (!session_committed.succeeded()) {
            durable_cleanup_->commit_failed(session_committed.status.code);
            return finish(Phase3aWorkflowCode::storage_failure);
        }
        durable_cleanup_->confirm_started();
        if (dependencies_.stages) dependencies_.stages->observation(
            CallerWorkflowObservation::durable_session_started, account.account_id, durable_session_id_);

        const auto final_policy = dependencies_.accounts.security_policy();
        auto authoritative = dependencies_.accounts.find_account_by_id(account.account_id);
        if (!final_policy || !final_policy.value.has_value() || !policy_matches(*final_policy.value) ||
            final_policy.value->revision != policy.value->revision ||
            final_policy.value->policy_hash != policy.value->policy_hash || !authoritative ||
            !authoritative.value.has_value() || authoritative.value->revision < account.revision ||
            !valid_registration_account(*authoritative.value, account.account_id,
                expected_name_key, *account.security_level, configuration_))
            return finish(Phase3aWorkflowCode::storage_failure);
        account = std::move(*authoritative.value);
        const auto level = core::SecurityLevel::create(*account.security_level);
        if (!level.has_value() || !session_.authenticate({expected_name_key,
                *account.display_name, *level, account.account_id}))
            return finish(Phase3aWorkflowCode::internal_failure);
        Phase3aRateScopeSet account_scope; account_scope.account_id = account.account_id;
        if (!dependencies_.rates.record_success(account_scope,
                dependencies_.clock.unix_seconds()).succeeded())
            return finish(Phase3aWorkflowCode::storage_failure);
        if (system_password_verified) {
            Phase3aRateScopeSet system_scope; system_scope.system_password = true;
            if (!dependencies_.rates.record_success(system_scope,
                    dependencies_.clock.unix_seconds()).succeeded())
                return finish(Phase3aWorkflowCode::storage_failure);
        }
        if (const auto stop = caller_information(account,
                pre_commit_statistics.last_successful_login_utc_seconds); stop.has_value())
            return finish(*stop);
        if (const auto stop = failed_login_notices(account.account_id); stop.has_value())
            return finish(*stop);
        if (const auto stop = present(
                "caller.continue.prompt", "Press <ENTER> to continue.");
            stop.has_value()) return finish(*stop);
        const auto paused = dependencies_.terminal.pause({"caller.continue"});
        if (paused != TerminalOutcome::completed) return finish(terminal_code(paused));

        if (const auto stop = optional_display(TrustedDisplayIdentity::welcome1,
                graphics_, "caller.welcomes", std::nullopt); stop.has_value()) return finish(*stop);
        if (const auto stop = optional_display(TrustedDisplayIdentity::all,
                graphics_, "caller.all", std::nullopt, &account); stop.has_value()) return finish(*stop);

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
        if (!handoff.has_value()) return finish(Phase3aWorkflowCode::internal_failure);
        successful_continuation_ = true;
        Phase3aWorkflowResult result;
        result.code = Phase3aWorkflowCode::authenticated_caller_ready;
        result.ready.emplace(std::move(*handoff));
        return result;
    }

    std::optional<Phase3aWorkflowCode> caller_information(const AccountRecord& account,
        std::optional<std::int64_t> previous_successful_login) {
        if (dependencies_.stages) dependencies_.stages->stage(CallerStage::caller_information);
        const auto current = dependencies_.local_time.format_utc_seconds(
            dependencies_.clock.unix_seconds());
        const auto today = dependencies_.welcome_statistics
            .record_successful_call_and_count_today();
        if (!today) return Phase3aWorkflowCode::storage_failure;
        if (!current.has_value() || current->size() < 16U ||
            !valid_local_timestamp(*current) || !today.value.has_value() ||
            (*current)[4] != '-' || (*current)[7] != '-' || (*current)[10] != ' ' ||
            (*current)[13] != ':' || (*current)[11] < '0' || (*current)[11] > '9' ||
            (*current)[12] < '0' || (*current)[12] > '9')
            return Phase3aWorkflowCode::internal_failure;
        const int hour = ((*current)[11] - '0') * 10 + ((*current)[12] - '0');
        if (hour > 23) return Phase3aWorkflowCode::internal_failure;
        const char* period = hour < 12 ? "morning" : hour < 18 ? "afternoon" : "evening";
        const bool returning = previous_successful_login.has_value();
        std::string greeting = "\nGood " + std::string(period) + ", " +
            *account.display_name + (returning ? ", welcome back.\n" :
                ", welcome to " + configuration_.prelogin_identity().bbs_name + ".\n");
        if (const auto stop = present("caller.greeting", greeting);
            stop.has_value()) return stop;
        std::string text = "Your IP address is: " + source_ip_ + "\n";
        if (previous_successful_login.has_value()) {
            const auto formatted = dependencies_.local_time.format_utc_seconds(*previous_successful_login);
            if (!formatted.has_value() || formatted->size() < 16U ||
                !valid_local_timestamp(*formatted) || (*formatted)[4] != '-' ||
                (*formatted)[7] != '-' || (*formatted)[10] != ' ' ||
                (*formatted)[13] != ':')
                return Phase3aWorkflowCode::internal_failure;
            text += "You last called on " + formatted->substr(5, 2) + "-" +
                formatted->substr(8, 2) + "-" + formatted->substr(2, 2) +
                " at " + formatted->substr(11, 5) + ".\n";
        }
        text += *today.value == 1U
            ? "There has been 1 call today.\n"
            : "There have been " + std::to_string(*today.value) + " calls today.\n";
        text += account.statistics.successful_login_count == 1U
            ? "You have called 1 time.\n"
            : "You have called " +
                std::to_string(account.statistics.successful_login_count) + " times.\n";
        const std::int64_t minutes = account.statistics.authenticated_session_seconds / 60;
        text += minutes < 1
            ? "You have spent less than one minute online.\n"
            : "You have spent " + std::to_string(minutes) +
                (minutes == 1 ? " minute online.\n" : " minutes online.\n");
        text += "Your security level is " + std::to_string(*account.security_level) + ".\n\n";
        return present("caller.information", text);
    }

    std::optional<Phase3aWorkflowCode> failed_login_notices(AccountId account_id) {
        ClaimFailedLoginNoticesRequest claim;
        claim.account_id = account_id;
        claim.claim_id = dependencies_.identifiers.generate();
        claim.session_id = durable_session_id_;
        claim.claimed_utc_seconds = dependencies_.clock.unix_seconds();
        claim.limit = kFailedLoginNoticeMaximum;
        if (!claim.claim_id.valid()) return Phase3aWorkflowCode::internal_failure;
        const auto claimed = dependencies_.phase3a_storage.claim_failed_login_notices(claim);
        if (!claimed) return claimed.status.code == StorageErrorCode::conflict ? std::nullopt
            : std::optional<Phase3aWorkflowCode>(Phase3aWorkflowCode::storage_failure);
        if (!claimed.value.has_value() || claimed.value->claim_id != claim.claim_id ||
            claimed.value->session_id != claim.session_id ||
            claimed.value->notices.size() > kFailedLoginNoticeMaximum)
            return Phase3aWorkflowCode::storage_failure;
        for (const auto& notice : claimed.value->notices) {
            const auto formatted = dependencies_.local_time.format_utc_seconds(notice.event_utc_seconds);
            if (notice.account_id != account_id || !notice.notice_id.valid() ||
                !canonicalize_source_ip(notice.source_ip).has_value() || !formatted.has_value() ||
                !valid_local_timestamp(*formatted)) return Phase3aWorkflowCode::storage_failure;
            if (const auto stop = present("caller.failed_login_notice",
                    "Failed login attempt - " + notice.source_ip + " - " + *formatted + "\n");
                stop.has_value()) return stop;
        }
        if (!claimed.value->notices.empty() &&
            !dependencies_.phase3a_storage.acknowledge_failed_login_notices(
                {account_id, claim.claim_id, durable_session_id_}).succeeded())
            return Phase3aWorkflowCode::storage_failure;
        return std::nullopt;
    }

    Phase3aWorkflowResult finish(Phase3aWorkflowCode code) {
        if (successful_continuation_) return {code, {}, {}, {}};
        bool cleanup_ok = true;
        if (dependencies_.stages) dependencies_.stages->stage(CallerStage::cleanup);
        if (durable_cleanup_ && !durable_cleanup_->finalize(0, session_outcome(code)))
            cleanup_ok = false;
        if (lease_owner_.has_value() && lease_owner_->active()) {
            if (!lease_owner_->release()) cleanup_ok = false;
            lease_owner_.reset();
        }
        if (session_.state() != core::SessionState::disconnected) {
            if (session_.state() != core::SessionState::disconnecting) session_.request_disconnect();
            if (session_.state() == core::SessionState::disconnecting) session_.finalize_disconnect();
        }
        if (!cleanup_ok && dependencies_.cleanup_health) dependencies_.cleanup_health->failed.store(true);
        Phase3aWorkflowResult result{
            cleanup_ok ? code : Phase3aWorkflowCode::cleanup_failure, {}, {}, {}};
        result.preauthentication_end = preauthentication_end_;
        return result;
    }

    Phase3aRegistrationDependencies dependencies_;
    const Phase3aConfigurationSnapshot& configuration_;
    core::Session& session_;
    const Phase3aRegistrationRequest& request_;
    const CancellationToken& cancellation_;
    Draft draft_;
    std::string caller_password_; // only with a connection sink; erased on every exit
    bool editing_{false};
    std::string source_ip_;
    AuthenticationProtocol protocol_{AuthenticationProtocol::telnet};
    GraphicsMode graphics_{GraphicsMode::ascii};
    Identifier128 durable_session_id_;
    Identifier128 registration_event_id_;
    AccountId committed_account_id_{0};
    std::int64_t committed_revision_{0};
    std::int64_t committed_utc_{0};
    ConnectionAttemptState attempts_;
    std::optional<AccountSessionAdmissionLeaseOwner> lease_owner_;
    bool name_check_failed_{false};
    bool name_notice_sent_{false};
    std::optional<Phase3aWorkflowCode> name_check_stop_;
    std::optional<Phase3aPreAuthenticationEnd> preauthentication_end_;
    std::optional<DurableSessionCleanup> durable_cleanup_;
    bool successful_continuation_{false};
};

}  // namespace

Phase3aRegistrationWorkflow::Phase3aRegistrationWorkflow(
    Phase3aRegistrationDependencies dependencies) noexcept : dependencies_(dependencies) {}

Phase3aWorkflowResult Phase3aRegistrationWorkflow::run(
    const Phase3aConfigurationSnapshot& configuration, core::Session& session,
    const Phase3aRegistrationRequest& request, const CancellationToken& cancellation) {
    RegistrationExecution execution(dependencies_, configuration, session, request, cancellation);
    return execution.execute_safely();
}

}  // namespace sfx::application
