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

void require(const bool condition, const std::string& message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

app::Identifier128 identifier(const std::uint32_t value) {
    test::DeterministicIdentifierSource source(value);
    return source.next();
}

template <typename T>
app::StorageResult<T> stored(T value) {
    return {app::StorageStatus::success(), std::move(value)};
}

app::StorageStatus failed_storage(
    const app::StorageErrorCode code,
    std::string operation) {
    return {code, std::move(operation)};
}

app::AccountRecord account(
    const std::uint16_t level = 10,
    const app::AccountState state = app::AccountState::active) {
    app::AccountRecord value;
    value.account_id = 42;
    value.display_name = "Caller Name";
    value.name_key = "caller name";
    value.security_level = level;
    value.state = state;
    value.revision = 7;
    value.created_utc_seconds = 1'700'000'000;
    value.modified_utc_seconds = 1'700'000'000;
    value.statistics.successful_login_count = 12;
    value.statistics.authenticated_session_seconds = 3456;
    return value;
}

app::SecurityPolicyRecord security_policy(const std::uint16_t threshold = 999) {
    return {1, 10, threshold, std::string(64, 'a'), 1, std::string(64, 'a')};
}

app::Phase3aConfigurationSnapshot configuration(
    const bool bootstrap = true,
    const std::uint16_t threshold = 999,
    const bool real_names_required = false,
    const bool allow_new_users = true,
    const std::uint16_t additional_level = 0) {
    app::Phase3aConfigurationValues values;
    values.foundation.logical_nodes = 128;
    if (threshold == 999) {
        values.foundation.security = core::SecurityPolicy::fresh_install();
    } else {
        const auto level10 = *core::SecurityLevel::create(10);
        const auto threshold_level = *core::SecurityLevel::create(threshold);
        const auto level999 = *core::SecurityLevel::create(999);
        std::vector<core::SecurityLevel> levels{level10, threshold_level, level999};
        if (additional_level && additional_level != 10 && additional_level != threshold && additional_level != 999)
            levels.push_back(*core::SecurityLevel::create(additional_level));
        values.foundation.security = *core::SecurityPolicy::create(
            std::move(levels), level10, threshold_level);
    }
    values.bootstrap_complete = bootstrap;
    values.allow_new_users = allow_new_users;
    values.real_names_required = real_names_required;
    values.collect_optional_real_name = !real_names_required;
    values.security_policy.revision = 1;
    values.security_policy.policy_hash = std::string(64, 'a');
    values.security_policy.new_user_security_level = 10;
    values.security_policy.sysop_security_threshold = threshold;
    values.connection_limits.maximum_logical_nodes = 128;
    values.prelogin_identity = {"Test BBS", "Test Sysop", "0.1-test"};
    auto result = app::Phase3aConfigurationSnapshot::create(std::move(values));
    if (!result.valid()) throw std::runtime_error("valid test configuration rejected");
    return std::move(*result.snapshot);
}

core::Session session(const core::TransportKind kind = core::TransportKind::telnet) {
    return core::Session("session-shell", *core::NodeNumber::create(1),
        {kind, "192.0.2.7", {}, "ansi"});
}

app::CallerNameResolution existing_resolution(
    std::string normalized = "caller name",
    const std::uint16_t level = 10) {
    return {app::CallerNameClassification::existing, std::move(normalized),
        42, "Caller Name", level};
}

app::CallerNameResolution registerable_resolution(std::string normalized) {
    return {app::CallerNameClassification::registerable,
        std::move(normalized), {}, {}, {}};
}

app::CallerNameResolution unavailable_resolution(std::string normalized) {
    return {app::CallerNameClassification::unavailable,
        std::move(normalized), {}, {}, {}};
}

struct Fixture {
    test::ScriptedTerminalFake terminal;
    test::FakeCallerNameClassificationPort names;
    test::FakePhase3aCredentialOperations credentials;
    test::FakeAccountStorage accounts;
    test::FakeAuthenticationStorage authentication_storage;
    test::FakePhase3aStorage phase3a_storage;
    test::FakeAccountSessionAdmissionPort admission;
    test::FakeAuthenticatedSessionCommitPort session_commit;
    test::FakeTrustedDisplaySelectionPort displays;
    test::FakePhase3aRateControlPort rates;
    test::FixedBbsLocalTimestampFormatter local_time;
    test::FakeCallerWelcomeStatisticsPort welcome_statistics;
    test::FakeClock clock;
    test::DeterministicIdentifierSource identifiers{1};
    std::shared_ptr<app::CallerCleanupHealth> cleanup_health = std::make_shared<app::CallerCleanupHealth>();
    const app::NewCallerNames* new_names{nullptr};
    app::ICallerStageObserver* observer{nullptr};
    app::CallerPasswordObserver caller_password;

    Fixture() {
        accounts.account_result = stored(account());
        accounts.policy_result = stored(security_policy());
        accounts.mutation_result.status = app::StorageStatus::success();
        authentication_storage.status = app::StorageStatus::success();
        authentication_storage.temporary_result = {
            failed_storage(app::StorageErrorCode::not_found, "temporary password"), {}};
        phase3a_storage.status_result = app::StorageStatus::success();
        phase3a_storage.dynamic_claim_result = true;
        session_commit.result.status = app::StorageStatus::success();
        rates.status = app::StorageStatus::success();
    }

    app::Phase3aExistingCallerWorkflow workflow() {
        return app::Phase3aExistingCallerWorkflow({terminal, names, credentials,
            accounts, authentication_storage, phase3a_storage, admission,
            session_commit, displays, rates, local_time, welcome_statistics,
            clock, identifiers, observer, cleanup_health, new_names,caller_password});
    }
};

void script_prelogin(
    Fixture& fixture,
    const bool ansi = true,
    const bool custom = false,
    const bool welcome = false) {
    fixture.terminal.display_request_hook=[ansi](const app::TerminalDisplayPresentationRequest& request) {
        if(request.identity!="prelogin.identification") return;
        std::string bytes;
        for(const auto& op:request.program->operations) bytes+=op.kind==app::TerminalDisplayOperationKind::logical_newline?"\r\n":op.bytes;
        const std::string square(1,static_cast<char>(0xfe));
        const std::string expected=ansi ? "\r\n\x1b[0;1;30;40m"+square+"\x1b[0;37;40m"+square+"\x1b[0;1;37;40m"+square+
            "\x1b[0;37;40m Identification required \x1b[0;1;37;40m"+square+"\x1b[0;37;40m"+square+"\x1b[0;1;30;40m"+square+"\x1b[0;37;40m\r\n\r\n"
            : "\r\nIdentification required:\r\n\r\n";
        require(bytes==expected,"exact post-ANSI heading glyphs, fixed colors and blank rows");
    };
    if (custom) {
        fixture.displays.results.push_back({app::TrustedDisplaySelectionCode::available,
            test::trusted_display_program(app::TrustedDisplayIdentity::sfprelog,
                app::GraphicsMode::ascii, "Custom prelogin\n")});
        fixture.terminal.script_presentation("prelogin.sfprelog");
    } else {
        fixture.displays.results.push_back(
            {app::TrustedDisplaySelectionCode::not_found, {}});
    }
    fixture.terminal.script_presentation("prelogin.builtin");
    fixture.terminal.script_key("prelogin.graphics",
        ansi ? app::TerminalOutcome::empty : app::TerminalOutcome::completed,
        ansi ? std::nullopt : std::optional<char>('n'));
    if (welcome) {
        const auto graphics = ansi ? app::GraphicsMode::ansi : app::GraphicsMode::ascii;
        fixture.displays.results.push_back({app::TrustedDisplaySelectionCode::available,
            test::trusted_display_program(app::TrustedDisplayIdentity::welcome1,
                graphics, "Welcome display\n")});
    } else {
        fixture.displays.results.push_back(
            {app::TrustedDisplaySelectionCode::invalid, {}});
    }
}

void script_name(Fixture& fixture, std::string first, std::string last, bool searched = true) {
    searched = searched && app::normalize_account_name(first + (last.empty() ? "" : " " + last)).has_value();
    fixture.terminal.script_visible(
        "caller.name.first", app::TerminalOutcome::completed, std::move(first));
    fixture.terminal.script_visible(
        "caller.name.last", app::TerminalOutcome::completed, std::move(last));
    if (searched) fixture.terminal.script_presentation("caller.name.searching");
}

void script_success_tail(Fixture& fixture, const std::size_t notices = 0) {
    fixture.terminal.script_presentation("caller.greeting");
    fixture.terminal.script_presentation("caller.information");
    for (std::size_t index = 0; index < notices; ++index) {
        fixture.terminal.script_presentation("caller.failed_login_notice");
    }
    fixture.terminal.script_presentation("caller.continue.prompt");
    fixture.terminal.script_pause("caller.continue");
}

app::Phase3aWorkflowResult run(
    Fixture& fixture,
    const app::Phase3aConfigurationSnapshot& config,
    core::Session& active_session, std::string source_ip = "192.0.2.7") {
    app::CancellationSource cancellation;
    auto workflow = fixture.workflow();
    return workflow.run(config, active_session,
        {std::move(source_ip), identifier(900), std::nullopt}, cancellation.token());
}

void test_current_login_address_information() {
    std::string previous;
    for (const auto& address : std::vector<std::string>{"198.51.100.41", "203.0.113.19", "127.0.0.1", "2001:db8::7"}) {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        script_success_tail(fixture);
        auto active = session();
        const auto result = run(fixture, configuration(), active, address);
        const auto& observations = fixture.terminal.observations();
        const auto greeting = std::find_if(observations.begin(), observations.end(),
            [](const auto& value) { return value.identity == "caller.greeting"; });
        const auto information = std::find_if(observations.begin(), observations.end(),
            [](const auto& value) { return value.identity == "caller.information"; });
        require(result.caller_ready() && greeting != observations.end() &&
            information != observations.end() && information == greeting + 1 &&
            information->non_secret_text.rfind("Your IP address is: " + address + "\n", 0) == 0,
            "current canonical connection IP is first information line immediately after greeting");
        require(information->non_secret_text.find("Your IP address is:", 1) == std::string::npos &&
            (previous.empty() || information->non_secret_text.find(previous) == std::string::npos),
            "distinct sessions display one current IP without prior-caller address leakage");
        previous = address;
    }
}

void test_prelogin_name_budget_and_registration_continuations() {
    {
        Fixture fixture;
        script_prelogin(fixture, true, true, true);
        script_name(fixture, std::string(14, 'A'), std::string(14, 'B'));
        const std::string complete = std::string(14, 'A') + " " + std::string(14, 'B');
        fixture.names.results.push_back(stored(registerable_resolution(complete)));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'N');
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::registration_requested &&
            result.registration.has_value() &&
            result.registration->normalized_name == complete &&
            result.registration->graphics == app::GraphicsMode::ansi,
            "29-character name continues to typed ANSI registration state");
        require(active.state() == core::SessionState::establishing,
            "registration continuation retains establishing shell");
        require(fixture.displays.requests.size() == 1 &&
            fixture.displays.requests[0].identity == app::TrustedDisplayIdentity::sfprelog &&
            fixture.displays.requests[0].graphics == app::GraphicsMode::ascii,
            "only prelog selected before authentication, never welcomes");
        require(fixture.terminal.clean(), "optional presentation script consumed");
    }
    {
        Fixture fixture;
        script_prelogin(fixture, false);
        script_name(fixture, std::string(14, 'C'), std::string(15, 'D'));
        const std::string complete = std::string(14, 'C') + " " + std::string(15, 'D');
        fixture.names.results.push_back(stored(registerable_resolution(complete)));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'n');
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.registration.has_value() &&
            result.registration->normalized_name == complete &&
            result.registration->graphics == app::GraphicsMode::ascii &&
            complete.size() == 30,
            "30-character complete name retains typed ASCII state without truncation");
        require(fixture.displays.requests.size()==1,
            "explicit N does not show welcomes before authentication");
    }
    {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, std::string(14, 'E'), std::string(16, 'F'));
        fixture.terminal.script_presentation("caller.name.invalid");
        script_name(fixture, std::string(14, 'E'), std::string(15, 'F'));
        const std::string accepted = std::string(14, 'E') + " " + std::string(15, 'F');
        fixture.names.results.push_back(stored(registerable_resolution(accepted)));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'N');
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.registration.has_value() &&
            result.registration->normalized_name == accepted &&
            fixture.names.names.size() == 1,
            "31-character composition is rejected and never classified or truncated");
    }
    {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "Retry", "Caller");
        fixture.names.results.push_back(stored(registerable_resolution("Retry Caller")));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'R');
        script_name(fixture, "New", "Caller");
        fixture.names.results.push_back(stored(registerable_resolution("New Caller")));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'N');
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.registration.has_value() &&
            result.registration->normalized_name == "New Caller" &&
            fixture.names.names.size() == 2,
            "Retry returns to name entry and New returns typed continuation");
    }
    {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "Good", "Bye");
        fixture.names.results.push_back(stored(registerable_resolution("Good Bye")));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'G');
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::caller_goodbye &&
            active.state() == core::SessionState::disconnected,
            "Goodbye terminates caller path");
    }
}

