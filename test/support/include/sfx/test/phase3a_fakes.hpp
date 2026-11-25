#pragma once
#include <functional>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sfx/application/phase3a.hpp"
#include "sfx/application/phase3a_workflow.hpp"
#include "sfx/application/terminal.hpp"

namespace sfx::test {

enum class ScriptedTerminalOperationKind : std::uint8_t {
    visible_input,
    secret_input,
    key_input,
    presentation,
    pause
};

struct ScriptedTerminalObservation {
    ScriptedTerminalOperationKind kind{ScriptedTerminalOperationKind::presentation};
    std::string identity;
    std::string non_secret_text;
    application::TerminalOutcome outcome{application::TerminalOutcome::invalid};
    bool hidden_input{false};
};

class ScriptedTerminalFake final : public application::ICallerTerminal {
public:
    application::TerminalTextStyle text_style{application::TerminalTextStyle::plain};
    application::TerminalOutcome style_outcome{application::TerminalOutcome::completed};
    application::TerminalOutcome clear_outcome{application::TerminalOutcome::completed};
    std::size_t clear_count{0};
    // Key requests contain only presentation/choice metadata, never secret input.
    std::function<void(const application::TerminalKeyRequest&)> key_request_hook;
    // Inspect synthetic non-secret presentation in focused tests without adding
    // private draft fields to the fake's retained diagnostic observations.
    std::function<void(const application::TerminalPresentationRequest&)> presentation_request_hook;
    std::function<void(const application::TerminalDisplayPresentationRequest&)> display_request_hook;
    application::TerminalOutcome set_text_style(application::TerminalTextStyle style) override {
        text_style = style;
        return style_outcome;
    }
    application::TerminalOutcome clear_screen() override { ++clear_count; return clear_outcome; }
    void script_visible(
        std::string identity,
        const application::TerminalOutcome outcome,
        std::string non_secret_value = {}) {
        scripts_.push_back({ScriptedTerminalOperationKind::visible_input,
            std::move(identity), outcome, std::move(non_secret_value), std::nullopt, 0});
    }

    void script_secret_completed(std::string identity, const std::size_t length) {
        scripts_.push_back({ScriptedTerminalOperationKind::secret_input,
            std::move(identity), application::TerminalOutcome::completed, {},
            std::nullopt, length});
    }

    void script_secret_outcome(
        std::string identity,
        const application::TerminalOutcome outcome) {
        scripts_.push_back({ScriptedTerminalOperationKind::secret_input,
            std::move(identity), outcome, {}, std::nullopt, 0});
    }

    void script_malformed_secret_outcome(
        std::string identity,
        const application::TerminalOutcome outcome,
        const std::size_t generated_length) {
        scripts_.push_back({ScriptedTerminalOperationKind::secret_input,
            std::move(identity), outcome, {}, std::nullopt, generated_length});
    }

    void script_key(
        std::string identity,
        const application::TerminalOutcome outcome,
        const std::optional<char> key = std::nullopt) {
        const bool identification=identity=="prelogin.graphics" &&
            (outcome==application::TerminalOutcome::empty ||
             (outcome==application::TerminalOutcome::completed && key &&
              (*key=='Y' || *key=='y' || *key=='N' || *key=='n')));
        scripts_.push_back({ScriptedTerminalOperationKind::key_input,
            std::move(identity), outcome, {}, key, 0});
        if(identification) script_presentation("prelogin.identification");
    }

    void script_presentation(
        std::string identity,
        const application::TerminalOutcome outcome =
            application::TerminalOutcome::completed) {
        scripts_.push_back({ScriptedTerminalOperationKind::presentation,
            std::move(identity), outcome, {}, std::nullopt, 0});
    }

    void script_pause(
        std::string identity,
        const application::TerminalOutcome outcome =
            application::TerminalOutcome::completed) {
        scripts_.push_back({ScriptedTerminalOperationKind::pause,
            std::move(identity), outcome, {}, std::nullopt, 0});
    }

