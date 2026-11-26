#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "sfx/application/phase3a_workflow.hpp"
#include "sfx/application/new_caller_names.hpp"
#include "sfx/test/account_storage_fakes.hpp"
#include "sfx/test/authentication_fakes.hpp"
#include "sfx/test/foundation_fakes.hpp"
#include "sfx/test/phase3a_fakes.hpp"
#include "sfx/test/phase3a_storage_fakes.hpp"

namespace app = sfx::application;
namespace core = sfx::core;
namespace test = sfx::test;

namespace {

std::size_t assertions = 0;
void require(bool condition, const std::string& message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

app::Identifier128 identifier(std::uint32_t value) {
    test::DeterministicIdentifierSource source(value);
    return source.next();
}
template <typename T> app::StorageResult<T> stored(T value) {
    return {app::StorageStatus::success(), std::move(value)};
}

app::CredentialEnvelope credential() {
    app::CredentialEnvelope value;
    value.envelope_format_version = 1;
    value.scheme_identifier = "argon2id";
    value.parameter_encoding_version = 1;
    value.canonical_parameters = {1, 2, 3};
    value.salt.assign(app::kArgon2idSaltBytes, 4);
    value.verifier.assign(app::kArgon2idVerifierBytes, 5);
    value.created_utc_seconds = 1'700'000'000;
    return value;
}

app::SecurityPolicyRecord policy(std::uint16_t new_user = 10, std::uint16_t threshold = 999) {
    return {1, new_user, threshold, std::string(64, 'a'), 1, std::string(64, 'a')};
}

app::AccountRecord account(std::string display = "New Caller", std::uint16_t level = 10,
    app::AccountId account_id = 42) {
    app::AccountRecord value;
    value.account_id = account_id;
    value.display_name = std::move(display);
    value.name_key = app::account_name_comparison_key(*value.display_name);
    value.security_level = level;
    value.state = app::AccountState::active;
    value.revision = 7;
    value.created_utc_seconds = 1'700'000'000;
    value.modified_utc_seconds = 1'700'000'000;
    value.statistics.successful_login_count = 1;
    return value;
}

struct ConfigurationOptions {
    bool real_names_required{false};
    bool optional_real_name{false};
    app::RegistrationFieldMode location{app::RegistrationFieldMode::off};
    app::RegistrationFieldMode birth_date{app::RegistrationFieldMode::off};
    std::optional<std::uint8_t> minimum_age;
    app::RegistrationFieldMode sex{app::RegistrationFieldMode::off};
    bool allow_x{false};
    app::EmailRegistrationMode email{app::EmailRegistrationMode::off};
    bool transfer{false};
    std::vector<app::TransferProtocolPreference> transfer_choices;
    std::uint16_t new_user_level{10};
    std::uint16_t threshold{999};
    bool allow_new_users{true};
};

app::Phase3aConfigurationSnapshot configuration(ConfigurationOptions options = {}) {
    app::Phase3aConfigurationValues values;
    values.foundation.logical_nodes = 128;
    if (options.threshold == 999 && options.new_user_level == 10) {
        values.foundation.security = core::SecurityPolicy::fresh_install();
    } else {
        const auto level10 = *core::SecurityLevel::create(10);
        const auto new_level = *core::SecurityLevel::create(options.new_user_level);
        const auto threshold = *core::SecurityLevel::create(options.threshold);
        const auto level999 = *core::SecurityLevel::create(999);
        std::vector<core::SecurityLevel> levels{level10, level999};
        if (std::find(levels.begin(), levels.end(), new_level) == levels.end())
            levels.push_back(new_level);
        if (std::find(levels.begin(), levels.end(), threshold) == levels.end())
            levels.push_back(threshold);
        values.foundation.security = *core::SecurityPolicy::create(
            std::move(levels), new_level, threshold);
    }
    values.bootstrap_complete = true;
    values.allow_new_users = options.allow_new_users;
    values.real_names_required = options.real_names_required;
    values.collect_optional_real_name = options.optional_real_name;
    values.location_mode = options.location;
    values.birth_date_mode = options.birth_date;
    values.minimum_age = options.minimum_age;
    values.sex_mode = options.sex;
    values.allow_sex_x = options.allow_x;
    values.email_mode = options.email;
    values.transfer_protocol_question_enabled = options.transfer;
    values.enabled_transfer_choices = std::move(options.transfer_choices);
    values.security_policy = {1, std::string(64, 'a'),
        options.new_user_level, options.threshold};
    values.connection_limits.maximum_logical_nodes = 128;
    values.prelogin_identity = {"Test BBS", "Test Sysop", "0.1-test"};
    auto result = app::Phase3aConfigurationSnapshot::create(std::move(values));
    if (!result.valid()) throw std::runtime_error("valid Window 4 configuration rejected");
    return std::move(*result.snapshot);
}

app::CallerNameResolution registerable(std::string name) {
    return {app::CallerNameClassification::registerable, std::move(name), {}, {}, {}};
}

app::CallerNameResolution unavailable(std::string name) {
    return {app::CallerNameClassification::unavailable, std::move(name), {}, {}, {}};
}

app::CallerNameResolution existing(std::string name, std::uint16_t level = 10,
    app::AccountId account_id = 42) {
    const std::string saved = name;
    return {app::CallerNameClassification::existing, std::move(name), account_id,
        saved, level};
}

core::Session session() {
    return core::Session("registration-shell", *core::NodeNumber::create(1),
        {core::TransportKind::telnet, "192.0.2.7", {}, "ansi"});
}

struct Fixture {
    test::ScriptedTerminalFake terminal;
    test::FakeCallerNameClassificationPort names;
    test::FakePhase3aCredentialOperations credentials;
    test::FakeAccountStorage accounts;
    test::FakeAuthenticationStorage authentication_storage;
    test::FakePhase3aStorage phase3a_storage;
    test::FakeRegistrationCommitPort registration_commit;
    test::FakeAccountSessionAdmissionPort admission;
    test::FakeAuthenticatedSessionCommitPort session_commit;
    test::FakeTrustedDisplaySelectionPort displays;
    test::FakePhase3aRateControlPort rates;
    test::FixedBbsLocalTimestampFormatter local_time;
    test::FakeCallerWelcomeStatisticsPort welcome_statistics;
    test::FixedBbsLocalDateProvider local_date;
    test::FakeClock clock;
    test::DeterministicIdentifierSource identifiers{1};
    std::shared_ptr<app::CallerCleanupHealth> cleanup_health = std::make_shared<app::CallerCleanupHealth>();
    const app::INewCallerNamePolicy* new_names{nullptr};
    app::ICallerStageObserver* observer{nullptr};
    app::CallerPasswordObserver caller_password;

    Fixture() {
        accounts.policy_result = stored(policy());
        accounts.account_result = stored(account());
        accounts.mutation_result.status = app::StorageStatus::success();
        phase3a_storage.status_result = app::StorageStatus::success();
        phase3a_storage.dynamic_claim_result = true;
        session_commit.result.status = app::StorageStatus::success();
        rates.status = app::StorageStatus::success();
        local_date.value = {2026, 8, 28};
        credentials.registration_result = {app::AuthenticationOperationCode::completed,
            app::PasswordPolicyCode::accepted, credential()};
    }

    app::Phase3aRegistrationWorkflow workflow() {
        return app::Phase3aRegistrationWorkflow({terminal, names, credentials, accounts,
            authentication_storage, phase3a_storage, registration_commit, admission,
            session_commit, displays, rates, local_time, welcome_statistics,
            local_date, clock, identifiers, observer, cleanup_health, new_names,caller_password});
    }

    app::Phase3aExistingCallerWorkflow existing_workflow() {
        return app::Phase3aExistingCallerWorkflow({terminal, names, credentials, accounts,
            authentication_storage, phase3a_storage, admission, session_commit, displays,
            rates, local_time, welcome_statistics, clock, identifiers, nullptr, cleanup_health});
    }
};

void script_name_checks(Fixture& fixture, std::string name, std::size_t count = 2) {
    for (std::size_t index = 0; index < count; ++index)
        fixture.names.results.push_back(stored(registerable(name)));
}

void script_password(Fixture& fixture, std::size_t length = 12) {
    fixture.terminal.script_secret_completed("registration.password", length);
    fixture.terminal.script_secret_completed("registration.password.confirm", length);
}

void script_review_confirm(Fixture& fixture) {
    fixture.terminal.script_presentation("registration.review");
    fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'C');
}

void script_success_tail(Fixture& fixture) {
    fixture.terminal.script_presentation("caller.greeting");
    fixture.terminal.script_presentation("caller.information");
    fixture.terminal.script_presentation("caller.continue.prompt");
    fixture.terminal.script_pause("caller.continue");
}

app::Phase3aWorkflowResult run(Fixture& fixture,
    const app::Phase3aConfigurationSnapshot& config, core::Session& active,
    std::string name = "New Caller", std::string source_ip = "192.0.2.7") {
    app::CancellationSource cancellation;
    auto workflow = fixture.workflow();
    return workflow.run(config, active,
        {{std::move(name), app::GraphicsMode::ansi}, std::move(source_ip), identifier(900)},
        cancellation.token());
}

void test_resume_newuser_entitlement_validation() {
    const app::Phase3aExistingCallerRequest::ResumeState valid{
        app::GraphicsMode::ansi, 1, 42};
    const app::Phase3aExistingCallerRequest::ResumeState zero_account{
        app::GraphicsMode::ansi, 1, 0};
    require(valid.valid() && !zero_account.valid(),
        "resume state requires a valid committed account identity");

    Fixture fixture;
    app::CancellationSource cancellation;
    auto active = session();
    auto workflow = fixture.existing_workflow();
    app::Phase3aExistingCallerRequest request{
        "192.0.2.7", identifier(900), zero_account};
    const auto result = workflow.run(
        configuration(), active, request, cancellation.token());
    require(result.code == app::Phase3aWorkflowCode::invalid_context &&
            !result.caller_ready() && fixture.credentials.observations.empty() &&
            fixture.admission.requests.empty() && fixture.session_commit.requests.empty(),
        "zero committed-account resume identity fails before authentication or continuation");
}

void test_minimal_success_and_exactly_once_handoff(std::string source_ip = "192.0.2.7") {
    Fixture fixture;
    unsigned password_handoffs{};
    fixture.caller_password=[&](app::AccountId id,const app::CredentialEnvelope& proof,std::string_view password){
        ++password_handoffs;require(id==42&&password.size()==12&&proof.verifier==credential().verifier,"new registration password binds the committed account and exact credential");};
    script_name_checks(fixture, "New Caller");
    script_password(fixture);
    script_review_confirm(fixture);
    fixture.registration_commit.results.push_back(
        {app::StorageStatus::success(), 42, 7, false});
    script_success_tail(fixture);
    auto active = session();
    auto result = run(fixture, configuration(), active, "New Caller", source_ip);
    require(password_handoffs==1,"committed new caller supplies automatic Door credentials without a second prompt");
    require(result.caller_ready() && result.ready.has_value(),
        "minimal registration reaches one ready handoff");
    require(fixture.session_commit.requests.size() == 1 &&
        fixture.session_commit.requests[0].session.journal_new_registration &&
        fixture.session_commit.requests[0].session.journal_registration_utc == fixture.clock.unix_seconds(),
        "journal records the actual registration audit timestamp at the single durable handoff");
    require(fixture.registration_commit.observations.size() == 1 &&
        fixture.registration_commit.observations[0].display_name == "New Caller" &&
        fixture.registration_commit.observations[0].security_level == 10,
        "commit contains normalized name and configured new-user level");
    require(fixture.registration_commit.observations[0].credential_bytes > 0 &&
        !fixture.registration_commit.observations[0].has_real_name,
        "commit fake records only safe credential length and field presence");
    require(fixture.terminal.hidden_input_requests() == 2 &&
        fixture.terminal.retained_secret_bytes() == 0 && fixture.terminal.clean(),
        "password and confirmation leave no secret or pending terminal script");
    require(active.state() == core::SessionState::authenticated,
        "successful registration authenticates the existing core session");
    const auto greeting = std::find_if(fixture.terminal.observations().begin(),
        fixture.terminal.observations().end(), [](const auto& observation) {
            return observation.identity == "caller.greeting";
        });
    const auto information = std::find_if(fixture.terminal.observations().begin(),
        fixture.terminal.observations().end(), [](const auto& observation) {
            return observation.identity == "caller.information";
        });
    const auto continuation = std::find_if(fixture.terminal.observations().begin(),
        fixture.terminal.observations().end(), [](const auto& observation) {
            return observation.identity == "caller.continue.prompt";
        });
    require(greeting != fixture.terminal.observations().end() &&
            greeting->non_secret_text ==
                "\nGood afternoon, New Caller, welcome to Test BBS.\n" &&
            information != fixture.terminal.observations().end() &&
            information->non_secret_text ==
                "Your IP address is: " + source_ip + "\n"
                "There has been 1 call today.\n"
                "You have called 1 time.\n"
                "You have spent less than one minute online.\n"
                "Your security level is 10.\n\n" &&
            continuation != fixture.terminal.observations().end() &&
            continuation->non_secret_text == "Press <ENTER> to continue.",
        "first registered login uses the exact natural first-call welcome and real continuation wait");
    auto ready = result.ready->take();
    require(ready.has_value() && result.ready->consumed() && ready->account_id == 42,
        "ready handoff transfers exactly once");
    require(!result.ready->take().has_value(), "consumed handoff cannot be replayed");
}

void test_fixed_field_order_validation_and_private_observation_safety() {
    Fixture fixture;
    ConfigurationOptions options;
    options.optional_real_name = true;
    options.location = app::RegistrationFieldMode::required;
    options.birth_date = app::RegistrationFieldMode::required;
    options.minimum_age = static_cast<std::uint8_t>(18);
    options.sex = app::RegistrationFieldMode::required;
    options.allow_x = true;
    options.email = app::EmailRegistrationMode::ask;
    options.transfer = true;
    options.transfer_choices = std::vector{app::TransferProtocolPreference::zmodem};
    script_name_checks(fixture, "New Caller");
    script_password(fixture);
    fixture.terminal.script_presentation("registration.real_name.explanation");
    fixture.terminal.script_visible("registration.real_name", app::TerminalOutcome::completed,
        "Private Person");
    fixture.terminal.script_visible("registration.location", app::TerminalOutcome::completed,
        "Private Place");
    fixture.terminal.script_visible("registration.birth_date", app::TerminalOutcome::completed,
        "02/30/2000");
    fixture.terminal.script_presentation("registration.birth_date.invalid");
    fixture.terminal.script_visible("registration.birth_date", app::TerminalOutcome::completed,
        "02/29/2000");
    fixture.terminal.script_key("registration.sex", app::TerminalOutcome::completed, 'x');
    fixture.terminal.script_visible("registration.email", app::TerminalOutcome::completed, "bad");
    fixture.terminal.script_presentation("registration.email.invalid");
    fixture.terminal.script_visible("registration.email", app::TerminalOutcome::completed,
        "private@example.test");
    fixture.terminal.script_key("registration.transfer_protocol", app::TerminalOutcome::empty);
    script_review_confirm(fixture);
    fixture.registration_commit.results.push_back(
        {{app::StorageErrorCode::conflict, "duplicate"}, {}, {}, false});
    auto active = session();
    auto result = run(fixture, configuration(std::move(options)), active);
    require(result.code == app::Phase3aWorkflowCode::registration_unavailable,
        "authoritative commit conflict fails closed with no continuation");
    const auto& observations = fixture.terminal.observations();
    std::vector<std::string> identities;
    for (const auto& observation : observations) identities.push_back(observation.identity);
    auto before = [&](const char* left, const char* right) {
        return std::find(identities.begin(), identities.end(), left) <
            std::find(identities.begin(), identities.end(), right);
    };
    require(before("registration.real_name", "registration.location") &&
        before("registration.location", "registration.birth_date") &&
        before("registration.birth_date", "registration.sex") &&
        before("registration.sex", "registration.email") &&
        before("registration.email", "registration.transfer_protocol"),
        "enabled fields are asked in the locked order");
    require(std::find(identities.begin(), identities.end(), "registration.phone") == identities.end(),
        "Phone is absent from registration");
    const auto review = std::find_if(observations.begin(), observations.end(),
        [](const auto& value) { return value.identity == "registration.review"; });
    require(review != observations.end() && review->non_secret_text ==
        "[private registration review redacted]",
        "fake records review occurrence without retaining private profile values");
    require(fixture.registration_commit.observations.size() == 1 &&
        fixture.registration_commit.observations[0].has_real_name &&
        fixture.registration_commit.observations[0].has_location &&
        fixture.registration_commit.observations[0].has_birth_date &&
        fixture.registration_commit.observations[0].has_sex &&
        fixture.registration_commit.observations[0].has_email &&
        fixture.registration_commit.observations[0].transfer_preference ==
            app::TransferProtocolPreference::select_at_transfer,
        "safe commit observation proves field presence and select-later fallback");
}

void test_password_failures_and_preconfirmation_zero_commit() {
    for (const auto outcome : {app::TerminalOutcome::cancelled,
             app::TerminalOutcome::timed_out, app::TerminalOutcome::disconnected,
             app::TerminalOutcome::shutdown_requested}) {
        Fixture fixture;
        fixture.names.results.push_back(stored(registerable("New Caller")));
        fixture.terminal.script_secret_outcome("registration.password", outcome);
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(!result.caller_ready() && fixture.registration_commit.observations.empty(),
            "secret stop outcome creates no account");
        require(fixture.terminal.retained_secret_bytes() == 0,
            "secret stop outcome retains zero secret bytes");
    }
    {
        Fixture fixture;
        fixture.names.results.push_back(stored(registerable("New Caller")));
        fixture.terminal.script_secret_completed("registration.password", 8);
        fixture.terminal.script_secret_completed("registration.password.confirm", 9);
        fixture.terminal.script_presentation("registration.password.mismatch");
        script_password(fixture, 12);
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'Q');
        fixture.terminal.script_presentation("registration.goodbye");
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::caller_goodbye &&
            fixture.registration_commit.observations.empty(),
            "mismatch restarts both prompts and final rejection creates no account");
        require(fixture.credentials.observations.size() == 1 &&
            fixture.terminal.hidden_input_requests() == 4,
            "only the matching replacement reaches credential creation");
    }
    {
        Fixture fixture;
        fixture.names.results.push_back(stored(registerable("New Caller")));
        fixture.credentials.registration_result = {app::AuthenticationOperationCode::backpressure,
            app::PasswordPolicyCode::accepted, {}};
        script_password(fixture);
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::kdf_backpressure &&
            fixture.registration_commit.observations.empty(),
            "KDF backpressure creates no account");
    }
}

