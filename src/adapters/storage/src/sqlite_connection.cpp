#include "sqlite_internal.hpp"
#include "sqlite_sfx_baseline.hpp"
#include "sfx/application/file.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>
#include <sodium.h>

namespace sfx::adapters::storage::internal {
void secure_lineage_sql(sqlite3_context* context,int,sqlite3_value**) noexcept {
    if(sodium_init()<0){sqlite3_result_error(context,"FTN secure lineage generator unavailable",-1);return;}
    std::array<unsigned char,16> bytes{};
    // libsodium uses the native secure random source and fails closed if it
    // cannot obtain entropy. No clock, path, or PRNG fallback is permitted.
    randombytes_buf(bytes.data(),bytes.size());
    if(sodium_is_zero(bytes.data(),bytes.size())){sqlite3_result_error(context,"FTN secure lineage generation failed",-1);return;}
    sqlite3_result_blob(context,bytes.data(),static_cast<int>(bytes.size()),SQLITE_TRANSIENT);
}
namespace {
std::atomic<std::uint64_t> next_pager_generation{1};

void file_fold_sql(sqlite3_context* context,int count,sqlite3_value** values) noexcept {
    try{
        if(count!=1||sqlite3_value_type(values[0])!=SQLITE_TEXT){sqlite3_result_error(context,"File text must be UTF-8",-1);return;}
        const auto* bytes=sqlite3_value_text(values[0]);const auto size=sqlite3_value_bytes(values[0]);
        if(!bytes||size<0||size>static_cast<int>(application::kFileLongDescriptionBytes)){sqlite3_result_error(context,"File text exceeds bound",-1);return;}
        const auto key=application::file_case_key({reinterpret_cast<const char*>(bytes),static_cast<std::size_t>(size)});
        if(!key){sqlite3_result_error(context,"Invalid file UTF-8",-1);return;}
        sqlite3_result_text(context,key->c_str(),static_cast<int>(key->size()),SQLITE_TRANSIENT);
    }catch(...){sqlite3_result_error_nomem(context);}
}

std::uint64_t saturating_add(const std::uint64_t left,
    const std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max() : left + right;
}

bool read_pager_counters(sqlite3* const database,
    SqlitePagerTelemetry::Slot* const output) noexcept {
    sqlite3_int64 hits = 0, misses = 0, writes = 0, ignored = 0;
    if (database == nullptr ||
        sqlite3_db_status64(database, SQLITE_DBSTATUS_CACHE_HIT, &hits, &ignored, 0) != SQLITE_OK ||
        sqlite3_db_status64(database, SQLITE_DBSTATUS_CACHE_MISS, &misses, &ignored, 0) != SQLITE_OK ||
        sqlite3_db_status64(database, SQLITE_DBSTATUS_CACHE_WRITE, &writes, &ignored, 0) != SQLITE_OK ||
        hits < 0 || misses < 0 || writes < 0) return false;
    output->available = true;
    output->cache_hits = static_cast<std::uint64_t>(hits);
    output->cache_misses = static_cast<std::uint64_t>(misses);
    output->cache_writes = static_cast<std::uint64_t>(writes);
    return true;
}

}  // namespace

void SqlitePagerTelemetry::begin(const std::size_t connection_slots) {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.assign(connection_slots, {});
    generation_ = next_pager_generation.fetch_add(1, std::memory_order_relaxed);
}

void SqlitePagerTelemetry::connection_opened(
    const std::size_t slot, sqlite3* const database) noexcept {
    Slot value;
    (void)read_pager_counters(database, &value);
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= slots_.size()) return;
    slots_[slot] = value;
    generation_ = next_pager_generation.fetch_add(1, std::memory_order_relaxed);
}

void SqlitePagerTelemetry::sample(
    const std::size_t slot, sqlite3* const database) noexcept {
    Slot value;
    if (!read_pager_counters(database, &value)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot < slots_.size()) slots_[slot] = value;
}

void SqlitePagerTelemetry::connection_closed(const std::size_t slot) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot >= slots_.size()) return;
    slots_[slot] = {};
    generation_ = next_pager_generation.fetch_add(1, std::memory_order_relaxed);
}

SqlitePagerCounterSnapshot SqlitePagerTelemetry::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    SqlitePagerCounterSnapshot result;
    result.connection_generation = generation_;
    for (const auto& slot : slots_) {
        if (!slot.available) continue;
        result.available = true;
        result.cache_hits = saturating_add(result.cache_hits, slot.cache_hits);
        result.cache_misses = saturating_add(result.cache_misses, slot.cache_misses);
        result.cache_writes = saturating_add(result.cache_writes, slot.cache_writes);
    }
    return result;
}
namespace {

void require_sqlite(sqlite3* database, const int code, const std::string& operation) {
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) {
        return;
    }
    const char* const message = database != nullptr ? sqlite3_errmsg(database) : sqlite3_errstr(code);
    throw StorageFailure(
        map_sqlite_error(code),
        operation,
        code,
        message != nullptr ? message : "unknown SQLite failure");
}

bool has_exact_sfx_plan(const std::vector<SqliteMigration>& migrations) {
    if (migrations.size()<3U || migrations.size()>31U) return false;
    const std::array<const SqliteMigration*, 31> expected{
        &sfx_initial_baseline_migration(), &sfx_caller_journal_migration(), &sfx_menu_preferences_migration(), &sfx_caller_utilities_migration(), &sfx_classic_information_migration(), &sfx_classic_delete_migration(), &sfx_other_board_text_migration(), &sfx_local_message_migration(), &sfx_message_admin_migration(), &sfx_message_group_migration(), &sfx_message_delete_migration(), &sfx_message_source_migration(), &sfx_message_maintenance_migration(),&sfx_qwk_migration(),&sfx_file_migration(),&sfx_door_migration(),&sfx_door_security_migration(),&sfx_door_program_migration(),&sfx_chat_migration(),&sfx_private_chat_migration(),&sfx_ftn_migration(),&sfx_ftn_mail_migration(),&sfx_optional_subject_migration(), &sfx_file_group_deletion_migration(),&sfx_ftn_wire_body_migration(),&sfx_ftn_subject_migration(),&sfx_message_location_migration(),&sfx_ftn_external_migration(),&sfx_ftn_recovery_migration(),&sfx_door_bulletin_migration(),&sfx_message_list_delete_migration()};
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        if (migrations[i].version != expected[i]->version || migrations[i].name != expected[i]->name ||
            migrations[i].content_sha256 != expected[i]->content_sha256 || migrations[i].sql != expected[i]->sql)
            return false;
    }
    return true;
}

void run_bootstrap_boundary(
    const std::function<application::StorageStatus(std::string_view)>& hook,
    const std::string& name) {
    if (!hook) return;
    try {
        const application::StorageStatus status = hook(name);
        if (!status) {
            throw StorageFailure(
                status.code,
                name,
                SQLITE_ABORT,
                status.operation);
        }
    } catch (const StorageFailure&) {
        throw;
    } catch (...) {
        throw StorageFailure(
            application::StorageErrorCode::internal,
            name,
            SQLITE_ABORT,
            "contain fresh-bootstrap boundary hook");
    }
}

std::string path_as_utf8(const std::filesystem::path& path) {
    return path.u8string();
}

[[noreturn]] void fail_migration_history(const std::string& detail) {
    throw StorageFailure(
        application::StorageErrorCode::incompatible_schema,
        "verify migration history",
        SQLITE_SCHEMA,
        detail);
}

std::int32_t verify_migration_history(
    SqliteConnection& connection,
    const std::vector<SqliteMigration>& approved_migrations) {
    if (!has_exact_sfx_plan(approved_migrations)) {
        fail_migration_history("the active migration plan differs from the approved SFX shipping lineage");
    }
    if (!validate_sfx_shipping_database(connection.get(), true)) {
        fail_migration_history(
            "the database identity, ledger, or stored schema differs from an exact approved SFX version");
    }
    const auto version=static_cast<std::int32_t>(connection.scalar_int64("PRAGMA user_version", "read verified SFX schema version"));
    if(version>approved_migrations.back().version) fail_migration_history("approved lineage cannot downgrade a newer schema");
    return version;
}

int migration_authorizer(
    void*,
    const int action,
    const char*,
    const char*,
    const char*,
    const char*) {
    return action == SQLITE_TRANSACTION || action == SQLITE_SAVEPOINT ||
            action == SQLITE_ATTACH || action == SQLITE_DETACH
        ? SQLITE_DENY
        : SQLITE_OK;
}

class MigrationSqlGuard final {
public:
    explicit MigrationSqlGuard(sqlite3* database) : database_(database) {
        require_sqlite(
            database_,
            sqlite3_set_authorizer(database_, migration_authorizer, nullptr),
            "protect managed migration transaction");
    }

