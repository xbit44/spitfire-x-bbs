#include "alpha_operational_journal.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <windows.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
namespace app = sfx::application;
namespace win = sfx::platform::win32;
namespace runtime = sfx::apps::runtime;
unsigned assertions = 0;
void require(bool ok, const char* label) { ++assertions; if (!ok) throw std::runtime_error(label); }
app::Identifier128 id(unsigned n) { app::Identifier128 value; value.bytes[0] = static_cast<std::uint8_t>(n); return value; }
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(file.good(), "write isolated log fixture");
}
app::OperationalEvent event(unsigned node, std::uint64_t generation, std::uint64_t attempt) {
    app::OperationalEvent e; e.kind = app::OperationalKind::stage_changed;
    auto& c = e.context; c.node = static_cast<std::uint16_t>(node); c.generation = generation;
    c.attempt = attempt; c.transport = node % 2 ? app::OperationalTransport::telnet : app::OperationalTransport::ssh;
    c.source_ip = "127.0.0.1"; c.stage = app::CallerStage::main_menu;
    c.account_id = node; c.caller = "Caller " + std::to_string(node);
    c.durable_session_id = id(node + 10); c.correlation_id = id(node + 20);
    return e;
}
void run(const std::filesystem::path& root) {
    const auto sid = win::current_process_user_sid_string();
    require(std::filesystem::create_directories(root/L"Logs"),"create isolated data-only journal fixture");
    win::WindowsStorageDaclPolicy protection(sid);
    require(protection.apply(root/L"Logs",win::StorageDaclObjectKind::directory).succeeded(),"protect isolated Logs, not a fabricated installation");
    const auto path = root / L"Logs" / L"SFXRuntime.log";
    const std::string legacy = "100|INFO|runtime|historical marker\r\n";
    write(path, legacy);
    require(protection.apply(path,win::StorageDaclObjectKind::file).succeeded(),"protect seeded log");
    {
        runtime::AlphaOperationalJournal journal(path, sid, id(1));
        require(journal.snapshot().empty(), "new Runtime validates but never imports durable history");
        std::array<std::thread, 4> threads;
        for (unsigned node = 1; node <= 4; ++node) threads[node - 1] = std::thread([&, node] {
            const auto attempt = journal.next_attempt();
            for (unsigned n = 0; n < 100; ++n) { journal.emit(event(node, n + 1, attempt)); Sleep(40); }
        });
        for (auto& thread : threads) thread.join();
        require(journal.flush(),"wait for bounded asynchronous persistence");
        require(journal.health() == 0, "concurrent append has no hidden write/validation failures");
        const auto records = journal.snapshot();
        require(records.size() == 32 && records.back().sequence == 400, "small status tail has coherent concurrent Runtime delivery sequence");
        app::OperationalLogCache cache; cache.bind(id(1));
        unsigned delivered = 0;
        do {
            const auto page = journal.page(cache.cursor());
            require(page && app::valid_operational_page(*page) && cache.accept(*page), "native journal pages coherently across four writers");
            delivered += static_cast<unsigned>(page->entries.size());
        } while (cache.more());
        require(delivered == 400 && cache.backlog().entries().size() == 400,
            "full native Runtime history survives beyond the 32-record status notification tail");
        const auto bytes = read(path);
        require(bytes.substr(0, legacy.size()) == legacy, "new version appends without rewriting legacy bytes");
        std::set<std::uint64_t> origins, attempts;
        std::istringstream lines(bytes); std::string line; unsigned count = 0;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto e = app::decode_operational_record(line, ++count);
            require(e.has_value(), "every concurrent line decodes without interleaving or partial content");
            if (!e->event) continue;
            const auto& c = e->event->context;
            require(origins.insert(c.origin_sequence).second && c.epoch == id(1), "unique full origin identity");
            attempts.insert(c.attempt);
            require(c.caller == "Caller " + std::to_string(c.node) && c.account_id == c.node &&
                c.durable_session_id == id(c.node + 10) && c.correlation_id == id(c.node + 20) &&
                c.transport == (c.node % 2 ? app::OperationalTransport::telnet : app::OperationalTransport::ssh),
                "caller/activity/transport/durable context never crosses four concurrent producers");
        }
        require(count == 401 && origins.size() == 400 && attempts.size() == 4, "all four producers actually ran, none skipped");
        const auto original = event(1, 100, journal.next_attempt());
        journal.emit(original);
        auto newer = event(1, 101, journal.next_attempt()); journal.emit(newer);
        const auto retained = journal.snapshot();
        require(retained[30].event->context.generation == 100 && retained[31].event->context.generation == 101,
            "node reuse cannot rewrite historical immutable context");
        auto stale = newer; stale.context.epoch = id(99);
        journal.emit(stale);
        require(journal.snapshot().back().sequence == retained.back().sequence &&
            (journal.health() & app::operational_log_event_rejected), "wrong Runtime origin rejects rather than being rebound");
        require(win::WindowsStorageDaclPolicy(sid).assess(path, win::StorageDaclObjectKind::file).status.succeeded(),
            "typed journal retains exact protected-file DACL");
    }
    {
        runtime::AlphaOperationalJournal restarted(path, sid, id(2));
        require(restarted.snapshot().empty(), "replacement Runtime begins a fresh transient history");
        restarted.emit(event(1, 1, restarted.next_attempt()));
        require(restarted.flush(),"persist new epoch before file-denial test");
        const auto latest = restarted.snapshot().back();
        require(latest.event->context.epoch == id(2) && latest.event->context.origin_sequence == 1,
            "new Runtime has independent origin sequence and preserved old history");
        const auto before = read(path);
        const HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        require(held != INVALID_HANDLE_VALUE, "acquire fixture lock handle compatible with the persistent writer");
        OVERLAPPED range{};
        require(LockFileEx(held,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,MAXDWORD,MAXDWORD,&range)!=FALSE,
            "lock actual write range using a separate file handle");
        restarted.emit(event(2, 1, restarted.next_attempt()));
        require(restarted.flush(),"observe real write denial before releasing handle");
        require(UnlockFileEx(held,0,MAXDWORD,MAXDWORD,&range)!=FALSE,"release exact fixture range");
        CloseHandle(held);
        require((restarted.health() & app::operational_log_write_failed) && read(path) == before &&
            restarted.snapshot().back().event->context.node == 2, "real write denial remains visible while live records continue");
        restarted.emit(event(3, 1, restarted.next_attempt()));
        require(restarted.flush() && read(path)==before && restarted.health(), "uncertain failed append preserves bytes until deliberate recovery while live records continue");
    }
    const std::string prefix = "100|INFO|test|";
    const std::string row = prefix + std::string(32 - prefix.size() - 2, 'x') + "\r\n";
    std::string full;
    full.reserve(app::kOperationalFileMaximum);
    while (full.size() < app::kOperationalFileMaximum) full += row;
    write(path, full);
    {
        runtime::AlphaOperationalJournal capped(path, sid, id(3));
        app::LoggingConfiguration limits; limits.runtime={1,2};
        require(capped.configure(limits) && capped.health()==0 && capped.snapshot().empty(),
            "complete full history is healthy and eligible for configured rotation");
        capped.emit(event(4, 1, capped.next_attempt()));
        require(capped.flush() && read(path)!=full && capped.snapshot().back().event.has_value() && !capped.health(),
            "configured cap rotates and continues rather than freezing the physical log");
        bool preserved=false;
        for (const auto& file:std::filesystem::directory_iterator(root/L"Logs"))
            if (file.path()!=path && read(file.path())==full) preserved=true;
        require(preserved,"complete old history survives byte-exact in an archive");
    }
    full.resize(full.size() - 32); write(path, full);
    {
        runtime::AlphaOperationalJournal nearly(path, sid, id(4));
        app::LoggingConfiguration limits; limits.runtime={1,2}; require(nearly.configure(limits),"one MiB crossing test");
        require(nearly.health() == 0, "below-cap valid history is initially healthy");
        nearly.emit(event(1, 1, nearly.next_attempt()));
        require(nearly.flush() && !nearly.health() && read(path).size()<1024*1024,
            "rotation precedes append so a complete record cannot cross the configured cap");
    }
    const std::string partial = legacy + "SFXLOG2|unfinished";
    write(path, partial);
    {
        runtime::AlphaOperationalJournal broken(path, sid, id(5));
        require((broken.health() & app::operational_log_history_invalid) && broken.snapshot().empty(),
            "partial final record is not trusted or silently hidden");
        broken.emit(event(1, 1, broken.next_attempt()));
        require(read(path) == partial && broken.snapshot().size() == 1, "preserve malformed history; live records continue without replay");
    }
    {
        auto typed = event(1, 1, 1); typed.context.epoch = id(6); typed.context.origin_sequence = 2;
        const auto valid = app::encode_operational_record(app::make_operational_entry(1, 100, typed)) + "\r\n";
        typed.context.origin_sequence = 1;
        const auto stale = app::encode_operational_record(app::make_operational_entry(1, 100, typed)) + "\r\n";
        typed.context.epoch = id(7);
        const auto other = app::encode_operational_record(app::make_operational_entry(1, 100, typed)) + "\r\n";
        const auto malformed = valid + valid + stale + other;
        write(path, malformed);
        runtime::AlphaOperationalJournal journal(path, sid, id(8));
        std::uint8_t read_health = 0;
        const auto reloaded = runtime::AlphaOperationalJournal::read(path, read_health);
        require(journal.health() == app::operational_log_history_invalid && reloaded.size() == 2,
            "duplicate/regressing origins rejected before they can poison authenticated status encoding");
        require(reloaded[0].event->context.epoch == id(6) && reloaded[1].event->context.epoch == id(7),
            "different historical epoch has independent origin ordering");
        journal.emit(event(2, 1, journal.next_attempt()));
        require(journal.snapshot().size() == 1 && read(path) == malformed,
            "invalid history is preserved while safe status/live history and callers continue");
    }
    require(!std::filesystem::exists(root / L"Logs" / L"CALLERS.LOG") &&
        !std::filesystem::exists(root / L"Logs" / L"HEYSYSOP.LOG"), "neither future journal is created");
    std::cout << "PASS L1 native journal: four writers/400 typed records; retention, actual write denial, restart, identity, DACL and partial-history gates\n";
}
void reporting_only(const std::filesystem::path& root) {
    const auto sid=win::current_process_user_sid_string();
    require(std::filesystem::create_directories(root / L"Logs"),"create fresh data-only reporting fixture");
    require(win::WindowsStorageDaclPolicy(sid).apply(root / L"Logs",win::StorageDaclObjectKind::directory).succeeded(),
        "protect only fixture mutable Logs; no trusted installation/provisioning is claimed");
    const auto path=root / L"Logs" / L"SFXRuntime.log";
    {
        runtime::AlphaOperationalJournal journal(path,sid,id(42));
        require(journal.reporting().available && journal.reporting().recent.empty(),"new journal has genuine zero counters");
        journal.emit({app::OperationalKind::configuration_saved,{}, {}});
        std::array<std::thread,4> writers;
        for (unsigned n=0;n<4;++n) writers[n]=std::thread([&] {
            for (unsigned i=0;i<20;++i) journal.emit({app::OperationalKind::throttle_cleared,{}, {}});
        });
        for (auto& writer:writers) writer.join();
        const auto report=journal.reporting();
        require(report.counts[static_cast<std::size_t>(app::OperationalCounter::throttle_clears)]==80 &&
            report.counts[static_cast<std::size_t>(app::OperationalCounter::configuration_saves)]==1,
            "concurrent real journal append preserves exact operating counts");
        require(report.recent.size()==32 && report.recent.front().sequence==50 && report.recent.back().sequence==81,
            "operating ring retains exact bounded recent events");
        app::AlphaRuntimeStatus status;
        status.lifecycle.configured_nodes=4;
        status.runtime_epoch=id(42); status.status_sequence=1; status.reporting=report;
        require(app::decode_runtime_status(app::encode_runtime_status(status)).has_value(),"production journal report passes exact wire validation");
        auto invalid=event(1,1,1); invalid.context.node=129;
        journal.emit(invalid);
        journal.emit({app::OperationalKind::backup_completed,{}, {}});
        status.reporting=journal.reporting();
        require(status.reporting.recent.back().event->context.origin_sequence==83 &&
            status.reporting.recent.back().sequence==82 &&
            app::decode_runtime_status(app::encode_runtime_status(status)).has_value(),
            "a rejected event consumes origin only; later real report remains valid");
        require(status.reporting.counts[static_cast<std::size_t>(app::OperationalCounter::backups_completed)]==1,
            "rejected events do not increment a successful backup counter");
    }
    runtime::AlphaOperationalJournal restarted(path,sid,id(43));
    const auto reset=restarted.reporting();
    require(reset.available && reset.recent.empty() &&
        std::all_of(reset.counts.begin(),reset.counts.end(),[](auto n){return n==0;}),
        "new Runtime does not relabel persistent history as current-lifetime counters");
    require(restarted.health()==0,"all retained SFXLOG5 records are strictly readable on restart");
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--reporting-only"), "test requires explicit disposable root");
        const auto base = std::filesystem::weakly_canonical(std::filesystem::path(argv[1]));
        const auto scratch = std::filesystem::weakly_canonical(std::filesystem::path(SFX_LOG_TEST_SCRATCH));
        const auto relative = base.lexically_relative(scratch);
        require(!relative.empty() && !relative.is_absolute() && *relative.begin() != ".." && relative != ".",
            "fixture stays inside configured build scratch, never installed production");
        std::filesystem::create_directories(base);
        const auto root = base / ("run-" + std::to_string(GetTickCount64()));
        require(!std::filesystem::exists(root), "never overwrite an earlier fixture");
        if (argc==3) reporting_only(root); else run(root);
        std::cout << "SFX L1 native operational journal PASS assertions=" << assertions << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << "SFX L1 native journal FAIL: " << e.what() << '\n'; return 1; }
}