    application::TerminalInputResult read_visible(
        const application::TerminalInputRequest& request) override {
        if (!application::valid_terminal_input_request(request)) {
            return unexpected_input(
                ScriptedTerminalOperationKind::visible_input, request.identity, request.prompt);
        }
        auto script = consume(ScriptedTerminalOperationKind::visible_input, request.identity);
        if (!script.has_value()) {
            return unexpected_input(
                ScriptedTerminalOperationKind::visible_input, request.identity, request.prompt);
        }
        application::TerminalInputResult result{script->outcome, {}};
        if (script->outcome == application::TerminalOutcome::completed) {
            if (script->value.empty()) {
                result.outcome = request.empty_policy == application::EmptyInputPolicy::use_default
                    ? application::TerminalOutcome::empty
                    : application::TerminalOutcome::invalid;
            } else if (script->value.size() > request.maximum_characters) {
                result.outcome = application::TerminalOutcome::overlength;
            } else {
                result.value = std::move(script->value);
            }
        } else if (script->outcome == application::TerminalOutcome::empty &&
            request.empty_policy != application::EmptyInputPolicy::use_default) {
            result.outcome = application::TerminalOutcome::invalid;
        }
        observe(ScriptedTerminalOperationKind::visible_input, request.identity,
            request.prompt, result.outcome, false);
        return result;
    }

    application::TerminalInputResult read_secret(
        const application::TerminalInputRequest& request) override {
        if (!application::valid_terminal_input_request(request)) {
            return unexpected_input(
                ScriptedTerminalOperationKind::secret_input, request.identity, request.prompt);
        }
        auto script = consume(ScriptedTerminalOperationKind::secret_input, request.identity);
        if (!script.has_value()) {
            return unexpected_input(
                ScriptedTerminalOperationKind::secret_input, request.identity, request.prompt);
        }
        application::TerminalInputResult result{script->outcome, {}};
        if (script->outcome == application::TerminalOutcome::completed) {
            if (script->secret_length == 0) {
                result.outcome = request.empty_policy == application::EmptyInputPolicy::use_default
                    ? application::TerminalOutcome::empty
                    : application::TerminalOutcome::invalid;
            } else if (script->secret_length > request.maximum_characters) {
                result.outcome = application::TerminalOutcome::overlength;
            } else {
                result.value.assign(script->secret_length, 'X');
            }
        } else if (script->outcome == application::TerminalOutcome::empty &&
            request.empty_policy != application::EmptyInputPolicy::use_default) {
            result.outcome = application::TerminalOutcome::invalid;
        }
        if (script->outcome != application::TerminalOutcome::completed &&
            script->secret_length > 0) {
            result.value.assign(script->secret_length, 'X');
        }
        observe(ScriptedTerminalOperationKind::secret_input, request.identity,
            request.prompt, result.outcome, true);
        return result;
    }

    application::TerminalKeyResult read_key(
        const application::TerminalKeyRequest& request) override {
        if (key_request_hook) key_request_hook(request);
        if (!application::valid_terminal_key_request(request)) {
            return unexpected_key(request.identity, request.prompt);
        }
        auto script = consume(ScriptedTerminalOperationKind::key_input, request.identity);
        if (!script.has_value()) return unexpected_key(request.identity, request.prompt);

        application::TerminalKeyResult result{script->outcome, std::nullopt};
        if (script->outcome == application::TerminalOutcome::completed) {
            if (!script->key.has_value() ||
                std::find(request.allowed_keys.begin(), request.allowed_keys.end(),
                    *script->key) == request.allowed_keys.end()) {
                result.outcome = application::TerminalOutcome::invalid;
            } else {
                result.key = script->key;
            }
        } else if (script->outcome == application::TerminalOutcome::empty) {
            if (request.default_key.has_value()) {
                result.key = request.default_key;
            } else {
                result.outcome = application::TerminalOutcome::invalid;
            }
        }
        observe(ScriptedTerminalOperationKind::key_input, request.identity,
            request.prompt, result.outcome, false);
        return result;
    }

    application::TerminalOutcome present(
        const application::TerminalPresentationRequest& request) override {
        if (!application::valid_terminal_presentation_request(request)) {
            return unexpected_output(
                ScriptedTerminalOperationKind::presentation, request.identity, {});
        }
        auto script = consume(ScriptedTerminalOperationKind::presentation, request.identity);
        if (!script.has_value()) {
            return unexpected_output(
                ScriptedTerminalOperationKind::presentation, request.identity,
                request.non_secret_text);
        }
        const bool private_registration_review = request.identity == "registration.review";
        if (presentation_request_hook) presentation_request_hook(request);
        observe(ScriptedTerminalOperationKind::presentation, request.identity,
            private_registration_review
                ? std::string_view{"[private registration review redacted]"}
                : std::string_view{request.non_secret_text},
            script->outcome, false);
        return script->outcome;
    }

