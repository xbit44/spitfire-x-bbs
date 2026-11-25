#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sqlite_sfx_baseline.hpp"
#include "sqlite3.h"
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace storage = sfx::adapters::storage;
namespace internal = sfx::adapters::storage::internal;
std::size_t assertions = 0;

void require(bool value, const std::string& message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}

class Database final {
public:
    explicit Database(const std::filesystem::path& path) : path_(path) {
        if (sqlite3_open_v2(path.string().c_str(), &db_,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            throw std::runtime_error("open SQLite database");
        }
        exec("PRAGMA foreign_keys=ON;PRAGMA legacy_alter_table=OFF");
    }
    ~Database() { if (db_ != nullptr) sqlite3_close_v2(db_); }
    sqlite3* get() const noexcept { return db_; }
    const std::filesystem::path& path() const noexcept { return path_; }
    void exec(const std::string& sql) const {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string detail = error == nullptr ? sqlite3_errmsg(db_) : error;
            sqlite3_free(error);
            throw std::runtime_error(detail);
        }
    }
private:
    std::filesystem::path path_;
    sqlite3* db_{nullptr};
};

std::int64_t number(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW) {
        if (statement != nullptr) sqlite3_finalize(statement);
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    const auto value = sqlite3_column_int64(statement, 0);
    const int next = sqlite3_step(statement);
    const int done = sqlite3_finalize(statement);
    if (next != SQLITE_DONE || done != SQLITE_OK) throw std::runtime_error("scalar result");
    return value;
}

std::string text(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW || sqlite3_column_text(statement, 0) == nullptr) {
        if (statement != nullptr) sqlite3_finalize(statement);
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    const std::string value = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    const int next = sqlite3_step(statement);
    const int done = sqlite3_finalize(statement);
    if (next != SQLITE_DONE || done != SQLITE_OK) throw std::runtime_error("text result");
    return value;
}

bool fails(const Database& db, const std::string& sql) {
    try { db.exec(sql); return false; } catch (const std::runtime_error&) { return true; }
}

std::string hash(const std::string& value) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(),
        reinterpret_cast<const unsigned char*>(value.data()),
        static_cast<unsigned long long>(value.size()));
    const char* digits = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : digest) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 15U]);
    }
    return result;
}

void ledger(sqlite3* db, const std::string& table,
            const storage::SqliteMigration& migration, std::int64_t applied) {
    const std::string sql = "INSERT INTO " + table + " VALUES(?1,?2,?3,?4)";
    sqlite3_stmt* statement = nullptr;
    const bool prepared = sqlite3_prepare_v2(
        db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK;
    const bool bound = prepared &&
        sqlite3_bind_int(statement, 1, migration.version) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, migration.name.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 3, migration.content_sha256.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int64(statement, 4, applied) == SQLITE_OK;
    const bool stepped = bound && sqlite3_step(statement) == SQLITE_DONE;
    const bool finalized = statement == nullptr || sqlite3_finalize(statement) == SQLITE_OK;
    statement = nullptr;
    if (!prepared || !bound || !stepped || !finalized) {
        throw std::runtime_error(sqlite3_errmsg(db));
    }
}

void create_active(const Database& db) {
    const auto& migration = internal::sfx_initial_baseline_migration();
    db.exec("BEGIN IMMEDIATE");
    try {
        db.exec(migration.sql);
        ledger(db.get(), "sfx_schema_migrations", migration, 2001);
        db.exec("PRAGMA application_id=1397119042;PRAGMA user_version=1;COMMIT");
    } catch (...) { try { db.exec("ROLLBACK"); } catch (...) {} throw; }
}

internal::SfxBaselineSchemaObjectKind kind(const std::string& value) {
    if (value == "table") return internal::SfxBaselineSchemaObjectKind::table;
    if (value == "index") return internal::SfxBaselineSchemaObjectKind::index;
    if (value == "trigger") return internal::SfxBaselineSchemaObjectKind::trigger;
    if (value == "view") return internal::SfxBaselineSchemaObjectKind::view;
    throw std::runtime_error("schema kind");
}

std::vector<internal::SfxBaselineSchemaObject> manifest(sqlite3* db) {
    sqlite3_stmt* statement = nullptr;
    const char* sql = "SELECT type,name,sql FROM sqlite_schema WHERE sql IS NOT NULL "
        "AND name NOT GLOB 'sqlite_*' ORDER BY type,name";
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
    std::vector<internal::SfxBaselineSchemaObject> result;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        const std::string type = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        const std::string name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
        const std::string sql_text = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
        result.push_back({kind(type), name, hash(sql_text)});
    }
    const int done = sqlite3_finalize(statement);
    if (step != SQLITE_DONE || done != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    return result;
}

std::map<std::pair<std::string,std::string>,std::string> definitions(sqlite3* db) {
    sqlite3_stmt* statement = nullptr;
    const char* sql = "SELECT type,name,sql FROM sqlite_schema WHERE sql IS NOT NULL "
        "AND name NOT GLOB 'sqlite_*' ORDER BY type,name";
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
    std::map<std::pair<std::string,std::string>,std::string> result;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        result.emplace(std::make_pair(
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 1))),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)));
    }
    sqlite3_finalize(statement);
    if (step != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    return result;
}