    ~MigrationSqlGuard() {
        (void)sqlite3_set_authorizer(database_, nullptr, nullptr);
    }

    MigrationSqlGuard(const MigrationSqlGuard&) = delete;
    MigrationSqlGuard& operator=(const MigrationSqlGuard&) = delete;

private:
    sqlite3* database_;
};

void migrate_caller_journals(SqliteConnection& connection,
    const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if (connection.scalar_int64("PRAGMA user_version", "read journal migration version") >= 2) return;
    sqlite3* const database = connection.get();
    connection.exec("BEGIN IMMEDIATE", "begin additive caller-journal migration");
    try {
        if (!validate_sfx_baseline_database(database, true))
            fail_migration_history("require exact unchanged v1 before journal upgrade");
        run_bootstrap_boundary(hook, "before-journal-migration");
        const auto& migration = sfx_caller_journal_migration();
        { MigrationSqlGuard guard(database); connection.exec(migration.sql, "apply caller-journal migration"); }
        sqlite3_stmt* raw{};
        require_sqlite(database, sqlite3_prepare_v2(database,
            "INSERT INTO sfx_schema_migrations(version,name,content_sha256,applied_utc) VALUES(2,?1,?2,?3)",
            -1, &raw, nullptr), "prepare journal migration ledger");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> ledger(raw, &sqlite3_finalize);
        require_sqlite(database, sqlite3_bind_text(raw, 1, migration.name.c_str(), -1, SQLITE_TRANSIENT), "bind journal migration name");
        require_sqlite(database, sqlite3_bind_text(raw, 2, migration.content_sha256.c_str(), -1, SQLITE_TRANSIENT), "bind journal migration hash");
        require_sqlite(database, sqlite3_bind_int64(raw, 3, utc_now()), "bind journal migration time");
        require_sqlite(database, sqlite3_step(raw), "record journal migration ledger");
        ledger.reset();
        connection.exec("PRAGMA user_version=2", "publish journal schema version");
        if (!validate_sfx_shipping_database(database, true)) fail_migration_history("verify exact journal migration result");
        run_bootstrap_boundary(hook, "before-journal-migration-commit");
        connection.exec("COMMIT", "commit additive caller-journal migration");
    } catch (...) { sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    run_bootstrap_boundary(hook, "after-journal-migration-commit");
}

void migrate_menu_preferences(SqliteConnection& connection,
    const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if (connection.scalar_int64("PRAGMA user_version", "read menu migration version") >= 3) return;
    sqlite3* const database = connection.get();
    connection.exec("BEGIN IMMEDIATE", "begin additive menu-preference migration");
    try {
        if (connection.scalar_int64("PRAGMA user_version", "verify menu predecessor") != 2 ||
            !validate_sfx_shipping_database(database, true))
            fail_migration_history("require exact v2 before menu-preference upgrade");
        run_bootstrap_boundary(hook, "before-menu-migration");
        const auto& migration = sfx_menu_preferences_migration();
        { MigrationSqlGuard guard(database); connection.exec(migration.sql, "apply menu-preference migration"); }
        sqlite3_stmt* raw{};
        require_sqlite(database, sqlite3_prepare_v2(database,
            "INSERT INTO sfx_schema_migrations(version,name,content_sha256,applied_utc) VALUES(3,?1,?2,?3)",
            -1, &raw, nullptr), "prepare menu migration ledger");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> ledger(raw, &sqlite3_finalize);
        require_sqlite(database, sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT), "bind menu migration name");
        require_sqlite(database, sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT), "bind menu migration hash");
        require_sqlite(database, sqlite3_bind_int64(raw,3,utc_now()), "bind menu migration time");
        require_sqlite(database, sqlite3_step(raw), "record menu migration ledger");
        ledger.reset();
        connection.exec("PRAGMA user_version=3", "publish menu schema version");
        if (!validate_sfx_shipping_database(database,true)) fail_migration_history("verify exact menu migration result");
        run_bootstrap_boundary(hook, "before-menu-migration-commit");
        connection.exec("COMMIT", "commit additive menu-preference migration");
    } catch (...) { sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr); throw; }
    run_bootstrap_boundary(hook, "after-menu-migration-commit");
}