void test_idempotent_retry_name_edit_and_real_name_alias_rule() {
    Fixture fixture;
    script_name_checks(fixture, "New Caller", 1);
    fixture.names.results.push_back(stored(registerable("Edited Caller")));
    fixture.names.results.push_back(stored(registerable("Edited Caller")));
    script_password(fixture, 10);
    fixture.terminal.script_presentation("registration.review");
    fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'R');
    fixture.terminal.script_presentation("registration.review");
    fixture.terminal.script_key("registration.edit.action", app::TerminalOutcome::completed, 'N');
    fixture.terminal.script_visible("registration.name.first", app::TerminalOutcome::completed, "Edited");
    fixture.terminal.script_visible("registration.name.last", app::TerminalOutcome::completed, "Caller");
    script_password(fixture, 14);
    fixture.terminal.script_presentation("registration.review");
    fixture.terminal.script_key("registration.edit.action", app::TerminalOutcome::empty);
    script_review_confirm(fixture);
    fixture.registration_commit.results.push_back(
        {{app::StorageErrorCode::io, "uncertain"}, {}, {}, false});
    fixture.registration_commit.results.push_back(
        {app::StorageStatus::success(), 42, 7, true});
    fixture.accounts.account_result = stored(account("Edited Caller"));
    script_success_tail(fixture);
    auto active = session();
    auto result = run(fixture, configuration(), active);
    require(result.caller_ready() && fixture.registration_commit.observations.size() == 2,
        "uncertain commit retry accepts an idempotent replay");
    require(fixture.registration_commit.observations[0].event_id ==
            fixture.registration_commit.observations[1].event_id &&
        fixture.registration_commit.observations[0].correlation_id ==
            fixture.registration_commit.observations[1].correlation_id,
        "uncertain retry preserves stable event and correlation identity");
    require(fixture.credentials.observations.size() == 2 &&
        fixture.registration_commit.observations.back().display_name == "Edited Caller",
        "name edit invalidates and replaces the password draft");

    Fixture alias_fixture;
    ConfigurationOptions alias_options;
    alias_options.real_names_required = true;
    script_name_checks(alias_fixture, "Real Caller");
    script_password(alias_fixture);
    script_review_confirm(alias_fixture);
    alias_fixture.registration_commit.results.push_back(
        {{app::StorageErrorCode::conflict, "stop after commit observation"}, {}, {}, false});
    auto alias_session = session();
    const auto alias_result = run(alias_fixture, configuration(alias_options), alias_session,
        "Real Caller");
    require(alias_result.code == app::Phase3aWorkflowCode::registration_unavailable &&
        alias_fixture.registration_commit.observations[0].has_real_name,
        "Real Names Required uses the account name without a duplicate prompt");
    require(std::none_of(alias_fixture.terminal.observations().begin(),
            alias_fixture.terminal.observations().end(), [](const auto& value) {
                return value.identity == "registration.real_name";
            }), "Real Names Required never asks a second Real Name question");
}

