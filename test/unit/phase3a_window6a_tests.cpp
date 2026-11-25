#include "sfx/application/phase3a_workflow.hpp"
#include "sfx/application/terminal.hpp"
#include "sfx/application/terminal_stream.hpp"
#include "sfx/test/account_storage_fakes.hpp"
#include "sfx/test/authentication_fakes.hpp"
#include "sfx/test/foundation_fakes.hpp"
#include "sfx/test/phase3a_fakes.hpp"
#include "sfx/test/phase3a_storage_fakes.hpp"
#include "sfx/test/terminal_stream_fake.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

std::vector<std::uint8_t> bytes(
    const std::initializer_list<std::uint8_t> values) {
    return std::vector<std::uint8_t>(values);
}

app::TerminalInputRequest line_request(
    std::string identity,
    const std::size_t maximum,
    const app::EmptyInputPolicy empty = app::EmptyInputPolicy::reject) {
    return {std::move(identity), "Input: ", maximum, empty};
}

app::Identifier128 identifier(const std::uint32_t value) {
    test::DeterministicIdentifierSource source(value);
    return source.next();
}

template <typename T>
app::StorageResult<T> stored(T value) {
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

app::AccountRecord workflow_account(
    std::string display_name,
    const std::uint16_t security_level) {
    app::AccountRecord value;
    value.account_id = 42;
    value.name_key =
        app::account_name_comparison_key(display_name);
    value.display_name = std::move(display_name);
    value.security_level = security_level;
    value.state = app::AccountState::active;
    value.revision = 7;
    value.created_utc_seconds = 1'700'000'000;
    value.modified_utc_seconds = 1'700'000'000;
    value.statistics.successful_login_count = 1;
    return value;
}

app::SecurityPolicyRecord workflow_policy(
    const std::uint16_t new_user_level,
    const std::uint16_t sysop_threshold) {
    return {1, new_user_level, sysop_threshold,
        std::string(64, 'a'), 1, std::string(64, 'a')};
}

app::Phase3aConfigurationSnapshot workflow_configuration(
    const std::uint16_t new_user_level = 10,
    const std::uint16_t sysop_threshold = 999) {
    app::Phase3aConfigurationValues values;
    values.foundation.logical_nodes = 128;
    const auto level10 = *core::SecurityLevel::create(10);
    const auto level999 = *core::SecurityLevel::create(999);
    const auto new_user = *core::SecurityLevel::create(new_user_level);
    const auto threshold = *core::SecurityLevel::create(sysop_threshold);
    std::vector<core::SecurityLevel> levels{level10, level999};
    if (std::find(levels.begin(), levels.end(), new_user) ==
        levels.end()) {
        levels.push_back(new_user);
    }
    if (std::find(levels.begin(), levels.end(), threshold) ==
        levels.end()) {
        levels.push_back(threshold);
    }
    values.foundation.security = *core::SecurityPolicy::create(
        std::move(levels), new_user, threshold);
    values.bootstrap_complete = true;
    values.allow_new_users = true;
    values.collect_optional_real_name = false;
    values.transfer_protocol_question_enabled = false;
    values.enabled_transfer_choices.clear();
    values.security_policy = {1, std::string(64, 'a'),
        new_user_level, sysop_threshold};
    values.connection_limits.maximum_logical_nodes = 128;
    values.prelogin_identity = {"Test BBS", "Test Sysop", "0.1-test"};
    auto created =
        app::Phase3aConfigurationSnapshot::create(std::move(values));
    if (!created.valid()) {
        throw std::runtime_error("valid workflow configuration rejected");
    }
    return std::move(*created.snapshot);
}

core::Session workflow_session() {
    return core::Session("window6a-shell",
        *core::NodeNumber::create(1),
        {core::TransportKind::telnet, "192.0.2.7", {}, "ansi"});
}

app::CallerNameResolution existing_resolution() {
    return {app::CallerNameClassification::existing,
        "Caller Name", 42, "Caller Name",
        static_cast<std::uint16_t>(10)};
}

app::CallerNameResolution registerable_resolution() {
    return {app::CallerNameClassification::registerable,
        "New Caller", {}, {}, {}};
}

struct ConcreteWorkflowFixture {
    test::DeterministicInteractiveByteStreamFake stream;
    app::StreamCallerTerminal terminal{stream};
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

    ConcreteWorkflowFixture() {
        accounts.mutation_result.status =
            app::StorageStatus::success();
        authentication_storage.status =
            app::StorageStatus::success();
        phase3a_storage.status_result =
            app::StorageStatus::success();
        phase3a_storage.dynamic_claim_result = true;
        session_commit.result.status =
            app::StorageStatus::success();
        rates.status = app::StorageStatus::success();
        local_date.value = {2026, 8, 29};
        credentials.registration_result = {
            app::AuthenticationOperationCode::completed,
            app::PasswordPolicyCode::accepted, credential()};
    }

    app::Phase3aExistingCallerWorkflow existing_workflow() {
        return app::Phase3aExistingCallerWorkflow({terminal, names,
            credentials, accounts, authentication_storage,
            phase3a_storage, admission, session_commit, displays,
            rates, local_time, welcome_statistics, clock, identifiers});
    }

    app::Phase3aRegistrationWorkflow registration_workflow() {
        return app::Phase3aRegistrationWorkflow({terminal, names,
            credentials, accounts, authentication_storage,
            phase3a_storage, registration_commit, admission,
            session_commit, displays, rates, local_time, welcome_statistics, local_date,
            clock, identifiers});
    }
};

enum class StreamFailureForm : std::uint8_t {
    explicit_failure,
    malformed_result,
    throwing_read
};

void script_stream_failure(
    test::DeterministicInteractiveByteStreamFake& stream,
    const StreamFailureForm form) {
    if (form == StreamFailureForm::explicit_failure) {
        stream.script_read_outcome(
            app::InteractiveStreamOutcome::failed);
    } else if (form == StreamFailureForm::malformed_result) {
        stream.script_malformed_read(
            app::InteractiveStreamOutcome::completed);
    } else {
        stream.script_read_exception();
    }
}

std::size_t rate_failures(
    const test::FakePhase3aRateControlPort& rates) {
    return static_cast<std::size_t>(std::count_if(
        rates.observations.begin(), rates.observations.end(),
        [](const test::RateObservation& observation) {
            return observation.operation == "failure";
        }));
}

void stream_result_contracts() {
    require(app::valid_interactive_stream_read_result(
            4, {app::InteractiveStreamOutcome::completed, bytes({'A'})}),
        "bounded completed read valid");
    require(!app::valid_interactive_stream_read_result(
            4, {app::InteractiveStreamOutcome::completed, {}}) &&
        !app::valid_interactive_stream_read_result(
            1, {app::InteractiveStreamOutcome::completed,
                bytes({'A', 'B'})}),
        "empty and oversized completed reads invalid");
    require(app::valid_interactive_stream_read_result(
            4, {app::InteractiveStreamOutcome::timed_out, {}}) &&
        !app::valid_interactive_stream_read_result(
            4, {app::InteractiveStreamOutcome::timed_out,
                bytes({'A'})}),
        "stop read cannot carry bytes");
    require(app::valid_interactive_stream_write_result(
            4, {app::InteractiveStreamOutcome::completed, 2}) &&
        !app::valid_interactive_stream_write_result(
            4, {app::InteractiveStreamOutcome::completed, 0}) &&
        !app::valid_interactive_stream_write_result(
            4, {app::InteractiveStreamOutcome::completed, 5}),
        "completed writes require bounded forward progress");
    require(app::valid_interactive_stream_write_result(
            4, {app::InteractiveStreamOutcome::failed, 3}),
        "partial failed write is representable");
    require(app::valid_terminal_key_request(
            {"accepted.case_pairs", "Choose: ", {'Y', 'y'}, 'Y'}),
        "accepted workflow case-paired choice contract preserved");

    test::DeterministicInteractiveByteStreamFake stream;
    app::StreamCallerTerminal terminal(stream);
    auto invalid = line_request("bad identity", 10);
    require(terminal.read_visible(invalid).outcome ==
            app::TerminalOutcome::invalid &&
        stream.read_observations().empty() &&
        stream.write_observations().empty(),
        "invalid terminal request touches no stream");
    stream.script_read_outcome(app::InteractiveStreamOutcome::failed);
    const auto failed = terminal.read_visible(line_request("read.failure", 10));
    require(failed.outcome == app::TerminalOutcome::terminal_failure &&
        failed.value.empty(), "stream read failure is infrastructure failure");

    test::DeterministicInteractiveByteStreamFake malformed_stream;
    malformed_stream.script_malformed_read(
        app::InteractiveStreamOutcome::completed);
    app::StreamCallerTerminal malformed_terminal(malformed_stream);
    const auto malformed = malformed_terminal.read_visible(
        line_request("read.malformed", 10));
    require(malformed.outcome == app::TerminalOutcome::terminal_failure &&
        malformed.value.empty(),
        "malformed stream result is infrastructure failure");
}

void fragmented_coalesced_newlines_and_paste() {
    test::DeterministicInteractiveByteStreamFake stream;
    stream.script_non_secret_text("Cal");
    stream.script_non_secret_text("ler\r");
    stream.script_non_secret_text("\nNext\nThird\r\nFourth\r");
    app::StreamCallerTerminal terminal(stream);

    const auto first = terminal.read_visible(line_request("first", 30));
    const auto second = terminal.read_visible(line_request("second", 30));
    const auto third = terminal.read_visible(line_request("third", 30));
    const auto fourth = terminal.read_visible(line_request("fourth", 30));
    require(first.value == "Caller" && second.value == "Next" &&
        third.value == "Third" && fourth.value == "Fourth",
        "fragmented and coalesced pasted lines preserve order");
    require(first.value.capacity() >= 30U,
        "visible input capacity is established before caller bytes");
    require(stream.output() ==
        "Input: Caller\r\nInput: Next\r\nInput: Third\r\nInput: Fourth\r\n",
        "CR LF and CRLF normalize to one caller CRLF");
    require(stream.pending_reads() == 0,
        "split CRLF LF cannot answer next prompt");

    test::DeterministicInteractiveByteStreamFake one_byte;
    for (const char value : std::string{"Bytewise\r\n"}) {
        one_byte.script_non_secret_bytes(
            bytes({static_cast<std::uint8_t>(value)}));
    }
    one_byte.script_non_secret_text("Next\r");
    app::StreamCallerTerminal bytewise(one_byte);
    const auto bytewise_first =
        bytewise.read_visible(line_request("bytewise", 20));
    const auto bytewise_second =
        bytewise.read_visible(line_request("bytewise.next", 20));
    require(bytewise_first.value == "Bytewise" &&
        bytewise_second.value == "Next" &&
        one_byte.read_observations().size() == 11U,
        "one-byte fragmentation and split CRLF are deterministic");
}

void editing_empty_and_bounds() {
    test::DeterministicInteractiveByteStreamFake edit_stream;
    edit_stream.script_non_secret_bytes(bytes(
        {'A', 'B', 0x08U, 'C', 0x7fU, 'D', '\r'}));
    app::StreamCallerTerminal editing(edit_stream);
    const auto edited = editing.read_visible(line_request("editing", 10));
    require(edited.value == "AD", "BS and DEL remove accepted bytes");
    require(edit_stream.output() ==
        "Input: AB\b \bC\b \bD\r\n",
        "visible editing emits destructive backspace");

    test::DeterministicInteractiveByteStreamFake empty_stream;
    empty_stream.script_non_secret_text("\rValue\r\n");
    app::StreamCallerTerminal empty(empty_stream);
    const auto reprompted =
        empty.read_visible(line_request("empty.reject", 10));
    require(reprompted.value == "Value" &&
        empty_stream.output() ==
            "Input: \r\nInput: Value\r\n",
        "rejected empty line re-prompts within one request");

    test::DeterministicInteractiveByteStreamFake default_stream;
    default_stream.script_non_secret_text("\n");
    app::StreamCallerTerminal default_terminal(default_stream);
    const auto defaulted = default_terminal.read_visible(
        line_request("empty.default", 10,
            app::EmptyInputPolicy::use_default));
    require(defaulted.outcome == app::TerminalOutcome::empty &&
        defaulted.value.empty(), "approved line default is typed empty");

    test::DeterministicInteractiveByteStreamFake exact_stream;
    exact_stream.script_non_secret_text("12345\r");
    app::StreamCallerTerminal exact(exact_stream);
    require(exact.read_visible(line_request("exact", 5)).value == "12345",
        "exact input bound succeeds");

    test::DeterministicInteractiveByteStreamFake long_stream;
    long_stream.script_non_secret_text("123456789\rNEXT\r");
    app::StreamCallerTerminal overlength(long_stream);
    const auto rejected =
        overlength.read_visible(line_request("overlength", 5));
    const auto next = overlength.read_visible(line_request("after.long", 10));
    require(rejected.outcome == app::TerminalOutcome::overlength &&
        rejected.value.empty(), "overlength is rejected without truncation");
    require(next.value == "NEXT",
        "overlength drains only its current logical line");
}

void invalid_controls_and_cancellation() {
    const std::array<std::uint8_t, 4> invalid{
        0x00U, 0x01U, 0x80U, 0xffU};
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_bytes(
            bytes({'A', invalid[index], '[', '3', '1', 'm', '\r',
                'O', 'K', '\r'}));
        app::StreamCallerTerminal terminal(stream);
        const auto rejected = terminal.read_visible(
            line_request("invalid." + std::to_string(index), 30));
        const auto next = terminal.read_visible(
            line_request("after.invalid." + std::to_string(index), 30));
        require(rejected.outcome == app::TerminalOutcome::invalid &&
            rejected.value.empty() && next.value == "OK",
            "unexpected control invalidates and drains current line");
    }

    test::DeterministicInteractiveByteStreamFake cancel_stream;
    cancel_stream.script_non_secret_bytes(
        bytes({'A', 'B', 0x11U, 'S', 'T', 'A', 'L', 'E', '\r'}));
    cancel_stream.script_non_secret_text("Fresh\r");
    app::StreamCallerTerminal cancelled(cancel_stream);
    const auto result =
        cancelled.read_visible(line_request("cancelled", 30));
    const auto fresh =
        cancelled.read_visible(line_request("after.cancel", 30));
    require(result.outcome == app::TerminalOutcome::cancelled &&
        result.value.empty() && fresh.value == "Fresh",
        "Ctrl+Q cancels and erases coalesced stale input");
}

