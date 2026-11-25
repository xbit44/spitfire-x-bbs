#include "sfx/application/display.hpp"
#include "sfx/application/terminal_stream.hpp"
#include "sfx/test/terminal_stream_fake.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace app = sfx::application;
namespace test = sfx::test;

std::size_t assertions = 0;

void require(const bool condition, const char* const message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    return value;
}

class FakeAssetReader final : public app::ITrustedDisplayAssetReader {
public:
    struct Entry {
        app::TrustedDisplayAssetReadCode code{
            app::TrustedDisplayAssetReadCode::available};
        std::string bytes;
        std::string stable_identity;
    };

    void put(
        std::string identity,
        std::string bytes,
        std::string stable_identity = {}) {
        const auto normalized = upper(std::move(identity));
        entries[normalized] = {app::TrustedDisplayAssetReadCode::available,
            std::move(bytes),
            stable_identity.empty() ? "file:" + normalized
                                    : std::move(stable_identity)};
    }

    void put_code(
        std::string identity,
        const app::TrustedDisplayAssetReadCode code) {
        entries[upper(std::move(identity))] = {code, {}, {}};
    }

    app::TrustedDisplayAssetReadResult read_asset(
        const std::string_view logical_identity,
        const std::size_t maximum_bytes) override {
        requests.push_back({std::string(logical_identity), maximum_bytes});
        if (throw_on_read) throw std::runtime_error("fixed reader exception");
        const auto normalized = upper(std::string(logical_identity));
        const auto found = entries.find(normalized);
        if (found == entries.end()) {
            return {app::TrustedDisplayAssetReadCode::not_found, {}, {}, {}};
        }
        if (found->second.code !=
            app::TrustedDisplayAssetReadCode::available) {
            return {found->second.code, {}, {}, {}};
        }
        if (found->second.bytes.size() > maximum_bytes) {
            return {app::TrustedDisplayAssetReadCode::invalid, {}, {}, {}};
        }
        return {app::TrustedDisplayAssetReadCode::available, normalized,
            found->second.stable_identity, found->second.bytes};
    }

    struct Request {
        std::string identity;
        std::size_t maximum_bytes{0};
    };

    std::map<std::string, Entry> entries;
    std::vector<Request> requests;
    bool throw_on_read{false};
};

app::TrustedDisplayRenderContext full_context() {
    app::TrustedDisplayRenderContext context;
    context.bbs_name = "Spitfire NG";
    context.sysop = "Sysop";
    context.version = "1.0";
    context.date = "2026-08-29";
    context.time = "12:34:56";
    context.datetime = "2026-08-29 12:34:56";
    context.node = "7";
    context.nodes = "128";
    context.transport = "SSH";
    context.columns = "80";
    context.rows = "25";
    context.first_name = "Caller";
    context.city_state = "Somewhere";
    context.phone_number = "555-0100";
    context.name = "Caller Name";
    context.alias = "Caller Name";
    context.email = "caller@example.test";
    context.security_level = "10";
    context.logons = "42";
    context.last_call = "2026-08-28 10:00:00";
    context.time_on = "3661";
    context.protocol = "ZMODEM";
    context.ip_address = "192.0.2.10";
    context.password_age = "5";
    return context;
}

app::TrustedDisplaySelectionRequest request(
    const app::TrustedDisplayIdentity identity,
    const app::GraphicsMode graphics,
    std::optional<std::uint16_t> security = std::nullopt) {
    return {identity, graphics, security, full_context()};
}

std::string output_bytes(
    const app::RenderedTrustedDisplayProgram& program) {
    std::string result;
    for (const auto& operation : program.terminal_program.operations) {
        if (operation.kind == app::TerminalDisplayOperationKind::cp437_text ||
            operation.kind ==
                app::TerminalDisplayOperationKind::ansi_sequence) {
            result += operation.bytes;
        } else if (operation.kind ==
                app::TerminalDisplayOperationKind::logical_newline ||
            operation.kind ==
                app::TerminalDisplayOperationKind::paging_newline) {
            result += "\r\n";
        }
    }
    return result;
}

std::size_t operations_of(
    const app::RenderedTrustedDisplayProgram& program,
    const app::TerminalDisplayOperationKind kind) {
    return static_cast<std::size_t>(std::count_if(
        program.terminal_program.operations.begin(),
        program.terminal_program.operations.end(),
        [kind](const auto& operation) { return operation.kind == kind; }));
}

constexpr std::string_view kAutomaticPagingPrompt =
    "MORE: <S>top, <N>onstop, < ENTER > to continue? ";
constexpr std::string_view kForcedPagingPrompt = "< ENTER > ";
constexpr std::uint16_t kPagingTestPageLength = 10U;

std::size_t occurrences(
    const std::string_view haystack,
    const std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = haystack.find(needle, offset)) !=
        std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

void add_rows(
    app::TerminalDisplayProgram& program,
    const std::size_t count,
    const std::string_view label = "R") {
    for (std::size_t index = 0; index < count; ++index) {
        const std::string text =
            std::string(label) + std::to_string(index + 1U);
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::cp437_text, text});
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_newline, {}});
        program.output_byte_count += text.size() + 2U;
    }
}

void add_tail(
    app::TerminalDisplayProgram& program,
    const std::string_view text = "TAIL") {
    program.operations.push_back(
        {app::TerminalDisplayOperationKind::cp437_text, std::string(text)});
    program.output_byte_count += text.size();
}

std::string program_signature(
    const app::RenderedTrustedDisplayProgram& program) {
    std::string signature =
        program.selected_logical_asset + "|" +
        std::to_string(program.terminal_program.output_byte_count) + "|";
    for (const auto& operation : program.terminal_program.operations) {
        signature += std::to_string(
            static_cast<unsigned int>(operation.kind));
        signature.push_back(':');
        signature += operation.bytes;
        signature.push_back('|');
    }
    return signature;
}