void test_policy_retry_transfer_modes_and_age_boundaries() {
    {
        Fixture fixture;
        fixture.names.results.push_back(stored(registerable("New Caller")));
        fixture.credentials.registration_results.push_back(
            {app::AuthenticationOperationCode::policy_rejected,
                app::PasswordPolicyCode::too_short, {}});
        fixture.credentials.registration_results.push_back(
            {app::AuthenticationOperationCode::completed,
                app::PasswordPolicyCode::accepted, credential()});
        script_password(fixture, 4);
        fixture.terminal.script_presentation("registration.password.policy");
        script_password(fixture, 16);
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.review.action",
            app::TerminalOutcome::completed, 'Q');
        fixture.terminal.script_presentation("registration.goodbye");
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::caller_goodbye &&
            fixture.credentials.observations.size() == 2 &&
            fixture.registration_commit.observations.empty(),
            "policy rejection erases and re-prompts before any account commit");
    }

    for (const bool enabled : {false, true}) {
        Fixture fixture;
        ConfigurationOptions options;
        options.transfer = enabled;
        options.transfer_choices.clear();
        script_name_checks(fixture, "New Caller");
        script_password(fixture);
        script_review_confirm(fixture);
        fixture.registration_commit.results.push_back(
            {{app::StorageErrorCode::conflict, "bounded stop"}, {}, {}, false});
        auto active = session();
        run(fixture, configuration(options), active);
        require(fixture.registration_commit.observations.size() == 1 &&
            fixture.registration_commit.observations[0].transfer_preference ==
                app::TransferProtocolPreference::select_at_transfer,
            enabled ? "empty transfer catalog falls back to select-later"
                    : "disabled transfer question falls back to select-later");
        require(std::none_of(fixture.terminal.observations().begin(),
                fixture.terminal.observations().end(), [](const auto& value) {
                    return value.identity == "registration.transfer_protocol";
                }), "disabled or empty transfer catalog asks no question");
    }
    {
        Fixture fixture;
        ConfigurationOptions options;
        options.transfer = true;
        options.transfer_choices = std::vector{app::TransferProtocolPreference::zmodem};
        script_name_checks(fixture, "New Caller");
        script_password(fixture);
        fixture.terminal.script_key("registration.transfer_protocol",
            app::TerminalOutcome::completed, '1');
        script_review_confirm(fixture);
        fixture.registration_commit.results.push_back(
            {{app::StorageErrorCode::conflict, "bounded stop"}, {}, {}, false});
        auto active = session();
        run(fixture, configuration(options), active);
        require(fixture.registration_commit.observations[0].transfer_preference ==
            app::TransferProtocolPreference::zmodem,
            "enabled typed transfer choice is committed without implementing transfer");
    }

    struct AgeCase { const char* dob; bool accepted; std::optional<std::uint8_t> minimum; };
    const std::vector<AgeCase> cases{
        {"08/28/2016", true, {}}, {"08/29/2016", false, {}},
        {"08/28/1926", true, {}}, {"08/27/1925", false, {}},
        {"08/28/2008", true, static_cast<std::uint8_t>(18)},
        {"08/29/2008", false, static_cast<std::uint8_t>(18)}};
    for (const auto& age_case : cases) {
        Fixture fixture;
        ConfigurationOptions options;
        options.birth_date = app::RegistrationFieldMode::required;
        options.minimum_age = age_case.minimum;
        script_name_checks(fixture, "New Caller", age_case.accepted ? 2U : 1U);
        script_password(fixture);
        fixture.terminal.script_visible("registration.birth_date",
            app::TerminalOutcome::completed, age_case.dob);
        if (age_case.accepted) {
            script_review_confirm(fixture);
            fixture.registration_commit.results.push_back(
                {{app::StorageErrorCode::conflict, "bounded stop"}, {}, {}, false});
        } else {
            fixture.terminal.script_presentation("registration.birth_date.rejected");
        }
        auto active = session();
        const auto result = run(fixture, configuration(options), active);
        require(result.code == app::Phase3aWorkflowCode::registration_unavailable,
            "DOB age boundary terminates at the expected registration boundary");
        require(fixture.registration_commit.observations.size() ==
                (age_case.accepted ? 1U : 0U),
            "accepted age reaches commit while rejected age persists no DOB or account");
    }
}