void migrate_caller_utilities(SqliteConnection& connection,
    const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if(connection.scalar_int64("PRAGMA user_version","read caller utilities migration version")>=4) return;
    sqlite3* const database=connection.get();
    connection.exec("BEGIN IMMEDIATE","begin caller utilities migration");
    try {
        if(connection.scalar_int64("PRAGMA user_version","verify caller utilities predecessor")!=3 ||
            !validate_sfx_shipping_database(database,true)) fail_migration_history("require exact v3 before caller utilities upgrade");
        run_bootstrap_boundary(hook,"before-caller-utilities-migration");
        const auto& migration=sfx_caller_utilities_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply caller utilities migration");}
        sqlite3_stmt* raw{};
        require_sqlite(database,sqlite3_prepare_v2(database,
            "INSERT INTO sfx_schema_migrations VALUES(4,?1,?2,?3)",-1,&raw,nullptr),"prepare caller utilities ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind caller utilities name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind caller utilities hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind caller utilities time");
        require_sqlite(database,sqlite3_step(raw),"record caller utilities ledger");ledger.reset();
        connection.exec("PRAGMA user_version=4","publish caller utilities schema version");
        if(!validate_sfx_shipping_database(database,true)) fail_migration_history("verify exact caller utilities migration result");
        run_bootstrap_boundary(hook,"before-caller-utilities-migration-commit");
        connection.exec("COMMIT","commit caller utilities migration");
    } catch(...) {sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-caller-utilities-migration-commit");
}

void migrate_classic_information(SqliteConnection& connection,
    const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if(connection.scalar_int64("PRAGMA user_version","read classic information migration version")>=5) return;
    sqlite3* const database=connection.get();
    connection.exec("BEGIN IMMEDIATE","begin classic information migration");
    try {
        if(connection.scalar_int64("PRAGMA user_version","verify classic information predecessor")!=4 ||
            !validate_sfx_shipping_database(database,true)) fail_migration_history("require exact v4 before classic information upgrade");
        run_bootstrap_boundary(hook,"before-classic-information-migration");
        const auto& migration=sfx_classic_information_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply classic information migration");}
        sqlite3_stmt* raw{};
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(5,?1,?2,?3)",-1,&raw,nullptr),"prepare classic information ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind classic information name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind classic information hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind classic information time");
        require_sqlite(database,sqlite3_step(raw),"record classic information ledger");ledger.reset();
        connection.exec("PRAGMA user_version=5","publish classic information schema version");
        if(!validate_sfx_shipping_database(database,true)) fail_migration_history("verify exact classic information migration result");
        run_bootstrap_boundary(hook,"before-classic-information-migration-commit");
        connection.exec("COMMIT","commit classic information migration");
    } catch(...) {sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-classic-information-migration-commit");
}
void migrate_classic_delete(SqliteConnection& connection,
    const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if(connection.scalar_int64("PRAGMA user_version","read classic delete migration version")>=6)return;
    sqlite3* const database=connection.get();
    connection.exec("BEGIN IMMEDIATE","begin classic delete migration");
    try {
        if(connection.scalar_int64("PRAGMA user_version","verify classic delete predecessor")!=5 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v5 before classic delete upgrade");
        run_bootstrap_boundary(hook,"before-classic-delete-migration");
        const auto& migration=sfx_classic_delete_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply classic delete migration");}
        sqlite3_stmt* raw{};
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(6,?1,?2,?3)",-1,&raw,nullptr),"prepare classic delete ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind classic delete name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind classic delete hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind classic delete time");
        require_sqlite(database,sqlite3_step(raw),"record classic delete ledger");ledger.reset();
        connection.exec("PRAGMA user_version=6","publish classic delete schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact classic delete migration result");
        run_bootstrap_boundary(hook,"before-classic-delete-migration-commit");
        connection.exec("COMMIT","commit classic delete migration");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-classic-delete-migration-commit");
}
void migrate_other_board_text(SqliteConnection& connection,
    const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if(connection.scalar_int64("PRAGMA user_version","read text catalog migration version")>=7)return;
    sqlite3* const database=connection.get();connection.exec("BEGIN IMMEDIATE","begin text catalog retirement");
    try {
        if(connection.scalar_int64("PRAGMA user_version","verify text catalog predecessor")!=6 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v6 before text catalog retirement");
        run_bootstrap_boundary(hook,"before-other-bbs-text-migration");
        const auto& migration=sfx_other_board_text_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"retire Other BBS database rows");}
        sqlite3_stmt* raw{};
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(7,?1,?2,?3)",-1,&raw,nullptr),"prepare text catalog ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind text catalog name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind text catalog hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind text catalog time");
        require_sqlite(database,sqlite3_step(raw),"record text catalog ledger");ledger.reset();
        connection.exec("PRAGMA user_version=7","publish text catalog schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify text catalog retirement result");
        run_bootstrap_boundary(hook,"before-other-bbs-text-migration-commit");connection.exec("COMMIT","commit text catalog retirement");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-other-bbs-text-migration-commit");
}
void migrate_local_messages(SqliteConnection& connection,
    const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if (connection.scalar_int64("PRAGMA user_version", "read local message migration version") >= 8) return;
    sqlite3* const database = connection.get();
    connection.exec("BEGIN IMMEDIATE", "begin core local message migration");
    try {
        if (connection.scalar_int64("PRAGMA user_version", "verify local message predecessor") != 7 ||
            !validate_sfx_shipping_database(database, true))
            fail_migration_history("require exact v7 before local message upgrade");
        run_bootstrap_boundary(hook, "before-local-message-migration");
        const auto& migration = sfx_local_message_migration();
        { MigrationSqlGuard guard(database); connection.exec(migration.sql, "apply core local message migration"); }
        sqlite3_stmt* raw{};
        require_sqlite(database, sqlite3_prepare_v2(database,
            "INSERT INTO sfx_schema_migrations VALUES(8,?1,?2,?3)", -1, &raw, nullptr), "prepare local message ledger");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> ledger(raw, &sqlite3_finalize);
        require_sqlite(database, sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT), "bind local message name");
        require_sqlite(database, sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT), "bind local message hash");
        require_sqlite(database, sqlite3_bind_int64(raw,3,utc_now()), "bind local message time");
        require_sqlite(database, sqlite3_step(raw), "record local message ledger");
        ledger.reset();
        connection.exec("PRAGMA user_version=8", "publish local message schema version");
        if (!validate_sfx_shipping_database(database, true))
            fail_migration_history("verify core local message migration result");
        run_bootstrap_boundary(hook, "before-local-message-migration-commit");
        connection.exec("COMMIT", "commit core local message migration");
    } catch (...) { sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    run_bootstrap_boundary(hook, "after-local-message-migration-commit");
}
void migrate_message_admin(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if(connection.scalar_int64("PRAGMA user_version","read echo administration version")>=9)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin echo administration upgrade");
    try {
        if(connection.scalar_int64("PRAGMA user_version","verify administration predecessor")!=8 || !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v8 before echo administration");
        run_bootstrap_boundary(hook,"before-message-admin-migration");
        const auto& migration=sfx_message_admin_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply echo administration migration");}
        sqlite3_stmt* raw{};
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(9,?1,?2,?3)",-1,&raw,nullptr),"prepare administration ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind administration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind administration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind administration time");
        require_sqlite(database,sqlite3_step(raw),"record administration ledger");ledger.reset();
        connection.exec("PRAGMA user_version=9","publish echo administration version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify echo administration upgrade");
        run_bootstrap_boundary(hook,"before-message-admin-migration-commit");connection.exec("COMMIT","commit echo administration upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-message-admin-migration-commit");
}
void migrate_message_group(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if(connection.scalar_int64("PRAGMA user_version","read empty group deletion version")>=10)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin empty group deletion upgrade");
    try {
        if(connection.scalar_int64("PRAGMA user_version","verify administration predecessor")!=9 || !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v9 before empty group deletion");
        run_bootstrap_boundary(hook,"before-message-group-migration");
        const auto& migration=sfx_message_group_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply empty group deletion migration");}
        sqlite3_stmt* raw{};
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(10,?1,?2,?3)",-1,&raw,nullptr),"prepare administration ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind administration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind administration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind administration time");
        require_sqlite(database,sqlite3_step(raw),"record administration ledger");ledger.reset();
        connection.exec("PRAGMA user_version=10","publish empty group deletion version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify empty group deletion upgrade");
        run_bootstrap_boundary(hook,"before-message-group-migration-commit");connection.exec("COMMIT","commit empty group deletion upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-message-group-migration-commit");
}
void migrate_message_delete(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if(connection.scalar_int64("PRAGMA user_version","read individual message deletion version")>=11)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin individual message deletion upgrade");
    try {
        if(connection.scalar_int64("PRAGMA user_version","verify deletion predecessor")!=10 || !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v10 before individual message deletion");
        run_bootstrap_boundary(hook,"before-message-delete-migration");
        const auto& migration=sfx_message_delete_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply individual message deletion migration");}
        sqlite3_stmt* raw{};
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(11,?1,?2,?3)",-1,&raw,nullptr),"prepare deletion ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind deletion name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind deletion hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind deletion time");
        require_sqlite(database,sqlite3_step(raw),"record deletion ledger");ledger.reset();
        connection.exec("PRAGMA user_version=11","publish individual message deletion version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify individual message deletion upgrade");
        run_bootstrap_boundary(hook,"before-message-delete-migration-commit");connection.exec("COMMIT","commit individual message deletion upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-message-delete-migration-commit");
}
void migrate_message_source(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if(connection.scalar_int64("PRAGMA user_version","read message source version")>=12)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin message source upgrade");
    try {
        if(connection.scalar_int64("PRAGMA user_version","verify source predecessor")!=11 || !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v11 before message source");
        run_bootstrap_boundary(hook,"before-message-source-migration");
        const auto& migration=sfx_message_source_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply message source migration");}
        sqlite3_stmt* raw{};
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(12,?1,?2,?3)",-1,&raw,nullptr),"prepare source ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind source name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind source hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind source time");
        require_sqlite(database,sqlite3_step(raw),"record source ledger");ledger.reset();
        connection.exec("PRAGMA user_version=12","publish message source version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify message source upgrade");
        run_bootstrap_boundary(hook,"before-message-source-migration-commit");connection.exec("COMMIT","commit message source upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-message-source-migration-commit");
}
void migrate_message_maintenance(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if(connection.scalar_int64("PRAGMA user_version","read message maintenance version")>=13)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin message maintenance upgrade");
    try {
        if(connection.scalar_int64("PRAGMA user_version","verify maintenance predecessor")!=12 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v12 before message maintenance");
        run_bootstrap_boundary(hook,"before-message-maintenance-migration");
        const auto& migration=sfx_message_maintenance_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply message maintenance migration without retention");}
        sqlite3_stmt* raw{};
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(13,?1,?2,?3)",-1,&raw,nullptr),"prepare maintenance ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind maintenance name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind maintenance hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind maintenance time");
        require_sqlite(database,sqlite3_step(raw),"record maintenance ledger");ledger.reset();
        connection.exec("PRAGMA user_version=13","publish message maintenance version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify message maintenance upgrade");
        run_bootstrap_boundary(hook,"before-message-maintenance-migration-commit");
        connection.exec("COMMIT","commit message maintenance upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-message-maintenance-migration-commit");
}
void migrate_qwk(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read QWK schema version")>=14)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin QWK schema upgrade");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify QWK predecessor")!=13 || !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v13 before QWK upgrade");
        run_bootstrap_boundary(hook,"before-qwk-migration");const auto& migration=sfx_qwk_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply additive QWK migration without message/scan mutation");}
        sqlite3_stmt* raw=nullptr;require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(14,?1,?2,?3)",-1,&raw,nullptr),"prepare QWK ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind QWK migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind QWK migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind QWK migration time");
        require_sqlite(database,sqlite3_step(raw),"record QWK ledger");ledger.reset();
        connection.exec("PRAGMA user_version=14","publish QWK schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact QWK schema upgrade");
        run_bootstrap_boundary(hook,"before-qwk-migration-commit");connection.exec("COMMIT","commit QWK schema upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-qwk-migration-commit");
}
void migrate_files(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook,
    bool lowercase_paths) {
    if(connection.scalar_int64("PRAGMA user_version","read file schema version")>=15)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin file schema upgrade");
    try {
        if(connection.scalar_int64("PRAGMA user_version","verify file predecessor")!=14 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v14 before file upgrade");
        run_bootstrap_boundary(hook,"before-file-migration");
        const auto& migration=sfx_file_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply additive native file migration");}
        // Select native defaults only while the table is first created, inside
        // the same transaction. Keep the accepted migration SQL/hash intact;
        // reopening an established schema never rewrites a saved Sysop path.
        if(lowercase_paths)connection.exec(
            "UPDATE sfx_file_area_paths SET storage_path=CASE area_id "
            "WHEN 1 THEN 'files/general' WHEN 2 THEN 'files/uploads' "
            "WHEN 3 THEN 'files/uploadtosysop' END WHERE area_id IN(1,2,3)",
            "select lowercase Linux fresh file-area paths");
        sqlite3_stmt* raw=nullptr;
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(15,?1,?2,?3)",-1,&raw,nullptr),"prepare file ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind file migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind file migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind file migration time");
        require_sqlite(database,sqlite3_step(raw),"record file ledger");ledger.reset();
        connection.exec("PRAGMA user_version=15","publish native file schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact native file schema upgrade");
        run_bootstrap_boundary(hook,"before-file-migration-commit");connection.exec("COMMIT","commit native file upgrade");
    } catch(...) {sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-file-migration-commit");
}
void migrate_doors(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read Door schema version")>=16)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin Door schema upgrade");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify Door predecessor")!=15 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v15 before Door upgrade");
        run_bootstrap_boundary(hook,"before-door-migration");const auto& migration=sfx_door_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply additive Door migration");}
        sqlite3_stmt* raw=nullptr;
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(16,?1,?2,?3)",-1,&raw,nullptr),"prepare Door ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind Door migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind Door migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind Door migration time");require_sqlite(database,sqlite3_step(raw),"record Door ledger");ledger.reset();
        connection.exec("PRAGMA user_version=16","publish Door schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact Door schema upgrade");
        run_bootstrap_boundary(hook,"before-door-migration-commit");connection.exec("COMMIT","commit Door schema upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-door-migration-commit");
}
void migrate_door_security(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read Door schema version")>=17)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin Door schema upgrade");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify Door predecessor")!=16 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v16 before Door upgrade");
        run_bootstrap_boundary(hook,"before-door-requirements-migration");const auto& migration=sfx_door_security_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply additive Door migration");}
        sqlite3_stmt* raw=nullptr;
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(17,?1,?2,?3)",-1,&raw,nullptr),"prepare Door ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind Door migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind Door migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind Door migration time");require_sqlite(database,sqlite3_step(raw),"record Door ledger");ledger.reset();
        connection.exec("PRAGMA user_version=17","publish Door schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact Door schema upgrade");
        run_bootstrap_boundary(hook,"before-door-requirements-migration-commit");connection.exec("COMMIT","commit Door schema upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-door-requirements-migration-commit");
}
void migrate_door_program(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read Door schema version")>=18)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin Door schema upgrade");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify Door predecessor")!=17 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v17 before Door upgrade");
        run_bootstrap_boundary(hook,"before-door-program-migration");const auto& migration=sfx_door_program_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply additive Door migration");}
        sqlite3_stmt* raw=nullptr;
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(18,?1,?2,?3)",-1,&raw,nullptr),"prepare Door ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind Door migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind Door migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind Door migration time");require_sqlite(database,sqlite3_step(raw),"record Door ledger");ledger.reset();
        connection.exec("PRAGMA user_version=18","publish Door schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact Door schema upgrade");
        run_bootstrap_boundary(hook,"before-door-program-migration-commit");connection.exec("COMMIT","commit Door schema upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-door-program-migration-commit");
}void migrate_chat(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read Chat schema version")>=19)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin Chat schema upgrade");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify Chat predecessor")!=18 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v18 before Chat upgrade");
        run_bootstrap_boundary(hook,"before-chat-migration");const auto& migration=sfx_chat_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply additive Chat migration");}
        sqlite3_stmt* raw=nullptr;
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(19,?1,?2,?3)",-1,&raw,nullptr),"prepare Chat ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind Chat migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind Chat migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind Chat migration time");require_sqlite(database,sqlite3_step(raw),"record Chat ledger");ledger.reset();
        connection.exec("PRAGMA user_version=19","publish Chat schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact Chat schema upgrade");
        run_bootstrap_boundary(hook,"before-chat-migration-commit");connection.exec("COMMIT","commit Chat schema upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-chat-migration-commit");
}
void migrate_private_chat(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read private Chat schema version")>=20)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin private Chat schema upgrade");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify private Chat predecessor")!=19 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v19 before private Chat upgrade");
        run_bootstrap_boundary(hook,"before-private-chat-migration");const auto& migration=sfx_private_chat_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply additive private Chat migration");}
        sqlite3_stmt* raw=nullptr;
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(20,?1,?2,?3)",-1,&raw,nullptr),"prepare private Chat ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind private Chat migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind private Chat migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind private Chat migration time");require_sqlite(database,sqlite3_step(raw),"record private Chat ledger");ledger.reset();
        connection.exec("PRAGMA user_version=20","publish private Chat schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact private Chat schema upgrade");
        run_bootstrap_boundary(hook,"before-private-chat-migration-commit");connection.exec("COMMIT","commit private Chat schema upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-private-chat-migration-commit");
}
void migrate_file_group_deletion(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read File Group deletion schema version")>=24)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin File Group deletion schema upgrade");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify File Group deletion predecessor")!=23 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v23 before File Group deletion upgrade");
        run_bootstrap_boundary(hook,"before-file-group-deletion-migration");const auto& migration=sfx_file_group_deletion_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply additive File Group deletion migration");}
        sqlite3_stmt* raw=nullptr;
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(24,?1,?2,?3)",-1,&raw,nullptr),"prepare File Group deletion ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind File Group deletion migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind File Group deletion migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind File Group deletion migration time");require_sqlite(database,sqlite3_step(raw),"record File Group deletion ledger");ledger.reset();
        connection.exec("PRAGMA user_version=24","publish File Group deletion schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact File Group deletion schema upgrade");
        run_bootstrap_boundary(hook,"before-file-group-deletion-migration-commit");connection.exec("COMMIT","commit File Group deletion schema upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-file-group-deletion-migration-commit");
}
void migrate_message_location(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read caller location schema")>=27)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin caller location migration");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify location predecessor")!=26||!validate_sfx_shipping_database(database,true))fail_migration_history("require exact v26 before caller location upgrade");
        run_bootstrap_boundary(hook,"before-message-location-migration");const auto& migration=sfx_message_location_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply fixed caller location migration");}
        sqlite3_stmt* raw{};require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(27,?1,?2,?3)",-1,&raw,nullptr),"prepare location ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind location name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind location hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind location time");require_sqlite(database,sqlite3_step(raw),"record location ledger");ledger.reset();
        connection.exec("PRAGMA user_version=27","publish caller location schema");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify caller location migration");
        run_bootstrap_boundary(hook,"before-message-location-migration-commit");connection.exec("COMMIT","commit caller location migration");
    }catch(...){if(!sqlite3_get_autocommit(database))sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}

