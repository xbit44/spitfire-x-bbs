#include "sfx/adapters/storage/control_panel_statistics.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite3.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
namespace app = sfx::application;
namespace storage = sfx::adapters::storage;
using namespace std::chrono_literals;
unsigned assertions = 0;
void require(bool condition, const char* text) { ++assertions; if (!condition) throw std::runtime_error(text); }
template<class T> T collect(std::future<T>& future) {
    require(future.wait_for(10s) == std::future_status::ready, "accepted read resolves within test deadline");
    return future.get();
}
void fatal_readers(const std::filesystem::path& directory, unsigned count) {
    std::filesystem::create_directories(directory);
    std::atomic<bool> armed{false};
    std::mutex mutex;
    std::condition_variable entered;
    unsigned running = 0;
    std::promise<void> release;
    auto permit = release.get_future().share();
    storage::SqliteStorageSettings settings;
    settings.data_directory = directory;
    settings.utc_now = [] { return std::int64_t{1'788'000'000}; };
    settings.readers.pool_size = count;
    settings.readers.maximum_pending_operations = 3;
    settings.readers.enqueue_wait = 10s;
    settings.reader_dequeued_hook = [&] {
        if (!armed.load()) return;
        { std::lock_guard<std::mutex> lock(mutex); ++running; entered.notify_all(); }
        if (permit.wait_for(10s) != std::future_status::ready) throw std::runtime_error("fixture gate expired");
        throw std::runtime_error("synthetic password / System Password / throwaway input");
    };
    sfx::test::FakeStoragePathPolicy policy;
    storage::SqliteStorage database(settings, policy);
    require(database.start().succeeded(), "start actual SQLite writer/readers");
    armed.store(true);
    std::vector<std::future<app::StorageResult<app::FirstRunState>>> futures;
    for (unsigned index = 0; index < count; ++index)
        futures.push_back(std::async(std::launch::async, [&] { return database.first_run_state(); }));
    { std::unique_lock<std::mutex> lock(mutex);
      require(entered.wait_for(lock, 10s, [&] { return running == count; }), "all injected readers are held after dequeue"); }
    for (unsigned index = 0; index < 3; ++index)
        futures.push_back(std::async(std::launch::async, [&] { return database.first_run_state(); }));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (database.reader_pool_health().queued_operations != 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(database.reader_pool_health().queued_operations == 3, "three accepted queued promises behind stalled workers");
    futures.push_back(std::async(std::launch::async, [&] { return database.first_run_state(); }));
    release.set_value();
    for (auto& future : futures) {
        const auto result = collect(future);
        require(!result && result.status.code == app::StorageErrorCode::internal &&
            result.status.operation.find("synthetic") == std::string::npos,
            "in-flight queued and capacity-waiting reads fail with typed sanitized status");
    }
    const auto health = database.reader_pool_health();
    require(health.failed && health.queued_operations == 0, "fatal pool closed admission and drained pending jobs");
    require(database.health().state == app::StorageRuntimeState::recovery_required,
        "reader worker failure surfaces in aggregate storage health");
    require(database.first_run_state().status.code == app::StorageErrorCode::internal,
        "later reader is rejected promptly after worker death");
    require(!database.shutdown().succeeded(), "shutdown joins workers but does not falsely acknowledge failed reader pool");
    storage::SqliteStorage reopened(settings, policy);
    armed.store(false);
    require(reopened.start().succeeded() && static_cast<bool>(reopened.first_run_state()),
        "fresh reader ownership starts clean against same durable database");
    require(reopened.shutdown().succeeded(), "fresh ownership stops cleanly");
    std::cout << "PASS reader fatal failure: " << count << " in-flight + 3 queued + 1 capacity contender; all resolved\n";
}
void exec_sql(sqlite3* database, const char* sql) {
    if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("statistics fixture SQL");
}
void statistics_snapshot(const std::filesystem::path& path) {
    // Tiny isolated reader-contract fixture, NOT a product schema or migration.
    sqlite3* writer = nullptr;
    require(sqlite3_open(path.u8string().c_str(), &writer) == SQLITE_OK, "open isolated statistics fixture");
    const auto close = [](sqlite3* value) { sqlite3_close(value); };
    std::unique_ptr<sqlite3, decltype(close)> owned(writer, close);
    exec_sql(writer, "PRAGMA journal_mode=WAL; CREATE TABLE sfx_account_sessions(started_utc_seconds INTEGER,persisted_seconds INTEGER);"
        "CREATE TABLE sfx_accounts(created_utc_seconds INTEGER);"
        "BEGIN; INSERT INTO sfx_account_sessions VALUES(1000,2); INSERT INTO sfx_accounts VALUES(1000); COMMIT;");
    for (unsigned round = 1; round <= 32; ++round) {
        const auto result = storage::ControlPanelStatisticsReader::read_checked(path, 1000, 2000, [&] {
            // The first SELECT has established the reader snapshot. Commit a new
            // account/session before categories two/three: they MUST stay at round.
            exec_sql(writer, "BEGIN; INSERT INTO sfx_account_sessions VALUES(1000,2);"
                "INSERT INTO sfx_accounts VALUES(1000); COMMIT;");
        });
        require(result && result.value && result.value->successful_logons.total == round &&
            result.value->new_users.total == round && result.value->caller_seconds.total == round * 2 &&
            result.value->successful_logons.today == round, "all categories share one snapshot despite intervening commit");
        const auto next = storage::ControlPanelStatisticsReader::read_checked(path, 1000, 2000);
        require(next && next.value && next.value->new_users.total == round + 1 &&
            next.value->successful_logons.total == round + 1, "next snapshot includes complete newer commit once");
    }
    exec_sql(writer, "ALTER TABLE sfx_accounts RENAME TO deliberately_missing;");
    const auto failed = storage::ControlPanelStatisticsReader::read_checked(path, 1000, 2000);
    require(!failed && !failed.value && failed.status.operation == "read coherent runtime statistics: new_users/1",
        "partial SELECT success is not published as coherent statistics");
    const auto compatibility = storage::ControlPanelStatisticsReader::read(path, 1000, 2000);
    require(!compatibility.successful_logons.available && !compatibility.new_users.available &&
        !compatibility.caller_seconds.available, "all categories unavailable together on failure");
    std::cout << "PASS statistics: 32 forced mid-read commits, exact snapshots and typed partial-read failure\n";
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "scratch root required");
        const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
        require(root.filename() == "stage2-shared-services", "refuse broad test cleanup");
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        fatal_readers(root / "one-reader", 1);
        fatal_readers(root / "two-readers", 2);
        statistics_snapshot(root / "statistics-only.db");
        std::filesystem::remove_all(root);
        std::cout << "PASS runtime shared services: " << assertions << " assertions\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL shared services: " << error.what() << '\n'; return 1; }
}