void test_sysop_sequence_optional_displays_and_post_commit_failures() {
    ConfigurationOptions sysop_options;
    sysop_options.threshold = 10;
    sysop_options.new_user_level = 10;
    {
        Fixture fixture;
        fixture.accounts.policy_result = stored(policy(10, 10));
        fixture.accounts.account_result = stored(account("New Caller", 10));
        script_name_checks(fixture, "New Caller");
        fixture.displays.results.push_back({app::TrustedDisplaySelectionCode::available,
            test::trusted_display_program(app::TrustedDisplayIdentity::newuser,
                app::GraphicsMode::ansi, "NEWUSER\n")});
        fixture.terminal.script_presentation("registration.newuser");
        fixture.displays.results.push_back({app::TrustedDisplaySelectionCode::available,
            test::trusted_display_program(app::TrustedDisplayIdentity::newreg,
                app::GraphicsMode::ansi, "NEWREG\n")});
        fixture.terminal.script_presentation("registration.newreg");
        script_password(fixture);
        script_review_confirm(fixture);
        fixture.registration_commit.results.push_back(
            {app::StorageStatus::success(), 42, 7, false});
        fixture.phase3a_storage.preference_result = stored(
            app::TransferPreferenceRecord{42,
                app::TransferProtocolPreference::zmodem,
                1, 1'700'000'000});
        fixture.terminal.script_secret_completed("registration.system_password", 18);
        fixture.credentials.system_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        script_success_tail(fixture);
        fixture.displays.results.push_back({app::TrustedDisplaySelectionCode::available,
            test::trusted_display_program(app::TrustedDisplayIdentity::welcome1,
                app::GraphicsMode::ansi,"WELCOME after information\n")});
        fixture.terminal.script_presentation("caller.welcomes");
        auto active = session();
        auto result = run(fixture, configuration(sysop_options), active);
        require(fixture.admission.requests.size() == 1 &&
            fixture.admission.requests[0].maximum_sessions_per_account == 1,
            "threshold-qualified level 10 still has one session; parallel sessions are 999-only");
        require(result.caller_ready() && fixture.displays.requests.size() == 4 &&
            fixture.displays.requests[0].identity == app::TrustedDisplayIdentity::newuser &&
            fixture.displays.requests[1].identity == app::TrustedDisplayIdentity::newreg &&
            fixture.displays.requests[3].identity == app::TrustedDisplayIdentity::all,
            "NEWUSER starts registration; NEWREG follows; ALL follows welcomes");
        const auto& newuser_context = fixture.displays.requests[0].context;
        require(!newuser_context.name && !newuser_context.first_name &&
                !newuser_context.security_level && !newuser_context.logons && !newuser_context.protocol &&
                !newuser_context.ip_address,
            "registration-start NEWUSER has no invented authenticated account context");
        require(fixture.credentials.observations.back().operation == "verify_system" &&
            fixture.rates.observations.front().operation == "await",
            "threshold-derived SY verification is rate-admitted before secret verification");
    }
    {
        Fixture fixture;
        fixture.accounts.policy_result = stored(policy(10, 10));
        fixture.accounts.account_result = stored(account("New Caller", 10));
        script_name_checks(fixture, "New Caller");
        fixture.names.results.push_back(stored(existing("New Caller", 10)));
        fixture.displays.results.push_back({app::TrustedDisplaySelectionCode::available,
            test::trusted_display_program(app::TrustedDisplayIdentity::newuser,
                app::GraphicsMode::ansi, "NEWUSER\n")});
        fixture.terminal.script_presentation("registration.newuser");
        fixture.displays.results.push_back(
            {app::TrustedDisplaySelectionCode::not_found, {}});
        script_password(fixture);
        script_review_confirm(fixture);
        fixture.registration_commit.results.push_back(
            {app::StorageStatus::success(), 42, 7, false});
        fixture.terminal.script_secret_completed("registration.system_password", 8);
        fixture.credentials.system_results.push_back(
            {app::AuthenticationOperationCode::rejected, false, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        fixture.terminal.script_visible("caller.name.first",
            app::TerminalOutcome::completed, "New");
        fixture.terminal.script_visible("caller.name.last",
            app::TerminalOutcome::completed, "Caller");
        fixture.terminal.script_presentation("caller.name.searching");
        fixture.terminal.script_secret_completed("authentication.password", 19);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.terminal.script_secret_completed("authentication.system_password", 17);
        fixture.credentials.system_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.phase3a_storage.dynamic_notices.push_back(
            {identifier(701), 42, "2001:db8::7", 1'700'000'000});
        fixture.terminal.script_presentation("caller.greeting");
        fixture.terminal.script_presentation("caller.information");
        fixture.terminal.script_presentation("caller.failed_login_notice");
        fixture.terminal.script_presentation("caller.continue.prompt");
        fixture.terminal.script_pause("caller.continue");
        auto active = session();
        auto result = run(fixture, configuration(sysop_options), active,
            "New Caller", "2001:0db8:0:0:0:0:0:7");
        require(result.caller_ready() &&
            fixture.registration_commit.observations.size() == 1 &&
            fixture.phase3a_storage.enqueue_requests.size() == 1,
            "one failed registration SY preserves one commit and resumes to a successful existing login");
        require(fixture.rates.observations.size() >= 2 &&
            fixture.rates.observations[0].operation == "await" &&
            fixture.rates.observations[1].operation == "failure" &&
            fixture.rates.observations[0].scopes.source_ip == "2001:db8::7" &&
            fixture.rates.observations[1].scopes.source_ip == "2001:db8::7" &&
            fixture.rates.observations[0].scopes.source &&
            fixture.rates.observations[0].scopes.global &&
            fixture.rates.observations[0].scopes.system_password &&
            !fixture.rates.observations[0].scopes.account_id.has_value() &&
            fixture.rates.observations[1].scopes.source &&
            fixture.rates.observations[1].scopes.global &&
            fixture.rates.observations[1].scopes.system_password &&
            !fixture.rates.observations[1].scopes.account_id.has_value(),
            "post-registration SY admission and failure use canonical source/global/SY scopes without account scope");
        require(fixture.phase3a_storage.enqueue_requests[0].account_id == 42 &&
            fixture.phase3a_storage.enqueue_requests[0].source_ip == "2001:db8::7" &&
            fixture.names.names.size() == 3 && fixture.names.names.back() == "New Caller",
            "failed SY enqueues one canonical known-account notice then returns directly to name entry");
        require(std::count_if(fixture.credentials.observations.begin(),
                    fixture.credentials.observations.end(), [](const auto& observation) {
                        return observation.operation == "create_registration";
                    }) == 1 &&
            std::count_if(fixture.credentials.observations.begin(),
                    fixture.credentials.observations.end(), [](const auto& observation) {
                        return observation.operation == "verify_existing";
                    }) == 1 &&
            std::count_if(fixture.credentials.observations.begin(),
                    fixture.credentials.observations.end(), [](const auto& observation) {
                        return observation.operation == "verify_system";
                    }) == 2,
            "questionnaire and commit are not repeated and the later SY follows a successful ordinary verification");
        require(fixture.admission.requests.size() == 1 &&
            fixture.session_commit.requests.size() == 1 &&
            active.state() == core::SessionState::authenticated &&
            fixture.displays.requests.size() == 4 &&
            fixture.displays.requests[0].identity ==
                app::TrustedDisplayIdentity::newuser,
            "admission and authentication require full success; NEWUSER was shown at registration start");
        require(std::count_if(fixture.displays.requests.begin(),
                    fixture.displays.requests.end(), [](const auto& request) {
                        return request.identity == app::TrustedDisplayIdentity::newuser;
                    }) == 1 &&
                std::count_if(fixture.terminal.observations().begin(),
                    fixture.terminal.observations().end(), [](const auto& observation) {
                        return observation.identity == "registration.newuser";
                    }) == 1,
        "resuming the newly committed account does not repeat registration-start NEWUSER");
        require(fixture.terminal.retained_secret_bytes() == 0 &&
            fixture.terminal.hidden_input_requests() == 5 &&
            result.ready->take().has_value() &&
            !result.ready->take().has_value(),
            "registration plaintext is not reused and the resumed path still hands off exactly once");
    }

    for (const bool admission_failure : {true, false}) {
        Fixture fixture;
        script_name_checks(fixture, "New Caller");
        script_password(fixture);
        script_review_confirm(fixture);
        fixture.registration_commit.results.push_back(
            {app::StorageStatus::success(), 42, 7, false});
        if (admission_failure) {
            fixture.admission.auto_acquire = false;
            fixture.admission.result = {app::AccountSessionAdmissionCode::account_limit, {}};
            fixture.terminal.script_presentation("authentication.already_online");
        } else {
            fixture.session_commit.result.status =
                {app::StorageErrorCode::io, "session commit"};
        }
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(!result.caller_ready() && fixture.registration_commit.observations.size() == 1,
            "post-account admission/session failure never rolls back the valid account");
        require(fixture.session_commit.requests.size() == (admission_failure ? 0U : 1U),
            "admission failure stops before session commit while session failure is attempted once");
        require(admission_failure || fixture.admission.releases.size() == 1,
            "session-start failure releases the move-only admission lease");
    }

    {
        Fixture fixture;
        fixture.accounts.policy_results.push_back(stored(policy()));
        fixture.accounts.policy_results.push_back(stored(policy(10, 10)));
        fixture.names.results.push_back(stored(registerable("New Caller")));
        script_password(fixture);
        script_review_confirm(fixture);
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.registration_commit.observations.empty(),
            "security policy revision/hash/authority drift before commit fails closed with no account");
    }
}

void test_failed_registration_sy_cross_account_newuser_binding() {
    ConfigurationOptions options;
    options.threshold = 999;
    options.new_user_level = 999;
    Fixture fixture;
    fixture.accounts.policy_result = stored(policy(999, 999));
    fixture.accounts.account_results.push_back(
        stored(account("New Caller", 999, 42)));
    fixture.accounts.account_result = stored(account("Other Caller", 10, 84));
    script_name_checks(fixture, "New Caller");
    fixture.names.results.push_back(stored(existing("Other Caller", 10, 84)));
    fixture.displays.results.push_back(
        {app::TrustedDisplaySelectionCode::not_found, {}});
    script_password(fixture);
    script_review_confirm(fixture);
    fixture.registration_commit.results.push_back(
        {app::StorageStatus::success(), 42, 7, false});

    fixture.terminal.script_secret_completed("registration.system_password", 8);
    fixture.credentials.system_results.push_back(
        {app::AuthenticationOperationCode::rejected, false, false, false, false});
    fixture.terminal.script_presentation("authentication.failure");
    fixture.terminal.script_visible("caller.name.first",
        app::TerminalOutcome::completed, "Other");
    fixture.terminal.script_visible("caller.name.last",
        app::TerminalOutcome::completed, "Caller");
    fixture.terminal.script_presentation("caller.name.searching");
    fixture.terminal.script_secret_completed("authentication.password", 14);
    fixture.credentials.existing_results.push_back(
        {app::AuthenticationOperationCode::completed, true, false, false, false});
    fixture.phase3a_storage.dynamic_notices.push_back(
        {identifier(702), 84, "198.51.100.84", 1'700'000'000});
    fixture.terminal.script_presentation("caller.greeting");
    fixture.terminal.script_presentation("caller.information");
    fixture.terminal.script_presentation("caller.failed_login_notice");
    fixture.terminal.script_presentation("caller.continue.prompt");
    fixture.terminal.script_pause("caller.continue");

    auto active = session();
    auto result = run(fixture, configuration(options), active);
    require(result.caller_ready() && result.ready.has_value() &&
            fixture.registration_commit.observations.size() == 1 &&
            fixture.phase3a_storage.enqueue_requests.size() == 1 &&
            fixture.phase3a_storage.enqueue_requests[0].account_id == 42,
        "cross-account resume preserves one committed new account and its failed-SY notice");
    require(std::count_if(fixture.displays.requests.begin(), fixture.displays.requests.end(),
                [](const auto& request) {
                    return request.identity == app::TrustedDisplayIdentity::newuser;
                }) == 1 &&
            std::none_of(fixture.terminal.observations().begin(),
                fixture.terminal.observations().end(), [](const auto& observation) {
                    return observation.identity == "registration.newuser";
                }),
        "cross-account resume does not repeat the initial optional NEWUSER request");
    require(fixture.admission.requests.size() == 1 &&
            fixture.admission.requests[0].account_id == 84 &&
            fixture.session_commit.requests.size() == 1 &&
            fixture.session_commit.requests[0].session.account_id == 84 &&
            fixture.session_commit.requests[0].successful_source.account_id == 84 &&
            fixture.session_commit.requests[0].expected_security_level == 10 &&
            fixture.phase3a_storage.claim_request.has_value() &&
            fixture.phase3a_storage.claim_request->account_id == 84,
        "admission, session commit, security, and notice claim belong to the authenticated account");
    const auto information = std::find_if(fixture.terminal.observations().begin(),
        fixture.terminal.observations().end(), [](const auto& observation) {
            return observation.identity == "caller.information";
        });
    require(information != fixture.terminal.observations().end() &&
            information->non_secret_text.find("You have called ") !=
                std::string::npos &&
            information->non_secret_text.find("Your security level is 10.\n\n") !=
                std::string::npos &&
            information->non_secret_text.find("Caller:") ==
                std::string::npos,
        "caller information belongs to the account actually authenticated");
    auto ready = result.ready->take();
    require(ready.has_value() && ready->account_id == 84 &&
            ready->display_name == "Other Caller" && ready->security_level == 10 &&
            !result.ready->take().has_value(),
        "cross-account resume hands off the authenticated account exactly once");
    require(fixture.terminal.hidden_input_requests() == 4 &&
            fixture.terminal.retained_secret_bytes() == 0 &&
            std::count_if(fixture.terminal.observations().begin(),
                fixture.terminal.observations().end(), [](const auto& observation) {
                    return observation.identity == "authentication.failure";
                }) == 1 &&
            std::none_of(fixture.terminal.observations().begin(),
                fixture.terminal.observations().end(), [](const auto& observation) {
                    return observation.identity == "authentication.final_attempt_warning" ||
                        observation.identity == "prelogin.builtin" ||
                        observation.identity == "prelogin.graphics";
                }),
        "cross-account resume preserves one shared failure, skips pre-login, and retains no secret");
}

void test_review_edits_every_field_and_boundary_stops() {
    Fixture fixture;
    ConfigurationOptions options;
    options.optional_real_name = true;
    options.location = app::RegistrationFieldMode::required;
    options.birth_date = app::RegistrationFieldMode::required;
    options.sex = app::RegistrationFieldMode::required;
    options.allow_x = true;
    options.email = app::EmailRegistrationMode::required;
    options.transfer = true;
    options.transfer_choices = std::vector{app::TransferProtocolPreference::xmodem,
        app::TransferProtocolPreference::zmodem};
    fixture.names.results.push_back(stored(registerable("New Caller")));
    script_password(fixture);
    fixture.terminal.script_presentation("registration.real_name.explanation");
    fixture.terminal.script_visible("registration.real_name", app::TerminalOutcome::empty);
    fixture.terminal.script_visible("registration.location", app::TerminalOutcome::completed, "Place One");
    fixture.terminal.script_visible("registration.birth_date", app::TerminalOutcome::completed, "01/01/2000");
    fixture.terminal.script_key("registration.sex", app::TerminalOutcome::completed, 'M');
    fixture.terminal.script_visible("registration.email", app::TerminalOutcome::completed, "one@example.test");
    fixture.terminal.script_key("registration.transfer_protocol", app::TerminalOutcome::completed, '1');

    fixture.terminal.script_presentation("registration.review");
    fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'R');
    auto edit = [&](char key) {
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.edit.action", app::TerminalOutcome::completed, key);
    };
    edit('R');
    fixture.terminal.script_presentation("registration.real_name.explanation");
    fixture.terminal.script_visible("registration.real_name", app::TerminalOutcome::completed, "Private Person");
    edit('L');
    fixture.terminal.script_visible("registration.location", app::TerminalOutcome::completed, "Place Two");
    edit('D');
    fixture.terminal.script_visible("registration.birth_date", app::TerminalOutcome::completed, "02/29/2000");
    edit('X');
    fixture.terminal.script_key("registration.sex", app::TerminalOutcome::completed, 'X');
    edit('E');
    fixture.terminal.script_visible("registration.email", app::TerminalOutcome::completed, "two@example.test");
    edit('T');
    fixture.terminal.script_key("registration.transfer_protocol", app::TerminalOutcome::completed, '2');
    edit('P');
    script_password(fixture, 20);
    edit('S');
    fixture.terminal.script_presentation("registration.review");
    fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'Q');
    fixture.terminal.script_presentation("registration.goodbye");
    auto active = session();
    unsigned edited_frames = 0;
    fixture.terminal.presentation_request_hook = [&](const app::TerminalPresentationRequest& request) {
        if (request.identity != "registration.review" || request.non_secret_text.rfind("[N] ", 0) != 0) return;
        const auto index = edited_frames++;
        const std::string expected = "[N] Caller Name: New Caller\n[P] Password: set\n[R] Real Name: " +
            std::string(index >= 1 ? "Private Person" : "(not supplied)") + "\n[L] Location: " +
            (index >= 2 ? "Place Two" : "Place One") + "\n[D] Date of birth: " +
            (index >= 3 ? "2000-02-29" : "2000-01-01") + "\n[X] Sex: " +
            (index >= 4 ? "X" : "M") + "\n[E] Email: " +
            (index >= 5 ? "two@example.test" : "one@example.test") + "\n[T] Default Transfer Protocol: " +
            (index >= 6 ? "Zmodem" : "Xmodem") + "\n";
        require(request.non_secret_text == expected,
            "F02 every enabled shortcut displays the current value after each edit; password remains only set");
    };
    const auto result = run(fixture, configuration(options), active);
    require(result.code == app::Phase3aWorkflowCode::caller_goodbye &&
        fixture.registration_commit.observations.empty() && edited_frames == 8,
        "all displayed profile/password/transfer edits remain draft-only before confirmation");
    require(std::count_if(fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& value) {
                return value.identity == "registration.review";
            }) == 10 && fixture.terminal.clear_count == 8,
        "deterministic review loop edits every displayed field before final rejection");
    require(fixture.credentials.observations.size() == 2 &&
        fixture.terminal.retained_secret_bytes() == 0,
        "password edit replaces the credential draft without retaining terminal secret bytes");

    struct StopCase { app::TerminalOutcome outcome; };
    for (const auto stop : {StopCase{app::TerminalOutcome::cancelled},
             StopCase{app::TerminalOutcome::timed_out},
             StopCase{app::TerminalOutcome::disconnected}}) {
        Fixture stopped;
        ConfigurationOptions stopped_options;
        stopped_options.location = app::RegistrationFieldMode::required;
        stopped.names.results.push_back(stored(registerable("New Caller")));
        script_password(stopped);
        stopped.terminal.script_visible("registration.location", stop.outcome);
        auto stopped_session = session();
        const auto stopped_result = run(stopped, configuration(stopped_options), stopped_session);
        require(!stopped_result.caller_ready() && stopped.registration_commit.observations.empty(),
            "pre-confirmation profile stop creates zero permanent rows");
    }
}

void test_failed_registration_sy_shared_attempt_budget() {
    ConfigurationOptions options;
    options.threshold = 10;
    options.new_user_level = 10;
    Fixture fixture;
    fixture.accounts.policy_result = stored(policy(10, 10));
    fixture.accounts.account_result = stored(account("New Caller", 10));
    script_name_checks(fixture, "New Caller");
    fixture.names.results.push_back(stored(existing("New Caller", 10)));
    fixture.names.results.push_back(stored(existing("New Caller", 10)));
    script_password(fixture);
    script_review_confirm(fixture);
    fixture.registration_commit.results.push_back(
        {app::StorageStatus::success(), 42, 7, false});

    fixture.terminal.script_secret_completed("registration.system_password", 8);
    fixture.credentials.system_results.push_back(
        {app::AuthenticationOperationCode::rejected, false, false, false, false});
    fixture.terminal.script_presentation("authentication.failure");
    for (int existing_attempt = 0; existing_attempt < 2; ++existing_attempt) {
        fixture.terminal.script_visible("caller.name.first",
            app::TerminalOutcome::completed, "New");
        fixture.terminal.script_visible("caller.name.last",
            app::TerminalOutcome::completed, "Caller");
        fixture.terminal.script_presentation("caller.name.searching");
        fixture.terminal.script_secret_completed("authentication.password", 14);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.terminal.script_secret_completed("authentication.system_password", 9);
        fixture.credentials.system_results.push_back(
            {app::AuthenticationOperationCode::rejected, false, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        if (existing_attempt == 0)
            fixture.terminal.script_presentation("authentication.final_attempt_warning");
    }

    auto active = session();
    const auto result = run(fixture, configuration(options), active);
    require(result.code == app::Phase3aWorkflowCode::authentication_denied &&
        !result.caller_ready() && active.state() == core::SessionState::disconnected,
        "the third overall failure across registration and existing login disconnects");
    require(fixture.registration_commit.observations.size() == 1 &&
        fixture.phase3a_storage.enqueue_requests.size() == 3 &&
        std::all_of(fixture.phase3a_storage.enqueue_requests.begin(),
            fixture.phase3a_storage.enqueue_requests.end(), [](const auto& notice) {
                return notice.account_id == 42 && notice.source_ip == "192.0.2.7";
            }),
        "one registration commit remains valid and each failed SY enqueues exactly one notice");
    const std::vector<std::string> credential_order = [&fixture] {
        std::vector<std::string> values;
        for (const auto& observation : fixture.credentials.observations)
            values.push_back(observation.operation);
        return values;
    }();
    require(credential_order == std::vector<std::string>{"create_registration",
                "verify_system", "verify_existing", "verify_system",
                "verify_existing", "verify_system"},
        "every later SY prompt follows a fresh successful ordinary-password verification");
    require(std::count_if(fixture.rates.observations.begin(),
                fixture.rates.observations.end(), [](const auto& observation) {
                    return observation.operation == "failure" &&
                        observation.scopes.source && observation.scopes.global &&
                        observation.scopes.system_password &&
                        !observation.scopes.account_id.has_value();
                }) == 3,
        "all three failed SY observations use source/global/SY and no account scope");
    require(std::count_if(fixture.terminal.observations().begin(),
                fixture.terminal.observations().end(), [](const auto& observation) {
                    return observation.identity == "authentication.failure";
                }) == 3 &&
        std::count_if(fixture.terminal.observations().begin(),
                fixture.terminal.observations().end(), [](const auto& observation) {
                    return observation.identity == "authentication.final_attempt_warning";
                }) == 1,
        "three generic failures share one final-attempt warning and one budget");
    require(fixture.admission.requests.empty() && fixture.session_commit.requests.empty() &&
        std::count_if(fixture.displays.requests.begin(), fixture.displays.requests.end(),
            [](const auto& request) {
                return request.identity == app::TrustedDisplayIdentity::newuser;
            }) == 1,
        "failed authentication blocks admission/session; initial rules display grants no authority");
    require(fixture.terminal.retained_secret_bytes() == 0 &&
        fixture.terminal.hidden_input_requests() == 7 &&
        std::none_of(fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "prelogin.builtin" ||
                    observation.identity == "prelogin.sfprelog" ||
                    observation.identity == "prelogin.graphics";
            }),
        "the resumed path repeats neither pre-login nor a retained registration secret");
}

void test_registration_inherits_transport_failures() {
    ConfigurationOptions options;
    options.threshold = 10;
    Fixture fixture;
    fixture.accounts.policy_result = stored(policy(10, 10));
    fixture.accounts.account_result = stored(account("New Caller", 10));
    script_name_checks(fixture, "New Caller");
    script_password(fixture);
    script_review_confirm(fixture);
    fixture.registration_commit.results.push_back({app::StorageStatus::success(), 42, 7, false});
    fixture.terminal.script_secret_completed("registration.system_password", 8);
    fixture.credentials.system_results.push_back(
        {app::AuthenticationOperationCode::rejected, false, false, false, false});
    fixture.terminal.script_presentation("authentication.failure");
    auto active = session();
    app::CancellationSource cancellation;
    auto workflow = fixture.workflow();
    const auto result = workflow.run(configuration(options), active,
        {{"New Caller", app::GraphicsMode::ansi, 2}, "192.0.2.7", identifier(900)}, cancellation.token());
    require(result.code == app::Phase3aWorkflowCode::authentication_denied && fixture.terminal.clean() &&
        fixture.registration_commit.observations.size() == 1 && fixture.session_commit.requests.empty(),
        "registration keeps two prior transport failures: one failed SY disconnects without online admission");
    require(!app::Phase3aRegistrationContinuation{"New Caller", app::GraphicsMode::ansi, 3}.valid(),
        "an exhausted transport failure budget cannot enter registration");
}

void test_failed_registration_sy_resume_stop_outcomes() {
    ConfigurationOptions options;
    options.threshold = 10;
    options.new_user_level = 10;
    for (const auto outcome : {app::TerminalOutcome::cancelled,
             app::TerminalOutcome::timed_out, app::TerminalOutcome::disconnected,
             app::TerminalOutcome::shutdown_requested}) {
        Fixture fixture;
        fixture.accounts.policy_result = stored(policy(10, 10));
        fixture.accounts.account_result = stored(account("New Caller", 10));
        script_name_checks(fixture, "New Caller");
        script_password(fixture);
        script_review_confirm(fixture);
        fixture.registration_commit.results.push_back(
            {app::StorageStatus::success(), 42, 7, false});
        fixture.terminal.script_secret_completed("registration.system_password", 8);
        fixture.credentials.system_results.push_back(
            {app::AuthenticationOperationCode::rejected, false, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        fixture.terminal.script_visible("caller.name.first", outcome);
        auto active = session();
        const auto result = run(fixture, configuration(options), active);
        const auto expected = outcome == app::TerminalOutcome::cancelled
            ? app::Phase3aWorkflowCode::cancelled
            : outcome == app::TerminalOutcome::timed_out
                ? app::Phase3aWorkflowCode::timed_out
                : outcome == app::TerminalOutcome::disconnected
                    ? app::Phase3aWorkflowCode::disconnected
                    : app::Phase3aWorkflowCode::shutdown_requested;
        require(result.code == expected && !result.caller_ready() &&
            active.state() == core::SessionState::disconnected &&
            fixture.registration_commit.observations.size() == 1 &&
            fixture.phase3a_storage.enqueue_requests.size() == 1,
            "resume cancellation/timeout/disconnect/shutdown preserves the account and one failed-SY notice");
        require(fixture.admission.requests.empty() && fixture.session_commit.requests.empty() &&
            fixture.terminal.retained_secret_bytes() == 0,
            "resume stop outcomes create no authenticated session, lease, handoff, or retained secret");
    }
}

void test_name_races_kdf_failures_and_complete_transfer_domain() {
    {
        Fixture fixture;
        fixture.names.results.push_back(stored(unavailable("New Caller")));
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::registration_unavailable &&
            fixture.terminal.hidden_input_requests() == 0 &&
            fixture.registration_commit.observations.empty(),
            "initial duplicate or reserved name stops before secret input and commit");
    }
    {
        Fixture fixture;
        fixture.names.results.push_back(stored(registerable("New Caller")));
        fixture.names.results.push_back(stored(unavailable("New Caller")));
        script_password(fixture);
        script_review_confirm(fixture);
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::registration_unavailable &&
            fixture.registration_commit.observations.empty(),
            "name becoming reserved before commit fails closed without invoking the commit port");
    }
    for (const auto code : {app::AuthenticationOperationCode::cancelled,
             app::AuthenticationOperationCode::unavailable}) {
        Fixture fixture;
        fixture.names.results.push_back(stored(registerable("New Caller")));
        fixture.credentials.registration_result =
            {code, app::PasswordPolicyCode::accepted, {}};
        script_password(fixture);
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(!result.caller_ready() && fixture.registration_commit.observations.empty() &&
            fixture.terminal.retained_secret_bytes() == 0,
            "KDF cancellation or failure erases prompt secrets and creates no account");
    }
    {
        Fixture fixture;
        fixture.names.results.push_back(stored(registerable("New Caller")));
        fixture.credentials.registration_result = {app::AuthenticationOperationCode::completed,
            app::PasswordPolicyCode::accepted, {}};
        script_password(fixture);
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::internal_failure &&
            fixture.registration_commit.observations.empty() &&
            fixture.terminal.retained_secret_bytes() == 0,
            "malformed successful KDF result fails closed after explicit secret erasure");
    }

    for (const auto preference : {app::TransferProtocolPreference::xmodem,
             app::TransferProtocolPreference::ymodem,
             app::TransferProtocolPreference::ymodem_g,
             app::TransferProtocolPreference::zmodem}) {
        Fixture fixture;
        ConfigurationOptions options;
        options.transfer = true;
        options.transfer_choices = {preference};
        script_name_checks(fixture, "New Caller");
        script_password(fixture);
        fixture.terminal.script_key("registration.transfer_protocol",
            app::TerminalOutcome::completed, '1');
        script_review_confirm(fixture);
        fixture.registration_commit.results.push_back(
            {{app::StorageErrorCode::conflict, "bounded stop"}, {}, {}, false});
        auto active = session();
        run(fixture, configuration(options), active);
        require(fixture.registration_commit.observations.size() == 1 &&
            fixture.registration_commit.observations[0].transfer_preference == preference,
            "every enabled typed transfer preference reaches the closed commit contract");
    }
    {
        Fixture fixture;
        ConfigurationOptions options;
        options.email = app::EmailRegistrationMode::ask;
        script_name_checks(fixture, "New Caller");
        script_password(fixture);
        fixture.terminal.script_visible("registration.email", app::TerminalOutcome::empty);
        script_review_confirm(fixture);
        fixture.registration_commit.results.push_back(
            {{app::StorageErrorCode::conflict, "bounded stop"}, {}, {}, false});
        auto active = session();
        run(fixture, configuration(options), active);
        require(fixture.registration_commit.observations.size() == 1 &&
            !fixture.registration_commit.observations[0].has_email,
            "Ask-mode email may be declined without inventing a value");
    }
}

enum class PreconfirmationBoundary {
    newreg,
    password_confirmation,
    real_name_explanation,
    real_name,
    location,
    birth_date,
    sex,
    email,
    transfer,
    review_presentation,
    review_action,
    edited_name_first,
    edited_name_last,
    edited_password
};

void script_stop_at(Fixture& fixture, ConfigurationOptions& options,
    const PreconfirmationBoundary boundary, const app::TerminalOutcome outcome) {
    fixture.names.results.push_back(stored(registerable("New Caller")));
    if (boundary == PreconfirmationBoundary::newreg) {
        fixture.displays.results.push_back({app::TrustedDisplaySelectionCode::available,
            test::trusted_display_program(app::TrustedDisplayIdentity::newreg,
                app::GraphicsMode::ansi, "NEWREG\n")});
        fixture.terminal.script_presentation("registration.newreg", outcome);
        return;
    }
    if (boundary == PreconfirmationBoundary::password_confirmation) {
        fixture.terminal.script_secret_completed("registration.password", 12);
        fixture.terminal.script_secret_outcome("registration.password.confirm", outcome);
        return;
    }
    script_password(fixture);
    switch (boundary) {
        case PreconfirmationBoundary::real_name_explanation:
            options.optional_real_name = true;
            fixture.terminal.script_presentation("registration.real_name.explanation", outcome);
            break;
        case PreconfirmationBoundary::real_name:
            options.optional_real_name = true;
            fixture.terminal.script_presentation("registration.real_name.explanation");
            fixture.terminal.script_visible("registration.real_name", outcome);
            break;
        case PreconfirmationBoundary::location:
            options.location = app::RegistrationFieldMode::required;
            fixture.terminal.script_visible("registration.location", outcome);
            break;
        case PreconfirmationBoundary::birth_date:
            options.birth_date = app::RegistrationFieldMode::required;
            fixture.terminal.script_visible("registration.birth_date", outcome);
            break;
        case PreconfirmationBoundary::sex:
            options.sex = app::RegistrationFieldMode::required;
            fixture.terminal.script_key("registration.sex", outcome);
            break;
        case PreconfirmationBoundary::email:
            options.email = app::EmailRegistrationMode::ask;
            fixture.terminal.script_visible("registration.email", outcome);
            break;
        case PreconfirmationBoundary::transfer:
            options.transfer = true;
            options.transfer_choices = std::vector{app::TransferProtocolPreference::zmodem};
            fixture.terminal.script_key("registration.transfer_protocol", outcome);
            break;
        case PreconfirmationBoundary::review_presentation:
            fixture.terminal.script_presentation("registration.review", outcome);
            break;
        case PreconfirmationBoundary::review_action:
            fixture.terminal.script_presentation("registration.review");
            fixture.terminal.script_key("registration.review.action", outcome);
            break;
        case PreconfirmationBoundary::edited_name_first:
            fixture.terminal.script_presentation("registration.review");
            fixture.terminal.script_key("registration.review.action",
                app::TerminalOutcome::completed, 'R');
            fixture.terminal.script_presentation("registration.review");
            fixture.terminal.script_key("registration.edit.action",
                app::TerminalOutcome::completed, 'N');
            fixture.terminal.script_visible("registration.name.first", outcome);
            break;
        case PreconfirmationBoundary::edited_name_last:
            fixture.terminal.script_presentation("registration.review");
            fixture.terminal.script_key("registration.review.action",
                app::TerminalOutcome::completed, 'R');
            fixture.terminal.script_presentation("registration.review");
            fixture.terminal.script_key("registration.edit.action",
                app::TerminalOutcome::completed, 'N');
            fixture.terminal.script_visible("registration.name.first",
                app::TerminalOutcome::completed, "Edited");
            fixture.terminal.script_visible("registration.name.last", outcome);
            break;
        case PreconfirmationBoundary::edited_password:
            fixture.terminal.script_presentation("registration.review");
            fixture.terminal.script_key("registration.review.action",
                app::TerminalOutcome::completed, 'R');
            fixture.terminal.script_presentation("registration.review");
            fixture.terminal.script_key("registration.edit.action",
                app::TerminalOutcome::completed, 'P');
            fixture.terminal.script_secret_outcome("registration.password", outcome);
            break;
        case PreconfirmationBoundary::newreg:
        case PreconfirmationBoundary::password_confirmation:
            break;
    }
}

void test_every_preconfirmation_terminal_stop_boundary() {
    const std::vector<PreconfirmationBoundary> boundaries{
        PreconfirmationBoundary::newreg,
        PreconfirmationBoundary::password_confirmation,
        PreconfirmationBoundary::real_name_explanation,
        PreconfirmationBoundary::real_name,
        PreconfirmationBoundary::location,
        PreconfirmationBoundary::birth_date,
        PreconfirmationBoundary::sex,
        PreconfirmationBoundary::email,
        PreconfirmationBoundary::transfer,
        PreconfirmationBoundary::review_presentation,
        PreconfirmationBoundary::review_action,
        PreconfirmationBoundary::edited_name_first,
        PreconfirmationBoundary::edited_name_last,
        PreconfirmationBoundary::edited_password};
    for (const auto boundary : boundaries) {
        for (const auto outcome : {app::TerminalOutcome::cancelled,
                 app::TerminalOutcome::timed_out,
                 app::TerminalOutcome::disconnected}) {
            Fixture fixture;
            ConfigurationOptions options;
            script_stop_at(fixture, options, boundary, outcome);
            auto active = session();
            const auto result = run(fixture, configuration(options), active);
            require(!result.caller_ready() && fixture.registration_commit.observations.empty() &&
                fixture.terminal.retained_secret_bytes() == 0,
                "cancel/timeout/disconnect at a pre-confirmation boundary creates no account or retained secret");
        }
    }
}


void test_exception_safe_durable_lifetime() {
    for (unsigned scenario = 0; scenario != 4; ++scenario) {
        Fixture fixture;
        script_name_checks(fixture, "New Caller");
        script_password(fixture);
        script_review_confirm(fixture);
        fixture.registration_commit.results.push_back({app::StorageStatus::success(), 42, 7, false});
        script_success_tail(fixture);
        if (scenario == 1) fixture.session_commit.commit_hook = [] {
            throw std::runtime_error("injected post-commit ambiguity");
        };
        else if (scenario != 3) fixture.welcome_statistics.count_hook = [] {
            throw std::runtime_error("ordinary/System/throwaway secret must not be observed");
        };
        if (scenario == 2) fixture.accounts.mutation_result.status = {
            app::StorageErrorCode::io, "injected finalization failure"};
        auto active = session();
        {
            auto result = run(fixture, configuration(), active);
            if (scenario == 3) {
                require(result.caller_ready(), "ready result retains durable ownership until consumed or discarded");
                require(fixture.accounts.finalize_requests.empty(), "durable session stays active while handoff owns it");
            } else {
                require(result.code == (scenario == 2 ? app::Phase3aWorkflowCode::cleanup_failure :
                    app::Phase3aWorkflowCode::internal_failure) && !result.ready,
                    "unexpected post-commit exception is contained with typed cleanup outcome");
                require(!active.caller() && active.state() == core::SessionState::disconnected,
                    "exception removes authenticated core session authority");
            }
        }
        require(fixture.accounts.finalize_requests.size() == (scenario == 2 ? 3U : 1U) &&
            fixture.admission.releases.size() == 1, "post-commit exception or abandoned handoff finalizes and releases account exactly once");
        require(fixture.cleanup_health->failed.load() == (scenario == 2),
            "unresolved finalization is sticky for assignment quarantine");
        require(fixture.terminal.retained_secret_bytes() == 0,
            "ordinary registration and System Password secrets remain erased on exceptional cleanup");
        require(fixture.registration_commit.observations.size() == 1, "committed registration is never rolled back by session failure");
    }
}

void test_c4_review_authority_and_failures() {
    for (const bool confirm : {false, true}) {
        Fixture fixture;
        script_name_checks(fixture, "New Caller");
        script_password(fixture);
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::invalid);
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'R');
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.edit.action", app::TerminalOutcome::invalid);
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.edit.action", app::TerminalOutcome::completed, 'N');
        fixture.terminal.script_visible("registration.name.first", app::TerminalOutcome::empty);
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.edit.action", app::TerminalOutcome::completed, 'P');
        fixture.terminal.script_secret_outcome("registration.password", app::TerminalOutcome::empty);
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.edit.action", app::TerminalOutcome::empty);
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, confirm ? 'C' : 'Q');
        if (confirm) {
            fixture.registration_commit.results.push_back({app::StorageStatus::success(), 42, 7, false});
            script_success_tail(fixture);
        } else fixture.terminal.script_presentation("registration.goodbye");
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(confirm ? result.caller_ready() : result.code == app::Phase3aWorkflowCode::caller_goodbye,
            "C4 invalid keys and unchanged edits return to an explicit final decision");
        require(fixture.registration_commit.observations.size() == (confirm ? 1U : 0U),
            "only final Confirm commits, exactly once; Review/Edit and Quit do not");
        require(fixture.terminal.clear_count == 4 && fixture.terminal.clean(),
            "each editor refresh clears and displays the draft, including invalid keys and unchanged values");
        require(fixture.terminal.retained_secret_bytes() == 0 && fixture.credentials.observations.size() == 1,
            "unchanged name/password retains one derived credential, never the original secret");
    }
    for (const bool style_failure : {false, true}) {
        Fixture fixture;
        fixture.names.results.push_back(stored(registerable("New Caller")));
        if (style_failure) fixture.terminal.style_outcome = app::TerminalOutcome::output_failure;
        else {
            script_password(fixture);
            fixture.terminal.script_presentation("registration.review");
            fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'R');
            fixture.terminal.clear_outcome = app::TerminalOutcome::output_failure;
        }
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(!result.caller_ready() && fixture.registration_commit.observations.empty() &&
            fixture.session_commit.requests.empty() && fixture.admission.requests.empty(),
            "C4 style/clear failures cannot acquire account or durable-session authority");
        require(fixture.terminal.clean() && fixture.terminal.retained_secret_bytes() == 0,
            "C4 presentation failure erases secrets and consumes only the expected operations");
    }
    for (unsigned selected = 1; selected <= 5; ++selected) {
        Fixture fixture;
        ConfigurationOptions options;
        options.transfer = true;
        options.transfer_choices = std::vector{app::TransferProtocolPreference::xmodem,
            app::TransferProtocolPreference::ymodem, app::TransferProtocolPreference::ymodem_g,
            app::TransferProtocolPreference::zmodem};
        script_name_checks(fixture, "New Caller");
        script_password(fixture);
        fixture.terminal.script_key("registration.transfer_protocol", app::TerminalOutcome::completed,
            static_cast<char>('0' + selected));
        script_review_confirm(fixture);
        fixture.registration_commit.results.push_back({{app::StorageErrorCode::conflict, "bounded stop"}, {}, {}, false});
        auto active = session();
        run(fixture, configuration(options), active);
        const auto& observations = fixture.terminal.observations();
        const auto prompt = std::find_if(observations.begin(), observations.end(), [](const auto& o) {
            return o.identity == "registration.transfer_protocol";
        });
        require(prompt != observations.end() && prompt->non_secret_text ==
            "\nSelect a default transfer protocol:\n\n[1] Xmodem\n[2] Ymodem\n[3] Ymodem-G\n[4] Zmodem\n[5] None\n\nSelection: ",
            "C4 five vertical labels and blank rows are literal, never internal enum strings");
        const auto expected = selected == 5 ? app::TransferProtocolPreference::select_at_transfer : options.transfer_choices[selected - 1];
        require(fixture.registration_commit.observations.size() == 1 &&
            fixture.registration_commit.observations[0].transfer_preference == expected,
            "all five choices commit their correct typed value; None means ask at transfer time");
    }
}

