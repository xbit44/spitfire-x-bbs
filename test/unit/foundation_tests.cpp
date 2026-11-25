#include "sfx/application/accounting.hpp"
#include "sfx/application/cancellation.hpp"
#include "sfx/application/commands.hpp"
#include "sfx/application/configuration.hpp"
#include "sfx/application/display.hpp"
#include "sfx/application/events.hpp"
#include "sfx/application/lifecycle.hpp"
#include "sfx/application/logging.hpp"
#include "sfx/core/node.hpp"
#include "sfx/core/security.hpp"
#include "sfx/core/session.hpp"
#include "sfx/test/foundation_fakes.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
namespace app = sfx::application;
namespace core = sfx::core;

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

core::SecurityLevel level(const std::uint16_t value) {
    const auto result = core::SecurityLevel::create(value);
    if (!result) throw std::runtime_error("invalid test security level");
    return *result;
}

void test_security() {
    require(!core::SecurityLevel::create(0), "level 0 invalid");
    require(!core::SecurityLevel::create(1000), "level 1000 invalid");
    require(core::SecurityLevel::create(1).has_value(), "level 1 valid");
    require(core::SecurityLevel::create(999).has_value(), "level 999 valid");
    const auto fresh = core::SecurityPolicy::fresh_install();
    require(fresh.configured_levels().size() == 2, "fresh install exactly two levels");
    require(fresh.configured_levels()[0].value() == 10, "fresh level 10");
    require(fresh.configured_levels()[1].value() == 999, "fresh level 999");
    require(!fresh.is_sysop(level(10)) && fresh.is_sysop(level(999)), "fresh threshold authority");
    const auto lowered = core::SecurityPolicy::create(
        {level(10), level(500), level(750), level(999)}, level(10), level(500));
    require(lowered.has_value(), "lowered threshold valid");
    require(lowered->is_sysop(level(500)) && lowered->is_sysop(level(750)), "threshold-derived sysop");
    require(!lowered->is_sysop(level(499)), "unconfigured level has no authority");
}

void test_node_and_session() {
    require(!core::NodeNumber::create(0) && !core::NodeNumber::create(129), "node bounds");
    const auto number = core::NodeNumber::create(128).value();
    core::LogicalNode node(number);
    require(node.transition(core::NodeState::ready), "offline-ready");
    require(!node.transition(core::NodeState::door), "ready-door forbidden");
    require(node.transition(core::NodeState::connecting), "ready-connecting");
    require(node.bind_session("session-128"), "bind active session");
    require(node.transition(core::NodeState::online), "connecting-online");
    require(node.transition(core::NodeState::chat), "online-chat");
    require(node.transition(core::NodeState::draining), "chat-draining");
    require(node.transition(core::NodeState::ready), "draining-ready");
    require(!node.session_identity(), "ready clears session truth");

    core::Session session("session-128", number,
        {core::TransportKind::telnet, "untrusted:23", "peer", "ansi"});
    session.set_session_limit(600);
    require(!session.authenticate({"caller-invalid", "Caller Invalid", level(10), 0}),
        "stable account identity required");
    require(session.authenticate({"caller-1", "Caller One", level(10), 101}), "authenticate once");
    require(session.caller()->account_id == 101, "stable account identity retained");
    require(!session.authenticate({"caller-2", "Caller Two", level(10), 102}), "caller replacement blocked");
    require(session.set_activity({core::ActivityKind::menu, "main"}), "explicit activity");
    session.update_elapsed(125);
    require(session.remaining_seconds() == 475, "time accounting state");
    require(session.request_disconnect() && session.finalize_disconnect(), "disconnect finalization");
    require(!session.caller().has_value(), "finalized session retains no authenticated caller authority");
}

void test_lifecycle() {
    app::RuntimeLifecycle lifecycle;
    require(lifecycle.request_start() && lifecycle.report_started(), "runtime starts");
    require(lifecycle.accepts_new_callers(), "online accepts callers");
    const app::ShutdownRequest drain{app::ShutdownDisposition::graceful_drain, {2, 1}, {5000}};
    require(lifecycle.request_shutdown(drain, 1000), "finite drain starts");
    require(lifecycle.state() == app::RuntimeState::draining && !lifecycle.accepts_new_callers(), "visible drain rejects new callers");
    require(lifecycle.cancel_shutdown(), "drain cancel");
    require(lifecycle.request_shutdown(drain, 1000), "drain restart");
    require(lifecycle.observe_active_work({1, 1}, 5000), "deadline observed");
    require(lifecycle.state() == app::RuntimeState::stopping, "deadline forces controlled stop");
    require(lifecycle.report_stopped(), "stop complete");
    require(lifecycle.request_start(), "restart");
    lifecycle.report_failure();
    require(lifecycle.state() == app::RuntimeState::recovery_required, "failure requires recovery");
    require(lifecycle.acknowledge_recovery(), "recovery acknowledged");

    sfx::test::FakeOwnership ownership;
    require(ownership.acquire(app::RuntimeOwnershipMode::desktop, "desktop") == app::OwnershipResult::acquired, "desktop ownership");
    require(ownership.acquire(app::RuntimeOwnershipMode::service, "service") == app::OwnershipResult::already_owned, "duplicate blocked");
    ownership.release("desktop");
    require(ownership.acquire(app::RuntimeOwnershipMode::service, "service") == app::OwnershipResult::acquired, "shared service contract");

    sfx::test::FakeLocalControl control;
    control.status.state = app::RuntimeState::online;
    require(control.attach({1, 0}, app::ControlAuthority::control_runtime) == app::AttachResult::attached, "local control attaches through fake");
    require(control.observe_status().state == app::RuntimeState::online, "control observes runtime truth");
    require(control.submit_shutdown({app::ShutdownDisposition::now, {}, {}}), "authorized fake channel submits shutdown");
    control.detach();
    require(!control.cancel_shutdown(), "detached control cannot mutate runtime");
}