void migrate_ftn_external(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read external FTN schema")>=28)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin external FTN migration");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify external FTN predecessor")!=27||!validate_sfx_shipping_database(database,true))fail_migration_history("require exact v27 before external FTN upgrade");
        run_bootstrap_boundary(hook,"before-ftn-external-migration");const auto& migration=sfx_ftn_external_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply fixed external FTN migration");}
        sqlite3_stmt* raw{};require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(28,?1,?2,?3)",-1,&raw,nullptr),"prepare external FTN ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind external FTN name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind external FTN hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind external FTN time");require_sqlite(database,sqlite3_step(raw),"record external FTN ledger");ledger.reset();
        connection.exec("PRAGMA user_version=28","publish external FTN schema");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify external FTN migration");
        run_bootstrap_boundary(hook,"before-ftn-external-migration-commit");connection.exec("COMMIT","commit external FTN migration");
    }catch(...){if(!sqlite3_get_autocommit(database))sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}

void migrate_ftn_recovery(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read FTN history recovery schema")>=29)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin FTN history recovery migration");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify FTN history recovery predecessor")!=28||!validate_sfx_shipping_database(database,true))fail_migration_history("require exact v28 before FTN history recovery upgrade");
        run_bootstrap_boundary(hook,"before-ftn-history-migration");const auto& migration=sfx_ftn_recovery_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply fixed FTN history recovery migration");}
        sqlite3_stmt* raw{};require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(29,?1,?2,?3)",-1,&raw,nullptr),"prepare FTN history recovery ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind FTN history recovery name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind FTN history recovery hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind FTN history recovery time");require_sqlite(database,sqlite3_step(raw),"record FTN history recovery ledger");ledger.reset();
        connection.exec("PRAGMA user_version=29","publish FTN history recovery schema");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify FTN history recovery migration");
        run_bootstrap_boundary(hook,"before-ftn-history-migration-commit");connection.exec("COMMIT","commit FTN history recovery migration");
    }catch(...){if(!sqlite3_get_autocommit(database))sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}