void test_f02_maximum_fields_and_f03_quit() {
    for (const bool supplied : {false, true}) {
        Fixture fixture;
        ConfigurationOptions options;
        options.optional_real_name = true;
        options.location = options.birth_date = options.sex = app::RegistrationFieldMode::required;
        options.email = app::EmailRegistrationMode::ask;
        options.transfer = true;
        options.transfer_choices = std::vector{app::TransferProtocolPreference::ymodem_g};
        const std::string real_name(64, 'R'), location(64, 'L');
        const std::string email = std::string(64, 'e') + "@" + std::string(63, 'a') + "." +
            std::string(63, 'b') + "." + std::string(57, 'c') + ".net";
        require(email.size() == 254, "F02 exercises the complete accepted email bound");
        script_name_checks(fixture, "New Caller"); script_password(fixture, 128);
        fixture.terminal.script_presentation("registration.real_name.explanation");
        fixture.terminal.script_visible("registration.real_name", app::TerminalOutcome::completed, real_name);
        fixture.terminal.script_visible("registration.location", app::TerminalOutcome::completed, location);
        fixture.terminal.script_visible("registration.birth_date", app::TerminalOutcome::completed, "01/01/2000");
        fixture.terminal.script_key("registration.sex", app::TerminalOutcome::completed, 'F');
        fixture.terminal.script_visible("registration.email", supplied ? app::TerminalOutcome::completed : app::TerminalOutcome::empty,
            supplied ? email : "");
        fixture.terminal.script_key("registration.transfer_protocol", app::TerminalOutcome::completed, '1');
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'R');
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.edit.action", app::TerminalOutcome::empty);
        fixture.terminal.script_presentation("registration.review");
        fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'Q');
        fixture.terminal.script_presentation("registration.goodbye");
        std::string edit_text;
        fixture.terminal.presentation_request_hook = [&](const app::TerminalPresentationRequest& request) {
            if (request.identity == "registration.review" && request.non_secret_text.rfind("[N] ", 0) == 0)
                edit_text = request.non_secret_text;
        };
        fixture.terminal.key_request_hook = [&](const app::TerminalKeyRequest& request) {
            if (request.identity == "registration.review.action") require(request.prompt ==
                "\n[R]eview/Edit your settings\n[C]onfirm settings\n[Q]uit & disconnect - no account created\n\nSelection: ",
                "F03 exact three options and one blank line before Selection, without another confirmation");
            if (request.identity != "registration.edit.action") return;
            require(edit_text == "[N] Caller Name: New Caller\n[P] Password: set\n[R] Real Name: " + real_name +
                "\n[L] Location: " + location + "\n[D] Date of birth: 2000-01-01\n[X] Sex: F\n[E] Email: " +
                (supplied ? email : "(not supplied)") + "\n[T] Default Transfer Protocol: Ymodem-G\n",
                "F02 full values and friendly labels survive maximum bounds and declined email");
            std::size_t row = 0, column = 0;
            for (const char ch : edit_text + request.prompt) {
                if (ch == '\n') { ++row; column = 0; }
                else { if (column == 80) { ++row; column = 0; } ++column; }
            }
            require(row < 25 && column <= 80 && edit_text.find('*') == std::string::npos &&
                edit_text.find("128") == std::string::npos,
                "F02 entire maximum-value editor fits 80x25 without password length or masks");
            require(fixture.registration_commit.observations.empty() && fixture.admission.requests.empty() &&
                fixture.session_commit.requests.empty(), "F02 editor has no account or session authority");
        };
        auto active = session();
        const auto result = run(fixture, configuration(options), active);
        require(result.code == app::Phase3aWorkflowCode::caller_goodbye,
            "F03 maximum-value path outcome=" + std::to_string(static_cast<int>(result.code)) +
            " pending=" + std::to_string(fixture.terminal.pending_operations()) + " last operation=" +
            (fixture.terminal.observations().empty() ? "none" : fixture.terminal.observations().back().identity));
        require(result.code == app::Phase3aWorkflowCode::caller_goodbye && fixture.terminal.clean() &&
            fixture.terminal.retained_secret_bytes() == 0 && fixture.registration_commit.observations.empty() &&
            fixture.admission.requests.empty() && fixture.session_commit.requests.empty(),
            "F03 Save then Quit creates nothing, consumes Goodbye and releases secret state");
    }
}