std::string normalized_schema_sql(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    bool single_quoted_literal = false;
    bool quoted_identifier = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (single_quoted_literal) {
            result.push_back(static_cast<char>(character));
            if (character == '\'' && index + 1U < value.size() && value[index + 1U] == '\'') {
                result.push_back(value[++index]);
            } else if (character == '\'') {
                single_quoted_literal = false;
            }
            continue;
        }
        if (quoted_identifier) {
            if (character == '"' && index + 1U < value.size() && value[index + 1U] == '"') {
                result.push_back('"');
                ++index;
            } else if (character == '"') {
                quoted_identifier = false;
            } else {
                result.push_back(static_cast<char>(std::tolower(character)));
            }
            continue;
        }
        if (character == '\'') {
            single_quoted_literal = true;
            result.push_back('\'');
        } else if (character == '"') {
            quoted_identifier = true;
        } else if (std::isspace(character) == 0) {
            result.push_back(static_cast<char>(std::tolower(character)));
        }
    }
    if (single_quoted_literal || quoted_identifier) {
        throw std::runtime_error("unterminated SQL token in semantic normalizer");
    }
    return result;
}

std::vector<std::string> rows(sqlite3* db, const std::string& sql,
                              const std::string& parameter = {}) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK ||
        (!parameter.empty() && sqlite3_bind_text(statement, 1, parameter.c_str(), -1,
            SQLITE_TRANSIENT) != SQLITE_OK)) {
        if (statement != nullptr) sqlite3_finalize(statement);
        throw std::runtime_error(sqlite3_errmsg(db));
    }
    std::vector<std::string> result;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        std::ostringstream row;
        for (int column = 0; column < sqlite3_column_count(statement); ++column) {
            if (column != 0) row << '|';
            if (sqlite3_column_type(statement, column) == SQLITE_NULL) row << "NULL";
            else row << reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
        }
        result.push_back(row.str());
    }
    sqlite3_finalize(statement);
    if (step != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    return result;
}