void migrate_door_bulletins(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read Door Bulletin schema")>=30)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin Door Bulletin migration");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify Door Bulletin predecessor")!=29||!validate_sfx_shipping_database(database,true))fail_migration_history("require exact v29 before Door Bulletin upgrade");
        run_bootstrap_boundary(hook,"before-door-bulletin-migration");const auto& migration=sfx_door_bulletin_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply fixed Door Bulletin migration");}
        sqlite3_stmt* raw{};require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(30,?1,?2,?3)",-1,&raw,nullptr),"prepare Door Bulletin ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind Door Bulletin name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind Door Bulletin hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind Door Bulletin time");require_sqlite(database,sqlite3_step(raw),"record Door Bulletin ledger");ledger.reset();
        connection.exec("PRAGMA user_version=30","publish Door Bulletin schema");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify Door Bulletin migration");
        run_bootstrap_boundary(hook,"before-door-bulletin-migration-commit");connection.exec("COMMIT","commit Door Bulletin migration");run_bootstrap_boundary(hook,"after-door-bulletin-migration-commit");
    }catch(...){if(!sqlite3_get_autocommit(database))sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}

void migrate_message_list_deletion(SqliteConnection& connection, const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook) {
    if (connection.scalar_int64("PRAGMA user_version","read message list deletion schema") >= 31) return;
    auto* database = connection.get();
    connection.exec("BEGIN IMMEDIATE","begin message list deletion migration");
    try {
        if (connection.scalar_int64("PRAGMA user_version","verify message list deletion predecessor") != 30 ||
            !validate_sfx_shipping_database(database,true))
            fail_migration_history("require exact v30 before message list deletion upgrade");
        run_bootstrap_boundary(hook,"before-message-list-deletion-migration");
        const auto& migration = sfx_message_list_delete_migration();
        { MigrationSqlGuard guard(database); connection.exec(migration.sql,"apply message list deletion migration"); }
        sqlite3_stmt* raw{};
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(31,?1,?2,?3)",
            -1,&raw,nullptr),"prepare message list deletion ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind migration time");
        require_sqlite(database,sqlite3_step(raw),"record message list deletion migration");
        ledger.reset();
        connection.exec("PRAGMA user_version=31","publish message list deletion schema");
        if (!validate_sfx_shipping_database(database,true)) fail_migration_history("verify message list deletion migration");
        run_bootstrap_boundary(hook,"before-message-list-deletion-migration-commit");
        connection.exec("COMMIT","commit message list deletion migration");
    } catch (...) {
        if (!sqlite3_get_autocommit(database)) sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);
        throw;
    }
    run_bootstrap_boundary(hook,"after-message-list-deletion-migration-commit");
}

void migrate_ftn_subject(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read FTN subject schema version")>=26)return;
    auto* database=connection.get();
    if(sqlite3_get_autocommit(database)!=1||connection.scalar_int64("PRAGMA foreign_keys","verify migration FK authority")!=1)
        fail_migration_history("require managed subject migration startup boundary");
    try{
        connection.exec("PRAGMA foreign_keys=OFF","enter one-table subject schema rebuild");
        if(connection.scalar_int64("PRAGMA foreign_keys","verify rebuild FK state")!=0)fail_migration_history("subject rebuild FK state");
        connection.exec("BEGIN IMMEDIATE","begin atomic FTN subject schema upgrade");
        if(connection.scalar_int64("PRAGMA user_version","verify FTN subject predecessor")!=25||!validate_sfx_shipping_database(database,true))fail_migration_history("require exact v25 before FTN subject upgrade");
        run_bootstrap_boundary(hook,"before-ftn-subject-migration");const auto& migration=sfx_ftn_subject_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply fixed FTN subject migration");}
        run_bootstrap_boundary(hook,"after-ftn-subject-rebuild");
        sqlite3_stmt* raw{};require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(26,?1,?2,?3)",-1,&raw,nullptr),"prepare FTN subject ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind FTN subject migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind FTN subject migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind FTN subject migration time");require_sqlite(database,sqlite3_step(raw),"record FTN subject ledger");ledger.reset();
        connection.exec("PRAGMA user_version=26","publish FTN subject schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact FTN subject schema upgrade");
        run_bootstrap_boundary(hook,"before-ftn-subject-migration-commit");connection.exec("COMMIT","commit FTN subject schema upgrade");
        connection.exec("PRAGMA foreign_keys=ON","restore FK authority after subject migration");
        if(connection.scalar_int64("PRAGMA foreign_keys","verify restored FK enforcement")!=1)fail_migration_history("restore subject migration FK authority");
    }catch(...){
        const auto rollback=sqlite3_get_autocommit(database)?SQLITE_OK:sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);
        const auto restore=sqlite3_exec(database,"PRAGMA foreign_keys=ON",nullptr,nullptr,nullptr);
        bool restored=false;try{restored=rollback==SQLITE_OK&&restore==SQLITE_OK&&sqlite3_get_autocommit(database)&&connection.scalar_int64("PRAGMA foreign_keys","verify failed-migration FK restoration")==1;}catch(...){}
        if(!restored)connection.close();
        throw;
    }
    run_bootstrap_boundary(hook,"after-ftn-subject-migration-commit");
}
void migrate_ftn_wire_body(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read FTN wire body schema version")>=25)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin FTN wire body schema upgrade");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify FTN wire body predecessor")!=24 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v24 before FTN wire body upgrade");
        run_bootstrap_boundary(hook,"before-ftn-wire-body-migration");const auto& migration=sfx_ftn_wire_body_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply additive FTN wire body migration");}
        sqlite3_stmt* raw=nullptr;
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(25,?1,?2,?3)",-1,&raw,nullptr),"prepare FTN wire body ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind FTN wire body migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind FTN wire body migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind FTN wire body migration time");require_sqlite(database,sqlite3_step(raw),"record FTN wire body ledger");ledger.reset();
        connection.exec("PRAGMA user_version=25","publish FTN wire body schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact FTN wire body schema upgrade");
        run_bootstrap_boundary(hook,"before-ftn-wire-body-migration-commit");connection.exec("COMMIT","commit FTN wire body schema upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-ftn-wire-body-migration-commit");
}
void migrate_ftn(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read FTN schema version")>=21)return;
    auto* database=connection.get();connection.exec("BEGIN IMMEDIATE","begin FTN schema upgrade");
    try{
        if(connection.scalar_int64("PRAGMA user_version","verify FTN predecessor")!=20 ||
            !validate_sfx_shipping_database(database,true))fail_migration_history("require exact v20 before FTN upgrade");
        run_bootstrap_boundary(hook,"before-ftn-migration");const auto& migration=sfx_ftn_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply additive FTN migration");}
        sqlite3_stmt* raw=nullptr;
        require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(21,?1,?2,?3)",-1,&raw,nullptr),"prepare FTN ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,&sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind FTN migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind FTN migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind FTN migration time");require_sqlite(database,sqlite3_step(raw),"record FTN ledger");ledger.reset();
        connection.exec("PRAGMA user_version=21","publish FTN schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact FTN schema upgrade");
        run_bootstrap_boundary(hook,"before-ftn-migration-commit");connection.exec("COMMIT","commit FTN schema upgrade");
    }catch(...){sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    run_bootstrap_boundary(hook,"after-ftn-migration-commit");
}
void migrate_ftn_mail(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read FTN mail schema version")>=22)return;
    auto* database=connection.get();
    if(sqlite3_get_autocommit(database)!=1||connection.scalar_int64("PRAGMA foreign_keys","verify migration FK authority")!=1)
        fail_migration_history("require managed FTN migration startup boundary");
    try{
        connection.exec("PRAGMA foreign_keys=OFF","enter one-table FTN schema rebuild");
        if(connection.scalar_int64("PRAGMA foreign_keys","verify rebuild FK state")!=0)fail_migration_history("FTN rebuild FK state");
        connection.exec("BEGIN IMMEDIATE","begin atomic FTN mail schema upgrade");
        if(connection.scalar_int64("PRAGMA user_version","verify FTN mail predecessor")!=21||!validate_sfx_shipping_database(database,true))fail_migration_history("require exact v21 before FTN mail upgrade");
        run_bootstrap_boundary(hook,"before-ftn-mail-migration");const auto& migration=sfx_ftn_mail_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply fixed FTN mail migration");}
        run_bootstrap_boundary(hook,"after-ftn-message-rebuild");
        sqlite3_stmt* raw{};require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(22,?1,?2,?3)",-1,&raw,nullptr),"prepare FTN mail ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind FTN mail migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind FTN mail migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind FTN mail migration time");require_sqlite(database,sqlite3_step(raw),"record FTN mail ledger");ledger.reset();
        connection.exec("PRAGMA user_version=22","publish FTN mail schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact FTN mail schema upgrade");
        run_bootstrap_boundary(hook,"before-ftn-mail-migration-commit");connection.exec("COMMIT","commit FTN mail schema upgrade");
        connection.exec("PRAGMA foreign_keys=ON","restore FK authority after FTN migration");
        if(connection.scalar_int64("PRAGMA foreign_keys","verify restored FK enforcement")!=1)fail_migration_history("restore FTN migration FK authority");
    }catch(...){
        const auto rollback=sqlite3_get_autocommit(database)?SQLITE_OK:sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);
        const auto restore=sqlite3_exec(database,"PRAGMA foreign_keys=ON",nullptr,nullptr,nullptr);
        bool restored=false;try{restored=rollback==SQLITE_OK&&restore==SQLITE_OK&&sqlite3_get_autocommit(database)&&connection.scalar_int64("PRAGMA foreign_keys","verify failed-migration FK restoration")==1;}catch(...){}
        if(!restored)connection.close();
        throw;
    }
    run_bootstrap_boundary(hook,"after-ftn-mail-migration-commit");
}
void migrate_optional_subject(SqliteConnection& connection,const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>& hook){
    if(connection.scalar_int64("PRAGMA user_version","read optional subject schema version")>=23)return;
    auto* database=connection.get();
    if(sqlite3_get_autocommit(database)!=1||connection.scalar_int64("PRAGMA foreign_keys","verify migration FK authority")!=1)
        fail_migration_history("require managed subject migration startup boundary");
    try{
        connection.exec("PRAGMA foreign_keys=OFF","enter one-table subject schema rebuild");
        if(connection.scalar_int64("PRAGMA foreign_keys","verify rebuild FK state")!=0)fail_migration_history("subject rebuild FK state");
        connection.exec("BEGIN IMMEDIATE","begin atomic optional subject schema upgrade");
        if(connection.scalar_int64("PRAGMA user_version","verify optional subject predecessor")!=22||!validate_sfx_shipping_database(database,true))fail_migration_history("require exact v22 before optional subject upgrade");
        run_bootstrap_boundary(hook,"before-optional-subject-migration");const auto& migration=sfx_optional_subject_migration();
        {MigrationSqlGuard guard(database);connection.exec(migration.sql,"apply fixed optional subject migration");}
        run_bootstrap_boundary(hook,"after-optional-subject-rebuild");
        sqlite3_stmt* raw{};require_sqlite(database,sqlite3_prepare_v2(database,"INSERT INTO sfx_schema_migrations VALUES(23,?1,?2,?3)",-1,&raw,nullptr),"prepare optional subject ledger");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> ledger(raw,sqlite3_finalize);
        require_sqlite(database,sqlite3_bind_text(raw,1,migration.name.c_str(),-1,SQLITE_TRANSIENT),"bind optional subject migration name");
        require_sqlite(database,sqlite3_bind_text(raw,2,migration.content_sha256.c_str(),-1,SQLITE_TRANSIENT),"bind optional subject migration hash");
        require_sqlite(database,sqlite3_bind_int64(raw,3,utc_now()),"bind optional subject migration time");require_sqlite(database,sqlite3_step(raw),"record optional subject ledger");ledger.reset();
        connection.exec("PRAGMA user_version=23","publish optional subject schema version");
        if(!validate_sfx_shipping_database(database,true))fail_migration_history("verify exact optional subject schema upgrade");
        run_bootstrap_boundary(hook,"before-optional-subject-migration-commit");connection.exec("COMMIT","commit optional subject schema upgrade");
        connection.exec("PRAGMA foreign_keys=ON","restore FK authority after subject migration");
        if(connection.scalar_int64("PRAGMA foreign_keys","verify restored FK enforcement")!=1)fail_migration_history("restore subject migration FK authority");
    }catch(...){
        const auto rollback=sqlite3_get_autocommit(database)?SQLITE_OK:sqlite3_exec(database,"ROLLBACK",nullptr,nullptr,nullptr);
        const auto restore=sqlite3_exec(database,"PRAGMA foreign_keys=ON",nullptr,nullptr,nullptr);
        bool restored=false;try{restored=rollback==SQLITE_OK&&restore==SQLITE_OK&&sqlite3_get_autocommit(database)&&connection.scalar_int64("PRAGMA foreign_keys","verify failed-migration FK restoration")==1;}catch(...){}
        if(!restored)connection.close();
        throw;
    }
    run_bootstrap_boundary(hook,"after-optional-subject-migration-commit");
}
}  // namespace