void stop_outcomes_and_partial_erasure() {
    struct Case {
        app::InteractiveStreamOutcome stream;
        app::TerminalOutcome terminal;
    };
    const std::array<Case, 5> cases{{
        {app::InteractiveStreamOutcome::cancelled,
            app::TerminalOutcome::cancelled},
        {app::InteractiveStreamOutcome::timed_out,
            app::TerminalOutcome::timed_out},
        {app::InteractiveStreamOutcome::disconnected,
            app::TerminalOutcome::disconnected},
        {app::InteractiveStreamOutcome::shutdown_requested,
            app::TerminalOutcome::shutdown_requested},
        {app::InteractiveStreamOutcome::failed,
            app::TerminalOutcome::terminal_failure}}};

    for (std::size_t index = 0; index < cases.size(); ++index) {
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("Partial");
        stream.script_read_outcome(cases[index].stream);
        app::StreamCallerTerminal terminal(stream);
        const auto result = terminal.read_visible(
            line_request("stop." + std::to_string(index), 30));
        require(result.outcome == cases[index].terminal &&
            result.value.empty(),
            "visible partial input erased on stream stop");
    }

    test::DeterministicInteractiveByteStreamFake secret_stream;
    secret_stream.script_generated_secret_input(7, 2, false);
    secret_stream.script_read_outcome(
        app::InteractiveStreamOutcome::timed_out);
    app::StreamCallerTerminal secret_terminal(secret_stream);
    const auto secret = secret_terminal.read_secret(
        line_request("secret.timeout", 30));
    require(secret.outcome == app::TerminalOutcome::timed_out &&
        secret.value.empty() &&
        secret_stream.output() == "Input: " &&
        secret_stream.retained_secret_bytes() == 0U,
        "partial secret erased with no echo or fake retention");
}