void verify_identity_manifest(const Database& proposed) {
    const auto& migration = internal::sfx_initial_baseline_migration();
    require(migration.version == 1 && migration.name == internal::sfx_baseline_migration_name,
        "clean baseline name/version are exact");
    require(migration.sql.size() == 25849 && hash(migration.sql) ==
        "c9ed2791ec70d18cb69696f837d27cc74f931d7c3253247768b7245bf0864806",
        "clean baseline bytes and SHA-256 reproduce");
    const std::string retired_prefix{'s', 'f', 'n', 'g', '_'};
    require(migration.sql.find(retired_prefix) == std::string::npos,
        "clean SQL contains no legacy object identity");
    require(number(proposed.get(), "PRAGMA application_id") == 1397119042 &&
            number(proposed.get(), "PRAGMA user_version") == 1,
        "clean database publishes SFXB/version 1");
    require(text(proposed.get(), "PRAGMA integrity_check") == "ok" &&
            number(proposed.get(), "SELECT count(*) FROM pragma_foreign_key_check") == 0,
        "clean database passes integrity and foreign-key checks");
    require(internal::validate_sfx_baseline_database(proposed.get(), true),
        "production validator accepts only the exact frozen SFX baseline");
    const auto actual = manifest(proposed.get());
    const auto expected = internal::sfx_baseline_schema_manifest();
    require(actual.size() == 47 && actual.size() == expected.size(),
        "independent manifest contains all 47 objects");
    for (std::size_t index = 0; index < actual.size(); ++index) {
        require(actual[index].kind == expected[index].kind && actual[index].name ==
            expected[index].name && actual[index].definition_sha256 ==
            expected[index].definition_sha256, "stored-definition hash matches manifest");
    }
}

void verify_schema_semantics(sqlite3* proposed) {
    require(normalized_schema_sql("CHECK(state='Active')") !=
            normalized_schema_sql("CHECK(state='active')"),
        "semantic normalizer preserves case-sensitive quoted literals");
    require(normalized_schema_sql("CREATE TABLE \"SFX_ACCOUNTS\" ( id INTEGER )") ==
            normalized_schema_sql("create table sfx_accounts(id integer)"),
        "semantic normalizer ignores formatting and quoted-identifier spelling");
    const auto new_defs = definitions(proposed);
    require(new_defs.size() == 47, "active clean schema has exactly 47 stored definitions");
    const auto tables = rows(proposed, "SELECT name FROM sqlite_schema WHERE type='table' "
        "AND name NOT GLOB 'sqlite_*' ORDER BY name");
    std::int64_t columns = 0;
    std::int64_t foreign_keys = 0;
    for (const std::string& table : tables) {
        const std::string column_sql = "SELECT cid,name,type,\"notnull\",dflt_value,pk,hidden "
            "FROM pragma_table_xinfo(?1) ORDER BY cid";
        const std::string foreign_sql = "SELECT id,seq,\"table\",\"from\",\"to\","
            "on_update,on_delete,match FROM pragma_foreign_key_list(?1) ORDER BY id,seq";
        const auto new_columns = rows(proposed, column_sql, table);
        const auto new_foreign = rows(proposed, foreign_sql, table);
        columns += new_columns.size();
        foreign_keys += new_foreign.size();
    }
    require(tables.size() == 22 && columns == 156 && foreign_keys == 22,
        "table/column/foreign-key totals match");
    require(number(proposed, "SELECT count(*) FROM sqlite_schema WHERE type='index' "
        "AND name GLOB 'sfx_*'") == 18 && number(proposed,
        "SELECT count(*) FROM sqlite_schema WHERE type='trigger' AND name GLOB 'sfx_*'") == 7 &&
        number(proposed, "SELECT count(*) FROM sqlite_schema WHERE type='view'") == 0,
        "index/trigger/view totals match");
    require(number(proposed, "SELECT count(*) FROM sfx_security_levels") == 2 &&
        number(proposed, "SELECT count(*) FROM sfx_security_policy") == 1 &&
        number(proposed, "SELECT count(*) FROM sfx_account_retention_policy") == 1 &&
        number(proposed, "SELECT count(*) FROM sfx_installation_state") == 1,
        "locked singleton and security seed cardinalities are exact");
}

