// Additive shipping migration 4; baseline migrations 1/2/3 stay byte-exact.
const std::string calling_limits_ddl=R"SQL(CREATE TABLE sfx_security_calling_limits (
    security_level INTEGER PRIMARY KEY REFERENCES sfx_security_levels(security_level),
    minutes_per_call INTEGER CHECK(minutes_per_call BETWEEN 1 AND 4294967295),
    minutes_per_day INTEGER CHECK(minutes_per_day BETWEEN 1 AND 4294967295),
    CHECK(security_level<>999 OR (minutes_per_call IS NULL AND minutes_per_day IS NULL))
) STRICT)SQL";
const std::string caller_daily_ddl=R"SQL(CREATE TABLE sfx_caller_daily_seconds (
    session_id BLOB NOT NULL REFERENCES sfx_account_sessions(session_id) ON DELETE CASCADE,
    local_date TEXT NOT NULL CHECK(length(local_date)=10 AND local_date GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
    seconds INTEGER NOT NULL CHECK(seconds>=0),
    PRIMARY KEY(session_id,local_date)
) STRICT)SQL";
const std::string caller_daily_index_ddl="CREATE INDEX sfx_caller_daily_date ON sfx_caller_daily_seconds(local_date,session_id)";
const std::string caller_audit_ddl=[] {
    const auto begin=proposed_migration_sql.find("CREATE TABLE sfx_account_audit(");
    auto ddl=proposed_migration_sql.substr(begin,proposed_migration_sql.find(';',begin)-begin);
    const std::string old="'session_finalization'))";
    const auto at=ddl.find(old);
    if(at==std::string::npos) throw std::logic_error("caller audit lineage missing");
    ddl.replace(at,old.size(),"'session_finalization','caller_action'))");return ddl;
}();
const std::string caller_utilities_migration_sql=calling_limits_ddl+";\n"+caller_daily_ddl+";\n"+caller_daily_index_ddl+";\n"+
    // Completed legacy installs preserve unlimited unconfigured allowances.
    "INSERT INTO sfx_security_calling_limits SELECT 10,180,180 FROM sfx_installation_state WHERE singleton_id=1 AND setup_state='incomplete';\n"
    "DROP INDEX sfx_account_audit_subject_time;DROP INDEX sfx_account_audit_kind_time;DROP INDEX sfx_account_audit_time;\n"
    "ALTER TABLE sfx_account_audit RENAME TO sfx_section6_previous_audit;\n"+caller_audit_ddl+";\n"
    "INSERT INTO sfx_account_audit SELECT * FROM sfx_section6_previous_audit;DROP TABLE sfx_section6_previous_audit;\n"
    "CREATE INDEX sfx_account_audit_subject_time ON sfx_account_audit(subject_account_id,occurred_utc_seconds DESC,event_id);\n"
    "CREATE INDEX sfx_account_audit_kind_time ON sfx_account_audit(event_kind,occurred_utc_seconds DESC,event_id);\n"
    "CREATE INDEX sfx_account_audit_time ON sfx_account_audit(occurred_utc_seconds DESC,event_id);\n";
std::vector<SfxBaselineSchemaObject> caller_utilities_manifest() {
    return {{SfxBaselineSchemaObjectKind::table,"sfx_security_calling_limits",stored_definition_sha256(calling_limits_ddl)},
        {SfxBaselineSchemaObjectKind::table,"sfx_caller_daily_seconds",stored_definition_sha256(caller_daily_ddl)},
        {SfxBaselineSchemaObjectKind::index,"sfx_caller_daily_date",stored_definition_sha256(caller_daily_index_ddl)}};
}