void secret_input_and_secret_sink() {
    test::DeterministicInteractiveByteStreamFake stream;
    stream.script_generated_secret_input(12, 1);
    app::StreamCallerTerminal terminal(stream);
    auto secret =
        terminal.read_secret(line_request("secret.complete", 20));
    require(secret.outcome == app::TerminalOutcome::completed &&
        secret.value.size() == 12U, "fragmented secret completes");
    require(secret.value.capacity() >=
            app::kTerminalSecretStorageCapacity,
        "secret assembly is preallocated beyond SSO before input");
    require(stream.output() == "Input: \r\n" &&
        stream.output().find('X') == std::string::npos &&
        stream.retained_secret_bytes() == 0U,
        "secret characters have zero echo and zero fake retention");
    require(std::all_of(stream.read_observations().begin(),
            stream.read_observations().end(),
            [](const test::ByteStreamReadObservation& observation) {
                return observation.returned_bytes <=
                    app::kInteractiveStreamMaximumReadBytes;
            }),
        "secret observations retain counts only");
    app::explicitly_erase_terminal_secret(secret.value);

    test::DeterministicInteractiveByteStreamFake overlength_stream;
    overlength_stream.script_generated_secret_input(6, 2);
    app::StreamCallerTerminal overlength(overlength_stream);
    const auto rejected =
        overlength.read_secret(line_request("secret.long", 5));
    require(rejected.outcome == app::TerminalOutcome::overlength &&
        rejected.value.empty() &&
        overlength_stream.output() == "Input: \r\n",
        "secret overlength returns no payload and no secret echo");

    test::DeterministicInteractiveByteStreamFake edit_stream;
    edit_stream.script_generated_secret_input(3, 3, false);
    edit_stream.script_non_secret_bytes(bytes({0x08U, 0x7fU}));
    edit_stream.script_generated_secret_input(1, 1, false);
    edit_stream.script_non_secret_text("\r");
    app::StreamCallerTerminal edit_terminal(edit_stream);
    auto edited =
        edit_terminal.read_secret(line_request("secret.edit", 10));
    require(edited.outcome == app::TerminalOutcome::completed &&
        edited.value.size() == 2U &&
        edit_stream.output() == "Input: \r\n",
        "secret BS and DEL edit without echo");
    app::explicitly_erase_terminal_secret(edited.value);

    test::DeterministicInteractiveByteStreamFake masked_stream;
    masked_stream.script_non_secret_bytes(bytes(
        {'A', 'B', 0x08U, 'C', 0x7fU, 'D', '\r'}));
    app::StreamCallerTerminal masked_terminal(masked_stream,
        {app::SecretInputEchoMode::masked});
    auto masked = masked_terminal.read_secret(
        line_request("secret.masked", 10));
    require(masked.outcome == app::TerminalOutcome::completed &&
        masked.value == "AD" &&
        masked_stream.output() == "Input: **\b \b*\b \b*\r\n" &&
        masked_stream.output().find('A') == std::string::npos &&
        masked_stream.output().find('D') == std::string::npos,
        "opt-in secret masking echoes one star per accepted byte and destructively erases edited masks without revealing input");
    app::explicitly_erase_terminal_secret(masked.value);

    test::DeterministicInteractiveByteStreamFake output_failure_stream;
    output_failure_stream.script_generated_secret_input(5);
    output_failure_stream.script_write(
        app::InteractiveStreamOutcome::completed, 7);
    output_failure_stream.script_write(
        app::InteractiveStreamOutcome::failed, 1);
    app::StreamCallerTerminal output_failure_terminal(
        output_failure_stream);
    const auto failed = output_failure_terminal.read_secret(
        line_request("secret.output_failure", 10));
    require(failed.outcome == app::TerminalOutcome::output_failure &&
        failed.value.empty() &&
        output_failure_stream.output() == "Input: \r",
        "completed secret is erased when newline output becomes uncertain");
}