void verify_behavior(const Database& db) {
    db.exec("INSERT INTO sfx_accounts(display_name,name_key,security_level,account_state,"
        "revision,purge_allowed,created_utc_seconds,modified_utc_seconds) "
        "VALUES('First Caller','first caller',10,'active',1,0,100,100)");
    require(number(db.get(), "SELECT account_id FROM sfx_accounts") == 1,
        "AUTOINCREMENT begins at AccountId 1");
    require(number(db.get(), "SELECT count(*) FROM sfx_account_transfer_preferences "
        "WHERE account_id=1 AND preference_code='select_at_transfer'") == 1,
        "default transfer preference trigger fires");
    db.exec("DELETE FROM sfx_account_transfer_preferences;DELETE FROM sfx_accounts");
    db.exec("INSERT INTO sfx_accounts(display_name,name_key,security_level,account_state,"
        "revision,purge_allowed,created_utc_seconds,modified_utc_seconds) "
        "VALUES('Second Caller','second caller',10,'active',1,0,101,101)");
    require(number(db.get(), "SELECT account_id FROM sfx_accounts") == 2,
        "deleted AccountId is not reused");
    require(fails(db, "UPDATE sfx_account_transfer_preferences SET preference_code='bad'"),
        "invalid transfer code is rejected");
    db.exec("UPDATE sfx_account_transfer_preferences SET preference_code='zmodem',"
        "revision=2,modified_utc_seconds=102 WHERE account_id=2");
    require(number(db.get(), "SELECT count(*) FROM sfx_account_transfer_preferences "
        "WHERE account_id=2 AND preference_code='zmodem' AND revision=2") == 1,
        "specific transfer preference persists");
    require(fails(db, "INSERT INTO sfx_account_profiles(account_id) VALUES(999)"),
        "foreign-key enforcement rejects unknown account");
    db.exec("INSERT INTO sfx_account_name_reservations(name_key,account_id,is_current,"
        "display_name,reserved_utc_seconds) VALUES('second caller',2,1,'Second Caller',101);"
        "INSERT INTO sfx_account_profiles(account_id) VALUES(2);"
        "INSERT INTO sfx_account_credentials(account_id,envelope_format_version,"
        "scheme_identifier,parameter_encoding_version,canonical_parameters,salt,verifier,"
        "created_utc_seconds,password_change_required) "
        "VALUES(2,1,'argon2id13',1,x'01',x'02',x'03',101,0);"
        "INSERT INTO sfx_account_statistics(account_id) VALUES(2)");
    db.exec("INSERT INTO sfx_authentication_rate_state(scope_code,subject_hash,failure_count,"
        "window_started_utc_seconds,last_failure_utc_seconds,blocked_until_utc_seconds,"
        "expires_utc_seconds) VALUES('account',zeroblob(32),1,110,110,111,120)");
    require(fails(db, "INSERT INTO sfx_authentication_rate_state(scope_code,subject_hash,"
        "failure_count,window_started_utc_seconds,last_failure_utc_seconds,"
        "blocked_until_utc_seconds,expires_utc_seconds) "
        "VALUES('source',x'01',1,110,110,111,120)"), "rate-state bounds are enforced");

    db.exec("INSERT INTO sfx_account_sessions(session_id,account_id,node_id,"
        "started_utc_seconds,last_persisted_utc_seconds,correlation_id) VALUES("
        "x'01010101010101010101010101010101',2,1,200,200,"
        "x'02020202020202020202020202020202');"
        "INSERT INTO sfx_account_sessions(session_id,account_id,node_id,"
        "started_utc_seconds,last_persisted_utc_seconds,correlation_id) VALUES("
        "x'03030303030303030303030303030303',2,2,200,200,"
        "x'04040404040404040404040404040404')");
    for (int index = 1; index <= 6; ++index) {
        db.exec("INSERT INTO sfx_failed_login_notices(notice_id,account_id,"
            "source_address_family,source_address,event_utc_seconds) VALUES("
            "CAST(printf('%016x'," + std::to_string(index) +
            ") AS BLOB),2,4,x'7f000001'," + std::to_string(200 + index) + ")");
    }
    require(number(db.get(), "SELECT count(*) FROM sfx_failed_login_notices "
        "WHERE account_id=2 AND claim_id IS NULL") == 5 &&
        number(db.get(), "SELECT min(event_utc_seconds) FROM sfx_failed_login_notices") == 202,
        "notice overflow retains deterministic newest five");
    db.exec("INSERT INTO sfx_failed_login_notice_claims(claim_ordinal,claim_id,account_id,"
        "session_id,claim_state,claimed_utc_seconds,claim_expires_utc_seconds) VALUES(1,"
        "x'11111111111111111111111111111111',2,x'01010101010101010101010101010101',"
        "'active',300,600);"
        "UPDATE sfx_failed_login_notices SET claim_id=x'11111111111111111111111111111111',"
        "claimed_session_id=x'01010101010101010101010101010101',claimed_utc_seconds=300,"
        "claim_expires_utc_seconds=600 WHERE account_id=2");
    require(fails(db, "INSERT INTO sfx_failed_login_notice_claims(claim_ordinal,claim_id,"
        "account_id,session_id,claim_state,claimed_utc_seconds,claim_expires_utc_seconds) "
        "VALUES(2,x'12121212121212121212121212121212',2,"
        "x'03030303030303030303030303030303','active',301,601)"),
        "one active delivery claim per account is enforced");
    for (int index = 7; index <= 12; ++index) {
        db.exec("INSERT INTO sfx_failed_login_notices(notice_id,account_id,"
            "source_address_family,source_address,event_utc_seconds) VALUES("
            "CAST(printf('%016x'," + std::to_string(index) +
            ") AS BLOB),2,4,x'7f000001'," + std::to_string(200 + index) + ")");
    }
    require(number(db.get(), "SELECT count(*) FROM sfx_failed_login_notices "
        "WHERE claim_id IS NULL") == 5 && number(db.get(),
        "SELECT count(*) FROM sfx_failed_login_notices WHERE claim_id IS NOT NULL") == 5,
        "immutable in-flight five coexist with newest-five unread queue");
    db.exec("DELETE FROM sfx_account_sessions WHERE session_id="
        "x'01010101010101010101010101010101'");
    require(number(db.get(), "SELECT count(*) FROM sfx_failed_login_notices "
        "WHERE account_id=2 AND claim_id IS NULL") == 5 && number(db.get(),
        "SELECT count(*) FROM sfx_failed_login_notice_claims WHERE account_id=2") == 0,
        "session deletion releases notices and resolves the claim receipt");

    db.exec("INSERT INTO sfx_registration_commit_receipts(registration_event_id,"
        "fingerprint_format_version,request_fingerprint,account_id,"
        "committed_account_revision,committed_utc_seconds) VALUES("
        "x'21212121212121212121212121212121',1,zeroblob(32),2,1,400)");
    require(fails(db, "UPDATE sfx_registration_commit_receipts SET committed_utc_seconds=401") &&
        fails(db, "DELETE FROM sfx_registration_commit_receipts") && fails(db,
        "INSERT INTO sfx_registration_commit_receipts(registration_event_id,"
        "fingerprint_format_version,request_fingerprint,account_id,"
        "committed_account_revision,committed_utc_seconds) VALUES("
        "x'21212121212121212121212121212121',1,zeroblob(32),2,1,400)"),
        "registration receipt replay identity is immutable and unique");
    db.exec("DELETE FROM sfx_account_sessions WHERE account_id=2;"
        "DELETE FROM sfx_failed_login_notices WHERE account_id=2;"
        "DELETE FROM sfx_authentication_rate_state;"
        "DELETE FROM sfx_account_transfer_preferences WHERE account_id=2;"
        "DELETE FROM sfx_account_profiles WHERE account_id=2;"
        "DELETE FROM sfx_account_credentials WHERE account_id=2;"
        "DELETE FROM sfx_account_statistics WHERE account_id=2;"
        "UPDATE sfx_account_name_reservations SET is_current=0,display_name=NULL,"
        "retired_utc_seconds=500 WHERE account_id=2;"
        "UPDATE sfx_accounts SET display_name=NULL,name_key=NULL,security_level=NULL,"
        "account_state='tombstoned',revision=2,purge_allowed=NULL,modified_utc_seconds=500,"
        "tombstoned_utc_seconds=500,tombstone_reason_code='sysop_action' WHERE account_id=2");
    require(number(db.get(), "SELECT count(*) FROM sfx_accounts WHERE account_id=2 "
        "AND account_state='tombstoned' AND name_key IS NULL") == 1 && number(db.get(),
        "SELECT count(*) FROM sfx_account_name_reservations WHERE account_id=2 "
        "AND is_current=0") == 1, "tombstone and permanent reservation semantics remain");
    require(number(db.get(), "SELECT count(*) FROM sfx_registration_commit_receipts "
        "WHERE account_id=2") == 1, "registration replay evidence survives tombstoning");
}

