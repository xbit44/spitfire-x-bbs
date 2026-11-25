#include "sfx/application/operational_log.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/hostname_lookup.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
namespace app = sfx::application;
unsigned assertions = 0;
void require(bool ok, const char* label) { ++assertions; if (!ok) throw std::runtime_error(label); }
app::Identifier128 id(unsigned value) { app::Identifier128 i; i.bytes[0] = static_cast<std::uint8_t>(value); return i; }
app::OperationalContext caller(unsigned node = 1) {
    app::OperationalContext c;
    c.epoch = id(1); c.origin_sequence = node;
    c.node = static_cast<std::uint16_t>(node); c.generation = 7; c.attempt = node;
    c.transport = app::OperationalTransport::ssh; c.source_ip = "203.0.113.4";
    c.peer_port = 40000; c.local_port = 2222; c.correlation_id = id(2);
    return c;
}
app::AlphaOperationalLogEntry entry(app::OperationalKind kind = app::OperationalKind::connection_assigned) {
    return app::make_operational_entry(1, 1788731908, {kind, {}, caller()});
}
void goldens_and_classification() {
    auto e = entry();
    require(app::valid_operational_entry(e), "anonymous assigned event validates");
    require(app::render_operational_line(e, "2026-09-06 14:58:28") ==
        "2026-09-06 14:58:28 [INFO] Node 1 SSH <unknown> [203.0.113.4]: connection accepted",
        "exact BBS-local anonymous presentation");
    require(app::render_operational_line(e, {}).find("BBS time unavailable") == 0,
        "missing timezone never falls back to machine or disguised UTC");
    auto c = caller(); c.caller = "Alice"; c.account_id = 42; c.durable_session_id = id(3);
    e = app::make_operational_entry(2, 1788731908, {app::OperationalKind::authenticated, {}, c});
    require(e.message == "Node 1 SSH <Alice> [203.0.113.4]: logged on", "exact authenticated presentation");
    c.caller = "Alice><Other\\Caller";
    e = app::make_operational_entry(2, 1788731908, {app::OperationalKind::authenticated, {}, c});
    require(app::valid_operational_entry(e) && e.message.find("Alice\\x3E\\x3COther\\x5CCaller") != e.message.npos,
        "name delimiters cannot impersonate another context while structured name remains exact");
    auto joker = caller();
    joker.attempted_name = "hax0r";
    e = app::make_operational_entry(3, 1788731908,
        {app::OperationalKind::connection_ended,
            app::OperationalOutcome::registration_closed, joker});
    require(app::valid_operational_entry(e) && e.message ==
        "Node 1 SSH <unknown> [203.0.113.4]: connection ended before authentication - attempted name \"hax0r\" rejected by JOKER.DAT",
        "confirmed JOKER ending has exact neutral presentation and unknown caller identity");
    joker.attempted_name = "A\"<>\\|B";
    e = app::make_operational_entry(4, 1788731908,
        {app::OperationalKind::connection_ended,
            app::OperationalOutcome::registration_closed, joker});
    require(app::valid_operational_entry(e) &&
        e.message.find("A\\x22\\x3C\\x3E\\x5C\\x7CB") != e.message.npos,
        "unverified attempted-name presentation escapes quotes, angles, slashes and delimiters");
    const auto joker_record = app::encode_operational_record(e);
    const auto joker_roundtrip = app::decode_operational_record(joker_record, 44);
    require(joker_record.find("SFXLOG6|") == 0 && joker_roundtrip &&
        joker_roundtrip->event->context.caller.empty() &&
        joker_roundtrip->event->context.attempted_name == joker.attempted_name,
        "SFXLOG6 round trip retains attempted claim separately from authenticated identity");
    for (unsigned k = 0; k <= static_cast<unsigned>(app::OperationalKind::session_persisted); ++k) {
        auto context = caller();
        const auto kind = static_cast<app::OperationalKind>(k);
        if (k <= 2 || (k >= 4 && k <= 7) || (k >= 20 && k <= 24) || k == 26 || k == 27) {
            context = {}; context.epoch = id(1); context.origin_sequence = 1;
            if (k <= 2 || (k >= 4 && k <= 7)) {
                context.transport = app::OperationalTransport::telnet;
                context.source_ip = "203.0.113.4"; context.local_port = 2323;
                if (k >= 4) context.attempt = 2;
            }
            if (k == 20 || k == 21) context.node = 2;
        }
        if (kind == app::OperationalKind::duplicate_denied) context.verified_account_id = 9;
        if (kind == app::OperationalKind::registration_committed) context.committed_account_id = 9;
        if (kind == app::OperationalKind::authenticated) {
            context.account_id = 9; context.caller = "Alice"; context.durable_session_id = id(3);
        }
        if (kind == app::OperationalKind::session_persisted) {
            context.verified_account_id = 9; context.durable_session_id = id(3);
        }
        auto value = app::make_operational_entry(1, 100, {kind, {}, context});
        const auto record = app::encode_operational_record(value);
        require(!record.empty(), "every real event kind has a valid template/context");
        auto roundtrip = app::decode_operational_record(record, 900);
        require(roundtrip && roundtrip->message == value.message && roundtrip->sequence == 900 &&
            roundtrip->event->context.origin_sequence == 1 && app::encode_operational_record(*roundtrip) == record,
            "every template retains original diagnostic identity through reload/delivery reset");
        require(value.message.find(" epoch ") == value.message.npos && value.message.find(" generation ") == value.message.npos,
            "normal rendering omits opaque diagnostic identities");
    }
    require(app::operational_sysop_attention(app::OperationalKind::duplicate_denied) &&
        app::operational_class(app::OperationalKind::duplicate_denied) == app::OperationalClass::authentication,
        "verified duplicate attempts classify independently for future attention sink");
    require(!app::operational_sysop_attention(app::OperationalKind::stage_changed), "ordinary activity is not attention spam");
    require(app::operational_level(app::OperationalKind::assignment_quarantined) == "ERROR", "cleanup failure is an error");
}
void bounds_and_rejections() {
    auto e = entry();
    const auto reject = [&](auto change) {
        auto invalid = e; change(invalid);
        require(!app::valid_operational_entry(invalid) && app::encode_operational_record(invalid).empty(),
            "invalid or inconsistent record rejected before wire/file/presentation");
    };
    reject([](auto& v) { v.event->context.epoch = {}; });
    reject([](auto& v) { v.event->context.origin_sequence = 0; });
    reject([](auto& v) { v.event->context.node = 129; });
    reject([](auto& v) { v.event->context.generation = 0; });
    reject([](auto& v) { v.event->context.attempt = 0; });
    reject([](auto& v) { v.event->context.caller = "unverified name"; });
    reject([](auto& v) { v.event->context.attempted_name = "hax0r"; });
    reject([](auto& v) { v.event->context.source_ip = "203.0.113.999"; });
    reject([](auto& v) { v.event->context.source_ip = "example.net"; });
    reject([](auto& v) { v.event->context.source_ip = "127.0.0.01"; });
    reject([](auto& v) { v.event->context.columns = 4097; });
    reject([](auto& v) { v.event->context.rows = 0; });
    reject([](auto& v) { v.event->context.resource_name = "unbound target"; });
    reject([](auto& v) { v.event->context.diagnostic_code = "raw\rinput"; });
    reject([](auto& v) { v.event->context.account_id = -1; v.event->context.caller = "Alice";
        v.event->context.durable_session_id = id(3); v.message = app::operational_message(*v.event); });
    reject([](auto& v) { v.event->context.committed_account_id = -1; });
    reject([](auto& v) { v.event->context.verified_account_id = -1; });
    reject([](auto& v) { v.event->kind = static_cast<app::OperationalKind>(255); });
    reject([](auto& v) { v.level = "ERROR"; });
    reject([](auto& v) { v.message = "raw authentication input"; });
    reject([](auto& v) { v.event->outcome = static_cast<app::OperationalOutcome>(255); });
    auto invalid_attempt = app::make_operational_entry(1, 100,
        {app::OperationalKind::connection_ended,
            app::OperationalOutcome::registration_closed, caller()});
    invalid_attempt.event->context.attempted_name = " bad";
    invalid_attempt.message = app::operational_message(*invalid_attempt.event);
    require(!app::valid_operational_entry(invalid_attempt),
        "attempted name repeats the normalized 30-byte account-name boundary at logging");
    invalid_attempt.event->context.attempted_name.assign(31, 'a');
    invalid_attempt.message = app::operational_message(*invalid_attempt.event);
    require(!app::valid_operational_entry(invalid_attempt),
        "oversized attempted name fails closed without truncation");
    for (const auto c : {'\r', '\n', '\x1b', '\x7f'}) {
        auto record = app::encode_operational_record(e); record.push_back(c);
        require(!app::decode_operational_record(record, 1), "file control/ANSI injection rejects");
    }
    auto c = caller(4);
    c.origin_sequence = c.generation = c.attempt = c.duration_seconds = UINT64_MAX;
    c.account_id = c.verified_account_id = c.committed_account_id = std::numeric_limits<app::AccountId>::max();
    c.durable_session_id.bytes.fill(255); c.correlation_id.bytes.fill(255); c.epoch.bytes.fill(255);
    c.caller.assign(30, '|'); c.resource_id = UINT64_MAX; c.resource_name.assign(128, '%');
    c.diagnostic_code.assign(96, 'a'); c.columns = c.rows = 4096;
    c.peer_port = c.local_port = 65535; c.source_ip = "255.255.255.255";
    c.hostname = std::string(63, 'a') + "." + std::string(63, 'b') + "." +
        std::string(63, 'c') + "." + std::string(61, 'd');
    const auto maximum = app::make_operational_entry(UINT64_MAX, INT64_MAX, {app::OperationalKind::stage_changed, {}, c});
    const auto record = app::encode_operational_record(maximum);
    require(!record.empty() && record.size() <= app::kOperationalRecordMaximum, "worst escaped record fits strict bound without truncation");
    const auto decoded = app::decode_operational_record(record, 7);
    require(decoded && decoded->event->context.resource_name == c.resource_name &&
        decoded->event->context.caller == c.caller && decoded->event->context.epoch == c.epoch,
        "literal percent/pipe and all full IDs round trip exactly");
    require(!app::decode_operational_record(std::string(app::kOperationalRecordMaximum + 1, 'x'), 1), "oversized record rejects");
    for (std::size_t i = 0; i < record.size(); ++i)
        require(!app::decode_operational_record(record.substr(0, i), 1) ||
            i > record.rfind('|'), "every structural truncation rejects; diagnostic code may legitimately be shorter");
    std::cout << "maximum escaped typed record=" << record.size() << " bytes; limit=" << app::kOperationalRecordMaximum << '\n';
}
void legacy_and_independent_sinks() {
    auto version4 = app::encode_operational_record(entry());
    version4.replace(0,7,"SFXLOG4");
    require(app::decode_operational_record(version4,1).has_value(),"strict historical SFXLOG4 reader");
    auto version3 = app::encode_operational_record(entry());
    version3.erase(version3.rfind('|'));
    version3.replace(0, 7, "SFXLOG3");
    const auto decoded3 = app::decode_operational_record(version3, 1);
    require(decoded3 && decoded3->event->context.attempted_name.empty(),
        "strict historical SFXLOG3 reader supplies no attempted identity");
    auto version2 = version3;
    version2.erase(version2.rfind('|'));
    version2.replace(0, 7, "SFXLOG2");
    const auto decoded2 = app::decode_operational_record(version2, 1);
    require(decoded2 && decoded2->event->context.hostname.empty() &&
        decoded2->event->context.attempted_name.empty(), "strict historical SFXLOG2 reader");
    require(!app::decode_operational_record(version2 + "|host.example", 1),
        "SFXLOG2 cannot smuggle new hostname field");
    auto warning = app::make_operational_entry(1, 1, {app::OperationalKind::hostname_exhausted, {}, {}});
    warning.event->context.epoch = id(1); warning.event->context.origin_sequence = 1;
    require(app::valid_operational_entry(warning) && warning.level == "WARNING" &&
        warning.message == app::kHostnameExhaustedWarning, "exact owner-approved global exhaustion warning");
    auto host = entry(); host.event->context.hostname = "node.example";
    host.message = app::operational_message(*host.event);
    auto encoded_host = app::encode_operational_record(host);
    const auto decoded_host = app::decode_operational_record(encoded_host, 1);
    require(decoded_host && decoded_host->event->context.hostname == "node.example" &&
        decoded_host->message == "Node 1 SSH <unknown> node.example [203.0.113.4]: connection accepted",
        "hostname remains correlated with immutable IP/epoch/generation in SFXLOG5");
    for (const auto* invalid : {"bad|name", "bad\r\nname", "Bad.Example", "bad.example.", "bad\x1b[31m"}) {
        host.event->context.hostname = invalid; host.message = app::operational_message(*host.event);
        require(!app::valid_operational_entry(host), "invalid/noncanonical untrusted hostname rejected by log contract");
    }
    const std::string legacy = "100|INFO|connection|Node 1 assignment 8 epoch 01000000000000000000000000000000 SSH: assigned";
    auto old = app::decode_operational_record(legacy, 1);
    require(old && !old->event && app::encode_operational_record(*old) == legacy, "legacy file bytes retained without fabricated typed origin");
    require(app::render_operational_line(*old, "2026-09-06 12:00:00") ==
        "2026-09-06 12:00:00 [INFO] [legacy] Node 1 SSH: assigned", "only exact generated legacy prefix is simplified");
    for (const auto value : {"100|INFO|runtime|", "-1|INFO|runtime|bad", "100|INFO|runtime|extra|field", "SFXLOG9|100|bad"})
        require(!app::decode_operational_record(value, 1), "strict legacy/version/malformed rejection");
    struct Sink final : app::IOperationalEventSink {
        app::Identifier128 epoch; std::uint64_t generation{0}; unsigned seen{0};
        void observe(const app::AlphaOperationalLogEntry& e) noexcept override {
            if (!e.event) return;
            epoch = e.event->context.epoch; generation = e.event->context.generation; ++seen;
        }
    } first, second;
    auto record = entry(); first.observe(record); second.observe(record);
    auto reused = entry(); reused.event->context.generation = 8;
    first.observe(reused);
    require(first.seen == 2 && second.seen == 1 && second.generation == 7 && first.generation == 8 &&
        first.epoch == second.epoch, "independent future sinks consume stable typed correlation with no text parsing or node lookup");
    auto degraded = app::make_operational_entry(1, 1, {app::OperationalKind::durable_journal_degraded, {}, {}});
    degraded.event->context.epoch = id(1); degraded.event->context.origin_sequence = 1;
    const auto journal_warning = app::encode_operational_record(degraded);
    require(!journal_warning.empty() && app::decode_operational_record(journal_warning, 1) &&
        degraded.level == "WARNING" && degraded.message.find("SQLite accounting continue") != std::string::npos,
        "typed durable journal failure round trip remains operational, not recursive attention");
    for (std::uint8_t bit = 1; bit <= 16; bit <<= 1)
        require(!app::operational_log_health_text(bit).empty(), "every failure state has explicit operator guidance");
    require(app::operational_log_health_text(0).empty(), "healthy logging emits no warning");
}
void operating_reporting() {
    for (unsigned k=static_cast<unsigned>(app::OperationalKind::botgate_passed);
        k<=static_cast<unsigned>(app::OperationalKind::runtime_stopped);++k) {
        auto context=caller();
        if (k>=static_cast<unsigned>(app::OperationalKind::botgate_blocked)) {
            context={}; context.epoch=id(1); context.origin_sequence=k;
        }
        if (k==static_cast<unsigned>(app::OperationalKind::botgate_blocked)) {
            context.attempt=1; context.transport=app::OperationalTransport::telnet;
            context.source_ip="192.0.2.1"; context.diagnostic_code="manual_ip_rule";
        }
        if (k==static_cast<unsigned>(app::OperationalKind::flood_aggregate)) {
            context.resource_id=100; context.resource_name="source_refused";
        }
        auto record=app::encode_operational_record(app::make_operational_entry(k,100,
            {static_cast<app::OperationalKind>(k),{},context}));
        require(!record.empty() && app::decode_operational_record(record,k).has_value(),"BotGate correlated/global kinds roundtrip");
        record.replace(0,7,"SFXLOG5");
        require(!app::decode_operational_record(record,k),"historical v5 rejects new v6 kinds");
    }
    app::OperationalReportingSnapshot snapshot;
    for (unsigned k = static_cast<unsigned>(app::OperationalKind::account_administration);
        k <= static_cast<unsigned>(app::OperationalKind::configuration_saved); ++k) {
        app::OperationalContext context; context.epoch=id(1); context.origin_sequence=k;
        const auto value=app::make_operational_entry(k,100,{static_cast<app::OperationalKind>(k),{},context});
        const auto record=app::encode_operational_record(value);
        require(!record.empty() && app::decode_operational_record(record,k).has_value(),"new committed operational kinds round trip");
        auto old=record; old.replace(0,7,"SFXLOG4");
        require(!app::decode_operational_record(old,k),"new kinds cannot masquerade as historical SFXLOG4");
        app::observe_operational_reporting(snapshot,value);
    }
    require(snapshot.available && snapshot.recent.size()==8,"new administration/lifecycle events enter bounded Events history");
    require(snapshot.counts[static_cast<std::size_t>(app::OperationalCounter::account_changes)]==1 &&
        snapshot.counts[static_cast<std::size_t>(app::OperationalCounter::backups_completed)]==1 &&
        snapshot.counts[static_cast<std::size_t>(app::OperationalCounter::backups_failed)]==1,"real kind counters remain separate");
    for (unsigned i=0;i<100;++i) app::observe_operational_reporting(snapshot,entry());
    require(snapshot.recent.size()==8 && snapshot.counts[0]==100,"connection traffic counts without flushing operating history");
    snapshot.counts[0]=UINT64_MAX;
    app::observe_operational_reporting(snapshot,entry());
    require(snapshot.counts[0]==UINT64_MAX,"operational counts saturate rather than wrap");
    for (unsigned i=100;i<200;++i) {
        app::OperationalContext context; context.epoch=id(1); context.origin_sequence=i;
        app::observe_operational_reporting(snapshot,app::make_operational_entry(i,100,
            {app::OperationalKind::drain_started,{},context}));
    }
    require(snapshot.recent.size()==32 && snapshot.recent.front().sequence==168 &&
        snapshot.recent.back().sequence==199,"exact last 32 operating events retained");
    const auto before=snapshot.counts;
    auto invalid=entry(); invalid.message="invented activity";
    app::observe_operational_reporting(snapshot,invalid);
    app::observe_operational_reporting(snapshot,{1,1,"INFO","runtime","legacy",{}});
    require(snapshot.counts==before,"legacy or invalid records never invent counters");
    for (std::size_t i=0;i<snapshot.counts.size();++i)
        require(!app::operational_counter_label(static_cast<app::OperationalCounter>(i)).empty(),"every implemented counter has a bounded label");
}
void administration_reporting() {
    app::AdministrationReportingCache cache;
    unsigned loads=0;
    const auto loader=[&]() -> std::optional<std::vector<app::AdministrationAuditSummary>> {
        ++loads; std::vector<app::AdministrationAuditSummary> rows;
        for (unsigned i=1;i<=64;++i) rows.push_back({i%2==0,0,1,0,static_cast<std::int64_t>(i),id(i)});
        return rows;
    };
    auto first=cache.refresh(1,loader);
    require(first.available && first.recent.size()==32 && first.recent.front().occurred_utc_seconds==64 &&
        first.recent.back().occurred_utc_seconds==33,"two bounded histories merge newest-first into exact32");
    for (unsigned i=0;i<1000;++i) (void)cache.refresh(1,loader);
    require(loads==1,"1000 idle status polls issue zero extra audit reads");
    require(cache.refresh(2,loader).available && loads==2,"new writer commit invalidates once");
    require(!cache.refresh(3,[&]() -> std::optional<std::vector<app::AdministrationAuditSummary>> {
        ++loads; return std::nullopt;
    }).available,"audit read failure is unavailable, not a stale success");
    require(!cache.refresh(3,loader).available && loads==3,"failure cannot become a tight polling loop");
    require(cache.refresh(4,loader).available && loads==4,"next real commit permits recovery");
    auto invalid=first; invalid.recent.front().kind=255;
    require(!app::valid_administration_reporting(invalid),"unknown audit codes reject");
    invalid=first; invalid.available=false;
    require(!app::valid_administration_reporting(invalid),"unavailable history cannot contain purported facts");
    invalid=first; invalid.recent[1]=invalid.recent[0];
    require(!app::valid_administration_reporting(invalid),"duplicate audit identity rejects");
    invalid=first; invalid.recent.front().subject_account_id=-1;
    require(!app::valid_administration_reporting(invalid),"negative audit account rejects");
}
}
int main() {
    {
        app::OperationalEvent e;e.kind=app::OperationalKind::message_maintenance_problem;
        e.context.epoch.bytes[0]=1;e.context.origin_sequence=1;e.context.resource_id=2;
        e.context.resource_name="Echo #2: committed removals=32; protected overage=1";e.context.diagnostic_code="partial";
        const auto record=app::make_operational_entry(1,1791278000,e);const auto encoded=app::encode_operational_record(record);
        require(record.level=="WARNING" && encoded.rfind("SFXLOG7|",0)==0 && app::decode_operational_record(encoded,1).has_value(),"new content-free maintenance warning uses additive log7");
        require(app::operational_class(e.kind)==app::OperationalClass::runtime && app::operational_filter_group(record)==4 && app::operational_sysop_attention(e.kind),"maintenance problems visible in existing operating logs");
        auto forged=encoded;forged.replace(0,7,"SFXLOG6");require(!app::decode_operational_record(forged,1),"old log6 cannot carry new kind silently");
        e.kind=app::OperationalKind::message_maintenance;
        const auto normal=app::make_operational_entry(2,1791278001,e);require(normal.level=="INFO" && !app::operational_sysop_attention(e.kind),"normal maintenance quiet, no private subjects/bodies/identities");
        e.kind=app::OperationalKind::file_maintenance_problem;e.context.resource_name="ClamAV scratch retained";
        const auto warning=app::make_operational_entry(3,1791278002,e);const auto file_record=app::encode_operational_record(warning);
        require(file_record.rfind("SFXLOG8|",0)==0 && app::decode_operational_record(file_record,3).has_value() && warning.level=="WARNING" && app::operational_filter_group(warning)==4 && app::operational_sysop_attention(e.kind),"file maintenance has distinct durable Sysop attention");
        auto old=file_record;old.replace(0,7,"SFXLOG7");require(!app::decode_operational_record(old,3),"log7 cannot accept new file event kind");
    }
    for (const auto& item:{std::pair{app::OperationalKind::connection_assigned,1U},
            std::pair{app::OperationalKind::authenticated,2U},std::pair{app::OperationalKind::botgate_blocked,3U},
            std::pair{app::OperationalKind::feed_update_failed,3U},std::pair{app::OperationalKind::configuration_saved,4U},
            std::pair{app::OperationalKind::runtime_started,4U},std::pair{app::OperationalKind::runtime_stopped,4U}}) {
        app::AlphaOperationalLogEntry view;
        view.event=app::OperationalEvent{item.first,{}, {}};
        require(app::operational_filter_group(view)==item.second,"typed Live Logs category");
        view.event->kind=app::OperationalKind::flood_aggregate;
        view.event->context.resource_name=std::string(app::operational_kind_label(item.first))+"_mixed_sources";
        require(app::operational_filter_group(view)==item.second,"aggregate retains originating filter category");
    }
    try {
        goldens_and_classification(); bounds_and_rejections(); legacy_and_independent_sinks();
        operating_reporting(); administration_reporting();
        std::cout << "SFX L1 operational log tests PASS assertions=" << assertions << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "SFX L1 operational log tests FAIL: " << e.what() << '\n'; return 1; }
}