void test_c4_save_and_optional_sex_shortcuts() {
    for (const bool sex_enabled : {false, true}) {
        for (const char save_key : {'S', 's', '\r'}) {
            for (const char decision : {'C', 'Q', '\0'}) {
                Fixture fixture;
                ConfigurationOptions options;
                options.sex = sex_enabled ? app::RegistrationFieldMode::required : app::RegistrationFieldMode::off;
                script_name_checks(fixture, "New Caller");
                script_password(fixture);
                if (sex_enabled) fixture.terminal.script_key("registration.sex", app::TerminalOutcome::completed, 'M');
                fixture.terminal.script_presentation("registration.review");
                fixture.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'R');
                fixture.terminal.script_presentation("registration.review");
                // X edits the enabled field; with the field disabled it is invalid,
                // and in neither case may it return to the final confirmation.
                fixture.terminal.script_key("registration.edit.action", app::TerminalOutcome::completed, 'X');
                if (sex_enabled) fixture.terminal.script_key("registration.sex", app::TerminalOutcome::completed, 'F');
                fixture.terminal.script_presentation("registration.review");
                fixture.terminal.script_key("registration.edit.action", save_key == '\r'
                    ? app::TerminalOutcome::empty : app::TerminalOutcome::completed,
                    save_key == '\r' ? std::nullopt : std::optional<char>{save_key});
                fixture.terminal.script_presentation("registration.review");
                fixture.terminal.script_key("registration.review.action", decision == '\0'
                    ? app::TerminalOutcome::cancelled : app::TerminalOutcome::completed,
                    decision == '\0' ? std::nullopt : std::optional<char>{decision});
                if (decision == 'C') {
                    fixture.registration_commit.results.push_back({app::StorageStatus::success(), 42, 7, false});
                    script_success_tail(fixture);
                } else if (decision == 'Q') fixture.terminal.script_presentation("registration.goodbye");
                auto active = session();
                unsigned final_prompts = 0, editor_prompts = 0, save_boundary_checks = 0;
                bool shows_sex = false;
                fixture.terminal.presentation_request_hook = [&](const app::TerminalPresentationRequest& request) {
                    if (request.identity == "registration.review")
                        shows_sex = request.non_secret_text.find("[X] Sex: ") != std::string::npos;
                };
                fixture.terminal.key_request_hook = [&](const app::TerminalKeyRequest& request) {
                    if (request.identity == "registration.edit.action") {
                        ++editor_prompts;
                        require(request.default_key == 'S' &&
                            request.prompt.find("[S] Save and return to confirmation") != std::string::npos &&
                            request.prompt.find("Selection [S]: ") != std::string::npos,
                            "S and Enter exclusively select Save in the editor");
                        require((std::find(request.allowed_keys.begin(), request.allowed_keys.end(), 'X') != request.allowed_keys.end()) == sex_enabled &&
                            shows_sex == sex_enabled,
                            "X is exposed only for the enabled Sex field, never as an editor exit");
                    }
                    if (request.identity == "registration.review.action" && ++final_prompts == 2) {
                        ++save_boundary_checks;
                        require(fixture.registration_commit.observations.empty() && fixture.admission.requests.empty() &&
                            fixture.admission.releases.empty() && fixture.session_commit.requests.empty(),
                            "at the post-Save prompt there has been no account commit, account lease or durable session");
                        require(!active.caller() && active.state() != core::SessionState::authenticated,
                            "Save returns the draft without granting online or authenticated authority");
                    }
                };
                {
                    const auto result = run(fixture, configuration(options), active);
                    require(save_boundary_checks == 1 && final_prompts == 2 && editor_prompts == 2 &&
                        fixture.terminal.clear_count == 2 && fixture.terminal.clean(),
                        "X stays inside the editor and S/lowercase-s/Enter reach one checked Save boundary");
                    const bool confirm = decision == 'C';
                    require(confirm ? result.caller_ready() : !result.caller_ready(),
                        "only the separate final Confirm grants caller readiness");
                    require(fixture.registration_commit.observations.size() == (confirm ? 1U : 0U) &&
                        fixture.session_commit.requests.size() == (confirm ? 1U : 0U) &&
                        fixture.admission.requests.size() == (confirm ? 1U : 0U),
                        "Confirm commits once; Save followed by Quit/cancellation commits nothing and leases no account");
                    if (decision == 'Q') require(result.code == app::Phase3aWorkflowCode::caller_goodbye,
                        "Quit after Save still sends Goodbye and ends registration");
                }
                require(fixture.terminal.retained_secret_bytes() == 0 &&
                    fixture.admission.releases.size() == (decision == 'C' ? 1U : 0U),
                    "each focused path erases secrets and releases any Confirm-owned account lease");
            }
        }
    }
}