void test_blank_and_substantive_name_guards() {
    {
        Fixture fixture;
        script_prelogin(fixture);
        for (int index = 0; index < 6; ++index)
            fixture.terminal.script_visible("caller.name.first",
                app::TerminalOutcome::completed, "");
        fixture.terminal.script_presentation("caller.name.blank.disconnect");
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::caller_goodbye &&
                active.state() == core::SessionState::disconnected &&
                fixture.names.names.empty() && fixture.credentials.observations.empty() &&
                fixture.session_commit.requests.empty(),
            "five blank names reprompt and the independent sixth blank disconnects cleanly without a record");
        const auto disconnected = std::find_if(fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "caller.name.blank.disconnect";
            });
        require(disconnected != fixture.terminal.observations().end() &&
                disconnected->non_secret_text ==
                    "No caller name was entered. Disconnecting.\n",
            "sixth blank uses the short neutral disconnect message");
    }
    {
        Fixture fixture;
        script_prelogin(fixture);
        for (int attempt = 0; attempt < 3; ++attempt) {
            script_name(fixture, "New", std::to_string(attempt + 1));
            const std::string name = "New " + std::to_string(attempt + 1);
            fixture.names.results.push_back(stored(registerable_resolution(name)));
            fixture.terminal.script_presentation("caller.name.available");
            fixture.terminal.script_key("caller.name.available.action",
                app::TerminalOutcome::completed, 'R');
            if (attempt == 1)
                fixture.terminal.script_presentation("caller.name.one_more_try");
            if (attempt == 2)
                fixture.terminal.script_presentation("caller.name.attempts.disconnect");
        }
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::caller_goodbye &&
                fixture.names.names.size() == 3 &&
                fixture.session_commit.requests.empty(),
            "three substantive re-entry choices exhaust only the name guard and leave no partial record");
        require(std::count_if(fixture.terminal.observations().begin(),
                    fixture.terminal.observations().end(), [](const auto& observation) {
                        return observation.identity == "caller.name.available.action" &&
                            observation.non_secret_text ==
                                "<R>e-Enter Name, <N>ew Caller, <G>oodbye? ";
                    }) == 3 &&
                std::count_if(fixture.terminal.observations().begin(),
                    fixture.terminal.observations().end(), [](const auto& observation) {
                        return observation.identity == "caller.name.one_more_try" &&
                            observation.non_secret_text == "One more try.\n";
                    }) == 1,
            "not-found choice text is exact and warns once before the third substantive attempt");
    }
    {
        Fixture fixture;
        script_prelogin(fixture);
        for (int attempt = 0; attempt < 3; ++attempt) {
            script_name(fixture, "Accepted", std::to_string(attempt + 1));
            const std::string name = "Accepted " + std::to_string(attempt + 1);
            fixture.names.results.push_back(stored(registerable_resolution(name)));
            fixture.terminal.script_presentation("caller.name.available");
            fixture.terminal.script_key("caller.name.available.action",
                app::TerminalOutcome::completed, attempt == 2 ? 'N' : 'R');
            if (attempt == 1)
                fixture.terminal.script_presentation("caller.name.one_more_try");
        }
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::registration_requested &&
                result.registration &&
                result.registration->normalized_name == "Accepted 3" &&
                active.state() == core::SessionState::establishing,
            "a valid available new-caller name remains admissible on substantive attempt three");
    }
}

void test_exact_complete_name_shortcut() {
    for(const std::string name:{"xbit","xbit ops"}) {
        Fixture fixture;
        auto saved=account();saved.display_name=name;saved.name_key=name;
        fixture.accounts.account_result=stored(saved);
        script_prelogin(fixture);
        auto entered=name;std::transform(entered.begin(),entered.end(),entered.begin(),
            [](unsigned char c){return static_cast<char>(std::toupper(c));});
        fixture.terminal.script_visible("caller.name.first",app::TerminalOutcome::completed,entered);
        fixture.terminal.script_presentation("caller.name.searching");
        auto resolution=existing_resolution(entered);resolution.saved_display_name=name;
        fixture.names.results.push_back(stored(resolution));
        fixture.terminal.script_secret_completed("authentication.password",12);
        fixture.credentials.existing_results.push_back({app::AuthenticationOperationCode::completed,true,false,false,false});
        script_success_tail(fixture);
        auto active=session();const auto result=run(fixture,configuration(),active);
        require(result.caller_ready() && fixture.terminal.clean(),"exact complete name skips surname and still authenticates normally: "+name+" code="+std::to_string(static_cast<int>(result.code))+" clean="+std::to_string(fixture.terminal.clean()));
        require(std::none_of(fixture.terminal.observations().begin(),fixture.terminal.observations().end(),
            [](const auto& o){return o.identity=="caller.name.last";}),"no surname prompt after exact case-insensitive full-name match");
    }
}
void test_connection_password_handoff(){
    Fixture fixture;unsigned captures{};
    const auto proof=app::make_argon2id_credential_envelope({},std::vector<std::uint8_t>(16,1),std::vector<std::uint8_t>(32,2),100,false);
    fixture.caller_password=[&](app::AccountId id,const app::CredentialEnvelope& credential,std::string_view password){
        ++captures;require(id==42&&credential.verifier==proof.verifier&&password.size()==12,"verified connection handoff carries exact account/proof and bounded secret");};
    script_prelogin(fixture);script_name(fixture,"Caller","Name");fixture.names.results.push_back(stored(existing_resolution("Caller Name")));
    fixture.terminal.script_secret_completed("authentication.password",12);
    app::PasswordAuthenticationResult verified{app::AuthenticationOperationCode::completed,true,false,false,false};verified.verified_credential=proof;
    fixture.credentials.existing_results.push_back(verified);script_success_tail(fixture);
    auto active=session();auto result=run(fixture,configuration(),active);
    require(result.caller_ready()&&captures==1&&fixture.terminal.clean(),"successful interactive login publishes one password handoff without another prompt");
}

void test_welcome_after_authenticated_information() {
    Fixture fixture;
    script_prelogin(fixture,true,false,true);
    script_name(fixture,"Caller","Name");
    fixture.names.results.push_back(stored(existing_resolution("Caller Name")));
    fixture.terminal.script_secret_completed("authentication.password",12);
    fixture.credentials.existing_results.push_back({app::AuthenticationOperationCode::completed,true,false,false,false});
    script_success_tail(fixture);
    fixture.terminal.script_presentation("caller.welcomes");
    auto active=session();
    fixture.terminal.display_request_hook=[&](const auto& request) {
        if(request.identity=="prelogin.identification") return;
        require(request.identity=="caller.welcomes" && active.state()==core::SessionState::authenticated,
            "welcome artwork is never presented before authentication");
        const auto& seen=fixture.terminal.observations();
        require(!seen.empty() && seen.back().identity=="caller.continue",
            "welcome follows the information continuation pause");
    };
    const auto result=run(fixture,configuration(),active);
    require(result.caller_ready() && fixture.terminal.clean(),"authenticated welcomes precede menu handoff");
}

void test_ordinary_success_and_ready_handoff() {
    Fixture fixture;
    script_prelogin(fixture);
    script_name(fixture, "caller", "name");
    fixture.names.results.push_back(stored(existing_resolution()));
    fixture.terminal.script_secret_completed("authentication.password", 12);
    fixture.credentials.existing_results.push_back(
        {app::AuthenticationOperationCode::completed, true, false, false, false});
    script_success_tail(fixture);

    auto active = session();
    auto result = run(fixture, configuration(), active);
    require(result.caller_ready() && active.state() == core::SessionState::authenticated,
        "ordinary caller reaches authenticated ready boundary");
    require(active.caller().has_value() && active.caller()->account_id == 42 &&
        active.caller()->display_name == "Caller Name" &&
        active.caller()->caller_identity == "caller name",
        "stable identity and saved display casing enter core session");
    require(fixture.session_commit.requests.size() == 1 &&
        fixture.session_commit.requests[0].successful_source.source_ip == "192.0.2.7" &&
        fixture.session_commit.requests[0].successful_source.protocol ==
            app::AuthenticationProtocol::telnet,
        "durable session and canonical successful source commit together");
    require(fixture.admission.requests.size() == 1 &&
        fixture.admission.requests[0].maximum_sessions_per_account == 1 &&
        fixture.admission.releases.empty(),
        "immutable account limit controls admission and successful lease transfers");
    require(fixture.rates.observations.size() == 2 &&
        fixture.rates.observations[0].operation == "await" &&
        fixture.rates.observations[0].scopes.account_id == 42 &&
        fixture.rates.observations[0].scopes.source &&
        fixture.rates.observations[0].scopes.global &&
        fixture.rates.observations[1].operation == "success" &&
        fixture.rates.observations[1].scopes.account_id == 42 &&
        !fixture.rates.observations[1].scopes.source &&
        !fixture.rates.observations[1].scopes.global,
        "ordinary admission uses account/source/global and success clears account only");
    auto handoff = std::move(*result.ready);
    auto delivered = handoff.take();
    require(delivered.has_value() && delivered->graphics == app::GraphicsMode::ansi &&
        delivered->display_name == "Caller Name" && !handoff.take().has_value(),
        "ready handoff is validated and single consumption");
    require(delivered->admission_lease.has_value() &&
        delivered->admission_lease->active() &&
        delivered->admission_lease->lease()->account_id == delivered->account_id &&
        delivered->admission_lease->lease()->intended_session_id ==
            delivered->durable_session_id,
        "successful handoff transfers matching admission-lease ownership");
    require(delivered->admission_lease->release() &&
        !delivered->admission_lease->release() &&
        fixture.admission.releases.size() == 1,
        "session owner releases transferred lease exactly once");
    require(fixture.terminal.retained_secret_bytes() == 0 && fixture.terminal.clean(),
        "successful workflow retains no terminal secret bytes");
    require(std::all_of(fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.non_secret_text.find(std::string(12, 'X')) ==
                    std::string::npos;
            }),
        "candidate secret never reaches captured output");
}