void stream_exception_containment_and_erasure() {
    test::DeterministicInteractiveByteStreamFake partial_secret_stream;
    partial_secret_stream.script_generated_secret_input(5, 5, false);
    partial_secret_stream.script_read_exception();
    app::StreamCallerTerminal partial_secret_terminal(
        partial_secret_stream);
    const auto partial_secret = partial_secret_terminal.read_secret(
        line_request("exception.secret.read", 20));
    require(partial_secret.outcome ==
            app::TerminalOutcome::terminal_failure &&
        partial_secret.value.empty() &&
        partial_secret_stream.output() == "Input: " &&
        partial_secret_stream.output().find('X') == std::string::npos &&
        partial_secret_stream.retained_secret_bytes() == 0U,
        "throwing read erases partial secret and returns closed failure");
    require(partial_secret_stream.read_observations().size() == 2U &&
        partial_secret_stream.read_observations().back().outcome ==
            app::InteractiveStreamOutcome::failed,
        "throwing read observation retains outcome and counts only");

    test::DeterministicInteractiveByteStreamFake secret_write_stream;
    secret_write_stream.script_generated_secret_input(5);
    secret_write_stream.script_write(
        app::InteractiveStreamOutcome::completed, 7);
    secret_write_stream.script_write_exception();
    app::StreamCallerTerminal secret_write_terminal(
        secret_write_stream);
    const auto secret_write = secret_write_terminal.read_secret(
        line_request("exception.secret.write", 20));
    require(secret_write.outcome ==
            app::TerminalOutcome::output_failure &&
        secret_write.value.empty() &&
        secret_write_stream.output() == "Input: " &&
        secret_write_stream.output().find('X') == std::string::npos &&
        secret_write_stream.retained_secret_bytes() == 0U,
        "throwing secret-newline write erases completed secret");
    secret_write_stream.script_non_secret_text("\nFresh\r");
    const auto after_secret_write =
        secret_write_terminal.read_visible(
            line_request("exception.secret.write.after", 20));
    require(after_secret_write.value == "Fresh" &&
        secret_write_stream.output() ==
            "Input: Input: \r\nInput: Fresh\r\n",
        "write exception resets CRLF carry before later input");

    test::DeterministicInteractiveByteStreamFake visible_stream;
    visible_stream.script_non_secret_text("Visible");
    visible_stream.script_read_exception();
    app::StreamCallerTerminal visible_terminal(visible_stream);
    const auto visible = visible_terminal.read_visible(
        line_request("exception.visible.read", 20));
    require(visible.outcome ==
            app::TerminalOutcome::terminal_failure &&
        visible.value.empty() &&
        visible_stream.output() == "Input: Visible",
        "throwing read erases partial visible input and returns closed failure");

    test::DeterministicInteractiveByteStreamFake key_read_stream;
    key_read_stream.script_read_exception();
    app::StreamCallerTerminal key_read_terminal(key_read_stream);
    const auto key_read = key_read_terminal.read_key(
        {"exception.key.read", "Choose: ", {'Y', 'N'}, std::nullopt});
    require(key_read.outcome == app::TerminalOutcome::terminal_failure &&
        !key_read.key.has_value(),
        "throwing key read is contained as terminal failure");

    test::DeterministicInteractiveByteStreamFake key_write_stream;
    key_write_stream.script_write_exception();
    app::StreamCallerTerminal key_write_terminal(key_write_stream);
    const auto key_write = key_write_terminal.read_key(
        {"exception.key.write", "Choose: ", {'Y', 'N'}, std::nullopt});
    require(key_write.outcome == app::TerminalOutcome::output_failure &&
        !key_write.key.has_value() &&
        key_write_stream.read_observations().empty(),
        "throwing key-prompt write is contained before input");

    test::DeterministicInteractiveByteStreamFake pause_read_stream;
    pause_read_stream.script_read_exception();
    app::StreamCallerTerminal pause_read_terminal(pause_read_stream);
    require(pause_read_terminal.pause({"exception.pause.read"}) ==
            app::TerminalOutcome::terminal_failure,
        "throwing pause read is contained as terminal failure");

    test::DeterministicInteractiveByteStreamFake pause_write_stream;
    pause_write_stream.script_non_secret_text("X");
    pause_write_stream.script_write_exception();
    app::StreamCallerTerminal pause_write_terminal(pause_write_stream);
    require(pause_write_terminal.pause({"exception.pause.write"}) ==
            app::TerminalOutcome::output_failure,
        "throwing pause BEL write is contained as output failure");

    test::DeterministicInteractiveByteStreamFake presentation_stream;
    presentation_stream.script_non_secret_text("YOld\r");
    app::StreamCallerTerminal presentation_terminal(
        presentation_stream);
    const auto selected = presentation_terminal.read_key(
        {"exception.pending.key", "Choose: ", {'Y', 'N'}, std::nullopt});
    require(selected.has_key() && selected.key == 'Y',
        "presentation exception setup leaves legitimate typeahead pending");
    presentation_stream.script_write_exception();
    require(presentation_terminal.present(
                {"exception.presentation", "Ordinary"}) ==
            app::TerminalOutcome::output_failure &&
        presentation_stream.write_observations().back().accepted_bytes == 0U,
        "ordinary presentation contains throwing write as output failure");
    presentation_stream.script_non_secret_text("Fresh\r");
    const auto after_presentation =
        presentation_terminal.read_visible(
            line_request("exception.presentation.after", 20));
    require(after_presentation.value == "Fresh",
        "throwing presentation write erases pending coalesced input");

    test::DeterministicInteractiveByteStreamFake uncertain_stream;
    uncertain_stream.script_write_exception(3);
    uncertain_stream.script_write(
        app::InteractiveStreamOutcome::completed, 64);
    app::StreamCallerTerminal uncertain_terminal(uncertain_stream);
    require(uncertain_terminal.present(
                {"exception.partial_write", "abcdef"}) ==
            app::TerminalOutcome::output_failure &&
        uncertain_stream.output() == "abc" &&
        uncertain_stream.write_observations().size() == 1U &&
        uncertain_stream.pending_writes() == 1U,
        "throwing uncertain partial write is never replayed");
}

