#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sqlite3.h"

namespace sfx::adapters::storage::internal {
void secure_lineage_sql(sqlite3_context*,int,sqlite3_value**) noexcept;

inline constexpr int sfx_application_id = 0x53465842;
inline constexpr int required_sqlite_version = 3'053'004;
inline constexpr const char* required_sqlite_source_id =
    "2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc";

class StorageFailure final : public std::runtime_error {
public:
    StorageFailure(
        application::StorageErrorCode code,
        std::string operation,
        int sqlite_code = SQLITE_OK,
        std::string detail = {});

    application::StorageErrorCode code() const noexcept;
    const std::string& operation() const noexcept;
    int sqlite_code() const noexcept;
    const std::string& detail() const noexcept;

private:
    application::StorageErrorCode code_;
    std::string operation_;
    int sqlite_code_;
    std::string detail_;
};

application::StorageErrorCode map_sqlite_error(int sqlite_code) noexcept;
application::StorageStatus status_from_exception(const StorageFailure& failure);
application::StorageStatus status_from_current_exception(const std::string& operation) noexcept;

class SqliteConnection final {
public:
    SqliteConnection(
        const std::filesystem::path& path,
        int open_flags,
        std::chrono::milliseconds busy_timeout,
        bool writer_connection);
    SqliteConnection(const std::filesystem::path& path,int open_flags,
        std::chrono::milliseconds busy_timeout,bool writer_connection,const char* private_vfs);
    ~SqliteConnection();

    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;

    sqlite3* get();
    const std::filesystem::path& path() const noexcept;
    void exec(const std::string& sql, const std::string& operation);
    std::int64_t scalar_int64(const std::string& sql, const std::string& operation);
    std::string scalar_text(const std::string& sql, const std::string& operation);
    void close();

private:
    void require_owner_thread() const;
    void configure(std::chrono::milliseconds busy_timeout, bool writer_connection);

    std::filesystem::path path_;
    sqlite3* database_{nullptr};
    std::thread::id owner_thread_;
};

void bootstrap_and_migrate(
    SqliteConnection& connection,
    const std::vector<SqliteMigration>& migrations,
    const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>&
        bootstrap_boundary_hook,
    SqliteStorageLayoutProfile layout_profile = SqliteStorageLayoutProfile::canonical);

application::StorageStatus verify_database_file(
    const std::filesystem::path& database_file,
    std::chrono::milliseconds busy_timeout,
    const std::vector<SqliteMigration>& approved_migrations,
    std::int32_t* schema_version = nullptr);

struct InternalBackupResult {
    application::StorageStatus status;
    std::uintmax_t bytes{0};
    std::int32_t schema_version{0};
};

InternalBackupResult backup_database(
    sqlite3* source,
    const std::filesystem::path& destination_file,
    std::chrono::milliseconds busy_timeout,
    const std::vector<SqliteMigration>& approved_migrations);

enum class WriteDurability : std::uint8_t {
    durable,
    coalesced_in_progress_statistics
};

// apply runs only on the owned writer thread inside a short transaction. It must
// perform database work only and remain safe to replay after a full rollback.
struct WriteRequest {
    std::string operation;
    std::size_t estimated_bytes{1};
    WriteDurability durability{WriteDurability::durable};
    std::function<int(sqlite3*)> apply;
};

// Fixed, SQL-only guards for the dedicated retention singleton; no caller
// function or general native side-effect callback is accepted by this path.
bool validate_ftn_retention_expiry(sqlite3*,const application::FtnExpiryIntent&,std::int64_t);
void finish_ftn_retention_expiry(sqlite3*,const application::FtnExpiryIntent&,application::FtnExpiryOutcome,std::uint32_t,std::int64_t);

class SqlitePagerTelemetry final {
public:
    struct Slot {
        bool available{false};
        std::uint64_t cache_hits{0};
        std::uint64_t cache_misses{0};
        std::uint64_t cache_writes{0};
    };

    void begin(std::size_t connection_slots);
    void connection_opened(std::size_t slot, sqlite3* database) noexcept;
    void sample(std::size_t slot, sqlite3* database) noexcept;
    void connection_closed(std::size_t slot) noexcept;
    SqlitePagerCounterSnapshot snapshot() const noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    std::uint64_t generation_{0};
};

class SqliteWriterCoordinator final {
public:
    SqliteWriterCoordinator();
    ~SqliteWriterCoordinator();

    SqliteWriterCoordinator(const SqliteWriterCoordinator&) = delete;
    SqliteWriterCoordinator& operator=(const SqliteWriterCoordinator&) = delete;

    application::StorageStatus start(
        std::filesystem::path database_path,
        SqliteStorageSettings settings,
        std::vector<SqliteMigration> migrations,
        SqlitePagerTelemetry& telemetry);
    std::future<application::StorageStatus> submit(WriteRequest request);
    std::future<application::StorageStatus> submit_ftn_retention_expiry(std::shared_ptr<application::FtnRetentionRemoval>);
    application::StorageStatus checkpoint(bool truncate);
    InternalBackupResult backup_to(const std::filesystem::path& destination_file);
    application::StorageStatus shutdown();
    application::StorageStatus reset_after_offline_recovery();
    application::StorageHealth health() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sfx::adapters::storage::internal