void test_r1_registration_name_gate() {
    struct Observer : app::ICallerStageObserver {
        unsigned denies{};
        void stage(app::CallerStage) noexcept override {}
        void attention(app::AttentionKind kind, app::AccountId, std::string_view, std::uint16_t) noexcept override {
            if (kind == app::AttentionKind::new_name_denied) ++denies;
        }
    };
    for (const bool ssh : {false, true}) for (const bool invalid : {false, true}) {
        Fixture fixture; Observer observer; fixture.observer = &observer;
        const auto list = invalid ? app::NewCallerNames::unavailable() : app::NewCallerNames::parse("new caller\r\n");
        fixture.new_names = &list;
        fixture.names.results.push_back(stored(registerable("New Caller")));
        fixture.terminal.script_presentation("registration.name.unavailable");
        core::Session active("r1-name-gate", *core::NodeNumber::create(1),
            {ssh ? core::TransportKind::ssh : core::TransportKind::telnet, "192.0.2.7", {}, "ansi"});
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::registration_unavailable && !result.ready &&
            active.state() == core::SessionState::disconnected && !active.caller(),
            "shared Telnet/SSH registration gate refuses before draft/authority creation");
        require(invalid ? !result.preauthentication_end.has_value() :
            result.preauthentication_end.has_value() &&
                result.preauthentication_end->reason ==
                    app::Phase3aPreAuthenticationEndReason::joker_name_rejected &&
                result.preauthentication_end->attempted_name == "New Caller",
            "only a confirmed JOKER match carries a bounded typed pre-authentication ending");
        require(fixture.terminal.hidden_input_requests() == 0 && fixture.credentials.observations.empty() &&
            fixture.registration_commit.observations.empty() && fixture.admission.requests.empty() &&
            fixture.session_commit.requests.empty() && fixture.terminal.clean(),
            "denied draft collects no password, commits nothing and creates no session/caller number");
        require(observer.denies == (invalid ? 0U : 1U), "one typed deny event; invalid-file warning not repeated by registration");
    }
}