    application::TerminalOutcome present_display(
        const application::TerminalDisplayPresentationRequest& request) override {
        if (!application::valid_terminal_display_presentation_request(request)) {
            return unexpected_output(
                ScriptedTerminalOperationKind::presentation, request.identity, {});
        }
        auto script = consume(
            ScriptedTerminalOperationKind::presentation, request.identity);
        if (!script.has_value()) {
            return unexpected_output(
                ScriptedTerminalOperationKind::presentation, request.identity,
                "[typed trusted display]");
        }
        if (display_request_hook) display_request_hook(request);
        observe(ScriptedTerminalOperationKind::presentation, request.identity,
            "[typed trusted display]", script->outcome, false);
        return script->outcome;
    }

    application::TerminalOutcome pause(
        const application::TerminalPauseRequest& request) override {
        if (!application::valid_terminal_pause_request(request)) {
            return unexpected_output(
                ScriptedTerminalOperationKind::pause, request.identity, {});
        }
        auto script = consume(ScriptedTerminalOperationKind::pause, request.identity);
        if (!script.has_value()) {
            return unexpected_output(
                ScriptedTerminalOperationKind::pause, request.identity, {});
        }
        observe(ScriptedTerminalOperationKind::pause, request.identity, {},
            script->outcome, false);
        return script->outcome;
    }

    const std::vector<ScriptedTerminalObservation>& observations() const noexcept {
        return observations_;
    }

    std::size_t pending_operations() const noexcept {
        return scripts_.size();
    }

    std::size_t unexpected_operations() const noexcept {
        return unexpected_operations_;
    }

    std::size_t hidden_input_requests() const noexcept {
        return hidden_input_requests_;
    }

    std::size_t retained_secret_bytes() const noexcept {
        return 0;
    }

    bool clean() const noexcept {
        return scripts_.empty() && unexpected_operations_ == 0;
    }

private:
    struct ScriptedOperation {
        ScriptedTerminalOperationKind kind;
        std::string identity;
        application::TerminalOutcome outcome;
        std::string value;
        std::optional<char> key;
        std::size_t secret_length;
    };

    std::optional<ScriptedOperation> consume(
        const ScriptedTerminalOperationKind expected_kind,
        const std::string_view expected_identity) {
        if (scripts_.empty() || scripts_.front().kind != expected_kind ||
            scripts_.front().identity != expected_identity) {
            return std::nullopt;
        }
        ScriptedOperation result = std::move(scripts_.front());
        scripts_.pop_front();
        return result;
    }

    application::TerminalInputResult unexpected_input(
        const ScriptedTerminalOperationKind kind,
        const std::string_view identity,
        const std::string_view prompt) {
        ++unexpected_operations_;
        observe(kind, identity, prompt, application::TerminalOutcome::invalid,
            kind == ScriptedTerminalOperationKind::secret_input);
        return {application::TerminalOutcome::invalid, {}};
    }

    application::TerminalKeyResult unexpected_key(
        const std::string_view identity,
        const std::string_view prompt) {
        ++unexpected_operations_;
        observe(ScriptedTerminalOperationKind::key_input, identity, prompt,
            application::TerminalOutcome::invalid, false);
        return {application::TerminalOutcome::invalid, std::nullopt};
    }

    application::TerminalOutcome unexpected_output(
        const ScriptedTerminalOperationKind kind,
        const std::string_view identity,
        const std::string_view non_secret_text) {
        ++unexpected_operations_;
        observe(kind, identity, non_secret_text,
            application::TerminalOutcome::output_failure, false);
        return application::TerminalOutcome::output_failure;
    }

    void observe(
        const ScriptedTerminalOperationKind kind,
        const std::string_view identity,
        const std::string_view non_secret_text,
        const application::TerminalOutcome outcome,
        const bool hidden) {
        if (hidden) ++hidden_input_requests_;
        observations_.push_back(
            {kind, std::string(identity), std::string(non_secret_text), outcome, hidden});
    }

    std::deque<ScriptedOperation> scripts_;
    std::vector<ScriptedTerminalObservation> observations_;
    std::size_t unexpected_operations_{0};
    std::size_t hidden_input_requests_{0};
};

class FixedBbsLocalDateProvider final : public application::IBbsLocalDateProvider {
public:
    application::CivilDate value{2000, 1, 1};