void test_unavailable_dummy_and_three_attempt_boundary() {
    Fixture fixture;
    script_prelogin(fixture);
    for (int attempt = 0; attempt < 3; ++attempt) {
        script_name(fixture, "Reserved", "Name");
        fixture.names.results.push_back(stored(unavailable_resolution("Reserved Name")));
        fixture.terminal.script_secret_completed("authentication.password", 9);
        fixture.credentials.dummy_results.push_back(
            {app::AuthenticationOperationCode::rejected, false, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        if (attempt == 1) {
            fixture.terminal.script_presentation(
                "authentication.final_attempt_warning");
        }
    }
    auto active = session();
    auto result = run(fixture, configuration(), active);
    require(result.code == app::Phase3aWorkflowCode::authentication_denied &&
        active.state() == core::SessionState::disconnected,
        "third unavailable-name attempt disconnects");
    require(fixture.credentials.observations.size() == 3 &&
        std::all_of(fixture.credentials.observations.begin(),
            fixture.credentials.observations.end(), [](const auto& value) {
                return value.operation == "verify_dummy" && value.candidate_length == 9;
            }),
        "unavailable names receive bounded dummy verification");
    require(fixture.phase3a_storage.enqueue_requests.empty(),
        "unknown or reserved target never receives a notice");
    require(std::count_if(fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "authentication.final_attempt_warning" &&
                    observation.non_secret_text ==
                        "Warning: this is your final login attempt.\n";
            }) == 1,
        "exactly one generic final-attempt warning follows the second failure");
    require(std::count_if(fixture.rates.observations.begin(),
            fixture.rates.observations.end(), [](const auto& value) {
                return value.operation == "failure" &&
                    !value.scopes.account_id.has_value() && value.scopes.source &&
                    value.scopes.global && !value.scopes.system_password;
            }) == 3,
        "dummy failures record source/global scopes without invented account scope");
}

void test_generic_failures_and_notice_enqueue() {
    const std::vector<app::AuthenticationOperationCode> generic_codes{
        app::AuthenticationOperationCode::rejected,
        app::AuthenticationOperationCode::malformed_record,
        app::AuthenticationOperationCode::unsupported_record};
    for (const auto code : generic_codes) {
        Fixture fixture;
        script_prelogin(fixture);
        for (int attempt = 0; attempt < 3; ++attempt) {
            script_name(fixture, "caller", "name");
            fixture.names.results.push_back(stored(existing_resolution()));
            fixture.terminal.script_secret_completed("authentication.password", 11);
            fixture.credentials.existing_results.push_back(
                {code, false, false, false, false});
            fixture.terminal.script_presentation("authentication.failure");
            if (attempt == 1) {
                fixture.terminal.script_presentation(
                    "authentication.final_attempt_warning");
            }
        }
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::authentication_denied,
            "generic verifier failure reaches same denied result");
        require(std::count_if(fixture.terminal.observations().begin(),
                fixture.terminal.observations().end(), [](const auto& observation) {
                    return observation.identity == "authentication.failure" &&
                        observation.non_secret_text == "Login unsuccessful.\n";
                }) == 3,
            "verifier classifications share exact generic caller text");
        const std::size_t expected_notices =
            code == app::AuthenticationOperationCode::rejected ? 3U : 0U;
        require(fixture.phase3a_storage.enqueue_requests.size() == expected_notices,
            "only actual rejected ordinary credentials enqueue notices");
        require(std::all_of(fixture.phase3a_storage.enqueue_requests.begin(),
                fixture.phase3a_storage.enqueue_requests.end(), [](const auto& notice) {
                    return notice.account_id == 42 && notice.source_ip == "192.0.2.7" &&
                        notice.notice_id.valid();
                }),
            "known-account notice contains only stable target/source/time identity");
    }

    for (const auto state : {app::AccountState::locked,
             app::AccountState::deletion_scheduled,
             app::AccountState::tombstoned}) {
        Fixture fixture;
        fixture.accounts.account_result = stored(account(10, state));
        script_prelogin(fixture);
        for (int attempt = 0; attempt < 3; ++attempt) {
            script_name(fixture, "caller", "name");
            fixture.names.results.push_back(stored(existing_resolution()));
            fixture.terminal.script_secret_completed("authentication.password", 10);
            fixture.credentials.dummy_results.push_back(
                {app::AuthenticationOperationCode::rejected, false, false, false, false});
            fixture.terminal.script_presentation("authentication.failure");
            if (attempt == 1) {
                fixture.terminal.script_presentation(
                    "authentication.final_attempt_warning");
            }
        }
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::authentication_denied &&
            fixture.credentials.observations.size() == 3,
            "ineligible account state uses dummy verification and generic denial");
        require(fixture.phase3a_storage.enqueue_requests.empty(),
            "ineligible state does not create a false wrong-password notice");
    }

    const std::vector<std::pair<app::AuthenticationOperationCode,
        app::Phase3aWorkflowCode>> typed{
        {app::AuthenticationOperationCode::backpressure,
            app::Phase3aWorkflowCode::kdf_backpressure},
        {app::AuthenticationOperationCode::storage_failure,
            app::Phase3aWorkflowCode::storage_failure},
        {app::AuthenticationOperationCode::throttled,
            app::Phase3aWorkflowCode::temporarily_unavailable},
        {app::AuthenticationOperationCode::unavailable,
            app::Phase3aWorkflowCode::temporarily_unavailable}};
    for (const auto& item : typed) {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {item.first, false, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == item.second,
            "KDF/storage/throttle failure retains typed internal outcome");
        require(std::count_if(fixture.terminal.observations().begin(),
                fixture.terminal.observations().end(), [](const auto& observation) {
                    return observation.identity == "authentication.failure";
                }) == 1,
            "typed internal failure still presents generic caller text");
    }
    {
        Fixture fixture;
        fixture.rates.status = failed_storage(
            app::StorageErrorCode::io, "record failure");
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::rejected, false, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.phase3a_storage.enqueue_requests.empty(),
            "failed durable rate update fails closed with generic caller text");
    }
    {
        Fixture fixture;
        fixture.phase3a_storage.status_result = failed_storage(
            app::StorageErrorCode::full, "notice enqueue");
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::rejected, false, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.phase3a_storage.enqueue_requests.size() == 1,
            "failed notice enqueue fails closed without disclosing its classification");
    }
}

void test_terminal_stop_outcomes_and_bootstrap() {
    const std::vector<std::pair<app::TerminalOutcome, app::Phase3aWorkflowCode>> outcomes{
        {app::TerminalOutcome::cancelled, app::Phase3aWorkflowCode::cancelled},
        {app::TerminalOutcome::timed_out, app::Phase3aWorkflowCode::timed_out},
        {app::TerminalOutcome::disconnected, app::Phase3aWorkflowCode::disconnected},
        {app::TerminalOutcome::shutdown_requested,
            app::Phase3aWorkflowCode::shutdown_requested},
        {app::TerminalOutcome::invalid, app::Phase3aWorkflowCode::internal_failure},
        {app::TerminalOutcome::overlength, app::Phase3aWorkflowCode::internal_failure}};
    for (const auto& item : outcomes) {
        Fixture fixture;
        fixture.displays.results.push_back(
            {app::TrustedDisplaySelectionCode::not_found, {}});
        fixture.terminal.script_presentation("prelogin.builtin");
        fixture.terminal.script_key("prelogin.graphics", item.first);
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == item.second &&
            active.state() == core::SessionState::disconnected,
            "graphics terminal stop/failure maps to closed typed result");
    }
    {
        Fixture fixture;
        fixture.terminal.script_presentation(
            "prelogin.builtin", app::TerminalOutcome::output_failure);
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::terminal_failure,
            "required built-in output failure is typed");
    }
    {
        Fixture fixture;
        auto active = session();
        auto result = run(fixture, configuration(false), active);
        require(result.code == app::Phase3aWorkflowCode::bootstrap_incomplete &&
            fixture.terminal.observations().empty() && fixture.names.names.empty(),
            "incomplete bootstrap fails closed before caller activity");
    }
    {
        Fixture fixture;
        script_prelogin(fixture);
        fixture.terminal.script_visible(
            "caller.name.first", app::TerminalOutcome::cancelled);
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::cancelled,
            "name-entry cancellation propagates without input");
    }
}

void test_temporary_password_replacement() {
    Fixture fixture;
    script_prelogin(fixture);
    script_name(fixture, "caller", "name");
    fixture.names.results.push_back(stored(existing_resolution()));
    fixture.terminal.script_secret_completed("authentication.password", 9);
    fixture.credentials.existing_results.push_back(
        {app::AuthenticationOperationCode::password_change_required,
            true, true, false, false});
    fixture.authentication_storage.temporary_result = stored(
        app::TemporaryPasswordRecord{42, 3, app::TemporaryPasswordState::active,
            1'699'999'000, 1'700'100'000, {}});

    fixture.terminal.script_secret_completed("authentication.replacement", 8);
    fixture.terminal.script_secret_completed("authentication.replacement.confirm", 7);
    fixture.terminal.script_presentation("authentication.replacement.mismatch");
    fixture.terminal.script_secret_completed("authentication.replacement", 8);
    fixture.terminal.script_secret_completed("authentication.replacement.confirm", 8);
    app::PasswordMutationResult policy;
    policy.code = app::AuthenticationOperationCode::policy_rejected;
    policy.policy_code = app::PasswordPolicyCode::too_short;
    fixture.credentials.replacement_results.push_back(policy);
    fixture.terminal.script_presentation("authentication.replacement.policy");
    fixture.terminal.script_secret_completed("authentication.replacement", 12);
    fixture.terminal.script_secret_completed("authentication.replacement.confirm", 12);
    app::PasswordMutationResult replaced;
    replaced.code = app::AuthenticationOperationCode::completed;
    replaced.account_revision = 8;
    replaced.credential_committed = true;
    replaced.revocation_completed = true;
    fixture.credentials.replacement_results.push_back(replaced);
    script_success_tail(fixture);

    auto active = session();
    auto result = run(fixture, configuration(), active);
    require(result.caller_ready(),
        "temporary credential requires and completes permanent replacement");
    require(std::count_if(fixture.credentials.observations.begin(),
            fixture.credentials.observations.end(), [](const auto& observation) {
                return observation.operation == "replace_temporary";
            }) == 2,
        "mismatch is rejected before mutation and policy rejection retries");
    require(fixture.credentials.observations.back().candidate_length == 9 &&
        fixture.credentials.observations.back().replacement_length == 12,
        "replacement receives only ephemeral expected secret lengths");
    require(fixture.terminal.hidden_input_requests() == 7 &&
        fixture.terminal.retained_secret_bytes() == 0,
        "temporary and replacement secrets are hidden and never retained");

    const std::vector<std::pair<app::TerminalOutcome, app::Phase3aWorkflowCode>>
        replacement_stops{
            {app::TerminalOutcome::cancelled, app::Phase3aWorkflowCode::cancelled},
            {app::TerminalOutcome::timed_out, app::Phase3aWorkflowCode::timed_out},
            {app::TerminalOutcome::disconnected,
                app::Phase3aWorkflowCode::disconnected},
            {app::TerminalOutcome::shutdown_requested,
                app::Phase3aWorkflowCode::shutdown_requested}};
    for (const auto& stop : replacement_stops) {
        Fixture aborted;
        script_prelogin(aborted);
        script_name(aborted, "caller", "name");
        aborted.names.results.push_back(stored(existing_resolution()));
        aborted.terminal.script_secret_completed("authentication.password", 9);
        aborted.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::password_change_required,
                true, true, false, false});
        aborted.authentication_storage.temporary_result =
            fixture.authentication_storage.temporary_result;
        aborted.terminal.script_secret_completed("authentication.replacement", 12);
        aborted.terminal.script_secret_outcome(
            "authentication.replacement.confirm", stop.first);
        auto aborted_session = session();
        auto aborted_result = run(aborted, configuration(), aborted_session);
        require(aborted_result.code == stop.second &&
            aborted.session_commit.requests.empty() &&
            aborted.terminal.retained_secret_bytes() == 0,
            "each replacement abort creates no durable/in-memory session or retained secret");
    }
    {
        Fixture missing_reset;
        script_prelogin(missing_reset);
        script_name(missing_reset, "caller", "name");
        missing_reset.names.results.push_back(stored(existing_resolution()));
        missing_reset.terminal.script_secret_completed("authentication.password", 9);
        missing_reset.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::password_change_required,
                true, true, false, false});
        missing_reset.terminal.script_presentation("authentication.failure");
        auto missing_session = session();
        auto missing_result = run(missing_reset, configuration(), missing_session);
        require(missing_result.code == app::Phase3aWorkflowCode::storage_failure &&
            missing_reset.session_commit.requests.empty(),
            "missing temporary-reset state fails closed with generic caller text");
    }
    {
        Fixture pressured;
        script_prelogin(pressured);
        script_name(pressured, "caller", "name");
        pressured.names.results.push_back(stored(existing_resolution()));
        pressured.terminal.script_secret_completed("authentication.password", 9);
        pressured.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::password_change_required,
                true, true, false, false});
        pressured.authentication_storage.temporary_result = fixture.authentication_storage.temporary_result;
        pressured.terminal.script_secret_completed("authentication.replacement", 12);
        pressured.terminal.script_secret_completed(
            "authentication.replacement.confirm", 12);
        app::PasswordMutationResult backpressure;
        backpressure.code = app::AuthenticationOperationCode::backpressure;
        pressured.credentials.replacement_results.push_back(backpressure);
        pressured.terminal.script_presentation("authentication.failure");
        auto pressured_session = session();
        auto pressured_result = run(pressured, configuration(), pressured_session);
        require(pressured_result.code == app::Phase3aWorkflowCode::kdf_backpressure &&
            pressured.session_commit.requests.empty(),
            "replacement KDF pressure remains typed and fail closed");
    }
}