StorageFailure::StorageFailure(
    const application::StorageErrorCode code,
    std::string operation,
    const int sqlite_code,
    std::string detail)
    : std::runtime_error(operation + (detail.empty() ? std::string{} : ": " + detail)),
      code_(code),
      operation_(std::move(operation)),
      sqlite_code_(sqlite_code),
      detail_(std::move(detail)) {}

application::StorageErrorCode StorageFailure::code() const noexcept { return code_; }
const std::string& StorageFailure::operation() const noexcept { return operation_; }
int StorageFailure::sqlite_code() const noexcept { return sqlite_code_; }
const std::string& StorageFailure::detail() const noexcept { return detail_; }

application::StorageErrorCode map_sqlite_error(const int sqlite_code) noexcept {
    switch (sqlite_code & 0xff) {
        case SQLITE_MISMATCH:
            return application::StorageErrorCode::invalid_argument;
        case SQLITE_NOTFOUND:
            return application::StorageErrorCode::not_found;
        case SQLITE_ABORT:
            return application::StorageErrorCode::conflict;
        case SQLITE_NOTICE:
            return application::StorageErrorCode::not_due;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            return application::StorageErrorCode::busy;
        case SQLITE_CONSTRAINT:
            return application::StorageErrorCode::constraint;
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            return application::StorageErrorCode::corrupt;
        case SQLITE_FULL:
            return application::StorageErrorCode::full;
        case SQLITE_PERM:
        case SQLITE_AUTH:
        case SQLITE_READONLY:
            return application::StorageErrorCode::permission_denied;
        case SQLITE_CANTOPEN:
        case SQLITE_IOERR:
            return application::StorageErrorCode::io;
        default:
            return application::StorageErrorCode::internal;
    }
}

application::StorageStatus status_from_exception(const StorageFailure& failure) {
    return {failure.code(), failure.operation()};
}

application::StorageStatus status_from_current_exception(const std::string& operation) noexcept {
    try {
        throw;
    } catch (const StorageFailure& failure) {
        return status_from_exception(failure);
    } catch (...) {
        return {application::StorageErrorCode::internal, operation};
    }
}

SqliteConnection::SqliteConnection(
    const std::filesystem::path& path,
    const int open_flags,
    const std::chrono::milliseconds busy_timeout,
    const bool writer_connection)
    :SqliteConnection(path,open_flags,busy_timeout,writer_connection,nullptr){}

SqliteConnection::SqliteConnection(const std::filesystem::path& path,const int open_flags,
    const std::chrono::milliseconds busy_timeout,const bool writer_connection,const char* private_vfs)
    : path_(path), owner_thread_(std::this_thread::get_id()) {
    if (sqlite3_libversion_number() != required_sqlite_version ||
        std::string(sqlite3_sourceid()) != required_sqlite_source_id) {
        throw StorageFailure(
            application::StorageErrorCode::incompatible_schema,
            "verify SQLite dependency",
            SQLITE_MISMATCH,
            "runtime SQLite version/source ID differs from the approved pin");
    }

    const std::string filename = path_as_utf8(path_);
    const int code = sqlite3_open_v2(
        filename.c_str(),
        &database_,
        open_flags | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW,
        private_vfs);
    if (code != SQLITE_OK) {
        const char* const message = database_ != nullptr ? sqlite3_errmsg(database_) : sqlite3_errstr(code);
        const std::string detail = message != nullptr ? message : "open failed";
        if (database_ != nullptr) {
            sqlite3_close_v2(database_);
            database_ = nullptr;
        }
        throw StorageFailure(map_sqlite_error(code), "open SQLite database", code, detail);
    }

    try {
        configure(busy_timeout, writer_connection);
        require_sqlite(database_,sqlite3_create_function_v2(database_,"sfx_secure_lineage",0,
            SQLITE_UTF8|SQLITE_DIRECTONLY,nullptr,secure_lineage_sql,nullptr,nullptr,nullptr),"register secure FTN history identity generator");
        require_sqlite(database_,sqlite3_create_function_v2(database_,"sfx_file_fold",1,
            SQLITE_UTF8|SQLITE_DETERMINISTIC|SQLITE_DIRECTONLY,nullptr,file_fold_sql,nullptr,nullptr,nullptr),"register bounded pinned Unicode file fold");
    } catch (...) {
        sqlite3_close_v2(database_);
        database_ = nullptr;
        throw;
    }
}

SqliteConnection::~SqliteConnection() {
    if (database_ != nullptr) {
        sqlite3_close_v2(database_);
    }
}

sqlite3* SqliteConnection::get() {
    require_owner_thread();
    return database_;
}

const std::filesystem::path& SqliteConnection::path() const noexcept { return path_; }