    application::CivilDate today() const noexcept override {
        return value;
    }
};

class FakeCallerNameClassificationPort final
    : public application::ICallerNameClassificationPort {
public:
    std::deque<application::StorageResult<application::CallerNameResolution>> results;
    std::vector<std::string> names;

    application::StorageResult<application::CallerNameResolution> classify(
        const std::string_view normalized_name) override {
        names.emplace_back(normalized_name);
        if (results.empty()) {
            return {{application::StorageErrorCode::internal, "fake classify"}, {}};
        }
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }
};

struct SafeCredentialObservation {
    std::string operation;
    application::AccountId account_id{0};
    std::size_t candidate_length{0};
    std::size_t replacement_length{0};
};

class FakePhase3aCredentialOperations final
    : public application::IPhase3aCredentialOperations {
public:
    std::deque<application::PasswordAuthenticationResult> existing_results;
    std::deque<application::PasswordAuthenticationResult> dummy_results;
    std::deque<application::PasswordAuthenticationResult> system_results;
    std::deque<application::PasswordMutationResult> replacement_results;
    application::IPhase3aCredentialOperations::RegistrationCredentialResult
        registration_result;
    std::deque<application::IPhase3aCredentialOperations::RegistrationCredentialResult>
        registration_results;
    std::vector<SafeCredentialObservation> observations;

    application::PasswordAuthenticationResult verify_existing_account(
        const application::AccountId account_id,
        const std::string_view candidate,
        std::int64_t,
        const application::AuthenticationAuditContext&,
        const application::CancellationToken&) override {
        observations.push_back(
            {"verify_existing", account_id, candidate.size(), 0});
        return pop(existing_results);
    }

    application::PasswordAuthenticationResult perform_dummy_verification(
        const std::string_view candidate,
        std::int64_t,
        const application::CancellationToken&) override {
        observations.push_back({"verify_dummy", 0, candidate.size(), 0});
        return pop(dummy_results);
    }

    application::IPhase3aCredentialOperations::RegistrationCredentialResult
    create_registration_credential(
        const std::string_view candidate,
        const application::PasswordPolicyContext&,
        std::int64_t,
        const application::CancellationToken&) override {
        observations.push_back({"create_registration", 0, candidate.size(), 0});
        if (!registration_results.empty()) {
            auto result = std::move(registration_results.front());
            registration_results.pop_front();
            return result;
        }
        return registration_result;
    }

    application::PasswordMutationResult replace_temporary_password(
        const application::TemporaryPasswordReplacementOperation& request,
        const application::CancellationToken&) override {
        observations.push_back({"replace_temporary", request.account_id,
            request.temporary_password.size(), request.permanent_password.size()});
        return pop(replacement_results);
    }

    application::PasswordAuthenticationResult verify_system_password(
        const std::string_view candidate,
        std::int64_t,
        const application::AuthenticationAuditContext&,
        const application::CancellationToken&) override {
        observations.push_back({"verify_system", 0, candidate.size(), 0});
        return pop(system_results);
    }

private:
    static application::PasswordAuthenticationResult pop(
        std::deque<application::PasswordAuthenticationResult>& values) {
        if (values.empty()) {
            return {application::AuthenticationOperationCode::unavailable,
                false, false, false, false};
        }
        const auto result = values.front();
        values.pop_front();
        return result;
    }

    static application::PasswordMutationResult pop(
        std::deque<application::PasswordMutationResult>& values) {
        if (values.empty()) {
            application::PasswordMutationResult result;
            result.code = application::AuthenticationOperationCode::unavailable;
            return result;
        }
        const auto result = values.front();
        values.pop_front();
        return result;
    }
};

struct SafeRegistrationCommitObservation {
    std::string display_name;
    std::uint16_t security_level{0};
    application::TransferProtocolPreference transfer_preference{
        application::TransferProtocolPreference::select_at_transfer};
    bool has_real_name{false};
    bool has_location{false};
    bool has_birth_date{false};
    bool has_sex{false};
    bool has_email{false};
    std::size_t credential_bytes{0};
    application::Identifier128 event_id;
    application::Identifier128 correlation_id;
    std::int64_t expected_policy_revision{0};
    std::string expected_policy_hash;
};

class FakeRegistrationCommitPort final
    : public application::IRegistrationCommitPort {
public:
    std::deque<application::MutationResult> results;
    std::vector<SafeRegistrationCommitObservation> observations;

    application::MutationResult commit_registration(
        const application::RegistrationCommitRequest& request) override {
        observations.push_back({request.account.display_name,
            request.security_level, request.transfer_preference,
            request.account.profile.real_name.has_value(),
            request.account.profile.location.has_value(),
            request.account.profile.birth_date.has_value(),
            request.account.profile.sex.has_value(),
            request.account.profile.email.has_value(),
            request.account.credential.canonical_parameters.size() +
                request.account.credential.salt.size() +
                request.account.credential.verifier.size(),
            request.account.audit.event_id, request.account.audit.correlation_id,
            request.expected_security_policy_revision,
            request.expected_security_policy_hash});
        if (results.empty()) {
            return {{application::StorageErrorCode::internal,
                        "fake registration commit"},
                {}, {}, false};
        }
        auto result = results.front();
        results.pop_front();
        return result;
    }
};

class FakeAccountSessionAdmissionPort final
    : public application::IAccountSessionAdmissionPort {
public:
    bool auto_acquire{true};
    application::AccountSessionAdmissionResult result;
    bool release_result{true};
    std::vector<application::AccountSessionAdmissionRequest> requests;
    std::vector<application::AccountSessionAdmissionLease> releases;

    application::AccountSessionAdmissionResult acquire(
        const application::AccountSessionAdmissionRequest& request,
        const application::CancellationToken&) override {
        requests.push_back(request);
        if (!auto_acquire) return result;
        return {application::AccountSessionAdmissionCode::acquired,
            application::AccountSessionAdmissionLease{
                request.lease_id, request.account_id, request.intended_session_id}};
    }

    bool release(
        const application::AccountSessionAdmissionLease& lease) noexcept override {
        releases.push_back(lease);
        return release_result;
    }
};

class FakeAuthenticatedSessionCommitPort final
    : public application::IAuthenticatedSessionCommitPort {
public:
    application::MutationResult result;
    std::vector<application::AuthenticatedSessionCommitRequest> requests;
    std::function<void()> commit_hook;

    application::MutationResult commit_authenticated_session(
        const application::AuthenticatedSessionCommitRequest& request) override {
        requests.push_back(request);
        if (commit_hook) commit_hook();
        return result;
    }
};

class FakeTrustedDisplaySelectionPort final
    : public application::ITrustedDisplaySelectionPort {
public:
    std::deque<application::TrustedDisplaySelectionResult> results;
    std::vector<application::TrustedDisplaySelectionRequest> requests;

    application::TrustedDisplaySelectionResult select(
        const application::TrustedDisplaySelectionRequest& request) override {
        requests.push_back(request);
        if (results.empty()) {
            return {application::TrustedDisplaySelectionCode::not_found, {}};
        }
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }
};

inline application::RenderedTrustedDisplayProgram trusted_display_program(
    const application::TrustedDisplayIdentity identity,
    const application::GraphicsMode graphics,
    std::string text) {
    application::TerminalDisplayProgram terminal;
    std::string literal;
    literal.reserve(text.size());
    const auto flush_literal = [&]() {
        if (literal.empty()) return;
        terminal.output_byte_count += literal.size();
        terminal.operations.push_back(
            {application::TerminalDisplayOperationKind::cp437_text,
                std::move(literal)});
        literal.clear();
    };
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\r' && text[index] != '\n') {
            literal.push_back(text[index]);
            continue;
        }
        flush_literal();
        const bool paging_row = text[index] == '\r' &&
            index + 1U < text.size() && text[index + 1U] == '\n';
        if (paging_row) ++index;
        terminal.operations.push_back({paging_row
                ? application::TerminalDisplayOperationKind::paging_newline
                : application::TerminalDisplayOperationKind::logical_newline,
            {}});
        terminal.output_byte_count += 2U;
    }
    flush_literal();
    const std::string extension =
        graphics == application::GraphicsMode::ansi ? ".ANS" : ".ASC";
    std::string basename;
    switch (identity) {
        case application::TrustedDisplayIdentity::sfprelog:
            basename = "SFPRELOG.ASC";
            break;
        case application::TrustedDisplayIdentity::welcome1:
            basename = "WELCOME1" + extension;
            break;
        case application::TrustedDisplayIdentity::newreg:
            basename = "NEWREG" + extension;
            break;
        case application::TrustedDisplayIdentity::newuser:
            basename = "NEWUSER" + extension;
            break;
        case application::TrustedDisplayIdentity::nonodes:
            basename = "NONODES" + extension;
            break;
        case application::TrustedDisplayIdentity::menu_main:
        case application::TrustedDisplayIdentity::menu_messages:
        case application::TrustedDisplayIdentity::menu_files:
        case application::TrustedDisplayIdentity::menu_doors:
        case application::TrustedDisplayIdentity::menu_bulletins:
        case application::TrustedDisplayIdentity::menu_door_bulletins:
        case application::TrustedDisplayIdentity::menu_chat:
        case application::TrustedDisplayIdentity::menu_settings:
        case application::TrustedDisplayIdentity::menu_file_groups:
        case application::TrustedDisplayIdentity::menu_file_areas:
        case application::TrustedDisplayIdentity::all:
        case application::TrustedDisplayIdentity::chatdone:
        case application::TrustedDisplayIdentity::eventime:
        case application::TrustedDisplayIdentity::goodbye:
        case application::TrustedDisplayIdentity::lockout:
        case application::TrustedDisplayIdentity::sfasleep:
        case application::TrustedDisplayIdentity::sfnewsletter:
        case application::TrustedDisplayIdentity::bulletin_page:
        case application::TrustedDisplayIdentity::door_bulletin_page:
        case application::TrustedDisplayIdentity::door_bulletin_generated:
        case application::TrustedDisplayIdentity::menu_batch_transfer:
        case application::TrustedDisplayIdentity::menu_qwk:
        case application::TrustedDisplayIdentity::door_submenu:
        case application::TrustedDisplayIdentity::door_prelaunch:
        case application::TrustedDisplayIdentity::menu_sysop:
            // Other identities intentionally have no asset in this fake.
            break;
    }
    application::RenderedTrustedDisplayProgram program;
    program.identity = identity;
    program.graphics = graphics;
    program.selected_logical_asset = std::move(basename);
    program.terminal_program = std::move(terminal);
    program.aggregate_source_bytes =
        program.terminal_program.output_byte_count;
    return program;
}