void selection_and_fallback() {
    {
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC", "plain\n");
        reader.put("WELCOME1.ANS", "\x1b[31mansi\x1b[0m\n");
        app::TrustedDisplaySelector selector(reader);
        const auto ascii = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        const auto ansi = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ansi));
        require(ascii.available() && ansi.available(),
            "selected graphics variants are available");
        require(ascii.program->selected_logical_asset == "WELCOME1.ASC" &&
                ansi.program->selected_logical_asset == "WELCOME1.ANS",
            "selected graphics variants use exact unsuffixed names");
        require(reader.requests.size() == 2U &&
                reader.requests[0].maximum_bytes ==
                    app::kMaximumTrustedDisplaySourceBytes,
            "selection uses one bounded asset read per unsuffixed candidate");
    }
    {
        FakeAssetReader reader;
        app::TrustedDisplaySelector selector(reader);
        const auto missing = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(missing.code == app::TrustedDisplaySelectionCode::not_found &&
                !missing.available(),
            "missing optional display is typed not-found");
        reader.put_code("WELCOME1.ASC",
            app::TrustedDisplayAssetReadCode::unavailable);
        const auto unavailable = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(unavailable.code ==
                app::TrustedDisplaySelectionCode::unavailable,
            "unreadable display is typed unavailable");
        reader.put("WELCOME1.ASC", std::string("bad\0asset", 9));
        const auto malformed = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(malformed.code == app::TrustedDisplaySelectionCode::invalid,
            "malformed display is invalid before output");
        reader.put("WELCOME1.ASC",
            std::string(app::kMaximumTrustedDisplaySourceBytes + 1U, 'X'));
        const auto oversized = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(oversized.code == app::TrustedDisplaySelectionCode::invalid,
            "oversized source is invalid");
    }
    {
        FakeAssetReader reader;
        reader.put("NEWUSER10.ANS", "exact");
        reader.put("NEWUSER.ANS", "fallback");
        app::TrustedDisplaySelector selector(reader);
        const auto exact = selector.select(request(
            app::TrustedDisplayIdentity::newuser,
            app::GraphicsMode::ansi, std::uint16_t{10}));
        require(exact.available() && exact.program->exact_security_match &&
                exact.program->selected_logical_asset == "NEWUSER10.ANS" &&
                output_bytes(*exact.program) == "exact",
            "NEWUSER exact-security candidate wins");

        reader.entries.erase("NEWUSER10.ANS");
        const auto fallback = selector.select(request(
            app::TrustedDisplayIdentity::newuser,
            app::GraphicsMode::ansi, std::uint16_t{10}));
        require(fallback.available() &&
                !fallback.program->exact_security_match &&
                fallback.program->selected_logical_asset == "NEWUSER.ANS",
            "missing exact-security NEWUSER uses unsuffixed fallback");

        reader.put("NEWUSER10.ANS", "@PASSWORD@");
        const auto invalid_exact = selector.select(request(
            app::TrustedDisplayIdentity::newuser,
            app::GraphicsMode::ansi, std::uint16_t{10}));
        require(invalid_exact.available() &&
                !invalid_exact.program->exact_security_match &&
                output_bytes(*invalid_exact.program) == "fallback",
            "invalid exact-security NEWUSER safely uses valid fallback");
    }
    {
        FakeAssetReader reader;
        reader.put("SFPRELOG.ASC", "prelogin");
        app::TrustedDisplaySelector selector(reader);
        auto valid = request(app::TrustedDisplayIdentity::sfprelog,
            app::GraphicsMode::ascii);
        require(selector.select(valid).available(),
            "SFPRELOG selects exact ASCII-only identity");
        valid.graphics = app::GraphicsMode::ansi;
        require(selector.select(valid).code ==
                app::TrustedDisplaySelectionCode::invalid,
            "SFPRELOG rejects ANSI selection");
        auto invalid_newuser = request(
            app::TrustedDisplayIdentity::newuser,
            app::GraphicsMode::ascii);
        reader.put("NEWUSER.ASC", "Registration rules");
        require(selector.select(invalid_newuser).available(),
            "preauthentication NEWUSER uses unsuffixed artwork without account authority");
    }
    {
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC", "typed");
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(selected.available() && selected.program.has_value(),
            "valid typed program available for metadata tamper checks");

        auto mismatched_mode = *selected.program;
        mismatched_mode.graphics = app::GraphicsMode::ansi;
        require(app::valid_rendered_trusted_display_program(
                    mismatched_mode),
            "typed result allows safe ASC fallback for ANSI callers");

        auto ansi_in_ascii = *selected.program;
        ansi_in_ascii.terminal_program.operations.push_back(
            {app::TerminalDisplayOperationKind::ansi_sequence, "\x1b[31m"});
        ansi_in_ascii.terminal_program.output_byte_count += 5U;
        require(!app::valid_rendered_trusted_display_program(
                    ansi_in_ascii),
            "typed result rejects ANSI operations bound to an ASC asset");

        auto wrong_identity = *selected.program;
        wrong_identity.identity = app::TrustedDisplayIdentity::newreg;
        require(!app::valid_rendered_trusted_display_program(
                    wrong_identity),
            "typed result binds the selected logical asset to its identity");

        auto invalid_exact = *selected.program;
        invalid_exact.identity = app::TrustedDisplayIdentity::newuser;
        invalid_exact.selected_logical_asset = "NEWUSER000.ASC";
        invalid_exact.exact_security_match = true;
        require(!app::valid_rendered_trusted_display_program(
                    invalid_exact),
            "typed result rejects invalid exact-security metadata");
    }
}

