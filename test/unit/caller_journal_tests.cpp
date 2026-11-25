#include "sfx/application/caller_journal.hpp"
#include "sfx/application/new_caller_names.hpp"
#include <iostream>
#include <stdexcept>

namespace {
namespace app = sfx::application;
unsigned checks{};
void check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
app::Identifier128 id() { app::Identifier128 value; value.bytes[0] = 1; return value; }
const app::JournalLocalTime time_format = [](std::int64_t utc) -> std::optional<std::string> {
    return utc == 100 ? "2026-09-09 13:18:33" : utc == 108 ? "2026-09-09 13:18:41" : "2026-09-09 13:43:56";
};
void names() {
    const auto empty = app::NewCallerNames::parse("");
    check(empty.valid() && empty.size() == 0, "empty list allows names");
    const auto list = app::NewCallerNames::parse("; comment\r\n  Xbit Ops \r\nXBIT OPS\r\n\r\nOther\r\n");
    check(list.valid() && list.size() == 2, "trim and normalized duplicate collapse");
    for (const auto* name : {"Xbit Ops", "xbit ops", "  XBIT OPS  ", "Other"})
        check(list.evaluate(name) == app::NewCallerNameDecision::unavailable, "exact normalized deny");
    for (const auto* name : {"Xbit", "Xbit Ops2", "Other Person", "A Other"})
        check(list.evaluate(name) == app::NewCallerNameDecision::allowed, "no substring matching");
    for (const auto& bytes : {std::string("\xef\xbb\xbf"), std::string("Name\rOther"), std::string("Name\x1b"),
            std::string("a\0b", 3), std::string(257, ';'), std::string(65537, ' '), std::string("\xc0\x80")}) {
        const auto invalid = app::NewCallerNames::parse(bytes);
        check(!invalid.valid(), "invalid file rejected");
        check(invalid.evaluate("Good Name") == app::NewCallerNameDecision::temporarily_unavailable, "invalid file closes new registration");
    }
    check(app::NewCallerNames::parse("; caf\xc3\xa9\r\n").valid(), "UTF-8 comment accepted");
    check(!app::NewCallerNames::parse("caf\xc3\xa9\r\n").valid(), "names retain current ASCII account rule");
    std::string lines(1024, '\n');
    check(app::NewCallerNames::parse(lines).valid(), "maximum line count");
    lines += '\n'; check(!app::NewCallerNames::parse(lines).valid(), "excess line count rejected");
}
app::CallerJournalRecord caller() {
    app::CallerJournalRecord record;
    record.caller_number = 7652; record.session.session_id = id(); record.session.account_id = 1;
    record.session.node_id = 1; record.session.started_utc_seconds = 100;
    record.session.completed_utc_seconds = 1623; record.session.persisted_seconds = 1523;
    record.session.outcome = app::SessionOutcomeCode::normal;
    record.display_name = "Xbit Ops"; record.transport = "SSH";
    record.source_ip = "192.168.1.12"; record.hostname_hint = "origmar.home"; record.ansi = 1;
    record.activities = {{app::CallerActivityKind::main_menu, 108, {}}};
    return record;
}
void formatting() {
    auto record = caller();
    const auto rendered = app::render_caller_journal(record, time_format);
    check(rendered && *rendered ==
        "CONNECT SSH/origmar.home/192.168.1.12\r\nCALLER #7652\r\n"
        "Xbit Ops called at 13:18:33 on 09-09-26 using Node 1 via SSH.\r\n"
        "Hostname is an untrusted display hint.\r\nANSI Color Graphics Selected.\r\n"
        "MAIN MENU entered at 09-09-26 13:18:41.\r\n"
        "Xbit Ops logged off normally at 13:43:56 on 09-09-26.\r\nDuration: 00:25:23\r\n"
        "SESSION 01000000000000000000000000000000\r\n\r\n---\r\n", "exact owner-direction caller block");
    for (const auto outcome : {app::SessionOutcomeCode::normal, app::SessionOutcomeCode::disconnected,
            app::SessionOutcomeCode::timed_out, app::SessionOutcomeCode::shutdown, app::SessionOutcomeCode::runtime_error}) {
        record.session.outcome = outcome;
        check(app::render_caller_journal(record, time_format).has_value(), "each truthful implemented outcome");
    }
    record.ansi = 0;
    check(app::render_caller_journal(record, time_format)->find("ASCII Text Selected.") != std::string::npos, "ASCII explicit choice");
    record.recovered = true;
    check(app::render_caller_journal(record, time_format)->find("last authoritative persisted evidence") != std::string::npos, "honest recovery wording");
    check(!app::render_caller_journal(record, {}), "no disguised local-clock timezone fallback");
    record.hostname_hint = "host\r\nInjected";
    check(!app::render_caller_journal(record, time_format), "hostname controls rejected");
    record = caller(); record.display_name = "Xbit|Other";
    check(app::render_caller_journal(record, time_format)->find("Xbit\\x7COther") != std::string::npos, "name field delimiter escaped");
    record.display_name = "Xbit\x1b";
    check(!app::render_caller_journal(record, time_format), "name controls rejected");
    record = caller(); record.session.completed_utc_seconds.reset();
    check(!app::render_caller_journal(record, time_format), "never publish unfinished session as complete");
}
void activities() {
    auto value = caller().activities;
    check(app::decode_caller_activities(*app::encode_caller_activities(value))->size() == 1, "typed activity codec roundtrip");
    value.resize(32, value.front());
    check(app::encode_caller_activities(value).has_value(), "maximum activity count");
    value.push_back(value.front()); check(!app::encode_caller_activities(value), "bounded activity list");
    for (const auto* invalid : {"0|100|bad\n", "90|100|\n", "2|0100|\n", "2|100|", "2|100|\x1b\n"})
        check(!app::decode_caller_activities(invalid), "malformed activity codec rejected");
}
void attention() {
    app::AttentionRecord record;
    record.kind = app::AttentionKind::password_attempts_exhausted; record.utc_seconds = 100;
    record.node = 2; record.transport = "Telnet"; record.source_ip = "127.0.0.1";
    record.identity = app::AttentionIdentity::claimed; record.account_name = "Xbit Ops";
    check(app::render_attention_journal(record, time_format) == std::optional<std::string>(
        "09-09-26 13:18:33 | PASSWORD_ATTEMPTS_EXHAUSTED | Node 2 | Telnet | IP 127.0.0.1 | "
        "Claimed account (unverified): Xbit Ops | Attempt limit reached; connection disconnected.\r\n"), "exact claimed-not-verified attention");
    record.kind = app::AttentionKind::duplicate_login;
    check(!app::render_attention_journal(record, time_format), "duplicate requires verified identity");
    record.identity = app::AttentionIdentity::verified;
    check(app::render_attention_journal(record, time_format).has_value(), "verified duplicate accepted");
    record.kind = app::AttentionKind::verified_sysop_utility;
    check(!app::render_attention_journal(record, time_format), "no fictitious reserved producer");
    app::ClamAvAttentionTracker health;
    auto failure=health.observe(false,false,true,"clamav_update_vendor_cooldown",100);
    check(failure&&app::render_attention_journal(*failure,time_format)->find("validated definitions remain usable")!=std::string::npos,"update failure truthfully retains usable definitions");
    check(!health.observe(false,false,true,"clamav_update_vendor_cooldown",101),"same automatic/manual failure is deduplicated within epoch");
    check(!health.observe(true,true,true,{},102),"routine clean scan produces no recovery without prior scan problem");
    check(health.observe(false,true,true,{},103)->kind==app::AttentionKind::clamav_recovery&&!health.observe(false,true,true,{},104),"one actual recovery transition, no routine success flood");
    failure=health.observe(true,false,false,"clamav_scan_unavailable",105);
    check(failure&&app::render_attention_journal(*failure,time_format)->find("held privately")!=std::string::npos,"scanner failure has no clean verdict");
    record.kind=app::AttentionKind::clamav_finding;record.filename="test.zip";record.signatures={"Eicar-Test-Signature"};record.deletion_confirmed=false;
    auto finding=app::render_attention_journal(record,time_format);
    check(finding&&finding->find("test.zip")!=std::string::npos&&finding->find("Eicar-Test-Signature")!=std::string::npos&&finding->find("NOT confirmed")!=std::string::npos,"verified finding retains filename signature and truthful failed deletion");
    record.deletion_confirmed=true;record.deletion_journal_confirmed=true;
    check(app::render_attention_journal(record,time_format)->find("deletion confirmed")!=std::string::npos,"positive deletion notice requires explicit confirmed disposition");
    record.filename="../outside.zip";check(!app::render_attention_journal(record,time_format),"finding never accepts a physical path");record.filename="test.zip";
    record.signatures={"bad\r\nforged"};check(!app::render_attention_journal(record,time_format),"finding rejects control/record injection");
}
}
int main() {
    try { names(); formatting(); activities(); attention();
        std::cout << "PASS caller journals and JOKER: " << checks << " checks; 0 failed; 0 skipped\n"; return 0;
    } catch (const std::exception& failure) { std::cerr << "FAIL after " << checks << ": " << failure.what() << '\n'; return 1; }
}
