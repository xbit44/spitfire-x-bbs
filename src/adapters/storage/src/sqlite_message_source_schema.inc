// Additive migration 12. Migrations 1-11 remain byte-exact.
// SQL is the owner-approved checkpoint proposal, including its historical comments.
const std::string message_source_migration_sql=R"SQL(-- REVIEW PROPOSAL ONLY. Not registered with SFX; never run on an installation.
-- Applied only after exact schema 11 validation, inside BEGIN IMMEDIATE.
ALTER TABLE sfx_local_echoes ADD COLUMN renegade_pipe_colors INTEGER NOT NULL DEFAULT 0 CHECK(renegade_pipe_colors IN (0,1));

CREATE TABLE sfx_message_presentation_policy (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    private_pipe_colors INTEGER NOT NULL CHECK(private_pipe_colors IN (0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT;
INSERT INTO sfx_message_presentation_policy VALUES(1,0,1);

CREATE TABLE sfx_message_sources (
    message_id INTEGER PRIMARY KEY REFERENCES sfx_local_messages(message_id),
    encoding TEXT NOT NULL CHECK(encoding='cp437'),
    format INTEGER NOT NULL CHECK(format IN (1,2)),
    profile_version INTEGER NOT NULL CHECK(profile_version=1),
    source BLOB NOT NULL CHECK(length(source) BETWEEN 1 AND 262144),
    source_sha256 BLOB NOT NULL CHECK(length(source_sha256)=32),
    extent_off INTEGER NOT NULL CHECK(extent_off BETWEEN 1 AND 1000),
    extent_on INTEGER NOT NULL CHECK(extent_on BETWEEN 1 AND 1000),
    charged_extent INTEGER NOT NULL CHECK(charged_extent=max(extent_off,extent_on)),
    visible_off BLOB NOT NULL CHECK(length(visible_off) BETWEEN 1 AND 80999),
    visible_on BLOB NOT NULL CHECK(length(visible_on) BETWEEN 1 AND 80999)
) STRICT;

CREATE TABLE sfx_message_source_write_scope (
    message_id INTEGER PRIMARY KEY REFERENCES sfx_local_messages(message_id),
    operation_id BLOB NOT NULL UNIQUE REFERENCES sfx_message_commit_receipts(operation_id) CHECK(length(operation_id)=16)
) STRICT;
CREATE TRIGGER sfx_message_sources_guard_insert BEFORE INSERT ON sfx_message_sources
WHEN NOT EXISTS(SELECT 1 FROM sfx_message_source_write_scope s JOIN sfx_message_commit_receipts r USING(operation_id) WHERE s.message_id=NEW.message_id AND r.message_id=NEW.message_id)
BEGIN SELECT RAISE(ABORT,'source outside atomic commit'); END;
CREATE TRIGGER sfx_message_sources_immutable_update BEFORE UPDATE ON sfx_message_sources
BEGIN SELECT RAISE(ABORT,'immutable original source'); END;
CREATE TRIGGER sfx_message_sources_guard_delete BEFORE DELETE ON sfx_message_sources
WHEN NOT EXISTS(SELECT 1 FROM sfx_local_messages m JOIN sfx_message_commit_receipts r USING(message_id) JOIN sfx_message_purged_receipts t ON t.operation_id=r.operation_id AND t.message_id=m.message_id AND t.actor_account_id=r.actor_account_id AND t.request_fingerprint=r.request_fingerprint WHERE m.message_id=OLD.message_id AND (EXISTS(SELECT 1 FROM sfx_message_delete_scope d WHERE d.message_id=m.message_id) OR (m.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope p WHERE p.echo_id=m.echo_id))))
BEGIN SELECT RAISE(ABORT,'source deletion outside authorized purge'); END;

-- Preserve all existing audit rows and their fingerprints; add action 10 only.
ALTER TABLE sfx_message_configuration_audit RENAME TO sfx_message_configuration_audit_v11;
CREATE TABLE sfx_message_configuration_audit (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    action INTEGER NOT NULL CHECK(action BETWEEN 0 AND 10),
    actor_account_id INTEGER REFERENCES sfx_accounts(account_id),
    installation_owner INTEGER NOT NULL CHECK(installation_owner IN (0,1)),
    result_configuration_revision INTEGER NOT NULL CHECK(result_configuration_revision>=1),
    result_entity_id INTEGER NOT NULL CHECK(result_entity_id>=0),
    result_entity_revision INTEGER NOT NULL CHECK(result_entity_revision>=0),
    occurred_utc_seconds INTEGER NOT NULL CHECK(occurred_utc_seconds BETWEEN -62135596800 AND 253402300799),
    CHECK(installation_owner=1 OR actor_account_id IS NOT NULL)
) STRICT;
INSERT INTO sfx_message_configuration_audit SELECT * FROM sfx_message_configuration_audit_v11;
DROP TABLE sfx_message_configuration_audit_v11;

-- Production registration, exact manifest/ledger checks and user_version=12
-- publication are gated. Do not append them to this isolated proposal.
)SQL";
std::vector<SfxBaselineSchemaObject> message_source_manifest(){
    // Independently construct expected definitions from fixed predecessor DDL
    // and the migration. Never derive expectations from the database under review.
    auto result=message_delete_manifest();
    sqlite3* raw{};
    if(sqlite3_open(":memory:",&raw)!=SQLITE_OK){if(raw)sqlite3_close(raw);throw std::runtime_error("source manifest memory database");}
    const auto close=[](sqlite3* db){sqlite3_close(db);};
    std::unique_ptr<sqlite3,decltype(close)> db(raw,close);
    const auto execute=[&](const std::string& sql){
        if(sqlite3_exec(raw,sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("source manifest DDL");
    };
    execute("PRAGMA foreign_keys=OFF");
    execute(proposed_migration_sql);
    for(const auto* migration:{&sfx_caller_journal_migration(),&sfx_menu_preferences_migration(),&sfx_caller_utilities_migration(),&sfx_classic_information_migration(),&sfx_classic_delete_migration(),&sfx_other_board_text_migration(),&sfx_local_message_migration(),&sfx_message_admin_migration(),&sfx_message_group_migration(),&sfx_message_delete_migration()})execute(migration->sql);
    execute(message_source_migration_sql);
    std::vector<SfxBaselineSchemaObject> changed;
    if(!collect_schema_manifest(raw,&changed))throw std::runtime_error("source manifest collection");
    for(const auto& d:changed){
        if(d.name!="sfx_local_echoes" && d.name!="sfx_message_configuration_audit" && d.name!="sfx_message_presentation_policy" && d.name!="sfx_message_sources" && d.name!="sfx_message_source_write_scope" && d.name!="sfx_message_sources_guard_insert" && d.name!="sfx_message_sources_immutable_update" && d.name!="sfx_message_sources_guard_delete")continue;
        const auto old=std::find_if(result.begin(),result.end(),[&](const auto& item){return item.name==d.name;});
        if(old==result.end())result.push_back(d);else *old=d;
    }
    return result;
}