void encoding_ansi_and_controls() {
    {
        FakeAssetReader reader;
        std::string cp437{"A"};
        constexpr std::array<unsigned char, 5> cp437_samples{
            0x7fU, 0x80U, 0x9bU, 0xdbU, 0xffU};
        for (const unsigned char value : cp437_samples) {
            cp437.push_back(static_cast<char>(value));
        }
        cp437 += "\r\nB\nC\rD";
        reader.put("WELCOME1.ASC", cp437);
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(selected.available(),
            "ASC accepts complete CP437 high-bit glyph repertoire");
        const auto bytes = output_bytes(*selected.program);
        require(bytes.find(static_cast<char>(0xdbU)) != std::string::npos &&
                bytes.find("\r\nB\r\nC\r\nD") != std::string::npos,
            "CP437 bytes are preserved and logical newlines become CRLF");
    }
    {
        const std::vector<std::string> allowed{
            "\x1b[0m", "\x1b[38;5;200m", "\x1b[2A", "\x1b[10;20H",
            "\x1b[2J", "\x1b[1K", "\x1b" "7", "\x1b" "8",
            "\x1b" "D", "\x1b" "E", "\x1b" "M", "\x1b[s", "\x1b[u",
            "\x1b[?7l", "\x1b[?7h", "\x1b[?25l", "\x1b[?25h"};
        for (const auto& sequence : allowed) {
            FakeAssetReader reader;
            reader.put("WELCOME1.ANS", sequence + "X");
            app::TrustedDisplaySelector selector(reader);
            const auto selected = selector.select(request(
                app::TrustedDisplayIdentity::welcome1,
                app::GraphicsMode::ansi));
            require(selected.available(),
                "approved safe ANSI sequence renders");
            require(operations_of(*selected.program,
                        app::TerminalDisplayOperationKind::ansi_sequence) ==
                    1U,
                "approved ANSI is isolated as one typed operation");
        }
    }
    {
        const std::vector<std::string> prohibited{
            "\x1b]0;title\x07", "\x1bPbad\x1b\\", "\x1b[6n",
            "\x1b[?1h", "\x1b[9999A", "\x1b[", std::string(1, '\a'),
            std::string(1, '\0'), std::string(1, '\x01')};
        for (const auto& source : prohibited) {
            FakeAssetReader reader;
            reader.put("WELCOME1.ANS", source);
            app::TrustedDisplaySelector selector(reader);
            require(selector.select(request(
                        app::TrustedDisplayIdentity::welcome1,
                        app::GraphicsMode::ansi)).code ==
                    app::TrustedDisplaySelectionCode::invalid,
                "prohibited ANSI/control input invalidates whole render");
        }
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC", "\x1b[31mplain");
        app::TrustedDisplaySelector selector(reader);
        require(selector.select(request(
                    app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ascii)).code ==
                app::TrustedDisplaySelectionCode::invalid,
            "ASC rejects raw ANSI escape sequences");
    }
    {
        FakeAssetReader reader;
        reader.put("WELCOME1.ANS",
            "@PROMPTOFF@@NOABORT@@BEEP@@CLS@@PAUSE@@ABORTON@@PROMPT@");
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ansi));
        require(selected.available(), "typed controls compile");
        require(operations_of(*selected.program,
                    app::TerminalDisplayOperationKind::paging_disabled) == 1U &&
                operations_of(*selected.program,
                    app::TerminalDisplayOperationKind::abort_disabled) == 1U &&
                operations_of(*selected.program,
                    app::TerminalDisplayOperationKind::caller_bell) == 1U &&
                operations_of(*selected.program,
                    app::TerminalDisplayOperationKind::clear_screen) == 1U &&
                operations_of(*selected.program,
                    app::TerminalDisplayOperationKind::pause) == 1U &&
                operations_of(*selected.program,
                    app::TerminalDisplayOperationKind::abort_enabled) == 1U &&
                operations_of(*selected.program,
                    app::TerminalDisplayOperationKind::paging_enabled) == 1U,
            "each approved readable control has one typed operation");

        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);
        const auto outcome = terminal.present_display(
            {"display.control-plan", &selected.program->terminal_program});
        require(outcome == app::TerminalOutcome::completed,
            "caller terminal executes typed control plan");
        require(stream.output() == "\a\x1b[2J\x1b[H\r\n",
            "typed BEL, clear, and pause emit only dedicated safe bytes");
    }
    {
        app::TerminalDisplayProgram program;
        program.operations = {
            {app::TerminalDisplayOperationKind::abort_disabled, {}},
            {app::TerminalDisplayOperationKind::pause, {}},
            {app::TerminalDisplayOperationKind::abort_enabled, {}},
            {app::TerminalDisplayOperationKind::pause, {}}};
        program.output_byte_count = 4U;
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text(std::string("\x11\r\x11", 3U));
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"display.abort-scope", &program}) ==
                    app::TerminalOutcome::cancelled &&
                stream.output() == "\a\r\n",
            "NOABORT consumes Ctrl+Q as a disallowed key until ABORTON");
    }
    {
        app::TerminalDisplayProgram program;
        program.operations = {
            {app::TerminalDisplayOperationKind::abort_disabled, {}},
            {app::TerminalDisplayOperationKind::pause, {}}};
        program.output_byte_count = 2U;
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text(std::string("\r\x11", 2U));
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"display.abort-reset", &program}) ==
                    app::TerminalOutcome::completed &&
                terminal.pause({"display.after-scope"}) ==
                    app::TerminalOutcome::cancelled &&
                stream.output() == "\r\n",
            "display abort state resets on every program exit");
    }
}

