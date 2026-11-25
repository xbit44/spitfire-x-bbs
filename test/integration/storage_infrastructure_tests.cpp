#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/storage.hpp"
#include "sfx/test/storage_fakes.hpp"

#include "sqlite_internal.hpp"
#include "sqlite_sfx_baseline.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace app = sfx::application;
namespace storage = sfx::adapters::storage;
namespace internal = sfx::adapters::storage::internal;
using namespace std::chrono_literals;

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

void remove_test_tree(const std::filesystem::path& path) {
    require(path.filename() == "stage5a-storage-tests", "refuse broad test cleanup");
    std::error_code error;
    std::filesystem::remove_all(path, error);
    require(!error, "clean test output");
}

storage::SqliteStorageSettings settings_for(const std::filesystem::path& directory) {
    storage::SqliteStorageSettings settings;
    settings.data_directory = directory;
    settings.queue.maximum_operations = 32;
    settings.queue.maximum_bytes = 64U * 1024U;
    settings.queue.dequeue_batch_limit = 8;
    settings.queue.dequeue_batch_window = 1ms;
    settings.queue.enqueue_wait = 100ms;
    settings.busy.sqlite_timeout = 50ms;
    settings.busy.transaction_deadline = 300ms;
    settings.busy.maximum_attempts = 3;
    settings.checkpoint.passive_interval = 1s;
    settings.checkpoint.frame_target = 16;
    settings.utc_now = [] { return std::int64_t{1'787'600'000}; };
    return settings;
}

int update_modeled_node(sqlite3* const database, const char* const sql, const int node_id) {
    sqlite3_stmt* statement = nullptr;
    int code = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
    if (code == SQLITE_OK) {
        code = sqlite3_bind_int(statement, 1, node_id);
    }
    if (code == SQLITE_OK) {
        code = sqlite3_step(statement);
        if (code == SQLITE_DONE) {
            code = sqlite3_changes(database) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT;
        }
    }
    const int finalize_code = sqlite3_finalize(statement);
    return code == SQLITE_OK ? finalize_code : code;
}

void test_dependency_pin_and_compile_options(const std::filesystem::path& root) {
    require(sqlite3_libversion_number() == internal::required_sqlite_version, "exact SQLite version");
    require(std::string(sqlite3_sourceid()) == internal::required_sqlite_source_id, "exact SQLite source ID");
    require(sqlite3_threadsafe() == 1, "serialized-capable SQLite build");
    require(sqlite3_compileoption_used("DQS=0") == 1, "DQS disabled");
    require(sqlite3_compileoption_used("DEFAULT_FOREIGN_KEYS") == 0,
            "foreign-key policy remains connection-owned");
    require(sqlite3_compileoption_used("OMIT_LOAD_EXTENSION") == 1, "extension loading omitted");
    require(sqlite3_compileoption_used("OMIT_SHARED_CACHE") == 1, "shared cache omitted");
    require(sqlite3_compileoption_used("ENABLE_API_ARMOR") == 1, "API armor enabled");
    require(sqlite3_compileoption_used("SECURE_DELETE") == 1, "secure delete default enabled");

    sqlite3* probe = nullptr;
    require(sqlite3_open(":memory:", &probe) == SQLITE_OK, "open SQLite compile probe");
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(probe, "PRAGMA trusted_schema", -1, &statement, nullptr) == SQLITE_OK,
            "prepare trusted-schema probe");
    require(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1,
            "upstream trusted-schema default retained for test compatibility");
    require(sqlite3_finalize(statement) == SQLITE_OK, "finalize trusted-schema probe");
    statement = nullptr;
    require(
        sqlite3_prepare_v2(
            probe,
            "SELECT \"sfx_legacy_double_quoted_literal\"",
            -1,
            &statement,
            nullptr) == SQLITE_ERROR,
        "DQS=0 rejects a double-quoted string-literal fallback");
    require(statement == nullptr, "failed DQS statement does not escape a prepared handle");
    require(sqlite3_close(probe) == SQLITE_OK, "close SQLite compile probe");

    const std::filesystem::path hardening_directory = root / "connection-hardening";
    std::filesystem::create_directories(hardening_directory);
    internal::SqliteConnection hardened(
        hardening_directory / "probe.db",
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        50ms,
        false);
    require(hardened.scalar_int64("PRAGMA foreign_keys", "verify hardened foreign keys") == 1,
            "owned connections enforce foreign keys");
    require(hardened.scalar_int64("PRAGMA trusted_schema", "verify hardened trusted schema") == 0,
            "owned connections disable trusted schema");
    require(hardened.scalar_int64("PRAGMA mmap_size", "verify hardened memory mapping") == 0,
            "owned connections disable memory mapping");

    internal::SqliteConnection writer_hardening(
        hardening_directory / "writer-probe.db",
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        50ms,
        true);
    require(writer_hardening.scalar_int64("PRAGMA page_size", "verify writer page size") == 4096,
            "writer enforces 4096-byte pages");
    require(writer_hardening.scalar_text("PRAGMA journal_mode", "verify writer WAL") == "wal",
            "writer enforces WAL");
    require(writer_hardening.scalar_int64("PRAGMA synchronous", "verify writer sync") == 2,
            "writer enforces FULL synchronization");
    require(writer_hardening.scalar_int64(
                "PRAGMA wal_autocheckpoint",
                "verify writer autocheckpoint") == 0,
            "writer disables SQLite autocheckpoint");
    require(writer_hardening.scalar_text(
                "PRAGMA locking_mode",
                "verify writer locking") == "normal",
            "writer retains NORMAL locking");
    require(writer_hardening.scalar_int64(
                "PRAGMA journal_size_limit",
                "verify writer journal limit") == 67'108'864,
            "writer enforces the measured journal-size limit");
}

void test_deterministic_fakes(const std::filesystem::path& root) {
    sfx::test::FakeStorageRuntime runtime;
    require(runtime.start().succeeded(), "fake storage starts");
    require(runtime.health().state == app::StorageRuntimeState::ready, "fake publishes ready");
    require(runtime.checkpoint().succeeded(), "fake checkpoint");
    require(runtime.shutdown().succeeded(), "fake shutdown");
    require(runtime.start_calls == 1 && runtime.checkpoint_calls == 1 && runtime.shutdown_calls == 1,
            "fake call accounting");

    sfx::test::FakeStoragePathPolicy policy;
    policy.accept = false;
    storage::SqliteStorage rejected(settings_for(root), policy);
    require(rejected.start().code == app::StorageErrorCode::invalid_location, "path policy rejection propagates");
}

void test_lifecycle_migration_backup_restore(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "lifecycle";
    const std::filesystem::path backups = root / "backups";
    std::filesystem::create_directories(data);
    std::filesystem::create_directories(backups);

    sfx::test::FakeStoragePathPolicy policy;
    storage::SqliteStorage database(settings_for(data), policy);
    require(database.start().succeeded(), "storage lifecycle starts");
    const app::StorageHealth ready = database.health();
    require(ready.state == app::StorageRuntimeState::ready, "storage reports ready");
    require(ready.schema_version == 27, "Section14 current shipping migration version published");
    require(ready.engine_version == "3.53.4", "health reports pinned engine");
    require(database.database_path().filename() == "SFX.DB", "single physical database name");
    require(database.checkpoint().succeeded(), "manual passive checkpoint");

    const std::filesystem::path backup_file = std::filesystem::absolute(backups / "qualification.db");
    const storage::SqliteBackupResult backup = database.create_backup(backup_file);
    require(backup.status && backup.database_bytes > 0 && backup.schema_version == 27, "live backup succeeds");
    require(database.validate_backup(backup_file).succeeded(), "backup validates while storage is active");
    require(database.restore_backup(backup_file).code == app::StorageErrorCode::shutting_down,
            "active restore rejected");
    require(database.shutdown().succeeded(), "storage lifecycle orderly shutdown");

    require(database.restore_backup(backup_file).succeeded(), "offline restore succeeds");
    require(std::filesystem::exists(data / "SFX.DB.pre-restore"), "restore preserves previous database");
    require(database.start().succeeded(), "restored database starts");
    require(database.health().schema_version == 27, "restored shipping schema version retained");
    require(database.shutdown().succeeded(), "restored database shuts down");

    const storage::SqliteBackupResult duplicate = [&] {
        storage::SqliteStorage duplicate_source(settings_for(data), policy);
        require(duplicate_source.start().succeeded(), "duplicate-backup source starts");
        const storage::SqliteBackupResult result = duplicate_source.create_backup(backup_file);
        require(duplicate_source.shutdown().succeeded(), "duplicate-backup source stops");
        return result;
    }();
    require(duplicate.status.code == app::StorageErrorCode::io, "backup never overwrites an existing file");

    const std::filesystem::path corrupt = std::filesystem::absolute(backups / "corrupt.db");
    {
        std::ofstream output(corrupt, std::ios::binary);
        output << "not a SQLite database";
    }
    require(!database.validate_backup(corrupt), "corrupt backup rejected");
}

void test_native_layout_profiles(const std::filesystem::path& root) {
    const auto backups = root / "layout-backups";
    std::filesystem::create_directories(backups);
    for (const auto profile : {storage::SqliteStorageLayoutProfile::canonical,
                              storage::SqliteStorageLayoutProfile::linux_lowercase_v1}) {
        const bool lower = profile == storage::SqliteStorageLayoutProfile::linux_lowercase_v1;
        const auto data = root / (lower ? "lowercase" : "canonical");
        std::filesystem::create_directories(data);
        auto settings = settings_for(data);
        settings.layout_profile = profile;
        sfx::test::FakeStoragePathPolicy policy;
        storage::SqliteStorage database(settings, policy);
        const std::string leaf = lower ? "sfx.db" : "SFX.DB";
        require(database.database_path().filename().u8string() == leaf,
                "selected physical database leaf is available before startup");
        require(database.start().succeeded(), "selected native layout starts");
        const auto verify_paths = [&] {
            internal::SqliteConnection probe(database.database_path(), SQLITE_OPEN_READONLY, 50ms, false);
            for (int area = 1; area <= 3; ++area) {
                const auto sql = "SELECT storage_path FROM sfx_file_area_paths WHERE area_id=" + std::to_string(area);
                const std::array<const char*, 3> canonical{{"Files/General", "Files/Uploads", "Files/UploadToSysop"}};
                const std::array<const char*, 3> native{{"files/general", "files/uploads", "files/uploadtosysop"}};
                require(probe.scalar_text(sql.c_str(), "verify native file-area seed") ==
                            (lower ? native[area - 1] : canonical[area - 1]),
                        "fresh native file-area defaults match their physical layout");
            }
            require(probe.scalar_text("SELECT content_sha256 FROM sfx_schema_migrations WHERE version=15",
                        "verify accepted file migration identity") == internal::sfx_file_migration().content_sha256,
                    "native seed selection preserves the frozen migration hash");
        };
        verify_paths();
        const auto backup = backups / (lower ? "lowercase.sqlite" : "canonical.sqlite");
        require(database.create_backup(backup).status.succeeded(), "selected layout backup succeeds");
        require(database.validate_backup(backup).succeeded(), "selected layout backup verifies");
        require(database.shutdown().succeeded(), "selected layout stops normally");
        {
            internal::SqliteConnection edit(database.database_path(), SQLITE_OPEN_READWRITE, 50ms, false);
            edit.exec("UPDATE sfx_file_area_paths SET storage_path='owner/KeepMySpelling' WHERE area_id=1",
                      "save an existing owner path for reopen qualification");
        }
        require(database.start().succeeded(), "selected layout reopens existing data");
        {
            internal::SqliteConnection probe(database.database_path(), SQLITE_OPEN_READONLY, 50ms, false);
            require(probe.scalar_text("SELECT storage_path FROM sfx_file_area_paths WHERE area_id=1",
                        "verify existing owner path") == "owner/KeepMySpelling",
                    "reopen preserves owner-selected path spelling");
        }
        require(database.shutdown().succeeded(), "selected layout stops before restore");
        require(database.restore_backup(backup).succeeded(), "selected layout offline restore succeeds");
        require(database.start().succeeded(), "selected layout restarts restored data");
        verify_paths();
        require(database.database_path().filename().u8string() == leaf,
                "backup restore retains selected physical database filename");
        require(database.shutdown().succeeded(), "restored selected layout stops");
    }
    for (const auto* legacy : {"SFX.DB", "SFX.DB-wal", "SFX.DB.restore-staging"}) {
        const auto data = root / (std::string("legacy-") + legacy);
        std::filesystem::create_directories(data);
        { std::ofstream sentinel(data / legacy, std::ios::binary); sentinel << "preserve legacy state"; }
        auto settings = settings_for(data);
        settings.layout_profile = storage::SqliteStorageLayoutProfile::linux_lowercase_v1;
        sfx::test::FakeStoragePathPolicy policy;
        storage::SqliteStorage database(settings, policy);
        const auto result = database.start();
        require(result.code == app::StorageErrorCode::incompatible_schema &&
                    result.operation == "linux_lowercase_layout_legacy_database_refused",
                "legacy database or sidecar is refused before parallel lower DB creation");
        require(std::distance(std::filesystem::directory_iterator(data), std::filesystem::directory_iterator{}) == 1,
                "legacy refusal leaves only its original entry");
        std::ifstream input(data / legacy, std::ios::binary);
        require(std::string(std::istreambuf_iterator<char>(input), {}) == "preserve legacy state",
                "legacy refusal preserves all original bytes");
    }
    const auto invalid = root / "invalid-profile";
    std::filesystem::create_directories(invalid);
    auto settings = settings_for(invalid);
    settings.layout_profile = static_cast<storage::SqliteStorageLayoutProfile>(999);
    sfx::test::FakeStoragePathPolicy policy;
    storage::SqliteStorage database(settings, policy);
    require(database.start().code == app::StorageErrorCode::invalid_argument,
            "unknown layout selection fails before any database creation");
    require(std::filesystem::is_empty(invalid), "unknown layout leaves destination unchanged");
}

void test_restore_after_startup_failure(const std::filesystem::path& root) {
    const std::filesystem::path source_directory = root / "recovery-source";
    const std::filesystem::path target_directory = root / "recovery-target";
    const std::filesystem::path backup_directory = root / "recovery-backup";
    std::filesystem::create_directories(source_directory);
    std::filesystem::create_directories(target_directory);
    std::filesystem::create_directories(backup_directory);

    sfx::test::FakeStoragePathPolicy policy;
    storage::SqliteStorage source(settings_for(source_directory), policy);
    require(source.start().succeeded(), "recovery source starts");
    const std::filesystem::path backup_file = backup_directory / "recovery.db";
    require(source.create_backup(backup_file).status.succeeded(),
            "recovery source backup succeeds");
    require(source.shutdown().succeeded(), "recovery source stops");

    sqlite3* poisoned = nullptr;
    const std::string target_file = (target_directory / "SFX.DB").string();
    require(sqlite3_open_v2(
                target_file.c_str(),
                &poisoned,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                nullptr) == SQLITE_OK,
            "create incompatible recovery target");
    require(sqlite3_exec(
                poisoned,
                "PRAGMA application_id=42;CREATE TABLE unrelated(value INTEGER);",
                nullptr,
                nullptr,
                nullptr) == SQLITE_OK,
            "poison recovery target application ID");
    require(sqlite3_close(poisoned) == SQLITE_OK, "close incompatible recovery target");

    storage::SqliteStorage target(settings_for(target_directory), policy);
    require(target.start().code == app::StorageErrorCode::incompatible_schema,
            "incompatible startup enters recovery path");
    require(target.health().state == app::StorageRuntimeState::recovery_required,
            "startup failure reports recovery required");
    require(target.restore_backup(backup_file).succeeded(),
            "offline restore succeeds after startup failure");
    require(target.health().state == app::StorageRuntimeState::stopped,
            "validated restore clears sticky recovery state");
    require(target.start().succeeded(), "restored storage restarts on same object");
    require(target.health().schema_version == 27, "restored shipping schema version is active");
    require(target.shutdown().succeeded(), "restored storage stops cleanly");
}

void test_baseline_publication_failure_rolls_back(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "failed-baseline-publication";
    std::filesystem::create_directories(data);
    sfx::test::FakeStoragePathPolicy policy;
    auto settings = settings_for(data);
    settings.bootstrap_boundary_hook = [](const std::string_view boundary) {
        return boundary == "before-baseline-commit"
            ? app::StorageStatus{app::StorageErrorCode::internal,
                  "inject pre-commit baseline failure"}
            : app::StorageStatus::success();
    };
    storage::SqliteStorage database(settings, policy);
    require(!database.start().succeeded(),
        "pre-commit baseline failure is reported");
    (void)database.shutdown();

    internal::SqliteConnection verification(
        data / "SFX.DB",
        SQLITE_OPEN_READONLY,
        50ms,
        false);
    require(verification.scalar_int64(
                "PRAGMA application_id", "verify failed baseline application ID") == 0 &&
            verification.scalar_int64(
                "PRAGMA user_version", "verify failed baseline version") == 0,
        "failed baseline publication leaves identity unpublished");
    require(verification.scalar_int64(
                "SELECT count(*) FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*'",
                "verify failed baseline objects") == 0,
        "failed baseline publication rolls back every schema object");
}

void test_missing_baseline_ledger_row_fails_closed(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "missing-baseline-ledger";
    std::filesystem::create_directories(data);
    sfx::test::FakeStoragePathPolicy policy;

    storage::SqliteStorage seed(settings_for(data), policy);
    require(seed.start().succeeded(), "baseline-ledger seed starts");
    require(seed.shutdown().succeeded(), "baseline-ledger seed stops");

    internal::SqliteConnection corrupter(
        data / "SFX.DB",
        SQLITE_OPEN_READWRITE,
        50ms,
        false);
    corrupter.exec(
        "DELETE FROM sfx_schema_migrations WHERE version=1",
        "remove exact SFX baseline ledger row");
    corrupter.close();

    storage::SqliteStorage reopened(settings_for(data), policy);
    require(reopened.start().code == app::StorageErrorCode::incompatible_schema,
        "missing clean-baseline ledger row fails closed");
    require(reopened.shutdown().code == app::StorageErrorCode::incompatible_schema,
        "missing-ledger startup failure remains visible");
}

void test_nonbaseline_plan_fails_closed(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "nonbaseline-plan";
    std::filesystem::create_directories(data);
    sfx::test::FakeStoragePathPolicy policy;
    auto altered = storage::sfx_storage_migrations();
    altered.front().name = "unexpected-sfx-baseline";
    storage::SqliteStorage database(settings_for(data), policy, altered);
    require(database.start().code == app::StorageErrorCode::migration_failed,
        "an altered production migration plan fails closed");
    (void)database.shutdown();

    internal::SqliteConnection verification(
        data / "SFX.DB",
        SQLITE_OPEN_READONLY,
        50ms,
        false);
    require(verification.scalar_int64(
                "PRAGMA application_id", "verify rejected plan application ID") == 0 &&
            verification.scalar_int64(
                "PRAGMA user_version", "verify rejected plan version") == 0,
        "an altered migration plan cannot publish database identity");
    require(verification.scalar_int64(
                "SELECT count(*) FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*'",
                "verify rejected plan objects") == 0,
        "an altered migration plan cannot publish schema objects");
}

void test_backup_plan_and_full_validation(const std::filesystem::path& root) {
    const std::filesystem::path source_data = root / "plan-source";
    const std::filesystem::path target_data = root / "plan-target";
    const std::filesystem::path backups = root / "plan-backups";
    std::filesystem::create_directories(source_data);
    std::filesystem::create_directories(target_data);
    std::filesystem::create_directories(backups);
    sfx::test::FakeStoragePathPolicy policy;

    storage::SqliteStorage source(settings_for(source_data), policy);
    require(source.start().succeeded(), "backup-validation source starts");
    require(source.shutdown().succeeded(), "backup-validation source pauses for seed data");
    {
        internal::SqliteConnection seed(
            source_data / "SFX.DB", SQLITE_OPEN_READWRITE, 50ms, false);
        seed.exec(
            "INSERT INTO sfx_accounts("
            "display_name,name_key,security_level,account_state,revision,purge_allowed,"
            "created_utc_seconds,modified_utc_seconds) "
            "VALUES('Backup Probe','backup probe',10,'active',1,0,0,0)",
            "seed backup validation account");
    }
    require(source.start().succeeded(), "backup-validation source restarts");
    const std::filesystem::path backup_file = backups / "approved.db";
    require(source.create_backup(backup_file).status.succeeded(),
        "backup-validation source backup succeeds");
    require(source.shutdown().succeeded(), "backup-validation source stops");
    require(source.validate_backup(backup_file).succeeded(),
        "complete exact SFX ledger and schema validate");

    storage::SqliteStorage target(settings_for(target_data), policy);
    require(target.start().succeeded() && target.shutdown().succeeded(),
        "clean SFX restore target is established");

    std::error_code copy_error;
    const std::filesystem::path altered_ledger = backups / "altered-ledger.db";
    std::filesystem::copy_file(backup_file, altered_ledger, copy_error);
    require(!copy_error, "copy altered-ledger backup probe");
    {
        internal::SqliteConnection corrupter(
            altered_ledger, SQLITE_OPEN_READWRITE, 50ms, false);
        corrupter.exec(
            "UPDATE sfx_schema_migrations SET name='altered-baseline' WHERE version=1",
            "alter backup migration identity");
    }
    require(source.validate_backup(altered_ledger).code ==
            app::StorageErrorCode::incompatible_schema,
        "backup validation rejects altered migration identity");
    require(target.restore_backup(altered_ledger).code ==
            app::StorageErrorCode::incompatible_schema,
        "altered migration identity is rejected before restore activation");
    require(target.start().succeeded() && target.shutdown().succeeded(),
        "rejected restore leaves the exact active SFX database unchanged");

    const std::filesystem::path missing_ledger = backups / "missing-ledger.db";
    copy_error.clear();
    std::filesystem::copy_file(backup_file, missing_ledger, copy_error);
    require(!copy_error, "copy missing-ledger backup probe");
    {
        internal::SqliteConnection corrupter(
            missing_ledger, SQLITE_OPEN_READWRITE, 50ms, false);
        corrupter.exec(
            "DELETE FROM sfx_schema_migrations WHERE version=1",
            "remove backup migration row");
    }
    require(source.validate_backup(missing_ledger).code ==
            app::StorageErrorCode::incompatible_schema,
        "backup validation rejects the missing baseline ledger row");

    const std::filesystem::path index_corrupt = backups / "index-corrupt.db";
    copy_error.clear();
    std::filesystem::copy_file(backup_file, index_corrupt, copy_error);
    require(!copy_error, "copy full-integrity backup probe");
    {
        sqlite3* corrupter = nullptr;
        require(sqlite3_open_v2(
                    index_corrupt.string().c_str(), &corrupter,
                    SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "open index-consistency corruption probe");
        require(sqlite3_exec(
                    corrupter,
                    "PRAGMA writable_schema=ON;"
                    "UPDATE sqlite_schema SET "
                    "sql='CREATE UNIQUE INDEX sfx_accounts_name_key_unique "
                    "ON sfx_accounts(account_id) WHERE name_key IS NOT NULL' "
                    "WHERE type='index' AND name='sfx_accounts_name_key_unique';"
                    "PRAGMA writable_schema=OFF;",
                    nullptr, nullptr, nullptr) == SQLITE_OK,
            "create index/schema consistency corruption");
        require(sqlite3_close(corrupter) == SQLITE_OK,
            "close index-consistency corruption probe");

        internal::SqliteConnection verifier(
            index_corrupt, SQLITE_OPEN_READONLY, 50ms, false);
        require(verifier.scalar_text(
                    "PRAGMA quick_check", "run preliminary quick check") == "ok",
            "quick_check does not substitute for exact schema/full validation");
    }
    const auto index_corrupt_status = source.validate_backup(index_corrupt);
    require(index_corrupt_status.code == app::StorageErrorCode::corrupt ||
            index_corrupt_status.code == app::StorageErrorCode::incompatible_schema,
        "final backup acceptance rejects index/schema inconsistency");

    const std::filesystem::path foreign_key_corrupt = backups / "foreign-key-corrupt.db";
    copy_error.clear();
    std::filesystem::copy_file(backup_file, foreign_key_corrupt, copy_error);
    require(!copy_error, "copy foreign-key backup probe");
    {
        internal::SqliteConnection corrupter(
            foreign_key_corrupt, SQLITE_OPEN_READWRITE, 50ms, false);
        corrupter.exec(
            "PRAGMA foreign_keys=OFF;"
            "UPDATE sfx_accounts SET security_level=998 WHERE name_key='backup probe'",
            "create foreign-key corruption");
        require(corrupter.scalar_text(
                    "PRAGMA integrity_check", "verify structural integrity") == "ok",
            "integrity_check does not replace foreign_key_check");
    }
    require(source.validate_backup(foreign_key_corrupt).code == app::StorageErrorCode::corrupt,
        "final backup acceptance uses complete foreign_key_check");
}

void test_independent_group_commit_outcomes(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "independent-group-commit";
    std::filesystem::create_directories(data);
    storage::SqliteStorageSettings settings = settings_for(data);
    settings.queue.dequeue_batch_limit = 8;
    settings.queue.dequeue_batch_window = 100ms;

    internal::SqlitePagerTelemetry telemetry;
    internal::SqliteWriterCoordinator coordinator;
    const std::filesystem::path database_file = data / "SFX.DB";
    require(coordinator.start(
                database_file, settings, storage::sfx_storage_migrations(), telemetry).succeeded(),
            "independent group-commit coordinator starts");

    auto first = coordinator.submit({
        "first independent write",
        1,
        internal::WriteDurability::durable,
        [](sqlite3* database) {
            return sqlite3_exec(
                database,
                "UPDATE sfx_account_retention_policy SET revision=revision+1 "
                "WHERE singleton_id=1",
                nullptr,
                nullptr,
                nullptr);
        }});
    auto failing = coordinator.submit({
        "independent constraint failure",
        1,
        internal::WriteDurability::durable,
        [](sqlite3* database) {
            return sqlite3_exec(
                database,
                "INSERT INTO sfx_account_retention_policy("
                "singleton_id,revision,audit_online_months,audit_total_months,"
                "session_online_months,session_total_months,modified_utc_seconds) "
                "VALUES(1,99,24,84,12,24,0)",
                nullptr,
                nullptr,
                nullptr);
        }});
    auto third = coordinator.submit({
        "third independent write",
        1,
        internal::WriteDurability::durable,
        [](sqlite3* database) {
            return sqlite3_exec(
                database,
                "UPDATE sfx_account_retention_policy SET revision=revision+1 "
                "WHERE singleton_id=1",
                nullptr,
                nullptr,
                nullptr);
        }});

    require(first.get().succeeded(), "first independent request commits");
    require(failing.get().code == app::StorageErrorCode::constraint,
            "only failing independent request reports its constraint");
    require(third.get().succeeded(), "later independent request commits");
    {
        internal::SqliteConnection verification(
            database_file,
            SQLITE_OPEN_READONLY,
            50ms,
            false);
        require(verification.scalar_int64(
                    "SELECT revision FROM sfx_account_retention_policy WHERE singleton_id=1",
                    "verify independent request outcomes") == 3,
                "successful requests survive a sibling constraint failure");
    }
    const app::StorageHealth health = coordinator.health();
    require(health.committed_operations == 2 && health.committed_batches == 1 &&
                health.maximum_committed_batch_operations == 2,
            "independent outcomes retain one bounded group commit");
    require(coordinator.shutdown().succeeded(), "independent group-commit coordinator stops");
}

void test_incomplete_passive_checkpoint_reporting(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "checkpoint-starvation";
    std::filesystem::create_directories(data);
    storage::SqliteStorageSettings settings = settings_for(data);
    settings.queue.dequeue_batch_limit = 16;
    settings.queue.dequeue_batch_window = 50ms;
    settings.checkpoint.passive_interval = std::chrono::hours(1);
    settings.checkpoint.frame_target = 100'000;
    settings.checkpoint.wal_warning_bytes = 512U * 1024U * 1024U;

    internal::SqlitePagerTelemetry telemetry;
    internal::SqliteWriterCoordinator coordinator;
    const std::filesystem::path database_file = data / "SFX.DB";
    require(coordinator.start(
                database_file, settings, storage::sfx_storage_migrations(), telemetry).succeeded(),
            "checkpoint-starvation coordinator starts");
    require(coordinator.checkpoint(false).succeeded(),
            "initial passive checkpoint establishes reader end mark");

    internal::SqliteConnection reader(database_file, SQLITE_OPEN_READONLY, 50ms, false);
    reader.exec("BEGIN", "begin long-held read transaction");
    sqlite3_stmt* held_statement = nullptr;
    require(sqlite3_prepare_v2(
                reader.get(),
                "SELECT revision FROM sfx_account_retention_policy WHERE singleton_id=1",
                -1,
                &held_statement,
                nullptr) == SQLITE_OK,
            "prepare long-held reader");
    require(sqlite3_step(held_statement) == SQLITE_ROW, "pin long-held reader snapshot");

    std::vector<std::future<app::StorageStatus>> writes;
    for (int index = 0; index < 8; ++index) {
        writes.push_back(coordinator.submit({
            "write behind long-held reader",
            1,
            internal::WriteDurability::durable,
            [](sqlite3* database) {
                return sqlite3_exec(
                    database,
                    "UPDATE sfx_account_retention_policy SET revision=revision+1 "
                    "WHERE singleton_id=1",
                    nullptr,
                    nullptr,
                    nullptr);
            }}));
    }
    for (auto& write : writes) {
        require(write.get().succeeded(), "writes behind long-held reader commit");
    }

    const app::StorageStatus incomplete = coordinator.checkpoint(false);
    require(incomplete.code == app::StorageErrorCode::busy,
            "incomplete PASSIVE checkpoint is reported, not accepted");
    const app::StorageHealth starved = coordinator.health();
    require(starved.state == app::StorageRuntimeState::degraded &&
                starved.incomplete_checkpoints >= 1 &&
                starved.last_checkpoint_log_frames > starved.last_checkpointed_frames,
            "checkpoint starvation publishes returned WAL frame counts");

    require(sqlite3_finalize(held_statement) == SQLITE_OK, "finalize long-held reader");
    reader.exec("COMMIT", "release long-held read transaction");
    reader.close();

    require(coordinator.checkpoint(false).succeeded(),
            "PASSIVE checkpoint completes after reader release");
    const app::StorageHealth recovered = coordinator.health();
    require(recovered.state == app::StorageRuntimeState::ready &&
                recovered.last_checkpoint_log_frames == recovered.last_checkpointed_frames,
            "checkpoint health recovers after reader starvation clears");
    require(coordinator.shutdown().succeeded(), "checkpoint-starvation coordinator stops");
}
void test_writer_backpressure_atomicity_and_busy(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "writer";
    std::filesystem::create_directories(data);
    storage::SqliteStorageSettings settings = settings_for(data);
    settings.queue.maximum_operations = 2;
    settings.queue.maximum_bytes = 32;
    settings.queue.dequeue_batch_limit = 1;
    settings.queue.dequeue_batch_window = 0ms;
    settings.queue.enqueue_wait = 30ms;
    settings.busy.sqlite_timeout = 40ms;
    settings.busy.transaction_deadline = 200ms;
    settings.busy.maximum_attempts = 3;

    internal::SqlitePagerTelemetry telemetry;
    internal::SqliteWriterCoordinator coordinator;
    const std::filesystem::path database_file = data / "SFX.DB";
    require(coordinator.start(
                database_file, settings, storage::sfx_storage_migrations(), telemetry).succeeded(),
        "writer coordinator starts");
    auto initialize = coordinator.submit({
        "initialize qualification writes",
        1,
        internal::WriteDurability::durable,
        [](sqlite3* database) {
            return sqlite3_exec(
                database,
                "CREATE TABLE stage5a_write_probe(value INTEGER PRIMARY KEY) STRICT;"
                "INSERT INTO stage5a_write_probe(value) VALUES(1)",
                nullptr,
                nullptr,
                nullptr);
        }});
    require(initialize.get().succeeded(), "writer initialization commits");

    std::promise<void> blocker_started;
    std::future<void> blocker_signal = blocker_started.get_future();
    auto blocker = coordinator.submit({
        "bounded queue blocker",
        1,
        internal::WriteDurability::durable,
        [&blocker_started](sqlite3*) {
            blocker_started.set_value();
            sqlite3_sleep(150);
            return SQLITE_OK;
        }});
    blocker_signal.get();
    auto queued_one = coordinator.submit({"queued one", 1, internal::WriteDurability::durable,
        [](sqlite3*) { return SQLITE_OK; }});
    auto queued_two = coordinator.submit({"queued two", 1, internal::WriteDurability::durable,
        [](sqlite3*) { return SQLITE_OK; }});
    auto rejected = coordinator.submit({"queue overflow", 1, internal::WriteDurability::durable,
        [](sqlite3*) { return SQLITE_OK; }});
    require(rejected.get().code == app::StorageErrorCode::backpressure, "bounded queue applies backpressure");
    require(blocker.get().succeeded() && queued_one.get().succeeded() && queued_two.get().succeeded(), "accepted queued writes commit");

    auto constraint = coordinator.submit({
        "atomic constraint failure",
        1,
        internal::WriteDurability::durable,
        [](sqlite3* database) {
            return sqlite3_exec(
                database,
                "INSERT INTO stage5a_write_probe(value) VALUES(2);"
                "INSERT INTO stage5a_write_probe(value) VALUES(1)",
                nullptr,
                nullptr,
                nullptr);
        }});
    require(constraint.get().code == app::StorageErrorCode::constraint, "constraint error mapped");

    {
        internal::SqliteConnection reader(database_file, SQLITE_OPEN_READONLY, 50ms, false);
        require(reader.scalar_int64("SELECT count(*) FROM stage5a_write_probe", "verify atomic rollback") == 1,
                "failed operation rolls back as one transaction");
    }

    internal::SqliteConnection holder(database_file, SQLITE_OPEN_READWRITE, 50ms, false);
    holder.exec("BEGIN IMMEDIATE", "hold writer lock");
    auto busy = coordinator.submit({"bounded busy operation", 1, internal::WriteDurability::durable,
        [](sqlite3* database) {
            return sqlite3_exec(
                database,
                "INSERT INTO stage5a_write_probe(value) VALUES(3)",
                nullptr,
                nullptr,
                nullptr);
        }});
    require(busy.get().code == app::StorageErrorCode::busy, "busy deadline is bounded and mapped");
    holder.exec("ROLLBACK", "release writer lock");

    require(coordinator.checkpoint(false).succeeded(), "coordinator checkpoint succeeds");
    const app::StorageHealth health = coordinator.health();
    require(health.queue_high_water_operations == 2, "queue high-water measured");
    require(health.rejected_operations >= 1 && health.busy_retries > 0 &&
                health.busy_retries < settings.busy.maximum_attempts,
            "backpressure and bounded retries measured");
    require(health.committed_operations == 4, "only committed operations counted");
    require(coordinator.shutdown().succeeded(), "writer drains and truncates on shutdown");
}

void test_injected_writer_failures(const std::filesystem::path& root) {
    for (const int code : {SQLITE_IOERR, SQLITE_FULL, SQLITE_CORRUPT, SQLITE_INTERNAL}) {
        const auto data = root / ("injected-writer-" + std::to_string(code));
        std::filesystem::create_directories(data);
        auto settings = settings_for(data);
        internal::SqlitePagerTelemetry telemetry;
        internal::SqliteWriterCoordinator coordinator;
        require(coordinator.start(data / "SFX.DB", settings, storage::sfx_storage_migrations(), telemetry).succeeded(),
            "injected writer fixture starts actual writer thread");
        auto failed = coordinator.submit({"bounded injected write", 1, internal::WriteDurability::durable,
            [code](sqlite3*) -> int {
                if (code == SQLITE_INTERNAL) throw std::runtime_error("Stagefive private writer exception");
                return code;
            }});
        require(failed.wait_for(5s) == std::future_status::ready, "failed writer promise resolves within deadline");
        const auto result = failed.get();
        const auto expected = code == SQLITE_IOERR ? app::StorageErrorCode::io :
            code == SQLITE_FULL ? app::StorageErrorCode::full : code == SQLITE_CORRUPT ?
            app::StorageErrorCode::corrupt : app::StorageErrorCode::internal;
        require(result.code == expected && result.operation.find("private writer exception") == std::string::npos,
            "writer failure preserves typed error without exception input");
        require(coordinator.health().committed_operations == 0, "failed request is never counted committed");
        if (code == SQLITE_CORRUPT) {
            require(coordinator.health().state == app::StorageRuntimeState::recovery_required,
                "fatal corruption closes writer authority");
            auto later = coordinator.submit({"after fatal writer", 1, internal::WriteDurability::durable,
                [](sqlite3*) { return SQLITE_OK; }});
            require(later.wait_for(5s) == std::future_status::ready && !later.get().succeeded(),
                "post-fatal request resolves unsuccessfully, never hangs");
            require(!coordinator.shutdown().succeeded(), "fatal writer joins without false cleanup success");
        } else {
            // The injected request failed, but the disposable filesystem is
            // healthy again: do not confuse this with permanent storage recovery.
            require(coordinator.shutdown().succeeded(), "nonfatal injected writer error still joins");
        }
        internal::SqliteConnection reader(data / "SFX.DB", SQLITE_OPEN_READONLY, 50ms, false);
        require(reader.scalar_text("PRAGMA integrity_check", "check failed-write fixture") == "ok",
            "injected operation failure leaves actual database intact");
    }
    std::cout << "PASS four real writer injections: IOERR, FULL, CORRUPT, throwing request; typed promises, zero false commits, fatal admission closed\n";
}

void test_transaction_batching_and_128_node_workload(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "modeled-128-nodes";
    std::filesystem::create_directories(data);
    storage::SqliteStorageSettings settings = settings_for(data);
    settings.queue.maximum_operations = 4096;
    settings.queue.maximum_bytes = 8U * 1024U * 1024U;
    settings.queue.dequeue_batch_limit = 64;
    settings.queue.dequeue_batch_window = 50ms;
    settings.queue.enqueue_wait = 1000ms;

    internal::SqlitePagerTelemetry telemetry;
    internal::SqliteWriterCoordinator coordinator;
    const std::filesystem::path database_file = data / "SFX.DB";
    require(coordinator.start(
                database_file, settings, storage::sfx_storage_migrations(), telemetry).succeeded(),
            "modeled workload coordinator starts");

    const auto started = std::chrono::steady_clock::now();
    std::vector<std::future<app::StorageStatus>> writes;
    writes.reserve(128U * 61U);
    for (int node = 1; node <= 128; ++node) {
        for (int minute = 0; minute < 60; ++minute) {
            (void)minute;
            writes.push_back(coordinator.submit({
                "modeled coalesced minute statistic",
                1,
                internal::WriteDurability::coalesced_in_progress_statistics,
                [node](sqlite3* database) {
                    return update_modeled_node(
                        database,
                        "INSERT INTO sfx_security_levels("
                        "security_level,label,description,level_state,"
                        "created_utc_seconds,modified_utc_seconds) "
                        "VALUES(?1,'Load',NULL,'active',0,1) "
                        "ON CONFLICT(security_level) DO UPDATE SET "
                        "modified_utc_seconds=modified_utc_seconds+1",
                        node);
                }}));
        }
        writes.push_back(coordinator.submit({
            "modeled durable session outcome",
            1,
            internal::WriteDurability::durable,
            [node](sqlite3* database) {
                return update_modeled_node(
                    database,
                    "UPDATE sfx_security_levels SET description='completed' "
                    "WHERE security_level=?1",
                    node);
            }}));
    }
    for (auto& write : writes) {
        require(write.get().succeeded(), "modeled 128-node write commits");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    {
        internal::SqliteConnection verification(database_file, SQLITE_OPEN_READONLY, 50ms, false);
        require(verification.scalar_int64(
                    "SELECT sum(modified_utc_seconds) FROM sfx_security_levels "
                    "WHERE security_level BETWEEN 1 AND 128",
                    "verify modeled periodic writes") == 7'680,
                "all modeled coalesced periodic writes commit");
        require(verification.scalar_int64(
                    "SELECT count(*) FROM sfx_security_levels "
                    "WHERE security_level BETWEEN 1 AND 128 AND description='completed'",
                    "verify modeled durable outcomes") == 128,
                "all modeled completed outcomes commit");
        require(verification.scalar_int64(
                    "SELECT count(*) FROM sfx_security_levels "
                    "WHERE security_level BETWEEN 1 AND 128 "
                    "AND modified_utc_seconds=60 AND description='completed'",
                    "verify modeled per-node totals") == 128,
                "each logical node has exact modeled totals");
    }

    const app::StorageHealth health = coordinator.health();
    require(health.committed_operations == 7'808, "modeled committed operation count");
    require(health.committed_batches > 0 &&
                health.committed_batches < health.committed_operations,
            "modeled writes use transaction batching");
    require(health.maximum_committed_batch_operations >= 2 &&
                health.maximum_committed_batch_operations <= 64,
            "measured transaction batch remains within configured bound");
    require(health.queue_high_water_operations <= 4096,
            "modeled queue remains within configured operation bound");
    require(coordinator.shutdown().succeeded(), "modeled workload drains and checkpoints");
    std::cout << "stage5a_128_node_workload operations=" << health.committed_operations
              << " batches=" << health.committed_batches
              << " maximum_batch=" << health.maximum_committed_batch_operations
              << " queue_high_water=" << health.queue_high_water_operations
              << " elapsed_ms=" << elapsed.count() << '\n';
}

void test_recovery_required_fails_closed(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "fail-closed";
    std::filesystem::create_directories(data);
    storage::SqliteStorageSettings settings = settings_for(data);
    settings.queue.dequeue_batch_limit = 1;
    settings.queue.dequeue_batch_window = 0ms;

    internal::SqlitePagerTelemetry telemetry;
    internal::SqliteWriterCoordinator coordinator;
    require(coordinator.start(
                data / "SFX.DB", settings, storage::sfx_storage_migrations(), telemetry).succeeded(),
            "fail-closed coordinator starts");

    std::promise<void> fatal_started;
    std::promise<void> release_fatal;
    std::shared_future<void> release_signal = release_fatal.get_future().share();
    auto fatal = coordinator.submit({
        "simulated corruption",
        1,
        internal::WriteDurability::durable,
        [&fatal_started, release_signal](sqlite3*) {
            fatal_started.set_value();
            release_signal.wait();
            return SQLITE_CORRUPT;
        }});
    fatal_started.get_future().get();

    auto pending = coordinator.submit({
        "must not execute after corruption",
        1,
        internal::WriteDurability::durable,
        [](sqlite3*) { return SQLITE_OK; }});
    release_fatal.set_value();

    require(fatal.get().code == app::StorageErrorCode::corrupt, "corruption is mapped");
    require(pending.get().code == app::StorageErrorCode::corrupt,
            "pending writes fail with recovery-required cause");
    require(coordinator.health().state == app::StorageRuntimeState::recovery_required,
            "fatal write moves storage to recovery required");
    require(coordinator.submit({
                "rejected after corruption",
                1,
                internal::WriteDurability::durable,
                [](sqlite3*) { return SQLITE_OK; }})
                .get()
                .code == app::StorageErrorCode::shutting_down,
            "new writes are rejected after corruption");
    require(coordinator.shutdown().code == app::StorageErrorCode::corrupt,
            "shutdown preserves fatal storage result");
    require(coordinator.health().state == app::StorageRuntimeState::recovery_required,
            "shutdown does not erase recovery-required health");
}

void test_disk_full_rolls_back(const std::filesystem::path& root) {
    const std::filesystem::path data = root / "disk-full";
    std::filesystem::create_directories(data);
    storage::SqliteStorageSettings settings = settings_for(data);
    settings.queue.dequeue_batch_window = 0ms;
    internal::SqlitePagerTelemetry telemetry;
    internal::SqliteWriterCoordinator coordinator;
    const std::filesystem::path database_file = data / "SFX.DB";
    require(coordinator.start(
                database_file, settings, storage::sfx_storage_migrations(), telemetry).succeeded(),
        "disk-full coordinator starts");

    auto initialize = coordinator.submit({
        "initialize disk-full probe",
        1,
        internal::WriteDurability::durable,
        [](sqlite3* database) {
            return sqlite3_exec(
                database,
                "CREATE TABLE stage5a_full_probe("
                "row_id INTEGER PRIMARY KEY,payload BLOB NOT NULL) STRICT",
                nullptr,
                nullptr,
                nullptr);
        }});
    require(initialize.get().succeeded(), "disk-full probe initializes");

    std::string fill_sql = "PRAGMA max_page_count=16;";
    for (int index = 0; index < 100; ++index) {
        fill_sql += "INSERT INTO stage5a_full_probe(payload) VALUES(zeroblob(4096));";
    }
    auto fill = coordinator.submit({
        "bounded disk-full operation",
        fill_sql.size(),
        internal::WriteDurability::durable,
        [fill_sql](sqlite3* database) {
            return sqlite3_exec(database, fill_sql.c_str(), nullptr, nullptr, nullptr);
        }});
    require(fill.get().code == app::StorageErrorCode::full, "SQLITE_FULL mapped without retry");
    require(coordinator.shutdown().succeeded(), "disk-full coordinator shuts down");

    internal::SqliteConnection verification(database_file, SQLITE_OPEN_READONLY, 50ms, false);
    require(verification.scalar_int64("SELECT count(*) FROM stage5a_full_probe", "verify full rollback") == 0,
            "disk-full transaction fully rolls back");
    require(verification.scalar_text("PRAGMA quick_check", "verify disk-full integrity") == "ok",
            "database remains healthy after SQLITE_FULL");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const bool layout_only = argc == 3 && std::string_view(argv[2]) == "--layout-profile-only";
        const bool preserve = layout_only || (argc == 3 && std::string_view(argv[2]) == "--preserve-fixture");
        require(argc == 2 || preserve, "storage tests require an output directory and optional --preserve-fixture");
        const std::filesystem::path root = std::filesystem::absolute(argv[1]);
        if (preserve) require(!std::filesystem::exists(root), "preserve earlier test evidence");
        else remove_test_tree(root);
        std::filesystem::create_directories(root);
        test_native_layout_profiles(root);
        if (layout_only) {
            std::cout << "PASS native storage layout profiles: canonical/lowercase seeds, accepted migration identity, backup/restore/reopen and legacy/unknown refusal\n";
            return 0;
        }
        test_dependency_pin_and_compile_options(root);
        test_deterministic_fakes(root);
        test_lifecycle_migration_backup_restore(root);
        test_restore_after_startup_failure(root);
        test_baseline_publication_failure_rolls_back(root);
        test_missing_baseline_ledger_row_fails_closed(root);
        test_nonbaseline_plan_fails_closed(root);
        test_backup_plan_and_full_validation(root);
        test_independent_group_commit_outcomes(root);
        test_writer_backpressure_atomicity_and_busy(root);
        test_injected_writer_failures(root);
        test_incomplete_passive_checkpoint_reporting(root);
        test_transaction_batching_and_128_node_workload(root);
        test_recovery_required_fails_closed(root);
        test_disk_full_rolls_back(root);
        if (!preserve) remove_test_tree(root);
        std::cout << "all SFX Stage 5A storage infrastructure tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Stage 5A storage test failure: " << error.what() << '\n';
        return 1;
    }
}