internal::SfxBaselineFacts completed_facts(const Database& db) {
    internal::SfxBaselineFacts facts;
    facts.canonical_protected_data_directory = true;
    facts.database_present = true;
    facts.database_bytes = std::filesystem::file_size(db.path());
    facts.file_identity_available = true;
    facts.file_volume_identity = 1;
    facts.file_identity = 2;
    facts.sqlite_header_valid = true;
    facts.application_id = static_cast<std::int32_t>(
        number(db.get(), "PRAGMA application_id"));
    facts.user_version = static_cast<std::int32_t>(
        number(db.get(), "PRAGMA user_version"));
    facts.integrity_check_ok = text(db.get(), "PRAGMA integrity_check") == "ok";
    facts.foreign_key_check_ok = number(db.get(),
        "SELECT count(*) FROM pragma_foreign_key_check") == 0;
    facts.schema_objects = manifest(db.get());
    for (const auto& object : facts.schema_objects) {
        if (object.kind == internal::SfxBaselineSchemaObjectKind::table) {
            facts.column_count += number(db.get(), "SELECT count(*) FROM pragma_table_xinfo('" +
                object.name + "')");
            facts.foreign_key_count += number(db.get(),
                "SELECT count(*) FROM pragma_foreign_key_list('" + object.name + "')");
        }
    }
    facts.ledger.push_back({1, internal::sfx_baseline_migration_name,
        internal::sfx_baseline_migration_sha256, 2001});
    facts.sqlite_sequence_table_names = rows(db.get(),
        "SELECT name FROM sqlite_sequence ORDER BY name");
    return facts;
}