void tokens_literal_and_formatting() {
    const std::vector<std::string> preauth{
        "BBSNAME", "SYSOP", "VERSION", "DATE", "TIME", "DATETIME",
        "NODE", "NODES", "TRANSPORT", "COLS", "ROWS"};
    for (const auto& token : preauth) {
        FakeAssetReader reader;
        reader.put("SFPRELOG.ASC", "@" + token + "@");
        app::TrustedDisplaySelector selector(reader);
        require(selector.select(request(
                    app::TrustedDisplayIdentity::sfprelog,
                    app::GraphicsMode::ascii)).available(),
            "eligible preauthentication token renders");
    }
    {
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC", "@@ @BBSNAME@ @GRAPHICS@");
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(selected.available() &&
                output_bytes(*selected.program) ==
                    "@ Spitfire NG ASCII",
            "literal at and graphics token render deterministically");
    }
    {
        auto context = full_context();
        context.bbs_name = "@CLS@";
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC", "@BBSNAME@");
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(
            {app::TrustedDisplayIdentity::welcome1,
                app::GraphicsMode::ascii, std::nullopt, context});
        require(selected.available() &&
                output_bytes(*selected.program) == "@CLS@" &&
                operations_of(*selected.program,
                    app::TerminalDisplayOperationKind::clear_screen) == 0U,
            "substituted token text is literal and never recursively parsed");
    }
    {
        const std::vector<std::string> authenticated{
            "FNAME", "CITYSTATE", "PHONENUM", "NAME", "ALIAS", "EMAIL",
            "SLEVEL", "LOGONS", "LASTCALL", "TIMEON", "PROTOCOL",
            "IPADDR", "PWAGE", "GRAPHICS"};
        for (const auto& token : authenticated) {
            FakeAssetReader reader;
            reader.put("NEWUSER10.ASC", "@" + token + "@");
            app::TrustedDisplaySelector selector(reader);
            require(selector.select(request(
                        app::TrustedDisplayIdentity::newuser,
                        app::GraphicsMode::ascii,
                        std::uint16_t{10})).available(),
                "eligible authenticated NEWUSER token renders");
        }
    }
    {
        for (const std::string source :
            {"@NAME@", "@PASSWORD@", "@BIRTHDATE@", "@MAILTOTAL@",
                "@UNKNOWN@"}) {
            FakeAssetReader reader;
            reader.put("WELCOME1.ASC", source);
            app::TrustedDisplaySelector selector(reader);
            require(selector.select(request(
                        app::TrustedDisplayIdentity::welcome1,
                        app::GraphicsMode::ascii)).code ==
                    app::TrustedDisplaySelectionCode::invalid,
                "ineligible or forbidden token invalidates render");
        }
    }
    {
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC",
            "[@BBSNAME|R15@][@BBSNAME|L15@]");
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(selected.available() &&
                output_bytes(*selected.program).find(
                    "[    Spitfire NG][Spitfire NG    ]") !=
                    std::string::npos,
            "bounded alignment is deterministic");
    }
    {
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC",
            "[@BBSNAME|L5@][@BBSNAME|R5@][@BBSNAME|C5@]");
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(selected.available() &&
                output_bytes(*selected.program) ==
                    "[Spitf][re NG][tfire]",
            "over-width values use deterministic bounded truncation");
    }
    {
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC", "[@NODE|Z3@][@NODES|Z2@]");
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(selected.available() &&
                output_bytes(*selected.program) == "[007][28]",
            "numeric zero padding validates and bounds both short and long values");

        reader.put("WELCOME1.ASC", "@BBSNAME|Z3@");
        require(selector.select(request(
                    app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ascii)).code ==
                    app::TrustedDisplaySelectionCode::invalid,
            "zero padding rejects nonnumeric values");
    }
    {
        FakeAssetReader reader;
        reader.put("NEWUSER10.ASC", "[@TIMEON:HMS@][@TIMEON:MIN@]");
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::newuser,
            app::GraphicsMode::ascii, std::uint16_t{10}));
        require(selected.available() &&
                output_bytes(*selected.program) == "[1:01:01][61]",
            "named numeric formats are deterministic");
    }
}

