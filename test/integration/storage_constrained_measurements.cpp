#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sqlite_internal.hpp"
#include "sqlite3.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace storage = sfx::adapters::storage;
namespace internal = sfx::adapters::storage::internal;
namespace app = sfx::application;
using namespace std::chrono_literals;

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

void remove_tree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    require(!error, "remove constrained-measurement tree");
}

storage::SqliteStorageSettings settings_for(const std::filesystem::path& directory) {
    storage::SqliteStorageSettings settings;
    settings.data_directory = directory;
    settings.utc_now = [] { return std::int64_t{1'787'552'000}; };
    settings.queue.maximum_operations = 128;
    settings.queue.maximum_bytes = 16U * 1024U;
    settings.queue.dequeue_batch_limit = 16;
    settings.queue.dequeue_batch_window = 20ms;
    settings.queue.enqueue_wait = 2000ms;
    settings.busy.sqlite_timeout = 25ms;
    settings.busy.transaction_deadline = 1500ms;
    settings.busy.maximum_attempts = 10;
    settings.checkpoint.passive_interval = std::chrono::seconds(3600);
    settings.checkpoint.frame_target = 1024;
    return settings;
}

int update_node(sqlite3* const database, const char* const sql, const int node) {
    sqlite3_stmt* statement = nullptr;
    int code = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
    if (code == SQLITE_OK) code = sqlite3_bind_int(statement, 1, node);
    if (code == SQLITE_OK) code = sqlite3_step(statement);
    const int finalize_code = sqlite3_finalize(statement);
    if (code == SQLITE_DONE && finalize_code == SQLITE_OK) return SQLITE_OK;
    return code == SQLITE_DONE ? finalize_code : code;
}

void verify_totals(
    const std::filesystem::path& database_file,
    const std::int64_t periodic,
    const std::int64_t outcomes,
    const std::int64_t exact_rows,
    const std::int64_t per_row_periodic,
    const std::int64_t per_row_outcomes) {
    internal::SqliteConnection verification(database_file, SQLITE_OPEN_READONLY, 50ms, false);
    require(
        verification.scalar_int64(
            "SELECT sum(modified_utc_seconds-created_utc_seconds) "
            "FROM sfx_security_levels "
            "WHERE security_level BETWEEN 1 AND 128",
            "verify constrained periodic writes") == periodic,
        "constrained periodic total is exact");
    require(
        verification.scalar_int64(
            "SELECT sum(created_utc_seconds) FROM sfx_security_levels "
            "WHERE security_level BETWEEN 1 AND 128",
            "verify constrained outcomes") == outcomes,
        "constrained outcome total is exact");
    const std::string exact_sql =
        "SELECT count(*) FROM sfx_security_levels "
        "WHERE security_level BETWEEN 1 AND 128 "
        "AND modified_utc_seconds-created_utc_seconds=" +
        std::to_string(per_row_periodic) + " AND created_utc_seconds=" +
        std::to_string(per_row_outcomes);
    const std::int64_t matching_rows = verification.scalar_int64(
        exact_sql, "verify constrained per-node totals");
    if (matching_rows != exact_rows) {
        std::cerr << "constrained_exact_rows expected=" << exact_rows
                  << " actual=" << matching_rows
                  << " periodic=" << per_row_periodic
                  << " outcomes=" << per_row_outcomes << '\n';
    }
    require(
        matching_rows == exact_rows,
        "constrained per-node totals are exact");
}

void slow_writer_service_time(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "slow-writer";
    std::filesystem::create_directories(data);
    storage::SqliteStorageSettings settings = settings_for(data);
    internal::SqlitePagerTelemetry telemetry;
    internal::SqliteWriterCoordinator coordinator;
    const std::filesystem::path database_file = data / "SFX.DB";
    require(
        coordinator.start(
            database_file, settings, storage::sfx_storage_migrations(), telemetry).succeeded(),
        "slow-writer coordinator starts");

    constexpr std::size_t operation_count = 1024;
    std::atomic<std::size_t> sequence{0};
    std::vector<std::future<app::StorageStatus>> writes;
    writes.reserve(operation_count);
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < operation_count; ++index) {
        const int node = static_cast<int>(index % 128U) + 1;
        writes.push_back(coordinator.submit({
            "modeled constrained slow-writer statistic",
            64,
            internal::WriteDurability::coalesced_in_progress_statistics,
            [node, &sequence](sqlite3* database) {
                if ((sequence.fetch_add(1) + 1U) % 16U == 0U) {
                    std::this_thread::sleep_for(8ms);
                }
                return update_node(
                    database,
                    "INSERT INTO sfx_security_levels("
                    "security_level,label,description,level_state,"
                    "created_utc_seconds,modified_utc_seconds) "
                    "VALUES(?1,'Load',NULL,'active',0,1) "
                    "ON CONFLICT(security_level) DO UPDATE SET "
                    "modified_utc_seconds=modified_utc_seconds+1",
                    node);
            }
        }));
    }
    for (auto& write : writes) {
        require(write.get().succeeded(), "slow-writer request commits");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    verify_totals(database_file, 1024, 0, 128, 8, 0);

    const app::StorageHealth health = coordinator.health();
    require(health.committed_operations == operation_count, "slow-writer operation count");
    require(health.rejected_operations == 0, "slow-writer has no backpressure loss");
    require(health.queue_high_water_operations <= settings.queue.maximum_operations,
            "slow-writer queue stays bounded");
    require(health.queue_high_water_bytes <= settings.queue.maximum_bytes,
            "slow-writer bytes stay bounded");
    require(health.maximum_committed_batch_operations <= settings.queue.dequeue_batch_limit,
            "slow-writer batch stays bounded");
    require(elapsed >= 400ms && elapsed < 30s, "slow-writer timing stays in acceptance window");
    require(coordinator.shutdown().succeeded(), "slow-writer shutdown succeeds");

    std::cout << "constrained_slow_writer operations=" << health.committed_operations
              << " batches=" << health.committed_batches
              << " maximum_batch=" << health.maximum_committed_batch_operations
              << " queue_high_water=" << health.queue_high_water_operations
              << " elapsed_ms=" << elapsed.count() << '\n';
}

void external_writer_contention(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "external-writer-contention";
    std::filesystem::create_directories(data);
    storage::SqliteStorageSettings settings = settings_for(data);
    internal::SqlitePagerTelemetry telemetry;
    internal::SqliteWriterCoordinator coordinator;
    const std::filesystem::path database_file = data / "SFX.DB";
    require(
        coordinator.start(
            database_file, settings, storage::sfx_storage_migrations(), telemetry).succeeded(),
        "contention coordinator starts");

    std::promise<int> lock_result;
    std::future<int> lock_ready = lock_result.get_future();
    std::promise<void> release_lock;
    std::shared_future<void> release_signal = release_lock.get_future().share();
    std::thread holder([database_file, &lock_result, release_signal] {
        sqlite3* database = nullptr;
        int code = sqlite3_open_v2(
            database_file.string().c_str(),
            &database,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,
            nullptr);
        if (code == SQLITE_OK) {
            code = sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        }
        lock_result.set_value(code);
        if (code == SQLITE_OK) {
            release_signal.wait();
            (void)sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
        }
        if (database != nullptr) (void)sqlite3_close(database);
    });
    const int acquired_code = lock_ready.get();
    if (acquired_code != SQLITE_OK) {
        holder.join();
    }
    require(acquired_code == SQLITE_OK, "external writer obtains bounded lock");

    const auto started = std::chrono::steady_clock::now();
    auto write = coordinator.submit({
        "modeled antivirus or external-process writer contention",
        64,
        internal::WriteDurability::durable,
        [](sqlite3* database) {
            return update_node(
                database,
                "INSERT INTO sfx_security_levels("
                "security_level,label,description,level_state,"
                "created_utc_seconds,modified_utc_seconds) "
                "VALUES(?1,'Load',NULL,'active',1,1) "
                "ON CONFLICT(security_level) DO UPDATE SET "
                "created_utc_seconds=created_utc_seconds+1",
                1);
        }
    });
    std::this_thread::sleep_for(225ms);
    release_lock.set_value();
    const app::StorageStatus write_status = write.get();
    holder.join();
    require(write_status.succeeded(), "contended request commits after bounded lock release");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    verify_totals(database_file, 0, 1, 1, 0, 1);
    const app::StorageHealth health = coordinator.health();
    require(health.busy_retries > 0, "external contention records busy retries");
    require(elapsed >= 200ms && elapsed < 2000ms, "external contention stays within deadline");
    require(coordinator.shutdown().succeeded(), "contention shutdown succeeds");

    std::cout << "constrained_external_lock busy_retries=" << health.busy_retries
              << " elapsed_ms=" << elapsed.count() << '\n';
}

void fixed_128_node_soak(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "fixed-soak";
    std::filesystem::create_directories(data);
    storage::SqliteStorageSettings settings = settings_for(data);
    settings.queue.maximum_operations = 512;
    settings.queue.maximum_bytes = 1024U * 1024U;
    settings.queue.dequeue_batch_limit = 32;
    settings.queue.enqueue_wait = 5000ms;
    internal::SqlitePagerTelemetry telemetry;
    internal::SqliteWriterCoordinator coordinator;
    const std::filesystem::path database_file = data / "SFX.DB";
    require(
        coordinator.start(
            database_file, settings, storage::sfx_storage_migrations(), telemetry).succeeded(),
        "fixed-soak coordinator starts");

    constexpr int cycles = 3;
    constexpr std::size_t operation_count = 128U * 61U * cycles;
    std::vector<std::future<app::StorageStatus>> writes;
    writes.reserve(operation_count);
    const auto started = std::chrono::steady_clock::now();
    for (int cycle = 0; cycle < cycles; ++cycle) {
        for (int node = 1; node <= 128; ++node) {
            for (int minute = 0; minute < 60; ++minute) {
                (void)minute;
                writes.push_back(coordinator.submit({
                    "fixed-soak coalesced minute statistic",
                    32,
                    internal::WriteDurability::coalesced_in_progress_statistics,
                    [node](sqlite3* database) {
                        return update_node(
                            database,
                            "INSERT INTO sfx_security_levels("
                            "security_level,label,description,level_state,"
                            "created_utc_seconds,modified_utc_seconds) "
                            "VALUES(?1,'Load',NULL,'active',0,1) "
                            "ON CONFLICT(security_level) DO UPDATE SET "
                            "modified_utc_seconds=modified_utc_seconds+1",
                            node);
                    }
                }));
            }
            writes.push_back(coordinator.submit({
                "fixed-soak durable outcome",
                32,
                internal::WriteDurability::durable,
                [node](sqlite3* database) {
                    return update_node(
                        database,
                        "UPDATE sfx_security_levels SET "
                        "created_utc_seconds=created_utc_seconds+1,"
                        "modified_utc_seconds=modified_utc_seconds+1 "
                        "WHERE security_level=?1",
                        node);
                }
            }));
        }
    }
    for (auto& write : writes) {
        require(write.get().succeeded(), "fixed-soak request commits");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    verify_totals(database_file, 23'040, 384, 128, 180, 3);

    const app::StorageHealth health = coordinator.health();
    require(health.committed_operations == operation_count, "fixed-soak operation count");
    require(health.rejected_operations == 0, "fixed-soak has no backpressure loss");
    require(health.queue_high_water_operations <= settings.queue.maximum_operations,
            "fixed-soak queue stays bounded");
    require(health.queue_high_water_bytes <= settings.queue.maximum_bytes,
            "fixed-soak bytes stay bounded");
    require(health.maximum_committed_batch_operations <= settings.queue.dequeue_batch_limit,
            "fixed-soak batch stays bounded");
    require(elapsed < 30s, "fixed-soak completes within bounded acceptance window");
    require(coordinator.shutdown().succeeded(), "fixed-soak shutdown succeeds");

    std::cout << "constrained_fixed_soak operations=" << health.committed_operations
              << " cycles=" << cycles
              << " batches=" << health.committed_batches
              << " maximum_batch=" << health.maximum_committed_batch_operations
              << " queue_high_water=" << health.queue_high_water_operations
              << " elapsed_ms=" << elapsed.count() << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "constrained measurements require an output directory");
        const std::filesystem::path root = std::filesystem::absolute(argv[1]);
        remove_tree(root);
        std::filesystem::create_directories(root);
        slow_writer_service_time(root);
        external_writer_contention(root);
        fixed_128_node_soak(root);
        remove_tree(root);
        std::cout << "all SFX Stage 5A constrained measurements passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "SFX Stage 5A constrained measurement failure: "
                  << exception.what() << '\n';
        return 1;
    }
}
