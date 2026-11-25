#include "sfx/application/hostname_lookup.hpp"
#include "sfx/application/caller_assignment.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace app = sfx::application;
unsigned checks{};
void check(bool ok, const char* label) { ++checks; if (!ok) throw std::runtime_error(label); }
app::Identifier128 epoch(unsigned n) { app::Identifier128 e; e.bytes[0] = static_cast<std::uint8_t>(n); return e; }
app::HostnameSubscription sub(unsigned node, unsigned address = 1, std::uint64_t generation = 1,
    std::uint64_t life = 1, std::uint64_t deadline = 2000) {
    return {{epoch(1), static_cast<std::uint16_t>(node), generation},
        {{127, 0, static_cast<std::uint8_t>(address / 256), static_cast<std::uint8_t>(address % 256)}}, life, deadline};
}
void validation() {
    for (const auto* value : {"localhost", "Host.Example.", "xn--bcher-kva.example", "a-b.2", "123"})
        check(app::normalize_hostname_hint(value).has_value(), "safe ASCII hostname hint");
    check(app::normalize_hostname_hint("Host.Example.")->view() == "host.example", "normalize case and one root dot");
    for (const auto* value : {"", ".", "a..b", "a..", "-a", "a-", "a.-b", "a-.b", "a_b", "a b", "a|b", "a%b",
        "a\r\nb", "a\x1b[31mb", "a\\b", "a/b", "a<b>", "a@b"})
        check(!app::normalize_hostname_hint(value), "hostile/invalid hint rejected");
    check(!app::normalize_hostname_hint(std::string("a\0b", 3)), "embedded NUL rejected");
    check(!app::normalize_hostname_hint(std::string(1, static_cast<char>(128))), "non-ASCII rejected");
    const auto maximum = std::string(63, 'a') + "." + std::string(63, 'b') + "." +
        std::string(63, 'c') + "." + std::string(61, 'd');
    check(maximum.size() == 253 && app::normalize_hostname_hint(maximum), "253-byte maximum valid");
    check(app::normalize_hostname_hint(maximum + ".").has_value(), "maximum plus root dot accepted");
    check(!app::normalize_hostname_hint(maximum + "x"), "254-byte canonical name rejected");
    check(!app::normalize_hostname_hint(std::string(64, 'a')), "64-byte label rejected");
    for (const auto* ip : {"127.0.0.1", "0.0.0.0", "255.255.255.255"})
        check(app::hostname_source_address(ip).has_value(), "canonical IPv4");
    for (const auto* ip : {"", "127.1", "127.0.0.01", "256.0.0.1", "-1.0.0.1", "1.2.3.4.", "::1", "1.2.3.+4"})
        check(!app::hostname_source_address(ip), "malformed address rejected");
}
void completion_and_bounds() {
    app::HostnameLookupModel m; check(m.activate(1, true), "activate");
    std::array<app::HostnameSubscription, app::kAlphaNodePoolCapacity> callers{};
    for (unsigned i = 0; i < 4; ++i) callers[i] = sub(i + 1, i + 1);
    m.synchronize(callers, 0); check(m.queued() == 4, "four distinct addresses queued");
    for (std::size_t i = 0; i < 4; ++i) {
        check(m.next(0) == i, "lowest free native slot");
        check(m.bind(i, m.slot(i).request, i + 100), "matching request binds");
        check(!m.bind(i, m.slot(i).request, 900), "no second handle binding");
    }
    check(!m.next(0) && m.submissions() == 4, "four native requests hard bound");
    for (std::size_t i = 0; i < 4; ++i) {
        check(!m.complete(i, 900, "bad.example", 1), "wrong handle rejected");
        check(m.complete(i, i + 100, "Node" + std::to_string(i + 1) + ".example", 1), "matching completion");
        check(m.slot(i).state == app::HostnameSlotState::completed, "receipt alone not reusable");
        check(!m.complete(i, i + 100, "duplicate.example", 1), "duplicate completion rejected");
        check(m.publications()[i].subscription == callers[i], "publication exact identity");
        check(m.publications()[i].hint.view() == "node" + std::to_string(i + 1) + ".example", "independent names");
    }
    m.drained();
    for (std::size_t i = 0; i < 4; ++i) check(m.slot(i).state == app::HostnameSlotState::free, "drain allows reuse");
    callers[0] = sub(1, 8, 2); m.synchronize(callers, 2);
    check(!m.next(999), "four starts per rolling second");
    check(m.next(1000) == 0U, "rate budget replenishes at one second");
    check(!m.bind(0, 1, 100), "old request cannot bind reused slot");
    check(m.bind(0, m.slot(0).request, 100), "native handle number may reuse after proven drain");
    check(m.complete(0, 100, "reused.example", 1001), "new request completes");
    m.drained();
    for (unsigned generation = 3; generation <= 4098; ++generation) {
        const auto now = static_cast<std::uint64_t>(generation) * 1000;
        callers[0] = sub(1, 8, generation, 1, now + 2000);
        m.synchronize(callers, now);
        if (const auto slot = m.next(now)) {
            check(m.bind(*slot, m.slot(*slot).request, 100), "repeated reused handle bind");
            check(m.complete(*slot, 100, "reused.example", now + 1), "repeated completion");
            m.drained();
        }
        check(m.publications()[0].subscription == callers[0], "4096 generation gates including cached results");
        check(m.publications()[0].hint.view() == "reused.example", "cached/live answer remains correlated");
        check(m.cached() <= 256 && m.queued() <= 8 && m.quarantined() == 0, "fixed bounds through reuse");
    }
}
void cache_and_queue() {
    app::HostnameLookupModel m; check(m.activate(1, true), "cache lifetime");
    std::array<app::HostnameSubscription, app::kAlphaNodePoolCapacity> c{};
    for (unsigned i = 0; i < 4; ++i) c[i] = sub(i + 1);
    m.synchronize(c, 0); check(m.queued() == 1, "deduplicate same address across four subscribers");
    check(m.next(0) == 0U && m.bind(0, 1, 42), "one shared native request");
    check(m.complete(0, 42, "shared.example", 1), "shared result"); m.drained();
    for (const auto& p : m.publications()) if (p.subscription.valid())
        check(p.hint.view() == "shared.example", "all subscribers independently answered");
    c[0] = sub(1, 1, 2, 1, 300002); m.synchronize(c, 300000);
    check(m.publications()[0].hint.size && m.submissions() == 1, "positive cache valid just before five minutes");
    c[0] = sub(1, 1, 3, 1, 302001); m.synchronize(c, 300001);
    check(m.next(300001) == 0U && m.bind(0, 2, 42), "positive cache expires at five minutes");
    check(m.complete(0, 42, "bad|host", 300002), "invalid result safely completes negatively"); m.drained();
    c[0] = sub(1, 1, 4, 1, 362000); m.synchronize(c, 360001);
    check(!m.next(360001) && !m.publications()[0].hint.size, "negative cache suppresses work for 60 seconds");
    c[0] = sub(1, 1, 5, 1, 362002); m.synchronize(c, 360002);
    check(m.next(360002) == 0U, "negative expires after 60 seconds");
    check(m.bind(0, 3, 42) && m.complete(0, 42, "", 360003), "no-data completion retires without hostname"); m.drained();
    check(m.activate(2, true) && m.cached() == 0, "logical restart clears cache only");
    // Fill 256 positive entries inside their five-minute reuse interval.
    for (unsigned i = 1; i <= 260; ++i) {
        const auto now = static_cast<std::uint64_t>(400000 + i * 1000);
        c = {}; c[0] = sub(1, i, i, 2, now + 2000);
        m.synchronize(c, now); const auto slot = m.next(now);
        check(slot.has_value(), "LRU test has one native slot");
        check(m.bind(*slot, m.slot(*slot).request, 42), "LRU bind");
        check(m.complete(*slot, 42, "cache.example", now + 1), "LRU complete"); m.drained();
        check(m.cached() == std::min<std::size_t>(i, 256), "256 entry hard cache bound");
    }
    c[0] = sub(1, 1, 300, 2, 662000); m.synchronize(c, 660010);
    check(m.next(660010).has_value(), "least recently used address evicted");
    // Four pending plus eight distinct queued, even with rapid caller replacement.
    app::HostnameLookupModel q; check(q.activate(1, true), "queue model");
    for (unsigned wave = 0; wave < 5; ++wave) {
        for (unsigned i = 0; i < 4; ++i) c[i] = sub(i + 1, 4 * wave + i + 1, wave + 1);
        q.synchronize(c, 0);
        for (unsigned i = 0; i < 4; ++i) (void)q.next(0);
        check(q.queued() <= 8 && q.submissions() == 4, "queue saturation does not grow native work");
    }
    check(q.queued() == 8, "eight queued distinct addresses maximum");
}
void cancellation_and_quarantine() {
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        app::HostnameLookupModel m; check(m.activate(1, true), "cancellation model");
        std::array<app::HostnameSubscription, app::kAlphaNodePoolCapacity> c{};
        for (unsigned i = 0; i < 4; ++i) c[i] = sub(i + 1, i + 1);
        m.synchronize(c, 0);
        for (unsigned i = 0; i < 4; ++i) check(m.next(0) == i && m.bind(i, i + 1, 42), "outstanding native slots");
        if (scenario == 0) m.synchronize(c, 2000); // timeout without completion
        else if (scenario == 1) check(m.activate(2, false), "Off invalidates all outstanding native work");
        else if (scenario == 2) m.close(); // final process coordinator close
        else for (unsigned i = 0; i < 4; ++i) m.uncertain(i); // successful/failed/indeterminate cancellation
        check(m.quarantined() == 4 && m.exhausted(), "all uncertain buffers permanently quarantined");
        for (unsigned i = 0; i < 4; ++i) check(!m.complete(i, 42, "late.example", 1), "late result cannot reclaim quarantine");
        m.drained(); check(m.quarantined() == 4, "drain does not reclaim uncertain buffers");
        for (unsigned life = 3; life < 10003; ++life) {
            const bool started = m.activate(life, true);
            check(started == (scenario != 2), "logical restart allowed except after final close");
            c[0] = sub(1, 1, life, life); m.synchronize(c, 0);
            check(!m.next(0) && m.submissions() == 4 && m.quarantined() == 4, "10000 restarts never replenish process arena");
            check(!m.publications()[0].hint.size, "exhaustion is numeric-IP-only");
        }
    }
    app::HostnameLookupModel queued; check(queued.activate(1, true), "queued completion lifetime");
    std::array<app::HostnameSubscription, app::kAlphaNodePoolCapacity> c{}; c[0] = sub(1);
    queued.synchronize(c, 0); check(queued.next(0) == 0U && queued.bind(0, 1, 7), "queued completion request");
    c = {}; queued.synchronize(c, 1); // unsubscribe before completion is dispatched
    check(queued.complete(0, 7, "old.example", 1), "unsubscribed matching completion still retires safely");
    check(!queued.publications()[0].subscription.valid(), "no publication into departed caller");
    queued.drained(); check(!queued.quarantined(), "receipt and drain—not cancellation status—retires");
    c[0] = sub(1, 2, 2); c[0].assignment.runtime_epoch = epoch(2);
    queued.synchronize(c, 2);
    check(!queued.publications()[0].hint.size, "old hostname never crosses source/epoch/generation");
    check(!queued.complete(0, 7, "stale.example", 2), "stale native handle before new bind rejected");
    app::HostnameLookupModel off; check(off.activate(1, false), "Off active");
    off.synchronize(c, 0); check(!off.next(0) && !off.queued() && !off.cached(), "Off no lookup or cache");
}
void full_capacity_subscribers() {
    app::HostnameLookupModel model;
    check(model.activate(1, true), "128-subscriber lookup activates");
    std::array<app::HostnameSubscription, app::kAlphaNodePoolCapacity> callers{};
    for (unsigned index = 0; index < callers.size(); ++index) callers[index] = sub(index + 1);
    model.synchronize(callers, 0);
    check(model.queued() == 1 && model.next(0) == 0U && model.bind(0, 1, 42),
        "128 same-address subscribers share one native request");
    check(model.complete(0, 42, "shared.example", 1), "128-subscriber shared answer completes");
    for (const auto& result : model.publications())
        check(result.subscription.valid() && result.hint.view() == "shared.example",
            "each high-node subscriber has its own exact correlated publication");
    callers[127] = sub(128, 2, 2);
    model.synchronize(callers, 2);
    check(model.publications()[127].hint.size == 0 && model.publications()[126].hint.size != 0,
        "node 128 replacement clears only its old hostname");
    check(!sub(129).valid(), "out-of-capacity hostname subscription refuses");
    app::HostnameLookupModel overloaded;
    check(overloaded.activate(1, true), "128 distinct subscriber model activates");
    for (unsigned index = 0; index < callers.size(); ++index) callers[index] = sub(index + 1, index + 1);
    overloaded.synchronize(callers, 0);
    check(overloaded.queued() == 8, "128 subscribers preserve eight queued DNS jobs maximum");
    for (unsigned slot = 0; slot < 4; ++slot) check(overloaded.next(0) == slot, "native DNS concurrency remains four");
    check(!overloaded.next(0) && overloaded.submissions() == 4, "128 nodes do not expand native DNS arena");
}
}
int main() {
    try {
        validation(); completion_and_bounds(); cache_and_queue(); cancellation_and_quarantine();
        full_capacity_subscribers();
        std::cout << "PASS hostname lookup: " << checks << " assertions; 0 failed; 0 skipped\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL hostname lookup after " << checks << ": " << e.what() << '\n'; return 1;
    }
}
