// Additive migration 5. Frozen migrations 1 through 4 remain byte-exact.
const std::string other_boards_ddl=R"SQL(CREATE TABLE sfx_other_boards (
    board_id INTEGER PRIMARY KEY CHECK(board_id BETWEEN 1 AND 99999),
    revision INTEGER NOT NULL CHECK(revision>=1),
    name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 64),
    telnet TEXT NOT NULL CHECK(length(telnet)<=255),
    ssh TEXT NOT NULL CHECK(length(ssh)<=255),
    removed INTEGER NOT NULL CHECK(removed IN (0,1)),
    CHECK(length(telnet)>0 OR length(ssh)>0)
) STRICT)SQL";
const std::string opening_date_ddl=R"SQL(CREATE TABLE sfx_bbs_opening_date (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    revision INTEGER NOT NULL CHECK(revision>=1),
    initialization_utc INTEGER NOT NULL CHECK(initialization_utc>=0),
    configured_date TEXT CHECK(configured_date IS NULL OR (length(configured_date)=10 AND configured_date GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'))
) STRICT)SQL";
const std::string classic_audit_ddl=R"SQL(CREATE TABLE sfx_classic_information_audit (
    event_id BLOB PRIMARY KEY CHECK(length(event_id)=16),
    action INTEGER NOT NULL CHECK(action BETWEEN 0 AND 3),
    board_id INTEGER,
    revision INTEGER NOT NULL CHECK(revision>=1),
    caller_account_id INTEGER REFERENCES sfx_accounts(account_id),
    installation_owner INTEGER NOT NULL CHECK(installation_owner IN (0,1)),
    occurred_utc INTEGER NOT NULL CHECK(occurred_utc>=0),
    CHECK(installation_owner=1 OR (action=0 AND caller_account_id IS NOT NULL))
) STRICT)SQL";
const std::string classic_information_migration_sql=other_boards_ddl+";\n"+opening_date_ddl+";\n"+classic_audit_ddl+";\n"
    "INSERT INTO sfx_bbs_opening_date SELECT 1,1,applied_utc,NULL FROM sfx_schema_migrations WHERE version=1;\n";
std::vector<SfxBaselineSchemaObject> classic_information_manifest() {
    return {{SfxBaselineSchemaObjectKind::table,"sfx_other_boards",stored_definition_sha256(other_boards_ddl)},
        {SfxBaselineSchemaObjectKind::table,"sfx_bbs_opening_date",stored_definition_sha256(opening_date_ddl)},
        {SfxBaselineSchemaObjectKind::table,"sfx_classic_information_audit",stored_definition_sha256(classic_audit_ddl)}};
}