void test_services() {
    app::CancellationSource source;
    const auto token = source.token();
    require(!token.should_stop(1000, {2000}) && token.should_stop(2000, {2000}), "finite deadline");
    source.cancel();
    require(token.is_cancelled(), "cancellation propagation");

    app::BoundedEventQueue events(2);
    require(events.publish({app::EventKind::runtime_state_changed, 0, {}, {}, "online"}), "event 1");
    require(events.publish({app::EventKind::security_alert, 0, {}, "session", "attempt"}), "event 2");
    require(!events.publish({app::EventKind::operational_alert, 0, {}, {}, "overflow"}), "bounded rejection");
    require(events.rejected_count() == 1 && events.pop()->sequence == 1, "event observability and order");

    sfx::test::FakeOutcomeSink sink;
    app::IdempotentOutcomeFinalizer finalizer(sink);
    const app::OutcomeRecord outcome{"session:1:final", app::OutcomeKind::session, "session-1", "disconnect", 42};
    require(finalizer.finalize(outcome) && !finalizer.finalize(outcome), "idempotent outcome");
    require(sink.records.size() == 1, "one outcome commit");
}

void test_configuration_and_logging() {
    app::ConfigurationManager manager;
    app::FoundationConfiguration valid;
    valid.logical_nodes = 128;
    require(manager.publish(valid).valid(), "valid configuration");
    require(manager.active()->telnet.bot_gate_enabled && !manager.active()->ssh.bot_gate_enabled, "listener participation defaults");
    auto invalid = valid;
    invalid.logical_nodes = 129;
    require(!manager.publish(invalid).valid(), "invalid configuration rejected");
    require(manager.generation() == 1 && manager.active()->logical_nodes == 128, "last valid retained");

    sfx::test::FakeLogSink sink;
    app::RedactingLogger logger(sink);
    logger.write({app::LogSeverity::security, "auth", "failed", "attempt rejected",
        {{"caller", "Example", app::FieldSensitivity::public_value},
         {"password", "never-log-me", app::FieldSensitivity::public_value},
         {"conversation", "private", app::FieldSensitivity::private_text}}});
    require(sink.records[0].fields[0].value == "Example", "public log field");
    require(sink.records[0].fields[1].value == "[REDACTED]", "defensive secret redaction");
    require(sink.records[0].fields[2].value == "[REDACTED]", "private-text redaction");
}

void test_display_and_commands() {
    app::DisplayTokenRegistry registry;
    require(registry.is_known_value("NAME"), "catalog token");
    require(!registry.parse_expression("PASSWORD") && !registry.parse_expression("BIRTHDATE"), "forbidden tokens");
    require(registry.parse_expression("NAME|L25")->format.width == 25, "field format");
    require(!registry.parse_expression("NAME|L201"), "format bound");
    require(registry.parse_expression("TIMELEFT:HMS").has_value(), "named format");
    require(registry.parse_expression("INCLUDE:HEADER.ANS").has_value(), "safe include");
    require(!registry.parse_expression("INCLUDE:..\\SECRET.ASC"), "traversal rejected");
    const auto trusted = app::parse_display("Hello @NAME@ @CLS@", app::DisplayTrust::trusted_sysop_asset, registry);
    require(trusted.valid() && trusted.segments.size() == 4, "trusted parsing");
    const auto inert = app::parse_display("caller says @CLS@", app::DisplayTrust::untrusted, registry);
    require(inert.valid() && inert.segments.size() == 1, "untrusted content inert");

    const auto policy = core::SecurityPolicy::create({level(10), level(500), level(999)}, level(10), level(500)).value();
    core::Session caller("caller-session", core::NodeNumber::create(1).value(), {});
    caller.authenticate({"caller", "Caller", level(10), 102});
    sfx::test::FakePresentation presentation;
    app::CommandContext context{caller, presentation, policy};
    app::NativeCommandRegistry commands;
    const core::SecurityRule sysop{core::SecurityRuleKind::sysop_only, level(1), level(999)};
    require(commands.register_native({"runtime.stop", sysop, [](app::CommandContext&) {
        return app::CommandResult{app::CommandStatus::completed, {}};
    }}), "native command registration");
    require(commands.execute("runtime.stop", context).status == app::CommandStatus::unauthorized, "operation-time authorization");
    require(commands.execute("missing", context).status == app::CommandStatus::unavailable, "canonical missing fallback");
}
}  // namespace

int main() {
    try {
        test_security();
        test_node_and_session();
        test_lifecycle();
        test_services();
        test_configuration_and_logging();
        test_display_and_commands();
        std::cout << "all SFX foundation unit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX foundation unit test failure: " << error.what() << '\n';
        return 1;
    }
}