void paging_conformance() {
    {
        app::TerminalDisplayProgram program;
        add_tail(program);
        require(app::valid_terminal_display_presentation_request(
                    {"paging.disabled", &program, {false, std::nullopt}}),
            "disabled paging needs no invented page length");
        require(!app::valid_terminal_display_presentation_request(
                    {"paging.missing", &program, {true, std::nullopt}}) &&
                !app::valid_terminal_display_presentation_request(
                    {"paging.low", &program,
                        {true, static_cast<std::uint16_t>(9U)}}) &&
                !app::valid_terminal_display_presentation_request(
                    {"paging.high", &program,
                        {true, static_cast<std::uint16_t>(25U)}}),
            "enabled paging requires the locked 10 through 24 page length");
        require(app::valid_terminal_display_presentation_request(
                    {"paging.ten", &program,
                        {true, static_cast<std::uint16_t>(10U)}}) &&
                app::valid_terminal_display_presentation_request(
                    {"paging.twentythree", &program,
                        {true, static_cast<std::uint16_t>(23U)}}) &&
                app::valid_terminal_display_presentation_request(
                    {"paging.twentyfour", &program,
                        {true, static_cast<std::uint16_t>(24U)}}),
            "observed page lengths 10 23 and 24 are accepted");
    }

    for (const std::uint16_t page_length :
        std::array<std::uint16_t, 3U>{10U, 23U, 24U}) {
        app::TerminalDisplayProgram program;
        add_rows(program, static_cast<std::size_t>(page_length));
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);
        const auto outcome = terminal.present_display(
            {"paging.boundary", &program, {true, page_length}});
        const std::size_t prompt = stream.output().find(kAutomaticPagingPrompt);
        const std::size_t expected_prefix = [&]() {
            app::TerminalDisplayProgram prefix;
            add_rows(prefix, static_cast<std::size_t>(page_length) - 1U);
            std::size_t size = 0;
            for (const auto& operation : prefix.operations) {
                size += operation.kind ==
                        app::TerminalDisplayOperationKind::paging_newline
                    ? 2U
                    : operation.bytes.size();
            }
            return size;
        }();
        require(outcome == app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 1U,
            "page length reserves exactly one prompt row");
        require(prompt == expected_prefix,
            "automatic prompt follows page length minus one CRLF rows");
    }

    {
        app::TerminalDisplayProgram program;
        add_rows(program, 18U);
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\r");
        stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.repeat", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 2U &&
                stream.output().find("TAIL") != std::string::npos,
            "successful continuation resets the automatic row counter");
    }
    {
        app::TerminalDisplayProgram program;
        add_rows(program, 8U, "A");
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_enabled, {}});
        add_rows(program, 9U, "B");
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\r");
        stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.forced-reset", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 1U &&
                stream.output().find(kForcedPagingPrompt) != std::string::npos,
            "PROMPT forces a distinct pause and resets automatic paging");
    }
    {
        app::TerminalDisplayProgram program;
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_disabled, {}});
        add_rows(program, 20U);
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_enabled, {}});
        add_rows(program, 9U, "B");
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\r");
        stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.promptoff", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 1U,
            "PROMPTOFF suppresses automatic paging until PROMPT restores it");
    }
    {
        app::TerminalDisplayProgram program;
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_disabled, {}});
        add_rows(program, 20U);
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_enabled, {}});
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.more-off", &program,
                        {false, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                stream.output().find("MORE:") == std::string::npos &&
                stream.output().find(kForcedPagingPrompt) ==
                    std::string::npos,
            "caller More Off suppresses automatic and forced prompts");
    }
    {
        app::TerminalDisplayProgram first;
        first.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_disabled, {}});
        add_rows(first, 12U);
        app::TerminalDisplayProgram second;
        add_rows(second, 9U);
        add_tail(second);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.scope-one", &first,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                terminal.present_display(
                    {"paging.scope-two", &second,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 1U,
            "paging control state resets between rendered programs");
    }

    {
        app::TerminalDisplayProgram program;
        add_rows(program, 30U);
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("N\r");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.nonstop", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 1U &&
                stream.output().find("TAIL") != std::string::npos,
            "N selects Nonstop for the remainder of one display");
    }
    {
        app::TerminalDisplayProgram program;
        add_rows(program, 9U, "A");
        add_rows(program, 12U, "B");
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_enabled, {}});
        add_rows(program, 9U, "C");
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("N");
        stream.script_non_secret_text("\r");
        stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.nonstop-reset", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 2U &&
                stream.output().find("TAIL") != std::string::npos,
            "PROMPT clears Nonstop and restores automatic paging");
    }
    {
        app::TerminalDisplayProgram program;
        add_rows(program, 20U);
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("S");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.stop", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                stream.output().find("TAIL") == std::string::npos,
            "S stops only the current display with a completed terminal result");
    }
    {
        app::TerminalDisplayProgram program;
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::abort_disabled, {}});
        add_rows(program, 9U, "A");
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::abort_enabled, {}});
        add_rows(program, 9U, "B");
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("S");
        stream.script_non_secret_text("S");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.abort-controls", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 2U &&
                stream.output().find("B9\r\n") != std::string::npos &&
                stream.output().find("TAIL") == std::string::npos,
            "NOABORT blocks Stop and ABORTON restores it");
    }
    {
        app::TerminalDisplayProgram program;
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_enabled, {}});
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("S");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.forced-s", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                stream.output() ==
                    std::string(kForcedPagingPrompt) + "\r\nTAIL",
            "S is a continuation key rather than Stop at forced PROMPT");
    }
    {
        app::TerminalDisplayProgram program;
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_enabled, {}});
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("YS");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.forced-invalid", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                stream.output() ==
                    std::string(kForcedPagingPrompt) + "\a\r\nTAIL",
            "forced PROMPT BELs invalid keys and accepts S only as continuation");
    }
    {
        app::TerminalDisplayProgram program;
        add_rows(program, 9U);
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("Y\r");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.invalid", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                stream.output().find(
                    std::string(kAutomaticPagingPrompt) + "\a\r\n") !=
                    std::string::npos,
            "invalid paging keys BEL and remain at the same prompt");
    }
    {
        app::TerminalDisplayProgram program;
        add_rows(program, 9U);
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text(" \r");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.space", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                stream.output().find("TAIL") != std::string::npos,
            "Space continues one automatic page");
    }

    {
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC",
            "A1\r\nA2\r\nA3\r\nA4\r\n@INCLUDE:COMMON.ASC@TAIL");
        reader.put("COMMON.ASC",
            "I1\r\nI2\r\nI3\r\nI4\r\nI5\r\n");
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);
        require(selected.available() &&
                terminal.present_display({"paging.include",
                    &selected.program->terminal_program,
                    {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 1U &&
                stream.output().find("I5\r\n" +
                    std::string(kAutomaticPagingPrompt)) != std::string::npos,
            "includes share one transparent render-wide paging counter");
    }
    {
        FakeAssetReader reader;
        std::string source;
        for (std::size_t index = 0; index < 12U; ++index) source += "C\r";
        for (std::size_t index = 0; index < 12U; ++index) source += "L\n";
        for (std::size_t index = 0; index < 9U; ++index) source += "P\r\n";
        source += "TAIL";
        reader.put("WELCOME1.ASC", source);
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);
        require(selected.available() &&
                operations_of(*selected.program,
                    app::TerminalDisplayOperationKind::logical_newline) ==
                    24U &&
                operations_of(*selected.program,
                    app::TerminalDisplayOperationKind::paging_newline) == 9U,
            "renderer retains CR-only LF-only versus CRLF paging origin");
        require(terminal.present_display({"paging.newline-origin",
                    &selected.program->terminal_program,
                    {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 1U,
            "only source CRLF rows advance automatic paging");
    }
    {
        FakeAssetReader reader;
        std::string source(400U, 'W');
        for (std::size_t index = 0; index < 12U; ++index) {
            source += "\x1b[1B";
        }
        source += "@CLS@";
        for (std::size_t index = 0; index < 8U; ++index) source += "A\r\n";
        source += "\x1b[2J\x1b[H";
        source += "B\r\nTAIL";
        reader.put("WELCOME1.ANS", source);
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ansi));
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);
        require(selected.available() &&
                terminal.present_display({"paging.nonrow-controls",
                    &selected.program->terminal_program,
                    {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 1U,
            "wrapping ANSI cursor movement clear home and CLS do not count or reset");
    }

    {
        app::TerminalDisplayProgram program;
        add_rows(program, 18U);
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\rS");
        stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.coalesced-current", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                occurrences(stream.output(), kAutomaticPagingPrompt) == 2U &&
                stream.output().find("TAIL") != std::string::npos,
            "coalesced trailing input cannot abort the active display");
    }
    {
        app::TerminalDisplayProgram program;
        add_rows(program, 9U);
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\rY");
        stream.script_non_secret_text("N");
        app::StreamCallerTerminal terminal(stream);
        const auto display = terminal.present_display(
            {"paging.coalesced-next", &program,
                {true, kPagingTestPageLength}});
        const app::TerminalKeyRequest next{
            "paging.following", "Next: ", {'N'}, std::nullopt};
        const auto key = terminal.read_key(next);
        require(display == app::TerminalOutcome::completed &&
                key.has_key() && key.key == 'N' &&
                stream.read_observations().size() == 2U,
            "paging quarantines coalesced tail while later real typeahead remains usable");
    }

    {
        struct StopCase {
            app::InteractiveStreamOutcome stream;
            app::TerminalOutcome terminal;
        };
        const std::array<StopCase, 5U> cases{{
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
        for (const auto& item : cases) {
            app::TerminalDisplayProgram program;
            program.operations.push_back(
                {app::TerminalDisplayOperationKind::paging_enabled, {}});
            program.output_byte_count = 0U;
            test::DeterministicInteractiveByteStreamFake stream;
            stream.script_read_outcome(item.stream);
            app::StreamCallerTerminal terminal(stream);
            require(terminal.present_display(
                        {"paging.stop-outcome", &program,
                            {true, kPagingTestPageLength}}) ==
                        item.terminal,
                "paging preserves typed Window 6A stop outcomes");
        }
    }
    {
        app::TerminalDisplayProgram program;
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_enabled, {}});
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text(std::string(1U, '\x11'));
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.ctrl-q", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::cancelled,
            "Ctrl+Q maps to the accepted typed cancellation outcome");
    }
    {
        app::TerminalDisplayProgram program;
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_enabled, {}});
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_malformed_read(
            app::InteractiveStreamOutcome::completed);
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.malformed-read", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::terminal_failure,
            "malformed paging read fails as terminal infrastructure");
    }
    {
        app::TerminalDisplayProgram program;
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_enabled, {}});
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_write_exception(3U);
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.partial-output", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::output_failure &&
                stream.output() == "< E" &&
                stream.write_observations().size() == 1U,
            "uncertain partial paging output fails once without replay");
    }
    {
        app::TerminalDisplayProgram program;
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::abort_disabled, {}});
        program.operations.push_back(
            {app::TerminalDisplayOperationKind::paging_enabled, {}});
        add_tail(program);
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text(std::string("\x11\r", 2U));
        app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display(
                    {"paging.noabort-cancel", &program,
                        {true, kPagingTestPageLength}}) ==
                    app::TerminalOutcome::completed &&
                stream.output() ==
                    std::string(kForcedPagingPrompt) + "\a\r\nTAIL",
            "NOABORT blocks paging cancellation until a valid response");
    }
}