void SqliteConnection::exec(const std::string& sql, const std::string& operation) {
    require_owner_thread();
    char* error = nullptr;
    const int code = sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, &error);
    if (code != SQLITE_OK) {
        const std::string detail = error != nullptr ? error : sqlite3_errmsg(database_);
        sqlite3_free(error);
        throw StorageFailure(map_sqlite_error(code), operation, code, detail);
    }
}

std::int64_t SqliteConnection::scalar_int64(const std::string& sql, const std::string& operation) {
    require_owner_thread();
    sqlite3_stmt* statement = nullptr;
    require_sqlite(
        database_,
        sqlite3_prepare_v3(database_, sql.c_str(), -1, SQLITE_PREPARE_PERSISTENT, &statement, nullptr),
        operation + " prepare");
    const int step_code = sqlite3_step(statement);
    if (step_code != SQLITE_ROW) {
        const int result_code = step_code == SQLITE_DONE ? SQLITE_NOTFOUND : step_code;
        sqlite3_finalize(statement);
        require_sqlite(database_, result_code, operation + " step");
    }
    const std::int64_t value = sqlite3_column_int64(statement, 0);
    require_sqlite(database_, sqlite3_finalize(statement), operation + " finalize");
    return value;
}

std::string SqliteConnection::scalar_text(const std::string& sql, const std::string& operation) {
    require_owner_thread();
    sqlite3_stmt* statement = nullptr;
    require_sqlite(
        database_,
        sqlite3_prepare_v3(database_, sql.c_str(), -1, SQLITE_PREPARE_PERSISTENT, &statement, nullptr),
        operation + " prepare");
    const int step_code = sqlite3_step(statement);
    if (step_code != SQLITE_ROW) {
        const int result_code = step_code == SQLITE_DONE ? SQLITE_NOTFOUND : step_code;
        sqlite3_finalize(statement);
        require_sqlite(database_, result_code, operation + " step");
    }
    const unsigned char* const text = sqlite3_column_text(statement, 0);
    const std::string value = text != nullptr ? reinterpret_cast<const char*>(text) : std::string{};
    require_sqlite(database_, sqlite3_finalize(statement), operation + " finalize");
    return value;
}

void SqliteConnection::close() {
    require_owner_thread();
    if (database_ == nullptr) {
        return;
    }
    const int code = sqlite3_close(database_);
    if (code != SQLITE_OK) {
        throw StorageFailure(map_sqlite_error(code), "close SQLite database", code, sqlite3_errmsg(database_));
    }
    database_ = nullptr;
}

void SqliteConnection::require_owner_thread() const {
    if (owner_thread_ != std::this_thread::get_id()) {
        throw StorageFailure(
            application::StorageErrorCode::internal,
            "enforce SQLite connection ownership",
            SQLITE_MISUSE,
            "connection used by a non-owning thread");
    }
}

void SqliteConnection::configure(
    const std::chrono::milliseconds busy_timeout,
    const bool writer_connection) {
    require_owner_thread();
    require_sqlite(database_, sqlite3_extended_result_codes(database_, 1), "enable extended SQLite errors");
    const auto timeout_count = std::min<std::int64_t>(
        busy_timeout.count(),
        static_cast<std::int64_t>(std::numeric_limits<int>::max()));
    require_sqlite(
        database_,
        sqlite3_busy_timeout(database_, static_cast<int>(std::max<std::int64_t>(0, timeout_count))),
        "set SQLite busy timeout");

    int enabled = 0;
    require_sqlite(database_, sqlite3_db_config(database_, SQLITE_DBCONFIG_DEFENSIVE, 1, &enabled), "enable defensive mode");
    require_sqlite(database_, sqlite3_db_config(database_, SQLITE_DBCONFIG_ENABLE_FKEY, 1, &enabled), "enable foreign keys");
    require_sqlite(database_, sqlite3_db_config(database_, SQLITE_DBCONFIG_DQS_DDL, 0, &enabled), "disable DDL double-quoted strings");
    require_sqlite(database_, sqlite3_db_config(database_, SQLITE_DBCONFIG_DQS_DML, 0, &enabled), "disable DML double-quoted strings");
    require_sqlite(database_, sqlite3_db_config(database_, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, &enabled), "disable trusted schema");

    exec(
        "PRAGMA foreign_keys=ON;"
        "PRAGMA trusted_schema=OFF;"
        "PRAGMA recursive_triggers=OFF;"
        "PRAGMA secure_delete=ON;"
        "PRAGMA read_uncommitted=OFF;"
        "PRAGMA mmap_size=0;",
        "apply SQLite connection hardening");

    if (writer_connection) {
        exec("PRAGMA page_size=4096;", "set SQLite page size");
        if (scalar_int64("PRAGMA page_size", "verify SQLite page size") != 4096) {
            throw StorageFailure(
                application::StorageErrorCode::incompatible_schema,
                "verify SQLite page size",
                SQLITE_FORMAT,
                "Spitfire-X BBS requires 4096-byte database pages");
        }
        const std::string journal_mode = scalar_text("PRAGMA journal_mode=WAL", "enable SQLite WAL");
        if (journal_mode != "wal") {
            throw StorageFailure(
                application::StorageErrorCode::io,
                "enable SQLite WAL",
                SQLITE_IOERR,
                "WAL mode was not accepted");
        }
        exec(
            "PRAGMA synchronous=FULL;"
            "PRAGMA wal_autocheckpoint=0;"
            "PRAGMA locking_mode=NORMAL;"
            "PRAGMA journal_size_limit=67108864;",
            "apply SQLite writer durability");
        if (scalar_int64("PRAGMA synchronous", "verify SQLite synchronization") != 2 ||
            scalar_int64("PRAGMA wal_autocheckpoint", "verify SQLite autocheckpoint") != 0 ||
            scalar_text("PRAGMA locking_mode", "verify SQLite locking mode") != "normal" ||
            scalar_int64("PRAGMA journal_size_limit", "verify SQLite journal limit") != 67'108'864) {
            throw StorageFailure(
                application::StorageErrorCode::internal,
                "verify SQLite writer durability",
                SQLITE_MISUSE,
                "required writer settings were not retained");
        }
    }

    if (scalar_int64("PRAGMA foreign_keys", "verify foreign keys") != 1 ||
        scalar_int64("PRAGMA trusted_schema", "verify trusted schema") != 0 ||
        scalar_int64("PRAGMA mmap_size", "verify memory mapping") != 0) {
        throw StorageFailure(
            application::StorageErrorCode::internal,
            "verify SQLite connection hardening",
            SQLITE_MISUSE,
            "required connection settings were not retained");
    }
}