void test_live_name_policy_rechecks_confirmation_and_edit() {
    struct Policy final : app::INewCallerNamePolicy {
        mutable std::vector<std::string> checked;
        bool invalid{};
        app::NewCallerNameDecision evaluate(std::string_view name) const override {
            checked.emplace_back(name);
            return checked.size() == 1 ? app::NewCallerNameDecision::allowed :
                invalid ? app::NewCallerNameDecision::temporarily_unavailable :
                          app::NewCallerNameDecision::unavailable;
        }
    };
    for (const bool ssh : {false, true}) for (const bool edit : {false, true})
        for (const bool invalid : {false, true}) {
            Fixture f; Policy live; live.invalid = invalid; f.new_names = &live;
            f.names.results.push_back(stored(registerable("New Caller")));
            f.names.results.push_back(stored(registerable(edit ? "Changed Caller" : "New Caller")));
            script_password(f);
            if (edit) {
                f.terminal.script_presentation("registration.review");
                f.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'R');
                f.terminal.script_presentation("registration.review");
                f.terminal.script_key("registration.edit.action", app::TerminalOutcome::completed, 'N');
                f.terminal.script_visible("registration.name.first", app::TerminalOutcome::completed, "Changed");
                f.terminal.script_visible("registration.name.last", app::TerminalOutcome::completed, "Caller");
            } else script_review_confirm(f);
            f.terminal.script_presentation("registration.name.unavailable");
            if (edit) {
                f.terminal.script_presentation("registration.review");
                f.terminal.script_key("registration.edit.action", app::TerminalOutcome::completed, 'S');
                f.terminal.script_presentation("registration.review");
                f.terminal.script_key("registration.review.action", app::TerminalOutcome::completed, 'Q');
                f.terminal.script_presentation("registration.goodbye");
            }
            core::Session active("live-name-policy", *core::NodeNumber::create(1),
                {ssh ? core::TransportKind::ssh : core::TransportKind::telnet, "192.0.2.7", {}, "ansi"});
            const auto result = run(f, configuration(), active);
            require(live.checked == std::vector<std::string>{"New Caller", edit ? "Changed Caller" : "New Caller"},
                "Telnet/SSH Confirm and changed draft name acquire another current name decision");
            require(result.code == (edit ? app::Phase3aWorkflowCode::caller_goodbye :
                app::Phase3aWorkflowCode::registration_unavailable) && f.terminal.clean(),
                "newly denied/invalid policy is honored without restarting or reusing the initial allowed decision");
            require((!invalid && !edit) ? result.preauthentication_end.has_value() &&
                    result.preauthentication_end->attempted_name == "New Caller" :
                    !result.preauthentication_end.has_value(),
                "ending confirmation match is labeled; edit-and-retry and invalid-file paths remain unlabeled");
            require(f.registration_commit.observations.empty() && f.session_commit.requests.empty() &&
                f.admission.requests.empty() && !active.caller() && f.terminal.retained_secret_bytes() == 0,
                "live recheck refusal creates no account/session/authority and erases pending secrets");
        }
}

}  // namespace

int main() {
    try {
        test_live_name_policy_rechecks_confirmation_and_edit();
        test_r1_registration_name_gate();
        test_f02_maximum_fields_and_f03_quit();
        test_c4_save_and_optional_sex_shortcuts();
        test_c4_review_authority_and_failures();
        test_exception_safe_durable_lifetime();
        test_resume_newuser_entitlement_validation();
        test_minimal_success_and_exactly_once_handoff();
        test_minimal_success_and_exactly_once_handoff("198.51.100.41");
        test_minimal_success_and_exactly_once_handoff("203.0.113.19");
        test_minimal_success_and_exactly_once_handoff("127.0.0.1");
        test_minimal_success_and_exactly_once_handoff("2001:db8::7");
        test_fixed_field_order_validation_and_private_observation_safety();
        test_password_failures_and_preconfirmation_zero_commit();
        test_idempotent_retry_name_edit_and_real_name_alias_rule();
        test_policy_retry_transfer_modes_and_age_boundaries();
        test_sysop_sequence_optional_displays_and_post_commit_failures();
        test_failed_registration_sy_cross_account_newuser_binding();
        test_review_edits_every_field_and_boundary_stops();
        test_failed_registration_sy_shared_attempt_budget();
        test_registration_inherits_transport_failures();
        test_failed_registration_sy_resume_stop_outcomes();
        test_name_races_kdf_failures_and_complete_transfer_domain();
        test_every_preconfirmation_terminal_stop_boundary();
        std::cout << "all SFX Phase 3A Window 4 registration tests passed assertions="
                  << assertions << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "SFX Phase 3A Window 4 test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
