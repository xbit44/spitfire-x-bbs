// Additive shipping migration 2. The accepted initial migration is untouched.
const std::string journal_counter_ddl = R"SQL(CREATE TABLE sfx_caller_number_state (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    last_number INTEGER NOT NULL CHECK(last_number>=0)
) STRICT)SQL";
const std::string journal_session_ddl = R"SQL(CREATE TABLE sfx_caller_journals (
    caller_number INTEGER PRIMARY KEY CHECK(caller_number>0),
    session_id BLOB NOT NULL UNIQUE REFERENCES sfx_account_sessions(session_id) CHECK(length(session_id)=16),
    display_name TEXT NOT NULL CHECK(length(display_name) BETWEEN 1 AND 30),
    transport TEXT NOT NULL CHECK(transport IN ('unknown','Telnet','SSH')),
    source_ip TEXT NOT NULL CHECK(length(source_ip)<=45),
    hostname_hint TEXT NOT NULL DEFAULT '' CHECK(length(hostname_hint)<=253),
    ansi INTEGER NOT NULL CHECK(ansi BETWEEN -1 AND 1),
    activities TEXT NOT NULL DEFAULT '' CHECK(length(CAST(activities AS BLOB))<=6144),
    recovered INTEGER NOT NULL DEFAULT 0 CHECK(recovered IN (0,1)),
    published INTEGER NOT NULL DEFAULT 0 CHECK(published IN (0,1))
) STRICT)SQL";
const std::string journal_pending_ddl = R"SQL(CREATE INDEX sfx_caller_journals_pending ON sfx_caller_journals(published,caller_number))SQL";
const std::string journal_intent_ddl = R"SQL(CREATE TABLE sfx_caller_journal_publication (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    caller_number INTEGER NOT NULL UNIQUE REFERENCES sfx_caller_journals(caller_number),
    file_identity TEXT NOT NULL CHECK(length(file_identity) BETWEEN 1 AND 128),
    byte_offset INTEGER NOT NULL CHECK(byte_offset BETWEEN 0 AND 10485760),
    block TEXT NOT NULL CHECK(length(CAST(block AS BLOB)) BETWEEN 1 AND 8192),
    CHECK(byte_offset+length(CAST(block AS BLOB))<=10485760)
) STRICT)SQL";
const std::string journal_migration_sql = journal_counter_ddl + ";\n" +
    journal_session_ddl + ";\n" + journal_pending_ddl + ";\n" + journal_intent_ddl + ";\n" +
    "INSERT INTO sfx_caller_number_state(singleton_id,last_number) "
    "SELECT 1,max((SELECT count(*) FROM sfx_account_sessions),"
    "coalesce((SELECT sum(successful_login_count) FROM sfx_account_statistics),0));\n";

std::vector<SfxBaselineSchemaObject> journal_manifest() {
    return {
        {SfxBaselineSchemaObjectKind::table, "sfx_caller_number_state", stored_definition_sha256(journal_counter_ddl)},
        {SfxBaselineSchemaObjectKind::table, "sfx_caller_journals", stored_definition_sha256(journal_session_ddl)},
        {SfxBaselineSchemaObjectKind::index, "sfx_caller_journals_pending", stored_definition_sha256(journal_pending_ddl)},
        {SfxBaselineSchemaObjectKind::table, "sfx_caller_journal_publication", stored_definition_sha256(journal_intent_ddl)}
    };
}