void verify_classifier(const Database& db) {
    using State = internal::SfxBaselineState;
    using Issue = internal::SfxBaselineIssue;
    internal::SfxBaselineFacts facts;
    facts.canonical_protected_data_directory = true;
    require(internal::classify_sfx_baseline_state(facts).state == State::absent_fresh,
        "absent protected state is fresh");
    facts.database_present = true;
    facts.file_identity_available = true;
    facts.file_volume_identity = 7;
    facts.file_identity = 9;
    require(internal::classify_sfx_baseline_state(facts).state == State::prepublication,
        "bound zero-byte state is prepublication");
    facts.database_bytes = 4096;
    facts.sqlite_header_valid = true;
    facts.integrity_check_ok = true;
    facts.foreign_key_check_ok = true;
    require(internal::classify_sfx_baseline_state(facts).state == State::prepublication,
        "exact empty SQLite state is prepublication");
    facts.wal = {true, 8192};
    facts.shared_memory = {true, 32768};
    require(internal::classify_sfx_baseline_state(facts).requires_sqlite_recovery,
        "recovery-bearing prepublication is recognized without mutation");
    auto complete = completed_facts(db);
    require(internal::classify_sfx_baseline_state(complete).state == State::completed,
        "exact clean database is completed");
    complete.wal = {true, 4096};
    complete.shared_memory = {true, 32768};
    require(internal::classify_sfx_baseline_state(complete).requires_sqlite_recovery,
        "completed database can require SQLite recovery");
    auto bad = completed_facts(db);
    bad.canonical_protected_data_directory = false;
    require(internal::classify_sfx_baseline_state(bad).issue ==
        Issue::unbound_path_or_identity, "unbound path fails closed");
    bad = completed_facts(db); bad.file_identity_available = false;
    require(internal::classify_sfx_baseline_state(bad).issue ==
        Issue::unbound_path_or_identity, "unbound file identity fails closed");
    bad = completed_facts(db); bad.application_id = 0x12345678;
    require(internal::classify_sfx_baseline_state(bad).issue ==
        Issue::unknown_database_identity, "unknown nonempty database fails closed");
    bad = completed_facts(db); bad.ledger.front().content_sha256[0] = '0';
    require(internal::classify_sfx_baseline_state(bad).issue ==
        Issue::migration_ledger_mismatch, "altered ledger fails closed");
    bad = completed_facts(db); bad.schema_objects.pop_back();
    require(internal::classify_sfx_baseline_state(bad).issue ==
        Issue::schema_manifest_mismatch, "partial schema fails closed");
    bad = completed_facts(db); bad.schema_objects.push_back(bad.schema_objects.front());
    require(internal::classify_sfx_baseline_state(bad).issue ==
        Issue::schema_manifest_mismatch, "duplicate schema fact fails closed");
    bad = completed_facts(db); bad.wal = {true,4096}; bad.rollback_journal = {true,4096};
    require(internal::classify_sfx_baseline_state(bad).issue ==
        Issue::conflicting_sidecars, "conflicting recovery material fails closed");
    internal::SfxBaselineFacts orphan;
    orphan.canonical_protected_data_directory = true;
    orphan.wal = {true,4096};
    require(internal::classify_sfx_baseline_state(orphan).issue ==
        Issue::orphaned_sidecar, "orphan sidecar fails closed");
    const auto first = internal::classify_sfx_baseline_state(completed_facts(db));
    const auto second = internal::classify_sfx_baseline_state(completed_facts(db));
    require(first.state == second.state && first.issue == second.issue,
        "classifier is deterministic and side-effect-free");
}

}  // namespace

