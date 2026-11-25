#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/control_panel.hpp"
#include "sfx/test/storage_fakes.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

namespace app = sfx::application;
namespace storage = sfx::adapters::storage;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

app::ControlPanelSqlitePagerCounters portable(
    const storage::SqlitePagerCounterSnapshot& value) {
    return {value.available, value.connection_generation, value.cache_hits,
        value.cache_misses, value.cache_writes};
}

storage::SqliteStorageSettings settings_for(const std::filesystem::path& directory) {
    storage::SqliteStorageSettings settings;
    settings.data_directory = directory;
    settings.queue.dequeue_batch_window = std::chrono::milliseconds(1);
    settings.utc_now = [] { return std::int64_t{1'788'000'000}; };
    return settings;
}

storage::SqlitePagerCounterSnapshot wait_for_change(
    storage::SqliteStorage& database,
    const storage::SqlitePagerCounterSnapshot& before,
    const bool writes) {
    auto current = database.pager_counters();
    for (int attempt = 0; attempt < 100; ++attempt) {
        const bool changed = writes ? current.cache_writes > before.cache_writes :
            current.cache_hits + current.cache_misses > before.cache_hits + before.cache_misses;
        if (changed) return current;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        current = database.pager_counters();
    }
    return current;
}

void run(const std::filesystem::path& root) {
    require(root.filename() == "alpha-slice-b-sqlite-pager",
        "refuse broad SQLite pager test cleanup");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error && std::filesystem::create_directories(root), "prepare pager test root");
    sfx::test::FakeStoragePathPolicy policy;
    std::uint64_t first_generation = 0;
    app::ControlPanelSqliteRateTracker tracker;
    {
        storage::SqliteStorage database(settings_for(root), policy);
        require(database.start().succeeded(), "start SQLite pager telemetry fixture");
        const auto initial = database.pager_counters();
        require(initial.available && initial.connection_generation != 0,
            "active writer and reader connections publish aggregate counters");
        first_generation = initial.connection_generation;
        require(tracker.sample(portable(initial), 1000).available,
            "first aggregate pager sample establishes a baseline");
        for (int index = 0; index < 24; ++index)
            require(database.first_run_state().succeeded(), "generate known database reads");
        const auto after_reads = wait_for_change(database, initial, false);
        require(after_reads.cache_hits + after_reads.cache_misses >
                initial.cache_hits + initial.cache_misses,
            "known reads advance aggregate CACHE_HIT plus CACHE_MISS counters");

        app::RateLimitSnapshot snapshot;
        snapshot.key.scope = app::RateLimitScope::source;
        snapshot.key.subject_hash.fill(0x5a);
        snapshot.failure_count = 1;
        snapshot.window_started_utc_seconds = 1'788'000'000;
        snapshot.last_failure_utc_seconds = 1'788'000'001;
        snapshot.blocked_until_utc_seconds = 1'788'000'002;
        snapshot.expires_utc_seconds = 1'788'000'900;
        require(database.store_rate_limit_snapshot(snapshot).succeeded(),
            "generate known database write");
        const auto after_write = wait_for_change(database, after_reads, true);
        require(after_write.cache_writes > after_reads.cache_writes,
            "known write advances aggregate CACHE_WRITE counters");
        const auto activity = tracker.sample(portable(after_write), 2000);
        require(activity.interval_available && activity.page_reads_per_second > 0 &&
                activity.page_writes_per_second > 0,
            "read and write rates respond to known pager activity");
        const auto settled = tracker.sample(portable(after_write), 3000);
        require(settled.interval_available && settled.page_reads_per_second == 0 &&
                settled.page_writes_per_second == 0,
            "unchanged counters settle to zero without resetting SQLite counters");
        require(database.shutdown().succeeded() && !database.pager_counters().available,
            "closed connections make pager telemetry unavailable");
    }
    {
        storage::SqliteStorage replacement(settings_for(root), policy);
        require(replacement.start().succeeded(), "restart SQLite pager telemetry fixture");
        const auto counters = replacement.pager_counters();
        require(counters.available && counters.connection_generation != first_generation,
            "replacement connections publish a new generation");
        require(!tracker.sample(portable(counters), 4000).interval_available,
            "replacement generation cannot create a false activity spike");
        require(replacement.shutdown().succeeded(), "stop replacement SQLite fixture");
    }
    std::filesystem::remove_all(root, error);
    require(!error, "clean pager test root");
}

}  // namespace

int main(const int argc, char** argv) {
    try {
        require(argc == 2, "SQLite pager test root is required");
        run(std::filesystem::absolute(argv[1]));
        std::cout << "SFX SQLite pager metrics tests PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX SQLite pager metrics tests FAIL: " << error.what() << '\n';
        return 1;
    }
}