void includes_paths_and_bounds() {
    {
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC",
            "@INCLUDE:PART.ASC@@INCLUDE:PART.ASC@");
        reader.put("PART.ASC", "X");
        app::TrustedDisplaySelector selector(reader);
        const auto selected = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(selected.available() &&
                selected.program->resolved_include_count == 2U &&
                output_bytes(*selected.program) == "XX",
            "repeated non-cyclic includes remain allowed and counted");
    }
    {
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC", "@INCLUDE:A.ASC@");
        reader.put("A.ASC", "@INCLUDE:WELCOME1.ASC@");
        app::TrustedDisplaySelector selector(reader);
        require(selector.select(request(
                    app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ascii)).code ==
                app::TrustedDisplaySelectionCode::invalid,
            "logical include cycle fails closed");

        reader.put("A.ASC", "@INCLUDE:B.ASC@", "same-file");
        reader.put("B.ASC", "X", "same-file");
        require(selector.select(request(
                    app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ascii)).code ==
                app::TrustedDisplaySelectionCode::invalid,
            "opened-file identity alias cycle fails closed");
    }
    {
        FakeAssetReader reader;
        reader.put("WELCOME1.ASC", "@INCLUDE:D1.ASC@");
        for (int index = 1; index <= 7; ++index) {
            reader.put("D" + std::to_string(index) + ".ASC",
                "@INCLUDE:D" + std::to_string(index + 1) + ".ASC@");
        }
        reader.put("D8.ASC", "depth-eight");
        app::TrustedDisplaySelector selector(reader);
        require(selector.select(request(
                    app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ascii)).available(),
            "include depth eight is accepted");
        reader.put("D8.ASC", "@INCLUDE:D9.ASC@");
        reader.put("D9.ASC", "too-deep");
        require(selector.select(request(
                    app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ascii)).code ==
                app::TrustedDisplaySelectionCode::invalid,
            "include depth beyond eight is rejected");
    }
    {
        FakeAssetReader reader;
        reader.put("PART.ASC", "X");
        reader.put("PART.ANS", "\x1b[31mX");
        reader.put("WELCOME1.ASC", "@INCLUDE:PART.ANS@");
        app::TrustedDisplaySelector selector(reader);
        require(selector.select(request(
                    app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ascii)).code ==
                app::TrustedDisplaySelectionCode::invalid,
            "ASCII root cannot include ANSI source");
        reader.put("WELCOME1.ANS",
            "@INCLUDE:PART.ASC@@INCLUDE:PART.ANS@");
        require(selector.select(request(
                    app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ansi)).available(),
            "ANSI root may include plain ASC and safe ANS");
        reader.put("WELCOME1.ASC", "@INCLUDE:MISSING.ASC@");
        require(selector.select(request(
                    app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ascii)).code ==
                app::TrustedDisplaySelectionCode::invalid,
            "missing include invalidates the complete render");
    }
    {
        FakeAssetReader reader;
        std::string sixty_four;
        for (std::size_t index = 0; index < 64U; ++index) {
            sixty_four += "@INCLUDE:PART.ASC@";
        }
        reader.put("PART.ASC", "X");
        reader.put("WELCOME1.ASC", sixty_four);
        app::TrustedDisplaySelector selector(reader);
        const auto accepted = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(accepted.available() &&
                accepted.program->resolved_include_count == 64U,
            "sixty-four resolved include instances are accepted");
        reader.put("WELCOME1.ASC",
            sixty_four + "@INCLUDE:PART.ASC@");
        require(selector.select(request(
                    app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ascii)).code ==
                app::TrustedDisplaySelectionCode::invalid,
            "sixty-fifth include instance is rejected");
    }
    {
        FakeAssetReader reader;
        std::string expansion;
        for (std::size_t index = 0; index < 700U; ++index) {
            expansion += "@BBSNAME@";
        }
        auto context = full_context();
        context.bbs_name = std::string(1600U, 'X');
        reader.put("WELCOME1.ASC", expansion);
        app::TrustedDisplaySelector selector(reader);
        require(selector.select({app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ascii, std::nullopt, context}).code ==
                app::TrustedDisplaySelectionCode::invalid,
            "rendered output beyond one MiB fails before presentation");

        std::string includes;
        for (std::size_t index = 0; index < 64U; ++index) {
            includes += "@INCLUDE:PART.ASC@";
        }
        reader.put("WELCOME1.ASC", includes);
        reader.put("PART.ASC", std::string(20U * 1024U, 'Y'));
        require(selector.select(request(
                    app::TrustedDisplayIdentity::welcome1,
                    app::GraphicsMode::ascii)).code ==
                app::TrustedDisplaySelectionCode::invalid,
            "aggregate source or output beyond one MiB fails closed");
    }
    {
        const std::vector<std::string> unsafe{
            "../A.ASC", "..\\A.ASC", "C:\\A.ASC", "\\\\host\\A.ASC",
            "\\\\?\\C:\\A.ASC", "\\\\.\\NUL", "A.ASC:stream",
            "A/B.ASC", "A\\B.ASC", ".ASC", "A..ASC", "A*.ASC",
            "A?.ASC", "A.ASC.", "A.ASC ", "CON.ASC", "nul.asc",
            "COM1.ASC", "LPT9.ASC"};
        for (const auto& name : unsafe) {
            require(!app::valid_include_filename(name),
                "unsafe include/path alias is rejected");
        }
        require(app::valid_include_filename("mixed_case.ASC") &&
                app::valid_include_filename("MIXED_CASE.ans"),
            "safe mixed-case simple basenames remain valid");
    }
    {
        FakeAssetReader reader;
        reader.throw_on_read = true;
        app::TrustedDisplaySelector selector(reader);
        const auto first = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        const auto second = selector.select(request(
            app::TrustedDisplayIdentity::welcome1,
            app::GraphicsMode::ascii));
        require(first.code == app::TrustedDisplaySelectionCode::unavailable &&
                second.code == first.code,
            "reader exceptions are contained as deterministic unavailable results");
    }
}