void test_sysop_threshold_ordering_and_failed_restart() {
    for (const auto& entry : std::vector<std::pair<std::uint16_t, std::uint16_t>>{
             {std::uint16_t{999}, std::uint16_t{999}},
             {std::uint16_t{500}, std::uint16_t{500}},
             {std::uint16_t{500}, std::uint16_t{750}},
             {std::uint16_t{500}, std::uint16_t{999}}}) {
        Fixture fixture;
        fixture.accounts.account_result = stored(account(entry.second));
        fixture.accounts.policy_result = stored(security_policy(entry.first));
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution(
            "caller name", entry.second)));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.terminal.script_secret_completed(
            "authentication.system_password", 14);
        fixture.credentials.system_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        script_success_tail(fixture);
        auto active = session(core::TransportKind::ssh);
        auto result = run(fixture, configuration(true, entry.first, false, true, entry.second), active);
        require(result.caller_ready(), "equal/above threshold requires successful SY sequence: threshold=" +
            std::to_string(entry.first) + " level=" + std::to_string(entry.second) +
            " outcome=" + std::to_string(static_cast<unsigned>(result.code)));
        require(fixture.admission.requests.size() == 1 &&
            fixture.admission.requests[0].maximum_sessions_per_account == (entry.second==999?128:1),
            "999-only concurrency uses node capacity; lower threshold-qualified Sysops retain one session");
        require(fixture.credentials.observations.size() == 2 &&
            fixture.credentials.observations[0].operation == "verify_existing" &&
            fixture.credentials.observations[1].operation == "verify_system",
            "ordinary credential always precedes system credential");
        require(std::count(fixture.accounts.calls.begin(),
                    fixture.accounts.calls.end(), "security_policy") == 3 &&
            std::count(fixture.accounts.calls.begin(),
                    fixture.accounts.calls.end(), "find_account_by_id") == 4,
            "successful SY path revalidates policy/account before admission and after commit");
        require(fixture.session_commit.requests[0].successful_source.protocol ==
            app::AuthenticationProtocol::ssh,
            "existing transport metadata determines successful history protocol");
        require(std::count_if(fixture.rates.observations.begin(),
                fixture.rates.observations.end(), [](const auto& observation) {
                    return observation.operation == "success" &&
                        observation.scopes.system_password &&
                        !observation.scopes.account_id.has_value() &&
                        !observation.scopes.source && !observation.scopes.global;
                }) == 1,
            "successful SY clears only the system-password scope");
    }

    Fixture fixture;
    fixture.accounts.account_result = stored(account(999));
    script_prelogin(fixture);
    script_name(fixture, "caller", "name");
    fixture.names.results.push_back(stored(existing_resolution("caller name", 999)));
    fixture.terminal.script_secret_completed("authentication.password", 10);
    fixture.terminal.script_secret_completed(
        "authentication.system_password", 14);
    fixture.terminal.script_presentation("authentication.failure");
    script_name(fixture, "caller", "name");
    fixture.names.results.push_back(stored(existing_resolution("caller name", 999)));
    fixture.terminal.script_secret_completed("authentication.password", 10);
    fixture.terminal.script_secret_completed(
        "authentication.system_password", 14);
    fixture.credentials.existing_results.push_back(
        {app::AuthenticationOperationCode::completed, true, false, false, false});
    fixture.credentials.existing_results.push_back(
        {app::AuthenticationOperationCode::completed, true, false, false, false});
    fixture.credentials.system_results.push_back(
        {app::AuthenticationOperationCode::rejected, false, false, false, false});
    fixture.credentials.system_results.push_back(
        {app::AuthenticationOperationCode::completed, true, false, false, false});
    script_success_tail(fixture);
    auto active = session();
    auto result = run(fixture, configuration(), active);
    require(result.caller_ready() &&
        std::count_if(fixture.credentials.observations.begin(),
            fixture.credentials.observations.end(), [](const auto& observation) {
                return observation.operation == "verify_existing";
            }) == 2,
        "failed SY restarts at name and requires newly verified ordinary password");
    require(fixture.phase3a_storage.enqueue_requests.size() == 1,
        "failed SY enqueues one generic known-account notice");
    const auto sy_failure = std::find_if(fixture.rates.observations.begin(),
        fixture.rates.observations.end(), [](const auto& observation) {
            return observation.operation == "failure" &&
                observation.scopes.system_password;
        });
    require(sy_failure != fixture.rates.observations.end() &&
        !sy_failure->scopes.account_id.has_value() && sy_failure->scopes.source &&
        sy_failure->scopes.global,
        "failed SY records source/global/SY without false account-password scope");
}

void test_notice_delivery_acknowledgement_and_replay_cleanup() {
    {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.phase3a_storage.dynamic_notices = {
            {identifier(501), 42, "198.51.100.10", 1'699'999'900},
            {identifier(502), 42, "2001:db8::20", 1'699'999'800}};
        script_success_tail(fixture, 2);
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.caller_ready() &&
            fixture.phase3a_storage.acknowledge_requests.size() == 1,
            "complete notice presentation acknowledges the claimed batch once");
        const auto& observations = fixture.terminal.observations();
        const auto greeting = std::find_if(observations.begin(), observations.end(),
            [](const auto& value) { return value.identity == "caller.greeting"; });
        const auto information = std::find_if(observations.begin(), observations.end(),
            [](const auto& value) { return value.identity == "caller.information"; });
        const auto notice = std::find_if(observations.begin(), observations.end(),
            [](const auto& value) { return value.identity == "caller.failed_login_notice"; });
        require(greeting < information && information < notice,
            "greeting and authoritative caller information precede notices");
        require(information->non_secret_text.find("There has been 1 call today.\n") !=
                    std::string::npos &&
                information->non_secret_text.find("You have called 12 times.\n") !=
                    std::string::npos &&
                information->non_secret_text.find("You have spent 57 minutes online.\n") !=
                    std::string::npos &&
                information->non_secret_text.find("Your security level is 10.\n\n") !=
                    std::string::npos &&
                information->non_secret_text.find("Caller:") == std::string::npos &&
                information->non_secret_text.find("seconds") == std::string::npos &&
                fixture.welcome_statistics.calls == 1,
            "first-login caller information uses natural grammar and real whole-minute values only");
        const auto continuation = std::find_if(observations.begin(), observations.end(),
            [](const auto& value) {
                return value.identity == "caller.continue.prompt";
            });
        require(greeting->non_secret_text ==
                "\nGood afternoon, Caller Name, welcome to Test BBS.\n" &&
            continuation != observations.end() && notice < continuation &&
            continuation->non_secret_text == "Press <ENTER> to continue.",
            "authentication is separated from the welcome and the exact continuation prompt follows the final notice");
        require(std::count_if(observations.begin(), observations.end(),
                [](const auto& value) {
                    return value.identity == "caller.failed_login_notice" &&
                        value.non_secret_text.find("Failed login attempt") != std::string::npos &&
                        value.non_secret_text.find("password") == std::string::npos &&
                        value.non_secret_text.find("SY:") == std::string::npos;
                }) == 2,
            "notices contain generic text, source, and local time without credential class");
    }
    {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.phase3a_storage.dynamic_notices = {
            {identifier(503), 42, "198.51.100.30", 1'699'999'700},
            {identifier(504), 42, "198.51.100.31", 1'699'999'600}};
        fixture.terminal.script_presentation("caller.greeting");
        fixture.terminal.script_presentation("caller.information");
        fixture.terminal.script_presentation("caller.failed_login_notice");
        fixture.terminal.script_presentation(
            "caller.failed_login_notice", app::TerminalOutcome::output_failure);
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::terminal_failure &&
            fixture.phase3a_storage.acknowledge_requests.empty(),
            "incomplete notice presentation leaves the claim unacknowledged for replay");
        require(fixture.accounts.finalize_requests.size() == 1 &&
            fixture.admission.releases.size() == 1 &&
            active.state() == core::SessionState::disconnected,
            "post-commit failure finalizes once, releases lease, and removes authenticated state");
    }
}

void test_admission_commit_and_cleanup_failures() {
    {
        Fixture fixture;
        fixture.admission.auto_acquire = false;
        fixture.admission.result.code = app::AccountSessionAdmissionCode::account_limit;
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.terminal.script_presentation("authentication.already_online");
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::temporarily_unavailable &&
            fixture.session_commit.requests.empty() && !active.caller().has_value(),
            "account-session limit race cannot start or authenticate a second session");
        const auto& observations = fixture.terminal.observations();
        require(std::any_of(observations.begin(), observations.end(), [](const auto& value) {
            return value.identity == "authentication.already_online" && value.non_secret_text ==
                "You are already online on another node.\n\nGoodbye.\n";
        }), "correct-password duplicate gets explicit notice before disconnect");
    }
    {
        Fixture fixture;
        fixture.admission.auto_acquire = false;
        fixture.admission.result.code = app::AccountSessionAdmissionCode::acquired;
        fixture.admission.result.lease =
            app::AccountSessionAdmissionLease{identifier(700), 99, identifier(701)};
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::internal_failure &&
            fixture.session_commit.requests.empty() &&
            fixture.admission.releases.size() == 1 &&
            !active.caller().has_value(),
            "mismatched admission lease is released once before durable work");
    }
    {
        Fixture fixture;
        fixture.session_commit.result.status =
            failed_storage(app::StorageErrorCode::full, "session commit");
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.admission.releases.size() == 1 &&
            fixture.accounts.finalize_requests.empty() && !active.caller().has_value(),
            "failed atomic commit releases lease without durable/core session");
    }
    {
        Fixture fixture;
        fixture.session_commit.result.replayed = true;
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        script_success_tail(fixture);
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.caller_ready() && fixture.session_commit.requests.size() == 1,
            "idempotent replayed session/source commit reaches one ready handoff");
    }
    {
        Fixture fixture;
        fixture.rates.status = failed_storage(
            app::StorageErrorCode::io, "record success");
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.accounts.finalize_requests.size() == 1 &&
            fixture.admission.releases.size() == 1 &&
            active.state() == core::SessionState::disconnected,
            "failed success-scope persistence finalizes durable session and releases lease");
    }
}

