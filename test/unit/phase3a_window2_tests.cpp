#include "sfx/application/phase3a.hpp"
#include "sfx/application/terminal.hpp"
#include "sfx/core/session.hpp"
#include "sfx/test/account_storage_fakes.hpp"
#include "sfx/test/phase3a_fakes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
namespace app = sfx::application;
namespace core = sfx::core;
namespace test = sfx::test;

std::size_t assertions = 0;

void require(const bool condition, const char* const message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

app::Identifier128 identifier(const std::uint32_t value) {
    test::DeterministicIdentifierSource source(value);
    return source.next();
}

app::Phase3aConfigurationValues valid_configuration(const bool bootstrap = true) {
    app::Phase3aConfigurationValues values;
    values.foundation.logical_nodes = 128;
    values.bootstrap_complete = bootstrap;
    values.location_mode = app::RegistrationFieldMode::required;
    values.birth_date_mode = app::RegistrationFieldMode::required;
    values.minimum_age = std::uint8_t{18};
    values.sex_mode = app::RegistrationFieldMode::required;
    values.allow_sex_x = true;
    values.email_mode = app::EmailRegistrationMode::ask;
    values.security_policy = {1, std::string(64U, 'a'), 10, 999};
    values.prelogin_identity = {"Test BBS", "Test Sysop", "SFX 0.1"};
    return values;
}

void terminal_contracts() {
    const app::TerminalInputRequest input{
        "caller.name", "Caller name: ", 30, app::EmptyInputPolicy::reject};
    require(app::valid_terminal_input_request(input), "valid visible input");
    auto invalid = input;
    invalid.maximum_characters = 0;
    require(!app::valid_terminal_input_request(invalid), "zero bound rejected");
    invalid = input;
    invalid.maximum_characters = app::kTerminalMaximumInputCharacters + 1U;
    require(!app::valid_terminal_input_request(invalid), "global bound enforced");
    invalid = input;
    invalid.identity = "caller name";
    require(!app::valid_terminal_input_request(invalid), "identity closed");
    invalid = input;
    invalid.prompt.push_back('\n');
    require(app::valid_terminal_input_request(invalid), "explicit logical prompt newlines support per-screen spacing");
    invalid.prompt.push_back('\x1b');
    require(!app::valid_terminal_input_request(invalid), "raw ANSI/control input prompt rejected");
    invalid.prompt.back() = '\0';
    require(!app::valid_terminal_input_request(invalid), "embedded NUL input prompt rejected");

    const app::TerminalKeyRequest key{
        "graphics.choice", "Use ANSI? ", {'Y', 'N'}, 'Y'};
    require(app::valid_terminal_key_request(key), "valid key request");
    auto invalid_key = key;
    invalid_key.allowed_keys.push_back('Y');
    require(!app::valid_terminal_key_request(invalid_key), "duplicate key rejected");
    invalid_key = key;
    invalid_key.default_key = 'Q';
    require(!app::valid_terminal_key_request(invalid_key), "unknown default rejected");
    require(app::valid_terminal_presentation_request({"prelogin", "Welcome"}),
        "presentation valid");
    require(app::valid_terminal_pause_request({"caller.pause"}), "pause valid");

    const std::array<app::TerminalOutcome, 8> no_input{
        app::TerminalOutcome::cancelled, app::TerminalOutcome::timed_out,
        app::TerminalOutcome::disconnected, app::TerminalOutcome::shutdown_requested,
        app::TerminalOutcome::invalid, app::TerminalOutcome::overlength,
        app::TerminalOutcome::terminal_failure,
        app::TerminalOutcome::output_failure};
    for (const auto outcome : no_input) {
        require(app::terminal_outcome_has_no_input(outcome), "terminal stop has no input");
    }
    require(!app::terminal_outcome_has_no_input(app::TerminalOutcome::completed) &&
        !app::terminal_outcome_has_no_input(app::TerminalOutcome::empty),
        "completed and empty carry input/default semantics");

    std::array<unsigned char, 9> occupied_bytes{
        0x53U, 0x46U, 0x58U, 0x2dU, 0x53U, 0x45U, 0x43U, 0x52U, 0x45U};
    app::detail::explicitly_erase_bytes(
        occupied_bytes.data(), occupied_bytes.size());
    require(std::all_of(occupied_bytes.begin(), occupied_bytes.end(),
                [](const unsigned char value) { return value == 0U; }),
        "explicit erasure overwrites every requested occupied byte");
    app::detail::explicitly_erase_bytes(nullptr, 0);
    std::string terminal_secret(17U, 'X');
    app::explicitly_erase_terminal_secret(terminal_secret);
    require(terminal_secret.empty(),
        "terminal secret wrapper clears only after explicit occupied-byte erasure");

    const app::TerminalInputResult completed{app::TerminalOutcome::completed, "Caller"};
    require(completed.has_value() &&
        app::valid_terminal_input_result(input, completed),
        "completed input requires a present bounded value");
    const app::TerminalInputResult missing{app::TerminalOutcome::completed, {}};
    require(!missing.has_value() &&
        !app::valid_terminal_input_result(input, missing),
        "completed input without payload rejected");
    const app::TerminalInputResult too_long{
        app::TerminalOutcome::completed, std::string(31U, 'A')};
    require(too_long.has_value() &&
        !app::valid_terminal_input_result(input, too_long),
        "completed overlength payload rejected");
    const app::TerminalInputRequest default_input{
        "caller.default", "Default: ", 30, app::EmptyInputPolicy::use_default};
    const app::TerminalInputResult empty_default{app::TerminalOutcome::empty, {}};
    require(!empty_default.has_value() &&
        app::valid_terminal_input_result(default_input, empty_default),
        "empty result valid only for permitted default");
    require(!app::valid_terminal_input_result(input, empty_default) &&
        !app::valid_terminal_input_result(default_input,
            {app::TerminalOutcome::empty, "unexpected"}),
        "empty result payload and policy invariants enforced");

    const app::TerminalKeyResult completed_key{app::TerminalOutcome::completed, 'Y'};
    const app::TerminalKeyResult default_key{app::TerminalOutcome::empty, 'Y'};
    require(completed_key.has_key() &&
        app::valid_terminal_key_result(key, completed_key),
        "completed key requires allowed payload");
    require(default_key.has_key() &&
        app::valid_terminal_key_result(key, default_key),
        "empty key result carries configured default");
    require(!app::TerminalKeyResult{
            app::TerminalOutcome::completed, std::nullopt}.has_key() &&
        !app::valid_terminal_key_result(key,
            {app::TerminalOutcome::completed, std::nullopt}) &&
        !app::valid_terminal_key_result(key,
            {app::TerminalOutcome::completed, 'Q'}),
        "completed key missing/disallowed payload rejected");
    require(!app::valid_terminal_key_result(key,
            {app::TerminalOutcome::empty, std::nullopt}) &&
        !app::valid_terminal_key_result(key,
            {app::TerminalOutcome::empty, 'N'}),
        "empty key requires the actual configured default");

    for (const auto outcome : no_input) {
        const app::TerminalInputResult clean_input_stop{outcome, {}};
        const app::TerminalInputResult leaking_input_stop{outcome, "leak"};
        const app::TerminalKeyResult clean_key_stop{outcome, std::nullopt};
        const app::TerminalKeyResult leaking_key_stop{outcome, 'Y'};
        require(app::valid_terminal_input_result(input, clean_input_stop) &&
            !leaking_input_stop.has_value() &&
            !app::valid_terminal_input_result(input, leaking_input_stop),
            "terminal stop cannot expose input payload");
        require(app::valid_terminal_key_result(key, clean_key_stop) &&
            !leaking_key_stop.has_key() &&
            !app::valid_terminal_key_result(key, leaking_key_stop),
            "terminal stop cannot expose key payload");
    }
}

void scripted_input_outcomes() {
    test::ScriptedTerminalFake terminal;
    terminal.script_visible("complete", app::TerminalOutcome::completed, "Caller");
    terminal.script_visible("empty", app::TerminalOutcome::empty);
    terminal.script_visible("cancel", app::TerminalOutcome::cancelled);
    terminal.script_visible("timeout", app::TerminalOutcome::timed_out);
    terminal.script_visible("disconnect", app::TerminalOutcome::disconnected);
    terminal.script_visible("shutdown", app::TerminalOutcome::shutdown_requested);
    terminal.script_visible("invalid", app::TerminalOutcome::invalid);
    terminal.script_visible("long", app::TerminalOutcome::completed, "123456");
    const auto request = [](std::string identity, const std::size_t maximum,
                             const app::EmptyInputPolicy empty =
                                 app::EmptyInputPolicy::reject) {
        return app::TerminalInputRequest{
            std::move(identity), "Input: ", maximum, empty};
    };
    require(terminal.read_visible(request("complete", 30)).value == "Caller",
        "completed input");
    require(terminal.read_visible(request("empty", 30,
        app::EmptyInputPolicy::use_default)).outcome == app::TerminalOutcome::empty,
        "empty/default input");
    require(terminal.read_visible(request("cancel", 30)).outcome ==
        app::TerminalOutcome::cancelled, "cancel outcome");
    require(terminal.read_visible(request("timeout", 30)).outcome ==
        app::TerminalOutcome::timed_out, "timeout outcome");
    require(terminal.read_visible(request("disconnect", 30)).outcome ==
        app::TerminalOutcome::disconnected, "disconnect outcome");
    require(terminal.read_visible(request("shutdown", 30)).outcome ==
        app::TerminalOutcome::shutdown_requested, "shutdown outcome");
    require(terminal.read_visible(request("invalid", 30)).outcome ==
        app::TerminalOutcome::invalid, "invalid outcome");
    require(terminal.read_visible(request("long", 5)).outcome ==
        app::TerminalOutcome::overlength, "overlength outcome");
    require(terminal.clean(), "script consumed");
}

void secret_output_order_and_unexpected_operations() {
    test::ScriptedTerminalFake terminal;
    terminal.script_presentation("prelogin");
    terminal.script_secret_completed("password", 12);
    terminal.script_key("graphics", app::TerminalOutcome::empty);
    terminal.script_pause("continue");
    terminal.script_presentation("failure", app::TerminalOutcome::output_failure);
    require(terminal.present({"prelogin", "Welcome"}) ==
        app::TerminalOutcome::completed, "presentation completion");
    auto secret = terminal.read_secret(
        {"password", "Password: ", 128, app::EmptyInputPolicy::reject});
    require(secret.outcome == app::TerminalOutcome::completed &&
        secret.value.size() == 12U, "hidden input produced");
    const auto key = terminal.read_key(
        {"graphics", "Use ANSI? ", {'Y', 'N'}, 'Y'});
    require(key.outcome == app::TerminalOutcome::empty && key.key == 'Y',
        "key default typed");
    require(terminal.pause({"continue"}) == app::TerminalOutcome::completed,
        "pause completion");
    require(terminal.present({"failure", "Safe output"}) ==
        app::TerminalOutcome::output_failure, "output failure typed");
    require(terminal.observations().size() == 5U &&
        terminal.observations()[0].identity == "prelogin" &&
        terminal.observations()[1].identity == "password" &&
        terminal.observations()[2].identity == "graphics" &&
        terminal.observations()[3].identity == "continue" &&
        terminal.observations()[4].identity == "failure", "exact operation order");
    require(terminal.hidden_input_requests() == 1U &&
        terminal.retained_secret_bytes() == 0U, "secret retained nowhere");
    require(terminal.observations()[1].hidden_input &&
        terminal.observations()[1].non_secret_text == "Password: ",
        "hidden observation contains prompt only");
    require(std::none_of(terminal.observations().begin(), terminal.observations().end(),
        [&secret](const test::ScriptedTerminalObservation& operation) {
            return operation.non_secret_text.find(secret.value) != std::string::npos;
        }), "secret absent from capture");
    app::explicitly_erase_terminal_secret(secret.value);
    require(terminal.clean(), "secret/output script clean");

    test::ScriptedTerminalFake retry;
    retry.script_visible("expected", app::TerminalOutcome::completed, "value");
    require(retry.read_visible(
        {"wrong", "Input: ", 10, app::EmptyInputPolicy::reject}).outcome ==
        app::TerminalOutcome::invalid, "unexpected read detected");
    require(retry.unexpected_operations() == 1U && retry.pending_operations() == 1U,
        "unexpected read preserves script");
    require(retry.read_visible(
        {"expected", "Input: ", 10, app::EmptyInputPolicy::reject}).value == "value",
        "deterministic retry succeeds");

    test::ScriptedTerminalFake unexpected_write;
    unexpected_write.script_pause("pause");
    require(unexpected_write.present({"write", "Safe"}) ==
        app::TerminalOutcome::output_failure, "unexpected write detected");
    require(unexpected_write.pending_operations() == 1U &&
        unexpected_write.pause({"pause"}) == app::TerminalOutcome::completed,
        "unexpected write preserves expected operation");

    test::ScriptedTerminalFake secret_stops;
    const std::array<app::TerminalOutcome, 3> stops{
        app::TerminalOutcome::cancelled, app::TerminalOutcome::timed_out,
        app::TerminalOutcome::disconnected};
    for (std::size_t index = 0; index < stops.size(); ++index) {
        secret_stops.script_secret_outcome("secret" + std::to_string(index), stops[index]);
    }
    for (std::size_t index = 0; index < stops.size(); ++index) {
        require(secret_stops.read_secret({"secret" + std::to_string(index),
            "Password: ", 128, app::EmptyInputPolicy::reject}).outcome == stops[index],
            "secret stop propagated");
    }
    require(secret_stops.retained_secret_bytes() == 0U && secret_stops.clean(),
        "secret stops retain nothing");
}

void immutable_configuration() {
    auto values = valid_configuration();
    const std::vector<app::RegistrationField> expected{
        app::RegistrationField::caller_name, app::RegistrationField::password,
        app::RegistrationField::real_name, app::RegistrationField::location,
        app::RegistrationField::birth_date, app::RegistrationField::sex,
        app::RegistrationField::email, app::RegistrationField::transfer_protocol,
        app::RegistrationField::final_review};
    auto result = app::Phase3aConfigurationSnapshot::create(values);
    require(result.valid(), "valid configuration");
    require(result.snapshot->caller_entry_allowed() &&
        result.snapshot->allow_new_users(), "caller entry represented");
    require(result.snapshot->enabled_registration_fields() == expected,
        "fixed field order");
    require(result.snapshot->minimum_age() == 18 &&
        result.snapshot->allow_sex_x(), "DOB and sex policy retained");
    require(result.snapshot->password_mode() == app::PasswordMode::standard &&
        result.snapshot->maximum_authentication_attempts() == 3,
        "security policy references retained");
    require(result.snapshot->connection_limits().maximum_connections_per_source == 4 &&
        result.snapshot->connection_limits().maximum_sessions_per_account == 1 &&
        result.snapshot->connection_limits().maximum_logical_nodes ==
            result.snapshot->foundation().logical_nodes,
        "accepted connection-limit defaults composed");
    require(result.snapshot->enabled_transfer_choices().size() == 4U &&
        result.snapshot->unavailable_transfer_fallback() ==
            app::TransferProtocolPreference::select_at_transfer,
        "transfer catalog typed");
    values.prelogin_identity.bbs_name = "Mutated";
    values.enabled_transfer_choices.clear();
    require(result.snapshot->prelogin_identity().bbs_name == "Test BBS" &&
        result.snapshot->enabled_transfer_choices().size() == 4U,
        "snapshot isolated from candidate");

    auto incomplete = app::Phase3aConfigurationSnapshot::create(
        valid_configuration(false));
    require(incomplete.valid() && !incomplete.snapshot->caller_entry_allowed(),
        "bootstrap incomplete represented fail closed");
    values = valid_configuration();
    values.enabled_transfer_choices.clear();
    auto empty_catalog = app::Phase3aConfigurationSnapshot::create(std::move(values));
    require(empty_catalog.valid() &&
        empty_catalog.snapshot->enabled_transfer_choices().empty() &&
        empty_catalog.snapshot->unavailable_transfer_fallback() ==
            app::TransferProtocolPreference::select_at_transfer,
        "empty catalog falls back to select later");
    values = valid_configuration();
    values.connection_limits = {12, 3, 128};
    auto nondefault_limits =
        app::Phase3aConfigurationSnapshot::create(std::move(values));
    require(nondefault_limits.valid() &&
        nondefault_limits.snapshot->connection_limits().maximum_connections_per_source == 12 &&
        nondefault_limits.snapshot->connection_limits().maximum_sessions_per_account == 3 &&
        nondefault_limits.snapshot->connection_limits().maximum_logical_nodes == 128,
        "nondefault valid connection limits retained immutably");
    values = valid_configuration();
    values.real_names_required = true;
    values.collect_optional_real_name = false;
    auto real_name = app::Phase3aConfigurationSnapshot::create(std::move(values));
    require(real_name.valid() &&
        std::find(real_name.snapshot->enabled_registration_fields().begin(),
            real_name.snapshot->enabled_registration_fields().end(),
            app::RegistrationField::real_name) ==
            real_name.snapshot->enabled_registration_fields().end(),
        "account real name prevents duplicate profile prompt");
}

void invalid_configuration() {
    const auto rejected = [](app::Phase3aConfigurationValues values) {
        return !app::Phase3aConfigurationSnapshot::create(std::move(values)).valid();
    };
    auto values = valid_configuration();
    values.real_names_required = true;
    require(rejected(std::move(values)), "real-name conflict rejected");
    values = valid_configuration();
    values.birth_date_mode = app::RegistrationFieldMode::off;
    require(rejected(std::move(values)), "minimum age without DOB rejected");
    values = valid_configuration();
    values.minimum_age = std::uint8_t{9};
    require(rejected(std::move(values)), "minimum age lower bound");
    values = valid_configuration();
    values.minimum_age = std::uint8_t{101};
    require(rejected(std::move(values)), "minimum age upper bound");
    values = valid_configuration();
    values.sex_mode = app::RegistrationFieldMode::off;
    require(rejected(std::move(values)), "X requires sex field");
    values = valid_configuration();
    values.location_mode = static_cast<app::RegistrationFieldMode>(99);
    require(rejected(std::move(values)), "invalid field mode");
    values = valid_configuration();
    values.email_mode = static_cast<app::EmailRegistrationMode>(99);
    require(rejected(std::move(values)), "invalid email mode");
    values = valid_configuration();
    values.password_mode = static_cast<app::PasswordMode>(99);
    require(rejected(std::move(values)), "invalid password mode");
    values = valid_configuration();
    values.enabled_transfer_choices.push_back(app::TransferProtocolPreference::xmodem);
    require(rejected(std::move(values)), "duplicate transfer choice");
    values = valid_configuration();
    values.enabled_transfer_choices.push_back(
        static_cast<app::TransferProtocolPreference>(99));
    require(rejected(std::move(values)), "invalid transfer choice");
    values = valid_configuration();
    values.security_policy.new_user_security_level = 0;
    require(rejected(std::move(values)), "new user level zero");
    values = valid_configuration();
    values.security_policy.new_user_security_level = 999;
    require(rejected(std::move(values)), "new user level mismatch");
    values = valid_configuration();
    values.security_policy.sysop_security_threshold = 0;
    require(rejected(std::move(values)), "threshold zero");
    values = valid_configuration();
    values.security_policy.sysop_security_threshold = 10;
    require(rejected(std::move(values)), "threshold mismatch");
    values = valid_configuration();
    values.security_policy.revision = 0;
    require(rejected(std::move(values)), "policy revision invalid");
    values = valid_configuration();
    values.security_policy.policy_hash = "bad";
    require(rejected(std::move(values)), "policy hash invalid");
    values = valid_configuration();
    values.maximum_authentication_attempts = 2;
    require(rejected(std::move(values)), "three-attempt lock");
    values = valid_configuration();
    values.connection_limits.maximum_connections_per_source = 0;
    require(rejected(std::move(values)), "invalid source connection limit");
    values = valid_configuration();
    values.connection_limits.maximum_sessions_per_account = 0;
    require(rejected(std::move(values)), "invalid account session limit");
    values = valid_configuration();
    values.connection_limits.maximum_logical_nodes = 129;
    require(rejected(std::move(values)), "invalid logical-node connection limit");
    values = valid_configuration();
    values.foundation.logical_nodes = 64;
    require(rejected(std::move(values)), "connection and foundation node mismatch");
    values = valid_configuration();
    values.prelogin_identity.bbs_name.clear();
    require(rejected(std::move(values)), "prelogin identity required");
    values = valid_configuration();
    values.foundation.logical_nodes = 0;
    require(rejected(std::move(values)), "foundation composed");
}

void stable_identity_ports_and_handoff() {
    static_assert(std::is_same<app::AccountId, core::AccountId>::value,
        "canonical AccountId");
    static_assert(std::has_virtual_destructor<app::ICallerNameClassificationPort>::value,
        "name classification port");
    static_assert(std::has_virtual_destructor<app::IPhase3aCredentialOperations>::value,
        "credential operations port");
    static_assert(std::has_virtual_destructor<app::IAccountSessionAdmissionPort>::value,
        "admission port");
    static_assert(std::has_virtual_destructor<app::IRegistrationCommitPort>::value,
        "registration port");
    static_assert(std::has_virtual_destructor<app::IAuthenticatedSessionCommitPort>::value,
        "session commit port");
    static_assert(std::has_virtual_destructor<app::ITrustedDisplaySelectionPort>::value,
        "display port");
    static_assert(!std::is_copy_constructible<app::AuthenticatedCallerReadyHandoff>::value,
        "handoff non-copyable");

    app::CallerNameResolution existing{app::CallerNameClassification::existing,
        "caller name", app::AccountId{42}, "Caller Name", std::uint16_t{10}};
    require(existing.valid(), "case-only existing name difference accepted");
    existing.saved_display_name = "Different Name";
    require(!existing.valid(), "different saved comparison key rejected");
    existing.saved_display_name = "Caller Name";
    existing.normalized_name = " caller name";
    require(!existing.valid(), "non-normalized submitted name rejected");
    existing.normalized_name = "caller name";
    existing.saved_display_name = "Caller  Name";
    require(!existing.valid(), "non-normalized saved display name rejected");
    existing.saved_display_name = "Caller\tName";
    require(!existing.valid(), "invalid saved display name rejected");
    existing.saved_display_name = "Caller Name";
    existing.account_id = 0;
    require(!existing.valid(), "invalid existing account rejected");
    app::CallerNameResolution free_name{app::CallerNameClassification::registerable,
        "Free Name", std::nullopt, std::nullopt, std::nullopt};
    require(free_name.valid(), "registerable name has no identity");
    free_name.account_id = 42;
    require(!free_name.valid(), "free name cannot leak identity");

    const app::AccountId account = 42;
    const auto session = identifier(100);
    const auto correlation = identifier(101);
    const app::AccountSessionAdmissionRequest admission{
        identifier(102), account, session, 1};
    const app::StartSessionRequest start{
        session, correlation, account, 7, 1'777'777'700};
    const app::RecordSuccessfulLoginIpRequest source{
        identifier(103), account, "192.0.2.10", app::AuthenticationProtocol::ssh,
        1'777'777'700};
    const app::ClaimFailedLoginNoticesRequest claim{
        account, identifier(104), session, 1'777'777'700, 5};
    const app::AcknowledgeFailedLoginNoticesRequest ack{
        account, claim.claim_id, session};
    const app::AuthenticatedSessionCommitRequest commit{
        start, source, 7, "caller name", 10, 1, std::string(64, 'a')};
    require(admission.account_id == account && start.account_id == account &&
        source.account_id == account && claim.account_id == account &&
        ack.account_id == account && commit.session.account_id == account &&
        app::valid_authenticated_session_commit_request(commit),
        "AccountId propagates through all coordination contracts");

    test::FakeAccountSessionAdmissionPort lease_port;
    auto lease_owner = app::AccountSessionAdmissionLeaseOwner::create(
        {identifier(105), account, session}, lease_port);
    require(lease_owner.has_value(), "valid admission lease owner");
    app::AuthenticatedCallerReady ready{
        account, session, correlation, 7, 10, app::GraphicsMode::ansi,
        "Caller Name", std::move(lease_owner)};
    auto handoff = app::AuthenticatedCallerReadyHandoff::create(std::move(ready));
    require(handoff.has_value() && !handoff->consumed(), "valid handoff");
    app::AuthenticatedCallerReadyHandoff moved(std::move(*handoff));
    require(handoff->consumed() && !moved.consumed(), "single authority moved");
    auto delivered = moved.take();
    require(delivered.has_value() && delivered->account_id == account &&
        delivered->durable_session_id == session &&
        delivered->correlation_id == correlation &&
        delivered->admission_lease.has_value() &&
        delivered->admission_lease->active(), "stable ready identity and lease");
    require(moved.consumed() && !moved.take().has_value(), "exactly once take");
    require(delivered->admission_lease->release() &&
        !delivered->admission_lease->release() && lease_port.releases.size() == 1,
        "transferred lease releases exactly once");

    auto invalid_ready = [&lease_port, session, correlation](
                             const app::AccountId ready_account,
                             const std::uint16_t node,
                             const std::uint16_t level,
                             std::string display) {
        auto owner = app::AccountSessionAdmissionLeaseOwner::create(
            {identifier(static_cast<std::uint32_t>(200 + node)), account, session},
            lease_port);
        app::AuthenticatedCallerReady value{ready_account, session, correlation,
            node, level, app::GraphicsMode::ansi, std::move(display),
            std::move(owner)};
        return app::AuthenticatedCallerReadyHandoff::create(std::move(value));
    };
    require(!invalid_ready(0, 7, 10, "Caller Name"),
        "invalid account blocks handoff");
    require(!invalid_ready(account, 0, 10, "Caller Name"),
        "invalid node blocks handoff");
    require(!invalid_ready(account, 7, 1000, "Caller Name"),
        "invalid security blocks handoff");
    require(!invalid_ready(account, 7, 10, " Caller Name"),
        "non-normalized display blocks handoff");

    require(app::valid_civil_date({2000, 2, 29}) &&
        !app::valid_civil_date({1900, 2, 29}) &&
        !app::valid_civil_date({2026, 13, 1}), "civil date deterministic");
    test::FixedBbsLocalDateProvider date;
    date.value = {2026, 8, 28};
    require(date.today().year == 2026 && date.today().month == 8 &&
        date.today().day == 28, "local date fake deterministic");
}

}  // namespace

int main() {
    try {
        std::cout << "phase3a-window2 terminal-contracts\n";
        terminal_contracts();
        std::cout << "phase3a-window2 scripted-input-outcomes\n";
        scripted_input_outcomes();
        std::cout << "phase3a-window2 secret-output-order-unexpected\n";
        secret_output_order_and_unexpected_operations();
        std::cout << "phase3a-window2 immutable-configuration\n";
        immutable_configuration();
        std::cout << "phase3a-window2 invalid-configuration\n";
        invalid_configuration();
        std::cout << "phase3a-window2 stable-identity-ports-handoff\n";
        stable_identity_ports_and_handoff();
        std::cout << "all SFX Phase 3A Window 2 contract tests passed assertions="
                  << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Phase 3A Window 2 contract test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