void key_choices_and_pause() {
    test::DeterministicInteractiveByteStreamFake key_stream;
    key_stream.script_non_secret_text("xyn");
    app::StreamCallerTerminal keys(key_stream);
    const auto selected = keys.read_key(
        {"key.choice", "Choose: ", {'Y', 'y', 'N', 'n'}, std::nullopt});
    require(selected.outcome == app::TerminalOutcome::completed &&
        selected.key == 'Y', "alphabetic key is canonicalized");
    require(key_stream.output() == "Choose: \aY\r\n",
        "disallowed printable key emits BEL and remains in request");

    test::DeterministicInteractiveByteStreamFake default_stream;
    default_stream.script_non_secret_text("\r\n");
    app::StreamCallerTerminal default_terminal(default_stream);
    const auto defaulted = default_terminal.read_key(
        {"key.default", "Default: ", {'Y', 'N'}, 'Y'});
    require(defaulted.outcome == app::TerminalOutcome::empty &&
        defaulted.key == 'Y' &&
        default_stream.output() == "Default: \r\n",
        "bare Enter returns exact approved default");

    test::DeterministicInteractiveByteStreamFake no_default_stream;
    no_default_stream.script_non_secret_text("\n");
    app::StreamCallerTerminal no_default(no_default_stream);
    const auto empty = no_default.read_key(
        {"key.no_default", "Choose: ", {'Y', 'N'}, std::nullopt});
    require(empty.outcome == app::TerminalOutcome::invalid &&
        !empty.key.has_value(), "bare Enter without default is invalid");

    test::DeterministicInteractiveByteStreamFake pause_stream;
    pause_stream.script_non_secret_bytes(
        bytes({'X', 0x08U, 0x7fU, '\r'}));
    app::StreamCallerTerminal pause_terminal(pause_stream);
    require(pause_terminal.pause({"pause.enter"}) ==
            app::TerminalOutcome::completed &&
        pause_stream.output() == "\a\r\n",
        "pause ignores editing, BELs printable input, and completes on Enter");

    test::DeterministicInteractiveByteStreamFake cancel_stream;
    cancel_stream.script_non_secret_bytes(bytes({0x11U}));
    app::StreamCallerTerminal cancel_terminal(cancel_stream);
    require(cancel_terminal.pause({"pause.cancel"}) ==
            app::TerminalOutcome::cancelled,
        "Ctrl+Q cancels pause");

    test::DeterministicInteractiveByteStreamFake invalid_key_stream;
    invalid_key_stream.script_non_secret_bytes(
        bytes({0x1bU, '[', 'A', '\r'}));
    app::StreamCallerTerminal invalid_key(invalid_key_stream);
    const auto invalid_key_result = invalid_key.read_key(
        {"key.control", "Choose: ", {'Y', 'N'}, 'Y'});
    require(invalid_key_result.outcome == app::TerminalOutcome::empty &&
        invalid_key_result.key == 'Y',
        "arrow ignored; subsequent real Enter selects the explicit default");

    test::DeterministicInteractiveByteStreamFake invalid_pause_stream;
    invalid_pause_stream.script_non_secret_bytes(
        bytes({0x00U, 'X', '\r'}));
    app::StreamCallerTerminal invalid_pause(invalid_pause_stream);
    require(invalid_pause.pause({"pause.control"}) ==
            app::TerminalOutcome::invalid,
        "NUL invalidates pause request");

    test::DeterministicInteractiveByteStreamFake coalesced_stream;
    coalesced_stream.script_non_secret_text("Y\r\nTyped\r");
    app::StreamCallerTerminal coalesced_terminal(coalesced_stream);
    const auto coalesced_key = coalesced_terminal.read_key(
        {"key.coalesced", "Choose: ", {'Y', 'N'}, std::nullopt});
    const auto coalesced_line = coalesced_terminal.read_visible(
        line_request("key.coalesced.next", 20));
    require(coalesced_key.has_key() && coalesced_key.key == 'Y' &&
        coalesced_line.value == "Typed",
        "accepted hotkey discards one coalesced Enter before next prompt");

    test::DeterministicInteractiveByteStreamFake typeahead_stream;
    typeahead_stream.script_non_secret_text("YTyped\r");
    app::StreamCallerTerminal typeahead_terminal(typeahead_stream);
    const auto typeahead_key = typeahead_terminal.read_key(
        {"key.typeahead", "Choose: ", {'Y', 'N'}, std::nullopt});
    const auto typeahead_line = typeahead_terminal.read_visible(
        line_request("key.typeahead.next", 20));
    require(typeahead_key.has_key() && typeahead_key.key == 'Y' &&
        typeahead_line.value == "Typed",
        "non-newline typeahead after immediate hotkey is preserved");
}

