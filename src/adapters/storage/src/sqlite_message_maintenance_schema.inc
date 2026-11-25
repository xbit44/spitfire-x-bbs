// Additive Section 8C migration 13. Migrations 1-12 remain byte-exact.
// Seeding only: never run retention, delete bodies, or recreate starter areas.
const std::string message_maintenance_migration_sql = R"SQL(
CREATE TABLE sfx_echo_retention (
    echo_id INTEGER PRIMARY KEY REFERENCES sfx_local_echoes(echo_id) ON DELETE CASCADE,
    maximum_count INTEGER CHECK(maximum_count IS NULL OR maximum_count BETWEEN 1 AND 999999999),
    maximum_age_days INTEGER CHECK(maximum_age_days IS NULL OR maximum_age_days BETWEEN 1 AND 65535)
) STRICT;
INSERT INTO sfx_echo_retention SELECT echo_id,1000,NULL FROM sfx_local_echoes;
CREATE TRIGGER sfx_echo_retention_seed AFTER INSERT ON sfx_local_echoes
BEGIN INSERT INTO sfx_echo_retention VALUES(NEW.echo_id,1000,NULL); END;

CREATE TABLE sfx_unread_mail_retention (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    maximum_age_days INTEGER CHECK(maximum_age_days IS NULL OR maximum_age_days BETWEEN 1 AND 65535),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT;
INSERT INTO sfx_unread_mail_retention VALUES(1,365,1);

CREATE TABLE sfx_message_permanent (
    message_id INTEGER PRIMARY KEY REFERENCES sfx_local_messages(message_id) ON DELETE CASCADE,
    permanent INTEGER NOT NULL CHECK(permanent IN (0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT;
CREATE TRIGGER sfx_message_permanent_public_insert BEFORE INSERT ON sfx_message_permanent
WHEN NOT EXISTS(SELECT 1 FROM sfx_local_messages WHERE message_id=NEW.message_id AND kind=0)
BEGIN SELECT RAISE(ABORT,'Permanent applies only to public messages'); END;
CREATE TRIGGER sfx_message_permanent_public_update BEFORE UPDATE ON sfx_message_permanent
WHEN NEW.message_id<>OLD.message_id OR NOT EXISTS(SELECT 1 FROM sfx_local_messages WHERE message_id=NEW.message_id AND kind=0)
BEGIN SELECT RAISE(ABORT,'Permanent identity is immutable'); END;
CREATE TRIGGER sfx_message_permanent_individual_delete BEFORE DELETE ON sfx_local_messages
WHEN EXISTS(SELECT 1 FROM sfx_message_permanent WHERE message_id=OLD.message_id AND permanent=1)
 AND NOT(OLD.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope WHERE echo_id=OLD.echo_id))
BEGIN SELECT RAISE(ABORT,'remove Permanent before individual deletion'); END;
CREATE TABLE sfx_message_permanent_receipts (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    actor_account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    message_id INTEGER NOT NULL CHECK(message_id>0),
    expected_permanent INTEGER NOT NULL CHECK(expected_permanent IN (0,1)),
    new_permanent INTEGER NOT NULL CHECK(new_permanent IN (0,1)),
    occurred_utc_seconds INTEGER NOT NULL CHECK(occurred_utc_seconds BETWEEN -62135596800 AND 253402300799)
) STRICT;
CREATE TRIGGER sfx_message_permanent_receipts_update BEFORE UPDATE ON sfx_message_permanent_receipts
BEGIN SELECT RAISE(ABORT,'immutable Permanent receipt'); END;
CREATE TRIGGER sfx_message_permanent_receipts_delete BEFORE DELETE ON sfx_message_permanent_receipts
BEGIN SELECT RAISE(ABORT,'immutable Permanent receipt'); END;

CREATE INDEX sfx_public_message_retention_age ON sfx_local_messages(echo_id,created_utc_seconds,arrival_ordinal,message_id) WHERE kind=0;
CREATE INDEX sfx_permanent_message_policy ON sfx_message_permanent(message_id) WHERE permanent=1;
CREATE TABLE sfx_message_maintenance_runs (
    run_id BLOB PRIMARY KEY CHECK(length(run_id)=16),
    kind INTEGER NOT NULL CHECK(kind IN (0,1)),
    installation_owner INTEGER NOT NULL CHECK(installation_owner IN (0,1)),
    actor_account_id INTEGER REFERENCES sfx_accounts(account_id),
    started_utc_seconds INTEGER NOT NULL CHECK(started_utc_seconds BETWEEN -62135596800 AND 253402300799),
    finished_utc_seconds INTEGER CHECK(finished_utc_seconds BETWEEN -62135596800 AND 253402300799),
    result INTEGER NOT NULL CHECK(result BETWEEN 0 AND 5),
    removed_public INTEGER NOT NULL CHECK(removed_public>=0),
    expired_unread INTEGER NOT NULL CHECK(expired_unread>=0),
    committed_batches INTEGER NOT NULL CHECK(committed_batches>=0),
    safe_reason INTEGER NOT NULL CHECK(safe_reason BETWEEN 0 AND 16),
    CHECK((installation_owner=1 AND actor_account_id IS NULL) OR (installation_owner=0 AND actor_account_id IS NOT NULL))
) STRICT;
CREATE TABLE sfx_message_maintenance_plan (
    run_id BLOB NOT NULL REFERENCES sfx_message_maintenance_runs(run_id) ON DELETE CASCADE,
    echo_id INTEGER NOT NULL CHECK(echo_id>0),
    expected_revision INTEGER NOT NULL CHECK(expected_revision>=1),
    maximum_count INTEGER CHECK(maximum_count IS NULL OR maximum_count BETWEEN 1 AND 999999999),
    maximum_age_days INTEGER CHECK(maximum_age_days IS NULL OR maximum_age_days BETWEEN 1 AND 65535),
    completed INTEGER NOT NULL CHECK(completed IN (0,1)),
    PRIMARY KEY(run_id,echo_id)
) STRICT;
CREATE TABLE sfx_message_maintenance_progress (
    run_id BLOB PRIMARY KEY REFERENCES sfx_message_maintenance_runs(run_id) ON DELETE CASCADE,
    arrival_fence INTEGER NOT NULL CHECK(arrival_fence>=0),
    unread_age_days INTEGER CHECK(unread_age_days IS NULL OR unread_age_days BETWEEN 1 AND 65535),
    unread_revision INTEGER NOT NULL CHECK(unread_revision>=1),
    private_cursor INTEGER NOT NULL CHECK(private_cursor>=0),
    private_done INTEGER NOT NULL CHECK(private_done IN (0,1)),
    last_sequence INTEGER NOT NULL CHECK(last_sequence>=0),
    last_public INTEGER NOT NULL CHECK(last_public>=0),
    last_private INTEGER NOT NULL CHECK(last_private>=0),
    last_complete INTEGER NOT NULL CHECK(last_complete IN (0,1)),
    policy_changed INTEGER NOT NULL CHECK(policy_changed IN (0,1))
) STRICT;
CREATE TABLE sfx_message_maintenance_echo_results (
    run_id BLOB NOT NULL REFERENCES sfx_message_maintenance_runs(run_id) ON DELETE CASCADE,
    echo_id INTEGER NOT NULL CHECK(echo_id>0),
    removed INTEGER NOT NULL CHECK(removed>=0),
    protected_overage INTEGER NOT NULL CHECK(protected_overage>=0),
    PRIMARY KEY(run_id,echo_id)
) STRICT;
CREATE INDEX sfx_message_maintenance_history ON sfx_message_maintenance_runs(kind,started_utc_seconds,run_id);
CREATE INDEX sfx_message_retention_parent ON sfx_local_messages(parent_message_id) WHERE parent_message_id IS NOT NULL;
CREATE TABLE sfx_message_integrity_holds (
    scope INTEGER PRIMARY KEY CHECK(scope>=-1),
    incident_id BLOB NOT NULL CHECK(length(incident_id)=16),
    occurred_utc_seconds INTEGER NOT NULL CHECK(occurred_utc_seconds BETWEEN -62135596800 AND 253402300799),
    findings INTEGER NOT NULL CHECK(findings>0)
) STRICT;

CREATE TABLE sfx_message_area_delete_receipts (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    target_kind INTEGER NOT NULL CHECK(target_kind IN(0,1)),
    target_id INTEGER NOT NULL CHECK(target_id>0),
    result_configuration_revision INTEGER NOT NULL CHECK(result_configuration_revision>=1),
    echo_count INTEGER NOT NULL CHECK(echo_count BETWEEN 0 AND 999),
    message_count INTEGER NOT NULL CHECK(message_count>=0),
    permanent_count INTEGER NOT NULL CHECK(permanent_count>=0 AND permanent_count<=message_count),
    occurred_utc_seconds INTEGER NOT NULL CHECK(occurred_utc_seconds BETWEEN -62135596800 AND 253402300799)
) STRICT;
CREATE TRIGGER sfx_message_area_delete_receipts_update BEFORE UPDATE ON sfx_message_area_delete_receipts
BEGIN SELECT RAISE(ABORT,'immutable confirmed area deletion receipt'); END;
CREATE TRIGGER sfx_message_area_delete_receipts_delete BEFORE DELETE ON sfx_message_area_delete_receipts
BEGIN SELECT RAISE(ABORT,'immutable confirmed area deletion receipt'); END;

ALTER TABLE sfx_message_configuration_audit RENAME TO sfx_message_configuration_audit_v12;
CREATE TABLE sfx_message_configuration_audit (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    action INTEGER NOT NULL CHECK(action BETWEEN 0 AND 11),
    actor_account_id INTEGER REFERENCES sfx_accounts(account_id),
    installation_owner INTEGER NOT NULL CHECK(installation_owner IN (0,1)),
    result_configuration_revision INTEGER NOT NULL CHECK(result_configuration_revision>=1),
    result_entity_id INTEGER NOT NULL CHECK(result_entity_id>=0),
    result_entity_revision INTEGER NOT NULL CHECK(result_entity_revision>=0),
    occurred_utc_seconds INTEGER NOT NULL CHECK(occurred_utc_seconds BETWEEN -62135596800 AND 253402300799),
    CHECK(installation_owner=1 OR actor_account_id IS NOT NULL)
) STRICT;
INSERT INTO sfx_message_configuration_audit SELECT * FROM sfx_message_configuration_audit_v12;
DROP TABLE sfx_message_configuration_audit_v12;
)SQL";

std::vector<SfxBaselineSchemaObject> message_maintenance_manifest() {
    // Construct expectations from fixed approved DDL, not the database under review.
    auto result = message_source_manifest();
    sqlite3* raw{};
    if (sqlite3_open(":memory:", &raw) != SQLITE_OK) {
        if (raw) sqlite3_close(raw);
        throw std::runtime_error("maintenance manifest memory database");
    }
    const auto close = [](sqlite3* db) { sqlite3_close(db); };
    std::unique_ptr<sqlite3, decltype(close)> db(raw, close);
    const auto execute = [&](const std::string& sql) {
        if (sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("maintenance manifest DDL");
    };
    execute("PRAGMA foreign_keys=OFF");
    execute(proposed_migration_sql);
    for (const auto* migration : {&sfx_caller_journal_migration(), &sfx_menu_preferences_migration(),
            &sfx_caller_utilities_migration(), &sfx_classic_information_migration(), &sfx_classic_delete_migration(),
            &sfx_other_board_text_migration(), &sfx_local_message_migration(), &sfx_message_admin_migration(),
            &sfx_message_group_migration(), &sfx_message_delete_migration(), &sfx_message_source_migration()}) execute(migration->sql);
    execute(message_maintenance_migration_sql);
    std::vector<SfxBaselineSchemaObject> changed;
    if (!collect_schema_manifest(raw, &changed)) throw std::runtime_error("maintenance manifest collection");
    for (const auto& entry : changed) {
        if (entry.name != "sfx_message_configuration_audit" &&
            entry.name.rfind("sfx_echo_retention",0) != 0 && entry.name.rfind("sfx_unread_mail_retention",0) != 0 &&
            entry.name.rfind("sfx_message_permanent",0) != 0 && entry.name.rfind("sfx_public_message_retention",0) != 0 &&
            entry.name.rfind("sfx_permanent_message_policy",0) != 0 && entry.name.rfind("sfx_message_maintenance",0) != 0 &&
            entry.name.rfind("sfx_message_integrity_holds",0) != 0 && entry.name.rfind("sfx_message_area_delete",0) != 0 && entry.name != "sfx_message_retention_parent") continue;
        const auto old = std::find_if(result.begin(), result.end(), [&](const auto& item) { return item.name == entry.name; });
        if (old == result.end()) result.push_back(entry); else *old = entry;
    }
    return result;
}