void test_f01_plain_identity_before_graphics() {
    for (const bool ansi : {false, true}) {
        for (unsigned variant = 0; variant != 6; ++variant) {
            Fixture fixture;
            const bool custom = variant == 0;
            const std::string custom_text = ansi ? "Custom prelogin" : "\n\nCustom prelogin\n\n\n";
            if (variant < 2) {
                fixture.displays.results.push_back({app::TrustedDisplaySelectionCode::available,
                    test::trusted_display_program(variant == 0 ? app::TrustedDisplayIdentity::sfprelog :
                        app::TrustedDisplayIdentity::welcome1, app::GraphicsMode::ascii, custom_text)});
            } else if (variant == 2) fixture.displays.results.push_back({app::TrustedDisplaySelectionCode::available, {}});
            else fixture.displays.results.push_back({variant == 3 ? app::TrustedDisplaySelectionCode::not_found :
                variant == 4 ? app::TrustedDisplaySelectionCode::invalid : app::TrustedDisplaySelectionCode::unavailable, {}});
            if(custom) fixture.terminal.script_presentation("prelogin.sfprelog");
            fixture.terminal.script_presentation("prelogin.builtin");
            fixture.terminal.script_key("prelogin.graphics", app::TerminalOutcome::completed, ansi ? 'Y' : 'N');
            fixture.displays.results.push_back({app::TrustedDisplaySelectionCode::not_found, {}});
            script_name(fixture, "Future", "Caller");
            fixture.names.results.push_back(stored(registerable_resolution("Future Caller")));
            fixture.terminal.script_presentation("caller.name.available");
            fixture.terminal.script_key("caller.name.available.action", app::TerminalOutcome::completed, 'G');
            bool checked = false;
            fixture.terminal.display_request_hook = [&](const app::TerminalDisplayPresentationRequest& request) {
                if(request.identity=="prelogin.identification") return;
                std::string forwarded;
                for (const auto& operation : request.program->operations) {
                    if (operation.kind == app::TerminalDisplayOperationKind::cp437_text) forwarded += operation.bytes;
                    else if (operation.kind == app::TerminalDisplayOperationKind::logical_newline) forwarded += '\n';
                }
                require(custom && request.identity == "prelogin.sfprelog" && forwarded ==
                    (ansi ? "Custom prelogin\n\n" : "\n\nCustom prelogin\n\n"),
                    "F01 preserves artwork and normalizes its final boundary to one blank row");
            };
            fixture.terminal.key_request_hook = [&](const app::TerminalKeyRequest& request) {
                if (request.identity != "prelogin.graphics") return;
                checked = true;
                const auto& seen = fixture.terminal.observations();
                require(seen.size() == (custom ? 2U : 1U) && seen.front().identity ==
                    (custom ? "prelogin.sfprelog" : "prelogin.builtin") &&
                    seen.back().identity == "prelogin.builtin" && seen.back().non_secret_text ==
                        "Your Sysop is Test Sysop\n"
                        "SPITFIRE-X Bulletin Board System - Version 0.1-test\n"
                        "In memory of Mike Woltz and Spitfire BBS\n"
                        "Copyright (C) 2026 Spitfire-X BBS Project\n\n",
                    "F01 always presents system identity after optional prelog and before graphics");
                require(fixture.terminal.text_style == app::TerminalTextStyle::plain &&
                    request.prompt == "Do you want ANSI COLOR GRAPHICS? [Y/n] ",
                    "F01 identity and graphics question do not depend on an ANSI choice");
            };
            auto active = session();
            const auto result = run(fixture, configuration(), active);
            require(checked && result.code == app::Phase3aWorkflowCode::caller_goodbye && fixture.terminal.clean(),
                "F01 valid/missing/invalid/unavailable/malformed selection preserves normal entry");
            require(fixture.terminal.text_style == (ansi ? app::TerminalTextStyle::ogsf_ansi : app::TerminalTextStyle::plain),
                "F01 applies only the explicit graphics choice after the question");
            require(fixture.admission.requests.empty() && fixture.session_commit.requests.empty(),
                "F01 presentation grants no account or durable-session authority");
        }
    }
}

void test_corrected_name_and_prelogin_semantics() {
    {
        Fixture fixture;
        script_prelogin(fixture, true, true);
        script_name(fixture, "Custom", "Only");
        fixture.names.results.push_back(stored(registerable_resolution("Custom Only")));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'G');
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::caller_goodbye &&
            std::count_if(fixture.terminal.observations().begin(),
                fixture.terminal.observations().end(), [](const auto& observation) {
                    return observation.identity == "prelogin.sfprelog";
                }) == 1 &&
            std::count_if(fixture.terminal.observations().begin(),
                fixture.terminal.observations().end(), [](const auto& observation) {
                    return observation.identity == "prelogin.builtin";
                }) == 1,
            "valid SFPRELOG supplements the always-present built-in body");
    }

    for (const auto fallback : {app::TrustedDisplaySelectionCode::not_found,
             app::TrustedDisplaySelectionCode::invalid,
             app::TrustedDisplaySelectionCode::unavailable}) {
        Fixture fixture;
        fixture.displays.results.push_back({fallback, {}});
        fixture.terminal.script_presentation("prelogin.builtin");
        fixture.terminal.script_key("prelogin.graphics",
            app::TerminalOutcome::empty);
        fixture.displays.results.push_back(
            {app::TrustedDisplaySelectionCode::not_found, {}});
        script_name(fixture, "Fallback", "Caller");
        fixture.names.results.push_back(
            stored(registerable_resolution("Fallback Caller")));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'G');
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        const auto built_in = std::find_if(fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "prelogin.builtin";
            });
        const auto built_in_count = std::count_if(
            fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "prelogin.builtin";
            });
        require(result.code == app::Phase3aWorkflowCode::caller_goodbye &&
            built_in_count == 1 &&
            built_in != fixture.terminal.observations().end() &&
            built_in->non_secret_text ==
                "Your Sysop is Test Sysop\n"
                "SPITFIRE-X Bulletin Board System - Version 0.1-test\n"
                "In memory of Mike Woltz and Spitfire BBS\n"
                "Copyright (C) 2026 Spitfire-X BBS Project\n\n",
            "absent, invalid, and unavailable SFPRELOG use exactly one owner-approved SFX identity banner");
        const auto graphics = std::find_if(
            fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "prelogin.graphics";
            });
        require(graphics != fixture.terminal.observations().end() && built_in < graphics &&
            graphics->non_secret_text ==
                "Do you want ANSI COLOR GRAPHICS? [Y/n] ",
            "built-in identity precedes the exact graphics question with one blank line");
    }

    {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "xbit", "");
        fixture.names.results.push_back(stored(registerable_resolution("xbit")));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'N');
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.registration.has_value() && result.registration->valid() &&
            result.registration->normalized_name == "xbit" &&
            result.registration->graphics == app::GraphicsMode::ansi,
            "optional empty last name retains a single-word caller identity and graphics");
    }

    for (const std::size_t length : {std::size_t{29}, std::size_t{30}}) {
        Fixture fixture;
        script_prelogin(fixture, false);
        const std::string name(length, length == 29 ? 'H' : 'J');
        fixture.terminal.script_visible(
            "caller.name.first", app::TerminalOutcome::completed, name);
        fixture.terminal.script_presentation("caller.name.searching");
        fixture.names.results.push_back(stored(registerable_resolution(name)));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'N');
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.registration.has_value() &&
            result.registration->normalized_name == name &&
            fixture.terminal.clean(),
            "29/30-character single-word name is accepted without unusable last prompt");
    }

    {
        Fixture fixture;
        script_prelogin(fixture);
        fixture.terminal.script_visible("caller.name.first",
            app::TerminalOutcome::completed, std::string(31, 'K'));
        fixture.terminal.script_presentation("caller.name.invalid");
        const std::string accepted(30, 'L');
        fixture.terminal.script_visible(
            "caller.name.first", app::TerminalOutcome::completed, accepted);
        fixture.terminal.script_presentation("caller.name.searching");
        fixture.names.results.push_back(stored(registerable_resolution(accepted)));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'N');
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.registration.has_value() &&
            result.registration->normalized_name == accepted &&
            fixture.names.names.size() == 1,
            "31-character single-word name is rejected and never truncated or classified");
    }

    {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "Single", "", false);
        fixture.terminal.script_presentation("caller.name.invalid");
        script_name(fixture, "Real", "Name");
        fixture.names.results.push_back(stored(registerable_resolution("Real Name")));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'N');
        auto active = session();
        auto result = run(fixture, configuration(true, 999, true), active);
        require(result.registration.has_value() &&
            result.registration->normalized_name == "Real Name" &&
            fixture.names.names.size() == 1,
            "Real Names Required rejects an empty second component and accepts two components");
    }

    {
        Fixture fixture;
        script_prelogin(fixture);
        fixture.terminal.script_visible("caller.name.first",
            app::TerminalOutcome::completed, std::string(29, 'M'));
        fixture.terminal.script_presentation("caller.name.invalid");
        script_name(fixture, std::string(28, 'N'), "O");
        const std::string accepted = std::string(28, 'N') + " O";
        fixture.names.results.push_back(stored(registerable_resolution(accepted)));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action",
            app::TerminalOutcome::completed, 'N');
        auto active = session();
        auto result = run(fixture, configuration(true, 999, true), active);
        require(result.registration.has_value() &&
            result.registration->normalized_name == accepted &&
            accepted.size() == 30,
            "required split reserves separator and one final component character");
    }

    app::Phase3aRegistrationContinuation invalid_name{" caller", app::GraphicsMode::ansi};
    app::Phase3aRegistrationContinuation invalid_graphics{
        "Caller", static_cast<app::GraphicsMode>(99)};
    require(!invalid_name.valid() && !invalid_graphics.valid(),
        "registration continuation rejects non-normalized names and invalid graphics");
}