void output_normalization_short_writes_and_failures() {
    test::DeterministicInteractiveByteStreamFake normalized_stream;
    app::StreamCallerTerminal normalized(normalized_stream);
    require(normalized.present(
            {"output.newlines", "A\nB\rC\r\nD"}) ==
            app::TerminalOutcome::completed &&
        normalized_stream.output() == "A\r\nB\r\nC\r\nD",
        "logical newlines normalize exactly once");

    test::DeterministicInteractiveByteStreamFake short_stream;
    short_stream.script_write(
        app::InteractiveStreamOutcome::completed, 1);
    short_stream.script_write(
        app::InteractiveStreamOutcome::completed, 2);
    short_stream.script_write(
        app::InteractiveStreamOutcome::completed, 1);
    app::StreamCallerTerminal short_writes(short_stream);
    require(short_writes.present({"output.short", "abcdef"}) ==
            app::TerminalOutcome::completed &&
        short_stream.output() == "abcdef" &&
        short_stream.write_observations().size() == 4U,
        "bounded write-all completes fragmented short writes");

    test::DeterministicInteractiveByteStreamFake failed_stream;
    failed_stream.script_write(
        app::InteractiveStreamOutcome::failed, 3);
    app::StreamCallerTerminal partial_failure(failed_stream);
    require(partial_failure.present(
            {"output.partial", "abcdef"}) ==
            app::TerminalOutcome::output_failure &&
        failed_stream.output() == "abc" &&
        failed_stream.write_observations().size() == 1U,
        "uncertain partial output fails without replay");

    test::DeterministicInteractiveByteStreamFake timeout_stream;
    timeout_stream.script_write(
        app::InteractiveStreamOutcome::timed_out, 0);
    app::StreamCallerTerminal timeout(timeout_stream);
    require(timeout.present({"output.timeout", "abcdef"}) ==
            app::TerminalOutcome::timed_out &&
        timeout_stream.output().empty(),
        "zero-byte output stop retains exact typed outcome");

    test::DeterministicInteractiveByteStreamFake partial_timeout_stream;
    partial_timeout_stream.script_write(
        app::InteractiveStreamOutcome::timed_out, 1);
    app::StreamCallerTerminal partial_timeout(partial_timeout_stream);
    require(partial_timeout.present(
            {"output.partial_timeout", "abcdef"}) ==
            app::TerminalOutcome::output_failure &&
        partial_timeout_stream.output() == "a",
        "partial stop is uncertain output failure");

    const std::array<std::uint8_t, 8> prohibited{
        0x00U, 0x07U, 0x1bU, 0x01U, 0x7fU, 0x80U, 0x9fU, 0xffU};
    for (const auto value : prohibited) {
        test::DeterministicInteractiveByteStreamFake rejected_stream;
        app::StreamCallerTerminal rejected_terminal(rejected_stream);
        std::string text{"Safe"};
        text.push_back(static_cast<char>(value));
        text += "unsafe";
        require(rejected_terminal.present(
                    {"output.control", std::move(text)}) ==
                app::TerminalOutcome::output_failure &&
            rejected_stream.output().empty() &&
            rejected_stream.write_observations().empty(),
            "ordinary output rejects control/high-bit bytes before writing");
    }

    test::DeterministicInteractiveByteStreamFake ordinary_stream;
    app::StreamCallerTerminal ordinary_terminal(ordinary_stream);
    require(ordinary_terminal.present(
                {"output.ordinary", "Built-in Phase 3A text\r\nstill works.\n"}) ==
            app::TerminalOutcome::completed &&
        ordinary_stream.output() ==
            "Built-in Phase 3A text\r\nstill works.\r\n",
        "ordinary printable Phase 3A text and logical newlines remain valid");
}