int main(int argc, char** argv) {
    const char* phase = "argument validation";
    try {
        if (argc != 2) throw std::runtime_error("expected one scratch-directory argument");
        phase = "library initialization";
        if (sodium_init() < 0) throw std::runtime_error("initialize libsodium");
        phase = "scratch preparation";
        const std::filesystem::path root(argv[1]);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        {
            phase = "clean SFX baseline construction";
            Database proposed(root / "sfx-baseline.db");
            create_active(proposed);
            phase = "identity and stored-definition validation";
            verify_identity_manifest(proposed);
            phase = "schema semantic validation";
            verify_schema_semantics(proposed.get());
            phase = "supplied-facts classifier validation";
            verify_classifier(proposed);
            phase = "accepted persistence behavior validation";
            verify_behavior(proposed);
        }
        phase = "scratch cleanup";
        std::filesystem::remove_all(root);
        std::cout << "all SFX Baseline Window B1 tests passed (" << assertions
                  << " assertions)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "SFX Baseline Window B1 test failure during " << phase
                  << " after " << assertions << " assertions: " << error.what()
                  << std::endl;
        return 1;
    } catch (...) {
        std::cout << "SFX Baseline Window B1 test failure during " << phase
                  << " after " << assertions << " assertions: unknown exception"
                  << std::endl;
        return 1;
    }
}