void test_classifier_authoritative_identity_binding() {
    for (const bool mismatched_name : {true, false}) {
        Fixture fixture;
        auto unrelated = account(mismatched_name ? 10 : 999);
        if (mismatched_name) {
            unrelated.display_name = "Different Caller";
            unrelated.name_key = "different caller";
        }
        fixture.accounts.account_result = stored(unrelated);
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 11);
        fixture.credentials.dummy_results.push_back(
            {app::AuthenticationOperationCode::rejected,
                false, false, false, false});
        fixture.terminal.script_presentation(
            "authentication.failure", app::TerminalOutcome::output_failure);
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::terminal_failure &&
            fixture.credentials.observations.size() == 1 &&
            fixture.credentials.observations.front().operation == "verify_dummy" &&
            fixture.credentials.observations.front().candidate_length == 11,
            "classifier/account name-key or security mismatch receives only dummy verification");
        require(fixture.phase3a_storage.enqueue_requests.empty() &&
            fixture.admission.requests.empty() &&
            fixture.session_commit.requests.empty() &&
            fixture.accounts.finalize_requests.empty() &&
            !active.caller().has_value(),
            "initial classifier/account mismatch creates no wrong-account notice or continuation");
        const auto failure = std::find_if(fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "authentication.failure";
            });
        require(failure != fixture.terminal.observations().end() &&
            failure->non_secret_text == "Login unsuccessful.\n",
            "initial classifier/account mismatch exposes only the generic failure");
    }

    {
        Fixture fixture;
        auto drifted = account();
        drifted.display_name = "Renamed Caller";
        drifted.name_key = "renamed caller";
        drifted.revision = 8;
        fixture.accounts.account_results.push_back(stored(account()));
        fixture.accounts.account_results.push_back(stored(drifted));
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed,
                true, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.credentials.observations.size() == 1 &&
            fixture.credentials.observations.front().operation == "verify_existing" &&
            fixture.admission.requests.empty() &&
            fixture.session_commit.requests.empty(),
            "name-key drift immediately after password verification stops before admission");
    }

    {
        Fixture fixture;
        auto drifted = account();
        drifted.display_name = "Remapped Caller";
        drifted.name_key = "remapped caller";
        drifted.revision = 8;
        fixture.accounts.account_results.push_back(stored(account()));
        fixture.accounts.account_results.push_back(stored(account()));
        fixture.accounts.account_results.push_back(stored(drifted));
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed,
                true, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.admission.requests.empty() &&
            fixture.session_commit.requests.empty(),
            "original classified name key is rechecked immediately before admission");
    }

    {
        Fixture fixture;
        auto recased = account();
        recased.display_name = "CALLER NAME";
        fixture.accounts.account_result = stored(recased);
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed,
                true, false, false, false});
        script_success_tail(fixture);
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.caller_ready() && active.caller().has_value() &&
            active.caller()->caller_identity == "caller name" &&
            active.caller()->display_name == "CALLER NAME" &&
            fixture.session_commit.requests.front().expected_account_name_key ==
                "caller name",
            "display recasing remains valid when the authoritative lowercase key is unchanged");
        auto delivered = result.ready->take();
        require(delivered.has_value() && delivered->admission_lease->release(),
            "recased identity transfers and releases the same admission lease");
    }
}

void test_authoritative_policy_account_and_commit_revalidation() {
    std::vector<app::SecurityPolicyRecord> mismatches{
        security_policy(), security_policy(), security_policy(), security_policy()};
    mismatches[0].revision = 2;
    mismatches[1].policy_hash = std::string(64, 'b');
    mismatches[2].new_user_level = 999;
    mismatches[3].sysop_threshold = 10;
    for (const auto& mismatch : mismatches) {
        Fixture fixture;
        fixture.accounts.policy_result = stored(mismatch);
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.terminal.observations().empty() && fixture.names.names.empty() &&
            fixture.credentials.observations.empty(),
            "database policy mismatch fails closed before caller workflow activity");
    }

    {
        Fixture fixture;
        fixture.accounts.account_results.push_back(stored(account()));
        fixture.accounts.account_results.push_back(stored(account()));
        fixture.accounts.account_results.push_back(
            stored(account(10, app::AccountState::locked)));
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.admission.requests.empty() && fixture.session_commit.requests.empty(),
            "post-auth account-state drift is rejected before admission");
    }

    {
        Fixture fixture;
        fixture.accounts.account_results.push_back(stored(account()));
        fixture.accounts.account_results.push_back(stored(account()));
        fixture.accounts.account_results.push_back(stored(account(999)));
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.admission.requests.empty() && fixture.session_commit.requests.empty(),
            "post-auth security-level elevation cannot grant stale authority");
    }

    {
        Fixture fixture;
        fixture.accounts.policy_results.push_back(stored(security_policy()));
        auto changed = security_policy();
        changed.sysop_threshold = 10;
        fixture.accounts.policy_results.push_back(stored(changed));
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.admission.requests.empty() && fixture.session_commit.requests.empty(),
            "post-auth authoritative threshold drift is rejected before admission");
    }

    {
        Fixture fixture;
        auto admitted = account();
        admitted.statistics.last_successful_login_utc_seconds = 1'699'999'000;
        auto post_commit = admitted;
        post_commit.revision = 8;
        post_commit.statistics.successful_login_count = 13;
        post_commit.statistics.last_successful_login_utc_seconds = 1'700'000'000;
        fixture.accounts.account_results.push_back(stored(admitted));
        fixture.accounts.account_results.push_back(stored(admitted));
        fixture.accounts.account_results.push_back(stored(admitted));
        fixture.accounts.account_results.push_back(stored(post_commit));
        fixture.welcome_statistics.result = {
            app::StorageStatus::success(), 3U};
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        script_success_tail(fixture);
        auto active = session();
        auto result = run(fixture, configuration(), active);
        require(result.caller_ready() && fixture.session_commit.requests.size() == 1,
            "stable authoritative account and policy reach ready handoff");
        const auto& commit = fixture.session_commit.requests.front();
        require(app::valid_authenticated_session_commit_request(commit) &&
            commit.expected_account_revision == 7 &&
            commit.expected_account_name_key == "caller name" &&
            commit.expected_security_level == 10 &&
            commit.expected_security_policy_revision == 1 &&
            commit.expected_security_policy_hash == std::string(64, 'a'),
            "composite commit carries exact account revision, identity, level, and policy preconditions");
        const auto information = std::find_if(
            fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "caller.information";
            });
        const auto greeting = std::find_if(
            fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "caller.greeting";
            });
        require(greeting != fixture.terminal.observations().end() &&
            greeting->non_secret_text ==
                "\nGood afternoon, Caller Name, welcome back.\n" &&
            information != fixture.terminal.observations().end() &&
            information->non_secret_text.find("There have been 3 calls today.\n") !=
                std::string::npos &&
            information->non_secret_text.find("You have called 13 times.") !=
                std::string::npos &&
            information->non_secret_text.find("You last called on 08-28-26 at 12:34.") !=
                std::string::npos &&
            information->non_secret_text.find("Caller:") ==
                std::string::npos &&
            std::find(fixture.local_time.requests.begin(),
                fixture.local_time.requests.end(), 1'699'999'000) !=
                fixture.local_time.requests.end(),
            "post-commit count is authoritative while prior timestamp is presented as previous");
        auto delivered = result.ready->take();
        require(delivered.has_value() && delivered->admission_lease->release(),
            "qualified session owner accepts and releases the transferred lease");
    }

    {
        Fixture fixture;
        auto changed = account();
        changed.display_name = "Different Caller";
        changed.name_key = "different caller";
        changed.revision = 8;
        fixture.accounts.account_results.push_back(stored(account()));
        fixture.accounts.account_results.push_back(stored(account()));
        fixture.accounts.account_results.push_back(stored(account()));
        fixture.accounts.account_results.push_back(stored(changed));
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::storage_failure &&
            fixture.session_commit.requests.size() == 1 &&
            fixture.accounts.finalize_requests.size() == 1 &&
            fixture.admission.releases.size() == 1 &&
            active.state() == core::SessionState::disconnected &&
            !active.caller().has_value(),
            "post-commit identity drift finalizes, releases, disconnects, and emits no ready handoff");
    }
}

void test_rate_secret_cleanup_and_final_warning() {
    {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.rates.gates.push_back({app::Phase3aRateGateCode::timed_out, 8});
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::timed_out &&
            fixture.terminal.hidden_input_requests() == 0 &&
            fixture.credentials.observations.empty(),
            "known-account Candidate A gate completes before any secret request");
    }
    {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "Unavailable", "Caller");
        fixture.names.results.push_back(
            stored(unavailable_resolution("Unavailable Caller")));
        fixture.rates.gates.push_back({app::Phase3aRateGateCode::unavailable, 8});
        fixture.terminal.script_presentation("authentication.failure");
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::temporarily_unavailable &&
            fixture.terminal.hidden_input_requests() == 0 &&
            fixture.credentials.observations.empty(),
            "dummy-attempt Candidate A gate completes before any secret request");
    }
    {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_malformed_secret_outcome(
            "authentication.password", app::TerminalOutcome::timed_out, 17);
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        require(result.code == app::Phase3aWorkflowCode::timed_out &&
            fixture.credentials.observations.empty() &&
            fixture.terminal.retained_secret_bytes() == 0 &&
            std::all_of(fixture.terminal.observations().begin(),
                fixture.terminal.observations().end(), [](const auto& observation) {
                    return !observation.hidden_input ||
                        observation.non_secret_text == "Enter your password: ";
                }),
            "malformed secret payload is rejected without retained or observed secret text");
    }
    {
        Fixture fixture;
        script_prelogin(fixture);
        for (int attempt = 0; attempt < 2; ++attempt) {
            script_name(fixture, "Reserved", "Caller");
            fixture.names.results.push_back(
                stored(unavailable_resolution("Reserved Caller")));
            fixture.terminal.script_secret_completed("authentication.password", 9);
            fixture.credentials.dummy_results.push_back(
                {app::AuthenticationOperationCode::rejected,
                    false, false, false, false});
            fixture.terminal.script_presentation("authentication.failure");
            if (attempt == 1) {
                fixture.terminal.script_presentation(
                    "authentication.final_attempt_warning",
                    app::TerminalOutcome::output_failure);
            }
        }
        auto active = session();
        const auto result = run(fixture, configuration(), active);
        const auto warning = std::find_if(fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "authentication.final_attempt_warning";
            });
        require(result.code == app::Phase3aWorkflowCode::terminal_failure &&
            fixture.credentials.observations.size() == 2 &&
            warning != fixture.terminal.observations().end() &&
            warning->non_secret_text ==
                "Warning: this is your final login attempt.\n" &&
            warning->non_secret_text.find("password") == std::string::npos &&
            warning->non_secret_text.find("account") == std::string::npos,
            "second-failure warning is generic and terminal failure stops before attempt three");
    }
}

void test_c3_closed_notice_and_failure() {
    for (const bool output_fails : {false, true}) {
        Fixture fixture;
        script_prelogin(fixture, false);
        script_name(fixture, "New", "Caller");
        fixture.names.results.push_back(stored(registerable_resolution("New Caller")));
        fixture.terminal.script_presentation("caller.name.available");
        fixture.terminal.script_key("caller.name.available.action", app::TerminalOutcome::completed, 'N');
        fixture.terminal.script_presentation("registration.closed", output_fails
            ? app::TerminalOutcome::output_failure : app::TerminalOutcome::completed);
        auto active = session();
        const auto result = run(fixture, configuration(true, 999, false, false), active);
        require(result.code == (output_fails ? app::Phase3aWorkflowCode::terminal_failure
                : app::Phase3aWorkflowCode::registration_unavailable),
            "closed notice is flushed before registration stop, with output failure propagated");
        require(fixture.admission.requests.empty() && fixture.session_commit.requests.empty() &&
            fixture.credentials.observations.empty() && active.state() == core::SessionState::disconnected,
            "closed registration creates no credential, account lease or durable session");
        require(fixture.terminal.observations().back().non_secret_text ==
            "Test BBS is not accepting new users at this time.\n\nPlease try again later.\n\nGoodbye.\n",
            "closed-registration exact configured-name transcript and blank rows");
    }
}