void deterministic_properties_and_concurrency() {
    {
        app::DisplayTokenRegistry registry;
        const std::string source =
            "@INCLUDE:..\\ESCAPE.ASC@ @CLS@ caller";
        const auto inert = app::parse_display(
            source, app::DisplayTrust::untrusted, registry);
        require(inert.valid() && inert.segments.size() == 1U &&
                inert.segments.front().kind ==
                    app::DisplaySegmentKind::literal &&
                inert.segments.front().literal == source,
            "untrusted content remains one inert literal without expansion");
    }
    {
        constexpr std::array<unsigned char, 20> alphabet{
            'A', 'z', '0', ' ', '@', '[', ']', ';', ':', '|',
            '\r', '\n', 0x00U, 0x07U, 0x1bU, 0x7fU, 0x80U,
            0x9bU, 0xdbU, 0xffU};
        std::uint32_t state = 0x6b53a19dU;
        for (std::size_t case_index = 0; case_index < 512U; ++case_index) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            const std::size_t length =
                1U + static_cast<std::size_t>(state % 384U);
            std::string source;
            source.reserve(length);
            for (std::size_t index = 0; index < length; ++index) {
                state ^= state << 13U;
                state ^= state >> 17U;
                state ^= state << 5U;
                source.push_back(static_cast<char>(
                    alphabet[state % alphabet.size()]));
            }
            FakeAssetReader first_reader;
            FakeAssetReader second_reader;
            first_reader.put("WELCOME1.ANS", source);
            second_reader.put("WELCOME1.ANS", source);
            app::TrustedDisplaySelector first_selector(first_reader);
            app::TrustedDisplaySelector second_selector(second_reader);
            const auto first = first_selector.select(request(
                app::TrustedDisplayIdentity::welcome1,
                app::GraphicsMode::ansi));
            const auto second = second_selector.select(request(
                app::TrustedDisplayIdentity::welcome1,
                app::GraphicsMode::ansi));
            require(first.code == second.code &&
                    first.available() == second.available(),
                "deterministic corpus returns identical closed result");
            if (first.available()) {
                require(first.program.has_value() &&
                        second.program.has_value() &&
                        app::valid_rendered_trusted_display_program(
                            *first.program) &&
                        first.program->terminal_program.output_byte_count <=
                            app::kMaximumRenderedDisplayBytes &&
                        program_signature(*first.program) ==
                            program_signature(*second.program),
                    "accepted corpus program is byte-identical and bounded");
            } else {
                require(!first.program.has_value() &&
                        !second.program.has_value(),
                    "rejected corpus returns no partial rendered program");
            }
        }
    }
    {
        constexpr std::size_t session_count = 128U;
        std::array<bool, session_count> succeeded{};
        std::vector<std::thread> workers;
        workers.reserve(session_count);
        for (std::size_t index = 0; index < session_count; ++index) {
            workers.emplace_back([index, &succeeded]() {
                FakeAssetReader reader;
                reader.put("WELCOME1.ANS",
                    "\x1b[32m@BBSNAME@ @NODE@\r\n"
                    "@INCLUDE:COMMON.ASC@\x1b[0m");
                reader.put("COMMON.ASC", "bounded CP437 \xdb");
                auto context = full_context();
                context.node = std::to_string(index + 1U);
                app::TrustedDisplaySelector selector(reader);
                const auto selected = selector.select(
                    {app::TrustedDisplayIdentity::welcome1,
                        app::GraphicsMode::ansi, std::nullopt, context});
                succeeded[index] = selected.available() &&
                    selected.program.has_value() &&
                    selected.program->resolved_include_count == 1U &&
                    selected.program->terminal_program.output_byte_count <=
                        app::kMaximumRenderedDisplayBytes;
            });
        }
        for (auto& worker : workers) worker.join();
        require(std::all_of(succeeded.begin(), succeeded.end(),
                    [](const bool value) { return value; }),
            "128 concurrent portable render sessions remain isolated and bounded");
    }
}