struct RateObservation {
    std::string operation;
    application::Phase3aRateScopeSet scopes;
    std::int64_t utc_seconds{0};
};

class FakePhase3aRateControlPort final
    : public application::IPhase3aRateControlPort {
public:
    std::deque<application::Phase3aRateGateResult> gates;
    application::StorageStatus status;
    std::vector<RateObservation> observations;

    application::Phase3aRateGateResult await_admission(
        const application::Phase3aRateScopeSet& scopes,
        const std::int64_t now,
        const application::CancellationToken&) override {
        observations.push_back({"await", scopes, now});
        if (gates.empty()) {
            return {application::Phase3aRateGateCode::admitted, 0};
        }
        const auto result = gates.front();
        gates.pop_front();
        return result;
    }

    application::StorageStatus record_failure(
        const application::Phase3aRateScopeSet& scopes,
        const std::int64_t now) override {
        observations.push_back({"failure", scopes, now});
        return status;
    }

    application::StorageStatus record_success(
        const application::Phase3aRateScopeSet& scopes,
        const std::int64_t now) override {
        observations.push_back({"success", scopes, now});
        return status;
    }
};

class FixedBbsLocalTimestampFormatter final
    : public application::IBbsLocalTimestampFormatter {
public:
    std::string value{"2026-08-28 12:34:56"};
    mutable std::vector<std::int64_t> requests;

    std::optional<std::string> format_utc_seconds(
        const std::int64_t utc_seconds) const override {
        requests.push_back(utc_seconds);
        if (value.empty()) return std::nullopt;
        return value;
    }
};

class FakeCallerWelcomeStatisticsPort final
    : public application::ICallerWelcomeStatisticsPort {
public:
    application::StorageResult<std::uint64_t> result{
        application::StorageStatus::success(), 1U};
    std::size_t calls{0};
    std::function<void()> count_hook;

    application::StorageResult<std::uint64_t>
    record_successful_call_and_count_today() override {
        ++calls;
        if (count_hook) count_hook();
        return result;
    }
};

}  // namespace sfx::test
