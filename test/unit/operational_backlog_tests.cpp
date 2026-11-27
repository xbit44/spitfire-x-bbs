#include "sfx/application/operational_backlog.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include <iostream>
#include <stdexcept>

namespace {
namespace app = sfx::application;
unsigned assertions = 0;
void require(bool ok, const char* label) { ++assertions; if (!ok) throw std::runtime_error(label); }
app::Identifier128 id(unsigned n) { app::Identifier128 i; i.bytes[0] = static_cast<std::uint8_t>(n); return i; }
app::OperationalEncodedEntry entry(std::uint64_t sequence, bool large = false) {
    app::OperationalEvent e; e.kind = app::OperationalKind::connection_assigned;
    auto& c = e.context; c.epoch = id(1); c.origin_sequence = sequence; c.node = 1;
    c.generation = 7; c.attempt = 3; c.transport = app::OperationalTransport::ssh;
    c.source_ip = "203.0.113.4";
    if (large) {
        c.hostname = std::string(63, 'a') + "." + std::string(63, 'b') + "." +
            std::string(63, 'c') + "." + std::string(61, 'd');
        c.resource_id = 9; c.resource_name = std::string(128, '%');
        c.diagnostic_code = std::string(96, 'd');
    }
    return {sequence, app::encode_operational_record(app::make_operational_entry(sequence, 1788731908, e))};
}
void run() {
    app::OperationalBacklog history(id(1));
    require(history.page({id(1), 0}).has_value(), "empty current history is a valid complete page");
    require(!history.page({id(2), 0}) && !history.page({id(1), 1}), "wrong epoch and future cursor reject");
    for (std::uint64_t i = 1; i <= 12000; ++i) {
        require(history.append(entry(i)), "append complete typed entry");
        require(history.bytes() <= app::kOperationalBacklogBytes &&
            history.entries().size() <= app::kOperationalBacklogEntries, "both bounds maintained on each append");
    }
    require(history.entries().size() == 10000 && history.oldest() == 2001 && history.newest() == 12000,
        "entry limit evicts precisely complete oldest entries");
    app::OperationalLogCache cache; cache.bind(id(1));
    std::uint64_t pages = 0, delivered = 0;
    do {
        const auto page = history.page(cache.cursor()); require(page.has_value(), "page exists");
        const auto bytes = app::encode_log_page(*page);
        require(!bytes.empty() && bytes.size() <= app::kOperationalPageBytes && bytes.size() <= 65536,
            "all pages fit unchanged frame ceiling");
        auto decoded = app::decode_log_page(bytes);
        require(decoded && cache.accept(*decoded), "strict page roundtrip and cache advance");
        delivered += page->entries.size(); ++pages;
    } while (cache.more());
    require(delivered == 10000 && pages == 79 && cache.gap() && cache.backlog().oldest() == 2001,
        "bounded paged reconstruction has exact count ordering and explicit eviction gap");
    const auto frozen = cache.cursor();
    cache.clear_presentation();
    require(cache.cursor().after == 0 && cache.backlog().entries().empty(), "presentation clears without Runtime mutation");
    const auto first = *history.page(cache.cursor());
    require(cache.accept(first) && history.newest() == frozen.after, "same-epoch backlog reconstructs");
    require(!cache.accept(first), "delayed duplicate page cannot overwrite advanced cache");
    auto crossed = *history.page(cache.cursor());
    for (auto& item : crossed.entries) {
        auto decoded = app::decode_operational_record(item.record, item.sequence);
        if (!decoded || !decoded->event) {
            throw std::runtime_error("page boundary fixture requires a decoded event");
        }
        decoded->event->context.origin_sequence -= first.entries.size();
        item.record = app::encode_operational_record(*decoded);
    }
    const auto unchanged = cache.cursor().after;
    require(app::valid_operational_page(crossed) && !cache.accept(crossed) &&
        cache.cursor().after == unchanged && cache.backlog().newest() == unchanged,
        "origin regression across page boundary rejects transactionally");
    auto bad = first; bad.cursor.epoch = id(2);
    require(!app::valid_operational_page(bad), "record and frame epoch must match");
    bad = first; bad.entries[1] = bad.entries[0]; require(!app::valid_operational_page(bad), "duplicate sequence rejects");
    bad = first; ++bad.next; require(!app::valid_operational_page(bad), "inconsistent next cursor rejects");
    bad = first; bad.gap = false; require(!app::valid_operational_page(bad), "hidden gap rejects");
    bad = first; bad.entries.clear(); bad.next = bad.cursor.after;
    require(!app::valid_operational_page(bad), "empty page cannot hide available history");
    auto bytes = app::encode_log_page(first);
    bytes.push_back(0); require(!app::decode_log_page(bytes), "trailing payload rejects");
    bytes.resize(app::kOperationalPageBytes + 1); require(!app::decode_log_page(bytes), "oversized payload rejects");
    bytes = app::encode_log_page(first); bytes[49] = 129;
    require(!app::decode_log_page(bytes), "oversized record count rejects before allocation");
    require(!app::decode_log_cursor(std::vector<std::uint8_t>(25)), "malformed cursor rejects");
    const auto cursor = app::encode_log_cursor({id(1), 12000});
    require(app::decode_log_cursor(cursor)->after == 12000, "cursor exact roundtrip");
    history = app::OperationalBacklog(id(1));
    const auto large = entry(1, true);
    require(large.record.size() > 839, "real maximal context reaches byte bound before entry bound");
    for (std::uint64_t i = 1; i <= 11000; ++i) {
        require(history.append(entry(i, true)), "large complete context retained");
        require(history.bytes() <= app::kOperationalBacklogBytes, "encoded byte cap never exceeded");
    }
    require(history.entries().size() < 10000 && history.oldest() > 1001,
        "byte limit evicts complete records before count limit");
    const auto maximal = history.page({id(1), 0});
    require(maximal && maximal->entries.size() < app::kOperationalPageEntries &&
        app::kOperationalPageBytes - app::encode_log_page(*maximal).size() < 10 + large.record.size() + 16,
        "largest content page fills byte budget without crossing it");
    cache.bind(id(2));
    require(!cache.accept(*maximal), "new Runtime rejects old-epoch backlog");
    app::OperationalBacklog replacement(id(2));
    require(cache.accept(*replacement.page({id(2), 0})) && cache.backlog().entries().empty(),
        "new Runtime starts empty, not prior durable or cached history");
    require(!history.append(entry(11002)), "producer sequence hole cannot enter history");
    app::AlphaRuntimeMetricSnapshot metrics; metrics.uptime_available = true;
    metrics.uptime_milliseconds = 0x100000001ULL;
    auto decoded = app::decode_runtime_metrics(app::encode_runtime_metrics(metrics));
    require(decoded && decoded->uptime_milliseconds == metrics.uptime_milliseconds,
        "uptime survives 32-bit tick boundary in matched numeric transport");
    metrics.uptime_available = false;
    require(app::encode_runtime_metrics(metrics).empty(), "unavailable uptime cannot carry invented duration");
    require(app::compact_uptime(0) == "0d 00:00" && app::compact_uptime(59999) == "0d 00:00" &&
        app::compact_uptime((3ULL*24*60+4*60+12)*60000) == "3d 04:12", "compact day/hour/minute boundaries");
    require(app::compact_uptime(0x100000000ULL) == "49d 17:02", "OS uptime does not wrap at 32-bit tick boundary");
    require(app::uptime_reference_fraction(0,0) == 0 && app::uptime_reference_fraction(30,120) == .25 &&
        app::uptime_reference_fraction(UINT64_MAX,UINT64_MAX) == 1 &&
        app::uptime_reference_fraction(120,30) == 1, "duration bars safely clamp zero, high counters and inconsistent snapshots");
    require(app::compact_uptime(3600000 + 8*3600000) == "0d 09:00", "sleep/hibernate counter advance remains elapsed duration");
}
}
int main() {
    try { run(); std::cout << "PASS UI6 bounded backlog assertions=" << assertions << " failures=0 skipped=0\n"; return 0; }
    catch (const std::exception& e) { std::cerr << "FAIL UI6 backlog: " << e.what() << '\n'; return 1; }
}