void newsletter_completion_and_color() {
    using Op=app::TerminalDisplayOperationKind;
    for(auto graphics:{app::GraphicsMode::ascii,app::GraphicsMode::ansi}) for(unsigned padding:{0U,22U,23U,24U,25U,80U}) {
        app::TerminalDisplayProgram program;add_rows(program,2,"News");
        for(unsigned n=0;n<padding;++n) {program.operations.push_back({Op::paging_newline,{}});program.output_byte_count+=2;}
        require(app::prepare_acknowledged_display(program,graphics,std::uint8_t{13}),"newsletter framing valid with trailing padding");
        test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);terminal.set_text_style(graphics==app::GraphicsMode::ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
        require(terminal.present_display({"newsletter.padding",&program,{true,std::uint16_t{24}}})==app::TerminalOutcome::completed,"only completion Enter required at EOF");
        const auto output=stream.output();
        require(output.find("MORE:")==std::string::npos && output.find("News1\r\nNews2")!=std::string::npos,"padding cannot create empty final MORE");
        require(graphics==app::GraphicsMode::ansi ? output.find("\x1b[0;1;35;40mPress <ENTER>")!=std::string::npos : output.find('\x1b')==std::string::npos,
            "completion follows caller color and ASCII has no escapes");
    }
    for(char response:{'\r','N','S'}) {
        app::TerminalDisplayProgram program;add_rows(program,60,"Appended news");
        program.operations.insert(program.operations.begin(),{Op::ansi_sequence,"\x1b[32m"});program.output_byte_count+=5;
        require(app::prepare_acknowledged_display(program,app::GraphicsMode::ansi,std::uint8_t{13}),"multipage authored newsletter framing");
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text(std::string(1,response));
        for(unsigned n=0;n<4;++n) stream.script_non_secret_text("\r");
        app::StreamCallerTerminal terminal(stream);terminal.set_text_style(app::TerminalTextStyle::ogsf_ansi);
        require(terminal.present_display({"newsletter.multipage",&program,{true,std::uint16_t{24}}})==app::TerminalOutcome::completed,"Enter, Nonstop and Stop retain pager semantics");
        const auto output=stream.output();
        require(output.find("\x1b[0;37;40mMORE:")!=std::string::npos,"MORE uses system gray");
        require(response=='S' ? output.find("Appended news60")==std::string::npos : output.find("Appended news60")!=std::string::npos,"Stop stops, Enter and Nonstop reach appended news");
        require(response=='S' || output.find("\x1b[0;1;32;40mAppended news24")!=std::string::npos,"authored foreground restored after system-colored MORE");
    }
    for(bool sauce:{false,true}) {
        FakeAssetReader reader;
        std::string raw="\x1b[32mNews without final newline";
        if(sauce) {
            std::string metadata(128,'\0');metadata.replace(0,7,"SAUCE00");
            for(unsigned n=0;n<4;++n) metadata[90+n]=static_cast<char>((raw.size()>>(8*n))&255U);
            metadata[94]=1;metadata[95]=1;metadata[96]=80;metadata[98]=25;raw+='\x1a';raw+=metadata;
        }
        reader.put("SFNWSLTR.ANS",raw);app::TrustedDisplaySelector selector(reader);
        app::TrustedDisplaySelectionRequest request;request.identity=app::TrustedDisplayIdentity::sfnewsletter;request.graphics=app::GraphicsMode::ansi;
        auto selected=selector.select(request);
        require(selected.available() && app::prepare_acknowledged_display(selected.program->terminal_program,request.graphics,std::uint8_t{13}),"no-final-newline/SAUCE newsletter valid");
        test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text("\r");app::StreamCallerTerminal terminal(stream);
        require(terminal.present_display({"newsletter.sauce",&selected.program->terminal_program,{true,std::uint16_t{24}}})==app::TerminalOutcome::completed &&
            stream.output().find("MORE:")==std::string::npos && stream.output().find("SAUCE")==std::string::npos,
            "no-final-newline and valid metadata end in one completion pause");
    }
}
}  // namespace

int main() {
    try {
        std::cout << "phase3a-window6b selection-fallback\n";
        selection_and_fallback();
        std::cout << "phase3a-window6b encoding-ansi-controls\n";
        encoding_ansi_and_controls();
        std::cout << "phase3a-window6b tokens-literal-formatting\n";
        tokens_literal_and_formatting();
        std::cout << "phase3a-window6b paging-conformance\n";
        paging_conformance();
        newsletter_completion_and_color();
        std::cout << "phase3a-window6b includes-paths-bounds\n";
        includes_paths_and_bounds();
        std::cout << "phase3a-window6b properties-concurrency\n";
        deterministic_properties_and_concurrency();
        std::cout << "all SFX Phase 3A Window 6B portable display tests passed assertions="
                  << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Phase 3A Window 6B portable display test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