void bootstrap_and_migrate(
    SqliteConnection& connection,
    const std::vector<SqliteMigration>& migrations,
    const std::function<std::int64_t()>& utc_now,
    const std::function<application::StorageStatus(std::string_view)>&
        bootstrap_boundary_hook,
    const SqliteStorageLayoutProfile layout_profile) {
    if(layout_profile!=SqliteStorageLayoutProfile::canonical &&
       layout_profile!=SqliteStorageLayoutProfile::linux_lowercase_v1)
        throw StorageFailure(application::StorageErrorCode::invalid_argument,
            "validate SQLite native layout profile",SQLITE_MISUSE);
    if (!has_exact_sfx_plan(migrations) || !utc_now) {
        throw StorageFailure(
            application::StorageErrorCode::migration_failed,
            "validate SFX baseline plan",
            SQLITE_MISUSE,
            "production requires the exact SFX shipping migration lineage and a clock");
    }
    sqlite3* const database = connection.get();
    const std::int64_t application_id =
        connection.scalar_int64("PRAGMA application_id", "read SFX application ID");
    const std::int64_t user_version =
        connection.scalar_int64("PRAGMA user_version", "read SFX schema version");
    const std::int64_t object_count = connection.scalar_int64(
        "SELECT count(*) FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*'",
        "inspect SFX bootstrap state");
    if (application_id != 0 || user_version != 0 || object_count != 0) {
        (void)verify_migration_history(connection, migrations);
        migrate_caller_journals(connection, utc_now, bootstrap_boundary_hook);
        migrate_menu_preferences(connection, utc_now, bootstrap_boundary_hook);
        if(migrations.size()>=4) migrate_caller_utilities(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=5) migrate_classic_information(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=6) migrate_classic_delete(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=7) migrate_other_board_text(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=8) migrate_local_messages(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=9) migrate_message_admin(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=10) migrate_message_group(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=11) migrate_message_delete(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=12) migrate_message_source(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=13) migrate_message_maintenance(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=14) migrate_qwk(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=15) migrate_files(connection,utc_now,bootstrap_boundary_hook,
            layout_profile==SqliteStorageLayoutProfile::linux_lowercase_v1);
        if(migrations.size()>=16) migrate_doors(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=17) migrate_door_security(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=18) migrate_door_program(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=19) migrate_chat(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=20) migrate_private_chat(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=21) migrate_ftn(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=22) migrate_ftn_mail(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=23) migrate_optional_subject(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=24) migrate_file_group_deletion(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=25) migrate_ftn_wire_body(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=26) migrate_ftn_subject(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=27) migrate_message_location(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=28) migrate_ftn_external(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=29) migrate_ftn_recovery(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=30) migrate_door_bulletins(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=31) migrate_message_list_deletion(connection,utc_now,bootstrap_boundary_hook);
        return;
    }

    connection.exec("BEGIN IMMEDIATE", "begin SFX initial migration");
    try {
        run_bootstrap_boundary(bootstrap_boundary_hook, "after-baseline-transaction-begin");
        {
            MigrationSqlGuard transaction_guard(database);
            connection.exec(migrations.front().sql, "apply SFX initial migration");
        }
        run_bootstrap_boundary(bootstrap_boundary_hook, "after-baseline-schema");

        sqlite3_stmt* insert = nullptr;
        require_sqlite(database, sqlite3_prepare_v2(
            database,
            "INSERT INTO sfx_schema_migrations(version,name,content_sha256,applied_utc) "
            "VALUES(?1,?2,?3,?4)",
            -1, &insert, nullptr), "prepare SFX baseline ledger record");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> insert_guard(
            insert, &sqlite3_finalize);
        require_sqlite(database, sqlite3_bind_int(insert, 1, 1), "bind SFX migration version");
        require_sqlite(database, sqlite3_bind_text(insert, 2,
            migrations.front().name.c_str(), -1, SQLITE_TRANSIENT), "bind SFX migration name");
        require_sqlite(database, sqlite3_bind_text(insert, 3,
            migrations.front().content_sha256.c_str(), -1, SQLITE_TRANSIENT), "bind SFX migration hash");
        require_sqlite(database, sqlite3_bind_int64(insert, 4, utc_now()), "bind SFX migration time");
        require_sqlite(database, sqlite3_step(insert), "insert SFX baseline ledger record");
        insert_guard.release();
        require_sqlite(database, sqlite3_finalize(insert), "finalize SFX baseline ledger record");

        connection.exec("PRAGMA application_id=1397119042", "publish SFX application ID");
        connection.exec("PRAGMA user_version=1", "publish SFX schema version");
        if (!validate_sfx_baseline_database(database, false)) {
            throw StorageFailure(
                application::StorageErrorCode::migration_failed,
                "validate SFX baseline before commit",
                SQLITE_SCHEMA,
                "the transaction does not match the frozen SFX schema and ledger");
        }
        run_bootstrap_boundary(bootstrap_boundary_hook, "before-baseline-commit");
        connection.exec("COMMIT", "commit SFX initial migration");
    } catch (...) {
        sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    run_bootstrap_boundary(bootstrap_boundary_hook, "after-baseline-commit");
    (void)verify_migration_history(connection, migrations);
    migrate_caller_journals(connection, utc_now, bootstrap_boundary_hook);
    migrate_menu_preferences(connection, utc_now, bootstrap_boundary_hook);
    if(migrations.size()>=4) migrate_caller_utilities(connection,utc_now,bootstrap_boundary_hook);
    if(migrations.size()>=5) migrate_classic_information(connection,utc_now,bootstrap_boundary_hook);
    if(migrations.size()>=6) migrate_classic_delete(connection,utc_now,bootstrap_boundary_hook);
    if(migrations.size()>=7) migrate_other_board_text(connection,utc_now,bootstrap_boundary_hook);
    if(migrations.size()>=8) migrate_local_messages(connection,utc_now,bootstrap_boundary_hook);
    if(migrations.size()>=9) migrate_message_admin(connection,utc_now,bootstrap_boundary_hook);
    if(migrations.size()>=10) migrate_message_group(connection,utc_now,bootstrap_boundary_hook);
    if(migrations.size()>=11) migrate_message_delete(connection,utc_now,bootstrap_boundary_hook);
    if(migrations.size()>=12) migrate_message_source(connection,utc_now,bootstrap_boundary_hook);
    if(migrations.size()>=13) migrate_message_maintenance(connection,utc_now,bootstrap_boundary_hook);
    if(migrations.size()>=14) migrate_qwk(connection,utc_now,bootstrap_boundary_hook);
    if(migrations.size()>=15) migrate_files(connection,utc_now,bootstrap_boundary_hook,
        layout_profile==SqliteStorageLayoutProfile::linux_lowercase_v1);
    if(migrations.size()>=16) migrate_doors(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=17) migrate_door_security(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=18) migrate_door_program(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=19) migrate_chat(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=20) migrate_private_chat(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=21) migrate_ftn(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=22) migrate_ftn_mail(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=23) migrate_optional_subject(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=24) migrate_file_group_deletion(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=25) migrate_ftn_wire_body(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=26) migrate_ftn_subject(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=27) migrate_message_location(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=28) migrate_ftn_external(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=29) migrate_ftn_recovery(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=30) migrate_door_bulletins(connection,utc_now,bootstrap_boundary_hook);
        if(migrations.size()>=31) migrate_message_list_deletion(connection,utc_now,bootstrap_boundary_hook);
}

application::StorageStatus verify_database_file(
    const std::filesystem::path& database_file,
    const std::chrono::milliseconds busy_timeout,
    const std::vector<SqliteMigration>& approved_migrations,
    std::int32_t* const schema_version) {
    try {
        SqliteConnection connection(database_file, SQLITE_OPEN_READONLY, busy_timeout, false);
        if (connection.scalar_text("PRAGMA integrity_check", "verify full backup integrity") != "ok") {
            return {application::StorageErrorCode::corrupt, "verify full backup integrity"};
        }
        if (connection.scalar_int64(
                "SELECT count(*) FROM pragma_foreign_key_check",
                "verify complete backup foreign-key consistency") != 0) {
            return {application::StorageErrorCode::corrupt, "verify complete backup foreign-key consistency"};
        }
        const std::int32_t version = verify_migration_history(connection, approved_migrations);
        if (schema_version != nullptr) {
            *schema_version = version;
        }
        return application::StorageStatus::success();
    } catch (...) {
        return status_from_current_exception("verify database file");
    }
}
InternalBackupResult backup_database(
    sqlite3* const source,
    const std::filesystem::path& destination_file,
    const std::chrono::milliseconds busy_timeout,
    const std::vector<SqliteMigration>& approved_migrations) {
    InternalBackupResult result;
    try {
        std::error_code file_error;
        if (std::filesystem::exists(destination_file, file_error) || file_error) {
            result.status = {application::StorageErrorCode::io, "create backup destination"};
            return result;
        }
        SqliteConnection destination(
            destination_file,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
            busy_timeout,
            false);
        sqlite3_backup* const backup = sqlite3_backup_init(destination.get(), "main", source, "main");
        if (backup == nullptr) {
            throw StorageFailure(
                map_sqlite_error(sqlite3_errcode(destination.get())),
                "initialize SQLite backup",
                sqlite3_extended_errcode(destination.get()),
                sqlite3_errmsg(destination.get()));
        }

        int step_code = SQLITE_OK;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            step_code = sqlite3_backup_step(backup, 128);
            if (step_code == SQLITE_BUSY || step_code == SQLITE_LOCKED) {
                sqlite3_sleep(10);
            }
        } while ((step_code == SQLITE_OK || step_code == SQLITE_BUSY || step_code == SQLITE_LOCKED) &&
                 std::chrono::steady_clock::now() < deadline);
        const int finish_code = sqlite3_backup_finish(backup);
        if (step_code != SQLITE_DONE) {
            require_sqlite(destination.get(), step_code, "copy SQLite backup");
        }
        require_sqlite(destination.get(), finish_code, "finish SQLite backup");
        // The standalone backup is one file, not a live WAL database. Convert
        // only this completed destination before closing it, so read-only
        // validation cannot leave orphan WAL/SHM files in the backup work area.
        if (destination.scalar_text("PRAGMA journal_mode=DELETE", "finish standalone backup journal") != "delete") {
            throw StorageFailure(application::StorageErrorCode::io,
                "finish standalone backup journal", SQLITE_ERROR);
        }
        destination.close();

        result.status = verify_database_file(destination_file, busy_timeout, approved_migrations, &result.schema_version);
        if (!result.status) {
            std::filesystem::remove(destination_file, file_error);
            return result;
        }
        result.bytes = std::filesystem::file_size(destination_file, file_error);
        if (file_error) {
            result.status = {application::StorageErrorCode::io, "measure backup file"};
        }
        return result;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(destination_file, ignored);
        result.status = status_from_current_exception("create SQLite backup");
        return result;
    }
}

}  // namespace sfx::adapters::storage::internal