void test_lease_capability_lifetime() {
    test::FakeAccountSessionAdmissionPort port;
    auto owner = app::AccountSessionAdmissionLeaseOwner::create(
        {identifier(810), 42, identifier(811)}, port);
    require(owner.has_value() && owner->active(),
        "valid admission lease becomes an active owner capability");
    auto moved = std::move(*owner);
    require(!owner->active() && moved.active() && port.releases.empty(),
        "moving lease capability transfers sole release authority");
    require(moved.release() && !moved.release() && port.releases.size() == 1,
        "moved capability releases exactly once without double release");

    {
        auto automatic = app::AccountSessionAdmissionLeaseOwner::create(
            {identifier(812), 42, identifier(813)}, port);
        require(automatic.has_value() && automatic->active(),
            "second lease capability is active before scope exit");
    }
    require(port.releases.size() == 2,
        "abandoned move-only capability cannot lose its release");
}

void test_time_of_day_welcome_wording() {
    struct Case { const char* timestamp; const char* period; };
    constexpr std::array<Case, 6> cases{{
        {"2026-08-28 00:00:00", "morning"},
        {"2026-08-28 11:59:59", "morning"},
        {"2026-08-28 12:00:00", "afternoon"},
        {"2026-08-28 17:59:59", "afternoon"},
        {"2026-08-28 18:00:00", "evening"},
        {"2026-08-28 23:59:59", "evening"},
    }};
    for (const auto& value : cases) {
        Fixture fixture;
        fixture.local_time.value = value.timestamp;
        fixture.welcome_statistics.result = {
            app::StorageStatus::success(), 2U};
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        script_success_tail(fixture);
        auto active = session();
        auto result = run(fixture, configuration(), active);
        const auto greeting = std::find_if(fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "caller.greeting";
            });
        const auto information = std::find_if(fixture.terminal.observations().begin(),
            fixture.terminal.observations().end(), [](const auto& observation) {
                return observation.identity == "caller.information";
            });
        require(result.caller_ready() && greeting != fixture.terminal.observations().end() &&
                greeting->non_secret_text == "\nGood " + std::string(value.period) +
                    ", Caller Name, welcome to Test BBS.\n" &&
                information != fixture.terminal.observations().end() &&
                information->non_secret_text.find("There have been 2 calls today.\n") !=
                    std::string::npos,
            "BBS-local hour boundaries select natural greeting periods and plural daily grammar");
        auto delivered = result.ready->take();
        require(delivered && delivered->admission_lease->release(),
            "time-of-day greeting test releases its admission lease");
    }
}

void test_staged_ssh_password_entry() {
    const auto make_login = [](Fixture& fixture, const app::Phase3aConfigurationSnapshot& config,
        core::Session& active, app::CancellationSource& cancellation) {
        return std::make_unique<app::Phase3aTransportPasswordLogin>(
            app::Phase3aExistingCallerDependencies{fixture.terminal, fixture.names, fixture.credentials,
                fixture.accounts, fixture.authentication_storage, fixture.phase3a_storage,
                fixture.admission, fixture.session_commit, fixture.displays, fixture.rates,
                fixture.local_time, fixture.welcome_statistics, fixture.clock, fixture.identifiers},
            config, active, app::Phase3aExistingCallerRequest{"192.0.2.7", identifier(900), {}},
            cancellation.token());
    };
    for (const bool succeeds : {false, true}) {
        Fixture fixture;
        const auto config = configuration();
        auto active = session(core::TransportKind::ssh);
        app::CancellationSource cancellation;
        auto login = make_login(fixture, config, active, cancellation);
        fixture.accounts.account_result = stored(account(999));
        fixture.names.results.push_back(stored(existing_resolution("caller name", 999)));
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        require(login->authenticate("caller name", "Fixture password") == app::TransportPasswordResult::accepted,
            "staged SSH Sysop verifies the ordinary password before SY");
        require(!login->new_registration_pending(),"verified existing Sysop is never an SSH registration challenge candidate");
        script_prelogin(fixture);
        for (int attempt = 0; attempt < 3; ++attempt) {
            fixture.terminal.script_secret_completed("authentication.system_password", 14);
            const bool verified = succeeds && attempt == 2;
            fixture.credentials.system_results.push_back({verified ?
                app::AuthenticationOperationCode::completed : app::AuthenticationOperationCode::rejected,
                verified, false, false, false});
            if (!verified) fixture.terminal.script_presentation("authentication.failure");
            if (attempt == 1) fixture.terminal.script_presentation("authentication.final_attempt_warning");
        }
        if (succeeds) script_success_tail(fixture);
        auto result = login->continue_on_shell();
        require(result.caller_ready() == succeeds && fixture.terminal.clean(),
            "SSH SY retries retain the final-attempt warning and require independent success");
        require(std::count_if(fixture.credentials.observations.begin(), fixture.credentials.observations.end(),
                [](const auto& item) { return item.operation == "verify_existing"; }) == 1 &&
                std::count_if(fixture.credentials.observations.begin(), fixture.credentials.observations.end(),
                [](const auto& item) { return item.operation == "verify_system"; }) == 3 &&
                fixture.phase3a_storage.enqueue_requests.size() == (succeeds ? 2U : 3U),
            "SSH SY cannot bypass the three-failure budget, duplicate ordinary auth, or lose failure notices");
        if (result.ready) {
            auto delivered = result.ready->take();
            require(delivered && delivered->admission_lease->release(), "SSH Sysop lease is released");
        } else {
            require(fixture.session_commit.requests.empty(), "failed SSH SY commits no successful session");
        }
    }
    for (const bool temporary : {false, true}) {
        Fixture fixture;
        const auto config = configuration();
        auto active = session(core::TransportKind::ssh);
        app::CancellationSource cancellation;
        auto login = make_login(fixture, config, active, cancellation);
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.credentials.existing_results.push_back(
            {temporary ? app::AuthenticationOperationCode::password_change_required :
                app::AuthenticationOperationCode::completed, true, temporary, false, false});
        require(login->authenticate("caller name", "Fixture password") == app::TransportPasswordResult::accepted,
            "SSH password callback uses the ordinary credential bridge");
        require(!login->new_registration_pending(),"ordinary and temporary-password existing accounts bypass registration challenge");
        require(fixture.terminal.observations().empty() && active.state() == core::SessionState::establishing,
            "SSH password acceptance emits no terminal prompts and grants no caller-ready authority");
        script_prelogin(fixture);
        if (temporary) {
            fixture.authentication_storage.temporary_result = stored(
                app::TemporaryPasswordRecord{42, 3, app::TemporaryPasswordState::active,
                    1'699'999'000, 1'700'100'000, {}});
            fixture.terminal.script_secret_completed("authentication.replacement", 12);
            fixture.terminal.script_secret_completed("authentication.replacement.confirm", 12);
            app::PasswordMutationResult replaced;
            replaced.code = app::AuthenticationOperationCode::completed;
            replaced.account_revision = 8;
            replaced.credential_committed = true;
            replaced.revocation_completed = true;
            fixture.credentials.replacement_results.push_back(replaced);
        }
        script_success_tail(fixture);
        auto result = login->continue_on_shell();
        require(result.caller_ready() && fixture.terminal.clean(),
            "SSH shell preserves ANSI, mandatory temporary replacement, welcome and continue without name/password reprompt");
        require(std::count_if(fixture.credentials.observations.begin(), fixture.credentials.observations.end(),
            [](const auto& item) { return item.operation == "verify_existing"; }) == 1,
            "SSH performs exactly one ordinary account password verification");
        auto delivered = result.ready->take();
        require(delivered && delivered->admission_lease->release(), "SSH releases exactly one admission lease");
        require(login->continue_on_shell().code == app::Phase3aWorkflowCode::invalid_context,
            "SSH verified continuation cannot be replayed");
    }
    {
        Fixture fixture;
        const auto config = configuration();
        auto active = session(core::TransportKind::ssh);
        app::CancellationSource cancellation;
        auto login = make_login(fixture, config, active, cancellation);
        for (unsigned index = 0; index != 3; ++index) {
            fixture.names.results.push_back(stored(existing_resolution()));
            const auto result = login->authenticate("caller name", std::string(129, 'X'));
            require(result == (index == 2 ? app::TransportPasswordResult::disconnect :
                    app::TransportPasswordResult::rejected),
                "oversized SSH passwords consume the same three-attempt budget without truncation");
        }
        require(fixture.credentials.observations.empty() && fixture.terminal.observations().empty(),
            "oversized SSH input reaches neither KDF nor terminal");
        require(!login->continue_on_shell().caller_ready(), "rejected SSH callback cannot admit a shell");
    }
    {
        Fixture fixture;
        const auto config = configuration();
        auto active = session(core::TransportKind::ssh);
        app::CancellationSource cancellation;
        auto login = make_login(fixture, config, active, cancellation);
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
        require(login->authenticate("caller name", "Fixture password") == app::TransportPasswordResult::accepted,
            "SSH verified identity binds before shell");
        fixture.accounts.account_result.value->revision++;
        require(!login->continue_on_shell().caller_ready() && fixture.terminal.observations().empty(),
            "account mutation between SSH auth and shell fails closed before presentation");
    }
}