void infrastructure_failure_stops_workflows() {
    const std::array<StreamFailureForm, 3> forms{
        StreamFailureForm::explicit_failure,
        StreamFailureForm::malformed_result,
        StreamFailureForm::throwing_read};

    for (const auto form : forms) {
        ConcreteWorkflowFixture fixture;
        fixture.accounts.policy_result =
            stored(workflow_policy(10, 999));
        fixture.accounts.account_result =
            stored(workflow_account("Caller Name", 10));
        fixture.names.results.push_back(
            stored(existing_resolution()));
        fixture.stream.script_non_secret_text(
            "Y\r\nCaller\rName\r");
        script_stream_failure(fixture.stream, form);

        auto active = workflow_session();
        app::CancellationSource cancellation;
        auto workflow = fixture.existing_workflow();
        const auto result = workflow.run(
            workflow_configuration(), active,
            {"192.0.2.7", identifier(900), std::nullopt},
            cancellation.token());

        require(result.code == app::Phase3aWorkflowCode::terminal_failure &&
            !result.ready.has_value(),
            "existing-caller stream infrastructure failure stops workflow");
        require(fixture.credentials.observations.empty() &&
            rate_failures(fixture.rates) == 0U &&
            fixture.phase3a_storage.enqueue_requests.empty(),
            "existing-caller infrastructure failure records no auth failure");
        require(fixture.admission.requests.empty() &&
            fixture.session_commit.requests.empty(),
            "existing-caller infrastructure failure cannot admit or commit");
    }

    for (const auto form : forms) {
        ConcreteWorkflowFixture fixture;
        fixture.accounts.policy_result =
            stored(workflow_policy(10, 999));
        fixture.names.results.push_back(
            stored(registerable_resolution()));
        script_stream_failure(fixture.stream, form);

        auto active = workflow_session();
        app::CancellationSource cancellation;
        auto workflow = fixture.registration_workflow();
        const auto result = workflow.run(
            workflow_configuration(), active,
            {{"New Caller", app::GraphicsMode::ansi},
                "192.0.2.7", identifier(901)},
            cancellation.token());

        require(result.code == app::Phase3aWorkflowCode::terminal_failure &&
            !result.ready.has_value(),
            "registration-password infrastructure failure stops workflow");
        require(fixture.credentials.observations.empty() &&
            fixture.registration_commit.observations.empty() &&
            rate_failures(fixture.rates) == 0U &&
            fixture.phase3a_storage.enqueue_requests.empty(),
            "registration-password failure creates no credential or durable effect");
        require(fixture.admission.requests.empty() &&
            fixture.session_commit.requests.empty(),
            "registration-password failure cannot admit or commit session");
    }

    for (const auto form : forms) {
        ConcreteWorkflowFixture fixture;
        fixture.accounts.policy_result =
            stored(workflow_policy(10, 10));
        fixture.accounts.account_result =
            stored(workflow_account("New Caller", 10));
        fixture.names.results.push_back(
            stored(registerable_resolution()));
        fixture.names.results.push_back(
            stored(registerable_resolution()));
        fixture.stream.script_generated_secret_input(12, 3);
        fixture.stream.script_generated_secret_input(12, 4);
        fixture.stream.script_non_secret_text("C\r\n");
        script_stream_failure(fixture.stream, form);
        fixture.registration_commit.results.push_back(
            {app::StorageStatus::success(), 42, 7, false});

        auto active = workflow_session();
        app::CancellationSource cancellation;
        auto workflow = fixture.registration_workflow();
        const auto result = workflow.run(
            workflow_configuration(10, 10), active,
            {{"New Caller", app::GraphicsMode::ansi},
                "192.0.2.7", identifier(902)},
            cancellation.token());

        require(result.code == app::Phase3aWorkflowCode::terminal_failure &&
            !result.ready.has_value(),
            "post-registration SY infrastructure failure stops workflow");
        require(!fixture.registration_commit.observations.empty(),
            "post-registration SY test reaches registration commit");
        require(fixture.registration_commit.observations.size() == 1U,
            "post-registration SY infrastructure failure preserves one commit");
        require(fixture.credentials.observations.size() == 1U &&
            fixture.credentials.observations.front().operation ==
                "create_registration" &&
            rate_failures(fixture.rates) == 0U &&
            fixture.phase3a_storage.enqueue_requests.empty(),
            "post-registration SY infrastructure failure records no SY failure or notice");
        require(fixture.admission.requests.empty() &&
            fixture.session_commit.requests.empty(),
            "post-registration SY infrastructure failure cannot admit or commit session");
    }
}

void c4_prompt_input_roles_and_clear() {
    const std::string system = "\x1b[0;37;40m", input = "\x1b[0;1;36;40m";
    for (const bool ansi : {false, true}) {
        test::DeterministicInteractiveByteStreamFake stream;
        app::StreamCallerTerminal terminal(stream, {app::SecretInputEchoMode::masked});
        require(terminal.set_text_style(ansi ? app::TerminalTextStyle::ogsf_ansi : app::TerminalTextStyle::plain)
            == app::TerminalOutcome::completed, "C4 explicit per-terminal text style selected");
        stream.script_non_secret_text("Mr.\r");
        require(terminal.read_visible({"c4.first", "Enter your first name: ", 28,
            app::EmptyInputPolicy::reject}).value == "Mr.", "C4 visible name accepted");
        stream.script_generated_secret_input(4, 2, false);
        stream.script_non_secret_bytes({'\b', '\r'});
        auto secret = terminal.read_secret({"c4.password", "\nEnter new password: ", 128,
            app::EmptyInputPolicy::reject});
        require(secret.has_value() && secret.value.size() == 3, "C4 masked backspace removes exactly one accepted character");
        app::explicitly_erase_terminal_secret(secret.value);
        const auto s = ansi ? system : "", i = ansi ? input : "";
        const auto expected = s + "Enter your first name: " + i + "Mr." + s + "\r\n" +
            s + "\r\nEnter new password: " + i + "****\b \b" + s + "\r\n";
        require(stream.output() == expected && stream.retained_secret_bytes() == 0,
            "C4 exact prompt 07/input-mask 0B transcript and erasure, or escape-free ASCII");
        stream.script_non_secret_text("?\r");
        const auto bad = terminal.read_key({"c4.review", "\n[R]eview/Edit your settings\n"
            "[C]onfirm settings\n[Q]uit/Exit new user registration\n\nSelection: ",
            {'R','C','Q'}, {}, true});
        require(bad.outcome == app::TerminalOutcome::invalid && !bad.key,
            "C4 invalid review key is reported for explicit workflow re-prompt");
        const auto before_clear = stream.output().size();
        require(terminal.clear_screen() == app::TerminalOutcome::completed, "C4 clear completes");
        std::string ascii_clear;
        for (int n = 0; n < 25; ++n) ascii_clear += "\r\n";
        require(stream.output().substr(before_clear) == (ansi ? system + "\x1b[2J\x1b[H" : ascii_clear),
            "C4 ANSI clear/home versus exactly 25 escape-free ASCII rows");
        require(ansi || stream.output().find('\x1b') == std::string::npos, "C4 ASCII never emits ANSI styling");
    }
    // Target semantic output rather than the number of writes: system styling
    // and prompt text may legitimately be batched into one transport write.
    class FailingOutput final : public app::IInteractiveSessionByteStream {
    public:
        test::DeterministicInteractiveByteStreamFake stream;
        std::string target;
        std::size_t accepted_before_throw{0};
        bool injected{false};
        app::InteractiveStreamReadResult read_some(std::size_t maximum) override {
            return stream.read_some(maximum);
        }
        app::InteractiveStreamWriteResult write_some(std::string_view output) override {
            if (!injected && output.find(target) != std::string_view::npos) {
                injected = true;
                stream.script_write_exception(accepted_before_throw);
            }
            return stream.write_some(output);
        }
    };
    for (const bool ansi : {false, true}) {
        for (const auto& target : {std::string("Password: "), input, std::string("*")}) {
            if (!ansi && target == input) continue;
            for (const std::size_t accepted : {std::size_t{0}, std::size_t{1}}) {
                FailingOutput fault;
                fault.target = target;
                fault.accepted_before_throw = accepted;
                app::StreamCallerTerminal terminal(fault, {app::SecretInputEchoMode::masked});
                terminal.set_text_style(ansi ? app::TerminalTextStyle::ogsf_ansi : app::TerminalTextStyle::plain);
                fault.stream.script_generated_secret_input(10, 1, false);
                const auto result = terminal.read_secret({"c4.failed", "Password: ", 128, app::EmptyInputPolicy::reject});
                require(fault.injected && result.outcome == app::TerminalOutcome::output_failure && result.value.empty(),
                    "semantic prompt/input-style/masked-echo exception returns output failure with no secret payload");
                require(fault.stream.output().find('X') == std::string::npos && fault.stream.retained_secret_bytes() == 0,
                    "failed output never reveals or retains generated secret bytes");
                if (target != "*") {
                    require(fault.stream.read_observations().empty(),
                        "failed prompt or input style never consumes a secret, including partial output before failure");
                } else {
                    require(fault.stream.read_observations().size() == 1 &&
                        fault.stream.read_observations().front().returned_bytes == 1,
                        "first mask-echo failure erases partial input and stops further secret reads");
                }
            }
        }
    }
}

void caller_color_roles() {
    for(const bool ansi:{false,true}) {
        std::uint8_t chosen=0;
        test::DeterministicInteractiveByteStreamFake stream;
        app::StreamCallerTerminal terminal(stream,{app::SecretInputEchoMode::masked,7,11,&chosen});
        terminal.set_text_style(ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
        require(terminal.present({"prelogin.identity","Before"})==app::TerminalOutcome::completed,"default pre-auth text");
        chosen=12;
        require(terminal.present({"caller.greeting","After",app::TerminalPresentationRole::caller_value})==app::TerminalOutcome::completed,"saved caller text");
        require(terminal.present({"caller.information","System"})==app::TerminalOutcome::completed,"system-role text keeps its color despite caller preference");
        require(terminal.present({"caller.failed_login_notice","Warning"})==app::TerminalOutcome::completed,"separate warning role");
        chosen=9;
        require(terminal.present({"caller.information","Changed",app::TerminalPresentationRole::caller_value})==app::TerminalOutcome::completed,"live changed caller text");
        chosen=0;
        require(terminal.present({"caller.information","Default",app::TerminalPresentationRole::caller_value})==app::TerminalOutcome::completed,"restore BBS default");
        const std::string gray=ansi?"\x1b[0;37;40m":"",red=ansi?"\x1b[0;1;31;40m":"",blue=ansi?"\x1b[0;1;34;40m":"";
        const std::string notification=ansi?"\x1b[0;1;37;40m":"";
        require(stream.output()==gray+"Before"+red+"After"+gray+"System"+notification+"Warning"+blue+"Changed"+gray+"Default",
            "explicit caller text follows preference, system text stays gray, failed-login notification stays white and ASCII has no escapes");
    }
}
}  // namespace

#include "terminal_navigation_checks.inc"

int main() {
    try {
        std::cout << "phase3a-window6a stream-result-contracts\n";
        stream_result_contracts();
        std::cout << "phase3a-window6a fragmented-coalesced-newlines-paste\n";
        fragmented_coalesced_newlines_and_paste();
        std::cout << "phase3a-window6a editing-empty-bounds\n";
        editing_empty_and_bounds();
        std::cout << "phase3a-window6a invalid-controls-cancellation\n";
        invalid_controls_and_cancellation();
        std::cout << "phase3a-window6a stop-outcomes-partial-erasure\n";
        stop_outcomes_and_partial_erasure();
        std::cout << "phase3a-window6a secret-input-secret-sink\n";
        secret_input_and_secret_sink();
        std::cout << "phase3a-window6a stream-exception-containment-erasure\n";
        stream_exception_containment_and_erasure();
        std::cout << "phase3a-window6a key-choices-pause\n";
        key_choices_and_pause();
        prompt_navigation_checks();
        std::cout << "phase3a-window6a output-normalization-writes\n";
        output_normalization_short_writes_and_failures();
        std::cout << "phase3a-window6a infrastructure-failure-workflows\n";
        infrastructure_failure_stops_workflows();
        c4_prompt_input_roles_and_clear();
        caller_color_roles();
        std::cout << "all SFX Phase 3A Window 6A terminal conformance tests passed assertions="
                  << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Phase 3A Window 6A terminal conformance test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