void test_ssh_provisional_registration() {
    const auto make_login = [](Fixture& f, const app::Phase3aConfigurationSnapshot& config,
        core::Session& active, app::CancellationSource& cancel) {
        return std::make_unique<app::Phase3aTransportPasswordLogin>(
            app::Phase3aExistingCallerDependencies{f.terminal, f.names, f.credentials, f.accounts,
                f.authentication_storage, f.phase3a_storage, f.admission, f.session_commit,
                f.displays, f.rates, f.local_time, f.welcome_statistics, f.clock, f.identifiers},
            config, active, app::Phase3aExistingCallerRequest{"192.0.2.7", identifier(900), {}}, cancel.token());
    };
    for (int scenario = 0; scenario != 10; ++scenario) {
        Fixture f;
        const auto config = configuration(true, 999, false, scenario != 1);
        auto active = session(core::TransportKind::ssh);
        app::CancellationSource cancel;
        auto login = make_login(f, config, active, cancel);
        f.names.results.push_back(stored(scenario == 2 ? unavailable_resolution("Proposed") :
            registerable_resolution("Proposed")));
        if (scenario == 3) f.rates.gates.push_back({app::Phase3aRateGateCode::unavailable, 0});
        f.credentials.dummy_results.push_back({scenario == 4 ? app::AuthenticationOperationCode::backpressure :
            app::AuthenticationOperationCode::rejected, false, false, false, false});
        if (scenario != 1 && scenario != 2 && scenario != 3 && scenario != 4) {
            f.names.results.push_back(stored(scenario == 5 ? existing_resolution("Proposed") :
                registerable_resolution("Proposed")));
        }
        std::string throwaway = "x";
        const auto outcome = login->authenticate("Proposed", throwaway);
        app::explicitly_erase_terminal_secret(throwaway);
        const bool accepted = scenario == 0 || scenario >= 6;
        require(login->new_registration_pending()==accepted,
            "only classified eligible provisional registrations expose the BotGate entry condition");
        require((outcome == app::TransportPasswordResult::accepted) == accepted,
            "only eligible available names with registration enabled and completed dummy KDF get provisional entry");
        require(f.terminal.observations().empty() && f.session_commit.requests.empty() &&
            active.state() == core::SessionState::establishing && !active.caller(),
            "provisional auth exposes no secret, prompts, account binding, durable call or online state");
        require(!f.rates.observations.empty() && f.rates.observations.front().operation == "await" &&
            f.rates.observations.front().scopes.source && f.rates.observations.front().scopes.global &&
            !f.rates.observations.front().scopes.account_id,
            "provisional entry uses canonical source/global rate admission, never invented account scope");
        require(std::none_of(f.rates.observations.begin(), f.rates.observations.end(),
            [](const auto& observation) { return observation.operation == "success"; }),
            "provisional entry never clears defense state as a successful account login");
        if (scenario == 3) require(f.credentials.observations.empty(), "rate failure blocks dummy KDF");
        if (!accepted) {
            require(!login->continue_on_shell().registration, "rejected auth cannot open registration");
            continue;
        }
        if (scenario == 6) cancel.cancel();
        else f.names.results.push_back(stored(scenario == 7 ? unavailable_resolution("Proposed") :
            registerable_resolution("Proposed")));
        if (scenario == 0 || scenario == 8) script_prelogin(f);
        if (scenario == 9) {
            f.terminal.script_presentation("prelogin.builtin");
            f.terminal.script_key("prelogin.graphics", app::TerminalOutcome::disconnected, {});
        }
        const auto result = login->continue_on_shell();
        require(result.registration.has_value() == (scenario == 0 || scenario == 8),
            "shell rechecks name eligibility and stops on cancellation/disconnect without wizard authority");
        require(!result.caller_ready() && f.session_commit.requests.empty(), "registration continuation is not caller ready");
        if (result.registration) require(result.registration->normalized_name == "Proposed" &&
            result.registration->valid() && result.registration->failed_authentication_attempts == 0,
            "continuation contains only proposed handle, graphics and original failure budget");
        require(login->continue_on_shell().code == app::Phase3aWorkflowCode::invalid_context,
            "provisional continuation cannot be consumed twice");
    }
    {
        Fixture f;
        const auto config = configuration();
        auto active = session(core::TransportKind::ssh);
        app::CancellationSource cancel;
        auto login = make_login(f, config, active, cancel);
        f.names.results.push_back(stored(existing_resolution()));
        f.credentials.existing_results.push_back({app::AuthenticationOperationCode::rejected, false, false, false, false});
        require(login->authenticate("caller name", "bad password") == app::TransportPasswordResult::rejected,
            "wrong existing password never becomes provisional registration");
        for (int i = 0; i < 3; ++i) f.names.results.push_back(stored(registerable_resolution("New Handle")));
        f.credentials.dummy_results.push_back({app::AuthenticationOperationCode::rejected, false, false, false, false});
        require(login->authenticate("New Handle", "x") == app::TransportPasswordResult::accepted,
            "changing to an eligible new name follows its own classifier result");
        script_prelogin(f);
        const auto result = login->continue_on_shell();
        require(result.registration && result.registration->failed_authentication_attempts == 1,
            "changing SSH username cannot reset the connection failure budget");
    }
}


void test_exception_safe_durable_lifetime() {
    for (unsigned scenario = 0; scenario != 4; ++scenario) {
        Fixture fixture;
        script_prelogin(fixture);
        script_name(fixture, "caller", "name");
        fixture.names.results.push_back(stored(existing_resolution()));
        fixture.terminal.script_secret_completed("authentication.password", 10);
        fixture.credentials.existing_results.push_back(
            {app::AuthenticationOperationCode::completed, true, false, false, false});
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

    }
}

void test_r1_name_filter_and_attention() {
    struct Observer : app::ICallerStageObserver {
        std::vector<app::AttentionKind> kinds;
        std::vector<app::AccountId> accounts;
        std::vector<std::string> names;
        void stage(app::CallerStage) noexcept override {}
        void attention(app::AttentionKind kind, app::AccountId account, std::string_view name, std::uint16_t) noexcept override {
            kinds.push_back(kind); accounts.push_back(account); names.emplace_back(name);
        }
    };
    const auto make_login = [](Fixture& f, const app::Phase3aConfigurationSnapshot& config,
        core::Session& active, app::CancellationSource& cancel) {
        return std::make_unique<app::Phase3aTransportPasswordLogin>(
            app::Phase3aExistingCallerDependencies{f.terminal, f.names, f.credentials, f.accounts,
                f.authentication_storage, f.phase3a_storage, f.admission, f.session_commit, f.displays,
                f.rates, f.local_time, f.welcome_statistics, f.clock, f.identifiers, f.observer, f.cleanup_health, f.new_names},
            config, active, app::Phase3aExistingCallerRequest{"192.0.2.7", identifier(900), {}}, cancel.token());
    };
    for (const bool ssh : {false, true}) for (unsigned scenario = 0; scenario < 3; ++scenario) {
        Fixture f; Observer observer; f.observer = &observer;
        std::optional<app::Phase3aPreAuthenticationEnd> ending;
        const auto deny = scenario == 2 ? app::NewCallerNames::unavailable() :
            app::NewCallerNames::parse("; exact list\r\n  RESERVED Caller  \r\n");
        f.new_names = &deny;
        const std::string proposed = scenario == 1 ? "reserved caller two" : "reserved caller";
        const auto config = configuration();
        auto active = session(ssh ? core::TransportKind::ssh : core::TransportKind::telnet);
        if (ssh) {
            app::CancellationSource cancellation;
            auto login = make_login(f, config, active, cancellation);
            f.names.results.push_back(stored(registerable_resolution(proposed)));
            f.names.results.push_back(stored(registerable_resolution(proposed)));
            f.credentials.dummy_results.push_back({app::AuthenticationOperationCode::rejected, false, false, false, false});
            const auto result = login->authenticate("  " + proposed + "  ", "fixture-throwaway");
            require(result == (scenario == 1 ? app::TransportPasswordResult::accepted : app::TransportPasswordResult::disconnect),
                "SSH exact/case/outer-space deny and invalid-list refusal precede provisional shell acceptance");
            require(f.terminal.observations().empty(), "SSH refusal neither fabricates names nor collects registration passwords");
            if (scenario != 1) require(login->registration_refusal_notice() == (scenario == 0 ?
                "That name is unavailable. Please choose another.\r\n" :
                "New registration is temporarily unavailable. Please try again later.\r\n"), "SSH neutral fixed refusal text");
            if (const auto* typed = login->preauthentication_end()) ending = *typed;
        } else {
            script_prelogin(f);
            script_name(f, "reserved", scenario == 1 ? "caller two" : "caller");
            f.names.results.push_back(stored(registerable_resolution(proposed)));
            if (scenario == 1) {
                f.terminal.script_presentation("caller.name.available");
                f.terminal.script_key("caller.name.available.action", app::TerminalOutcome::completed, 'N');
            } else f.terminal.script_presentation("registration.name.unavailable");
            const auto result = run(f, config, active);
            ending = result.preauthentication_end;
            require(result.code == (scenario == 1 ? app::Phase3aWorkflowCode::registration_requested :
                app::Phase3aWorkflowCode::registration_unavailable), "Telnet exact deny/invalid list, non-substring continuation");
            require(f.terminal.clean() && f.credentials.observations.empty(), "Telnet denied name consumes no password or KDF");
            if (scenario != 1) require(active.state() == core::SessionState::disconnected, "denied Telnet context fully disconnects");
        }
        require(observer.kinds.size() == (scenario == 0 ? 1U : 0U) && (observer.kinds.empty() ||
            (observer.kinds[0] == app::AttentionKind::new_name_denied && observer.accounts[0] == 0 && observer.names[0] == proposed)),
            "one typed deny event; invalid-file alert belongs to Runtime startup, never repeated per caller");
        require(scenario == 0 ? ending.has_value() &&
                ending->reason == app::Phase3aPreAuthenticationEndReason::joker_name_rejected &&
                ending->attempted_name == proposed : !ending.has_value(),
            "Telnet and SSH expose a separate attempted name only for a confirmed ending JOKER match");
        require(!active.caller() && f.session_commit.requests.empty() && f.admission.requests.empty(),
            "name refusal/noncommitted continuation grants no account lease, caller number or durable session");
    }
    for (const bool ssh : {false, true}) for (const bool success : {false, true}) {
        Fixture f; Observer observer; f.observer = &observer;
        const auto denied = app::NewCallerNames::parse("Caller Name\r\n"); f.new_names = &denied;
        const auto config = configuration();
        auto active = session(ssh ? core::TransportKind::ssh : core::TransportKind::telnet);
        app::CancellationSource cancellation;
        auto login = ssh ? make_login(f, config, active, cancellation) : nullptr;
        if (!ssh) script_prelogin(f);
        for (unsigned n = 0; n < (success ? 1U : 3U); ++n) {
            f.names.results.push_back(stored(existing_resolution()));
            f.credentials.existing_results.push_back({success ? app::AuthenticationOperationCode::completed :
                app::AuthenticationOperationCode::rejected, success, false, false, false});
            if (ssh) {
                require(login->authenticate("caller name", "fixture-ordinary") == (success ? app::TransportPasswordResult::accepted :
                    n == 2 ? app::TransportPasswordResult::disconnect : app::TransportPasswordResult::rejected),
                    "SSH existing account password policy and exhaustion remain unchanged");
            } else {
                script_name(f, "caller", "name"); f.terminal.script_secret_completed("authentication.password", 12);
                if (!success) {
                    f.terminal.script_presentation("authentication.failure");
                    if (n == 1) f.terminal.script_presentation("authentication.final_attempt_warning");
                }
            }
        }
        if (success) { if (ssh) script_prelogin(f); script_success_tail(f); }
        if (!ssh || success) {
            auto result = ssh ? login->continue_on_shell() : run(f, config, active);
            require(result.caller_ready() == success && f.terminal.clean(), "listed existing account still logs in, failed attempts disconnect");
        }
        require(observer.kinds.size() == (success ? 0U : 1U) && (success ||
            (observer.kinds[0] == app::AttentionKind::password_attempts_exhausted && observer.accounts[0] == 42 && observer.names[0] == "caller name")),
            "one known-account exhaustion alert, no credentials or raw input in typed observer");
        require(f.session_commit.requests.size() == (success ? 1U : 0U) && f.terminal.retained_secret_bytes() == 0,
            "failed auth consumes no caller number/session; successful existing account commits once and clears secrets");
    }
}

}  // namespace

int main() {
    try {
        test_current_login_address_information();
        test_r1_name_filter_and_attention();
        test_f01_plain_identity_before_graphics();
        test_exception_safe_durable_lifetime();
        test_c3_closed_notice_and_failure();
        test_prelogin_name_budget_and_registration_continuations();
        test_blank_and_substantive_name_guards();
        test_ordinary_success_and_ready_handoff();
        test_exact_complete_name_shortcut();
        test_welcome_after_authenticated_information();
        test_unavailable_dummy_and_three_attempt_boundary();
        test_generic_failures_and_notice_enqueue();
        test_terminal_stop_outcomes_and_bootstrap();
        test_temporary_password_replacement();
        test_sysop_threshold_ordering_and_failed_restart();
        test_notice_delivery_acknowledgement_and_replay_cleanup();
        test_admission_commit_and_cleanup_failures();
        test_corrected_name_and_prelogin_semantics();
        test_classifier_authoritative_identity_binding();
        test_authoritative_policy_account_and_commit_revalidation();
        test_rate_secret_cleanup_and_final_warning();
        test_lease_capability_lifetime();
        test_time_of_day_welcome_wording();
        test_staged_ssh_password_entry();
        test_connection_password_handoff();
        test_ssh_provisional_registration();
        std::cout << "all SFX Phase 3A Window 3 workflow tests passed assertions="
                  << assertions << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "SFX Phase 3A Window 3 test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
