// Additive Section 8A migration. Accepted migrations 1 through 7 are untouched.
// Original FTN MSGID/REPLY fields are intentionally deferred; a literal
// write-once database constraint remains a gate before any network import.
struct MessageSchemaDefinition {
    SfxBaselineSchemaObjectKind kind;
    const char* name;
    const char* ddl;
};
const std::vector<MessageSchemaDefinition> local_message_definitions{
    {SfxBaselineSchemaObjectKind::table, "sfx_message_groups", R"SQL(CREATE TABLE sfx_message_groups (
    group_id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 64 AND name=trim(name,' ') AND name NOT GLOB '*[^ -~]*'),
    display_order INTEGER NOT NULL CHECK(display_order>=0),
    access_minimum INTEGER NOT NULL CHECK(access_minimum BETWEEN 1 AND 999),
    enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_local_echoes", R"SQL(CREATE TABLE sfx_local_echoes (
    echo_id INTEGER PRIMARY KEY AUTOINCREMENT,
    group_id INTEGER NOT NULL REFERENCES sfx_message_groups(group_id),
    conference_number INTEGER NOT NULL UNIQUE CHECK(conference_number BETWEEN 1 AND 999),
    name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 64 AND name=trim(name,' ') AND name NOT GLOB '*[^ -~]*'),
    display_order INTEGER NOT NULL CHECK(display_order>=0),
    access_minimum INTEGER NOT NULL CHECK(access_minimum BETWEEN 1 AND 999),
    reading_minimum INTEGER NOT NULL CHECK(reading_minimum BETWEEN 1 AND 999),
    posting_minimum INTEGER NOT NULL CHECK(posting_minimum BETWEEN 1 AND 999),
    enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1),
    next_message_number INTEGER NOT NULL CHECK(next_message_number>=1)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_local_messages", R"SQL(CREATE TABLE sfx_local_messages (
    message_id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind INTEGER NOT NULL CHECK(kind IN (0,1)),
    echo_id INTEGER REFERENCES sfx_local_echoes(echo_id),
    area_number INTEGER CHECK(area_number>=1),
    author_account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    recipient_account_id INTEGER REFERENCES sfx_accounts(account_id),
    subject BLOB NOT NULL CHECK(length(subject) BETWEEN 1 AND 70),
    body BLOB NOT NULL CHECK(length(body) BETWEEN 1 AND 1048576),
    encoding TEXT NOT NULL CHECK(encoding='cp437'),
    body_lines INTEGER NOT NULL CHECK(body_lines BETWEEN 1 AND 65535),
    arrival_ordinal INTEGER NOT NULL UNIQUE CHECK(arrival_ordinal>=1),
    created_utc_seconds INTEGER NOT NULL CHECK(created_utc_seconds BETWEEN -62135596800 AND 253402300799),
    parent_message_id INTEGER REFERENCES sfx_local_messages(message_id),
    CHECK((kind=0 AND echo_id IS NOT NULL AND area_number IS NOT NULL) OR
          (kind=1 AND echo_id IS NULL AND area_number IS NULL AND recipient_account_id IS NOT NULL)),
    UNIQUE(echo_id,area_number),
    UNIQUE(message_id,recipient_account_id)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_message_recipient_state", R"SQL(CREATE TABLE sfx_message_recipient_state (
    message_id INTEGER PRIMARY KEY,
    recipient_account_id INTEGER NOT NULL,
    opened INTEGER NOT NULL CHECK(opened IN (0,1)),
    inbox_removed INTEGER NOT NULL CHECK(inbox_removed IN (0,1)),
    opened_sequence INTEGER NOT NULL CHECK(opened_sequence>=0 AND (opened=1)=(opened_sequence>0)),
    FOREIGN KEY(message_id,recipient_account_id) REFERENCES sfx_local_messages(message_id,recipient_account_id)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_caller_echo_state", R"SQL(CREATE TABLE sfx_caller_echo_state (
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    echo_id INTEGER NOT NULL REFERENCES sfx_local_echoes(echo_id),
    highest_opened INTEGER NOT NULL CHECK(highest_opened>=0),
    unread_enabled INTEGER NOT NULL CHECK(unread_enabled IN (0,1)),
    to_you_enabled INTEGER NOT NULL CHECK(to_you_enabled IN (0,1)),
    preference_revision INTEGER NOT NULL CHECK(preference_revision>=1),
    PRIMARY KEY(account_id,echo_id)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_security_message_limits", R"SQL(CREATE TABLE sfx_security_message_limits (
    security_level INTEGER PRIMARY KEY REFERENCES sfx_security_levels(security_level),
    maximum_lines INTEGER NOT NULL CHECK(maximum_lines BETWEEN 1 AND 65535),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_system_sysop", R"SQL(CREATE TABLE sfx_system_sysop (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    account_id INTEGER REFERENCES sfx_accounts(account_id),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_message_counters", R"SQL(CREATE TABLE sfx_message_counters (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    last_arrival_ordinal INTEGER NOT NULL CHECK(last_arrival_ordinal>=0),
    total_committed INTEGER NOT NULL CHECK(total_committed>=0),
    last_read_sequence INTEGER NOT NULL CHECK(last_read_sequence>=0)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_caller_message_statistics", R"SQL(CREATE TABLE sfx_caller_message_statistics (
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    messages_entered INTEGER NOT NULL CHECK(messages_entered>=0)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_message_configuration_state", R"SQL(CREATE TABLE sfx_message_configuration_state (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_message_commit_receipts", R"SQL(CREATE TABLE sfx_message_commit_receipts (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    actor_account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    message_id INTEGER NOT NULL UNIQUE REFERENCES sfx_local_messages(message_id)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_message_arrival_intents", R"SQL(CREATE TABLE sfx_message_arrival_intents (
    message_id INTEGER PRIMARY KEY,
    recipient_account_id INTEGER NOT NULL,
    FOREIGN KEY(message_id,recipient_account_id) REFERENCES sfx_local_messages(message_id,recipient_account_id)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_message_scan_receipts", R"SQL(CREATE TABLE sfx_message_scan_receipts (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    enabled INTEGER NOT NULL CHECK(enabled IN (0,1))
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::table, "sfx_message_configuration_audit", R"SQL(CREATE TABLE sfx_message_configuration_audit (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    action INTEGER NOT NULL CHECK(action BETWEEN 0 AND 4),
    actor_account_id INTEGER REFERENCES sfx_accounts(account_id),
    installation_owner INTEGER NOT NULL CHECK(installation_owner IN (0,1)),
    result_configuration_revision INTEGER NOT NULL CHECK(result_configuration_revision>=1),
    result_entity_id INTEGER NOT NULL CHECK(result_entity_id>=0),
    result_entity_revision INTEGER NOT NULL CHECK(result_entity_revision>=0),
    occurred_utc_seconds INTEGER NOT NULL CHECK(occurred_utc_seconds BETWEEN -62135596800 AND 253402300799),
    CHECK(installation_owner=1 OR actor_account_id IS NOT NULL)
) STRICT)SQL"},
    {SfxBaselineSchemaObjectKind::index, "sfx_messages_echo_number",
        "CREATE INDEX sfx_messages_echo_number ON sfx_local_messages(echo_id,area_number,message_id)"},
    {SfxBaselineSchemaObjectKind::index, "sfx_messages_recipient_arrival",
        "CREATE INDEX sfx_messages_recipient_arrival ON sfx_local_messages(recipient_account_id,arrival_ordinal,message_id)"},
    {SfxBaselineSchemaObjectKind::index, "sfx_messages_author_arrival",
        "CREATE INDEX sfx_messages_author_arrival ON sfx_local_messages(author_account_id,arrival_ordinal,message_id)"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_local_messages_immutable_update",
        "CREATE TRIGGER sfx_local_messages_immutable_update BEFORE UPDATE ON sfx_local_messages BEGIN SELECT RAISE(ABORT,'immutable message'); END"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_local_messages_immutable_delete",
        "CREATE TRIGGER sfx_local_messages_immutable_delete BEFORE DELETE ON sfx_local_messages BEGIN SELECT RAISE(ABORT,'message retention deferred'); END"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_message_receipts_immutable_update",
        "CREATE TRIGGER sfx_message_receipts_immutable_update BEFORE UPDATE ON sfx_message_commit_receipts BEGIN SELECT RAISE(ABORT,'immutable receipt'); END"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_message_receipts_immutable_delete",
        "CREATE TRIGGER sfx_message_receipts_immutable_delete BEFORE DELETE ON sfx_message_commit_receipts BEGIN SELECT RAISE(ABORT,'immutable receipt'); END"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_message_scan_receipts_immutable_update",
        "CREATE TRIGGER sfx_message_scan_receipts_immutable_update BEFORE UPDATE ON sfx_message_scan_receipts BEGIN SELECT RAISE(ABORT,'immutable scan receipt'); END"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_message_scan_receipts_immutable_delete",
        "CREATE TRIGGER sfx_message_scan_receipts_immutable_delete BEFORE DELETE ON sfx_message_scan_receipts BEGIN SELECT RAISE(ABORT,'immutable scan receipt'); END"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_system_sysop_eligible_update", R"SQL(CREATE TRIGGER sfx_system_sysop_eligible_update BEFORE UPDATE OF account_id ON sfx_system_sysop
WHEN NEW.account_id IS NULL OR NOT EXISTS(SELECT 1 FROM sfx_accounts a JOIN sfx_security_levels l ON l.security_level=a.security_level,sfx_security_policy p WHERE a.account_id=NEW.account_id AND a.account_state='active' AND l.level_state='active' AND a.security_level>=p.sysop_threshold)
BEGIN SELECT RAISE(ABORT,'System Sysop must be eligible'); END)SQL"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_accounts_protect_system_sysop", R"SQL(CREATE TRIGGER sfx_accounts_protect_system_sysop BEFORE UPDATE OF account_state,security_level ON sfx_accounts
WHEN EXISTS(SELECT 1 FROM sfx_system_sysop s,sfx_security_policy p WHERE s.account_id=OLD.account_id AND (NEW.account_state<>'active' OR NEW.security_level IS NULL OR NEW.security_level<p.sysop_threshold))
BEGIN SELECT RAISE(ABORT,'reassign System Sysop first'); END)SQL"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_levels_protect_system_sysop", R"SQL(CREATE TRIGGER sfx_levels_protect_system_sysop BEFORE UPDATE OF level_state ON sfx_security_levels
WHEN NEW.level_state<>'active' AND EXISTS(SELECT 1 FROM sfx_system_sysop s JOIN sfx_accounts a ON a.account_id=s.account_id WHERE a.security_level=OLD.security_level)
BEGIN SELECT RAISE(ABORT,'reassign System Sysop first'); END)SQL"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_policy_protect_system_sysop", R"SQL(CREATE TRIGGER sfx_policy_protect_system_sysop BEFORE UPDATE OF sysop_threshold ON sfx_security_policy
WHEN EXISTS(SELECT 1 FROM sfx_system_sysop s JOIN sfx_accounts a ON a.account_id=s.account_id WHERE a.security_level<NEW.sysop_threshold)
BEGIN SELECT RAISE(ABORT,'reassign System Sysop first'); END)SQL"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_accounts_remove_message_preferences", R"SQL(CREATE TRIGGER sfx_accounts_remove_message_preferences AFTER UPDATE OF account_state ON sfx_accounts
WHEN NEW.account_state='tombstoned'
BEGIN DELETE FROM sfx_caller_echo_state WHERE account_id=NEW.account_id; END)SQL"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_security_levels_default_message_limit", R"SQL(CREATE TRIGGER sfx_security_levels_default_message_limit AFTER INSERT ON sfx_security_levels
BEGIN INSERT INTO sfx_security_message_limits VALUES(NEW.security_level,200,1); END)SQL"},
    {SfxBaselineSchemaObjectKind::trigger, "sfx_first_run_designate_system_sysop", R"SQL(CREATE TRIGGER sfx_first_run_designate_system_sysop AFTER UPDATE OF setup_state ON sfx_installation_state
WHEN NEW.setup_state='complete' AND OLD.setup_state='incomplete'
BEGIN UPDATE sfx_system_sysop SET account_id=NEW.staged_sysop_account_id,revision=revision+1 WHERE singleton_id=1 AND account_id IS NULL; END)SQL"}
};
const std::string local_message_migration_sql = [] {
    std::string result;
    for (const auto& object : local_message_definitions) result += std::string(object.ddl) + ";\n";
    result +=
        "INSERT INTO sfx_message_groups VALUES(1,'Local',0,1,1,1);\n"
        "INSERT INTO sfx_local_echoes SELECT 1,1,1,'Sysop Notices',0,1,1,sysop_threshold,1,1,1 FROM sfx_security_policy;\n"
        "INSERT INTO sfx_local_echoes VALUES(2,1,2,'Public Chat',1,1,1,1,1,1,1);\n"
        "INSERT INTO sfx_security_message_limits SELECT security_level,CASE WHEN security_level=999 THEN 1000 ELSE 200 END,1 FROM sfx_security_levels;\n"
        "INSERT INTO sfx_message_counters VALUES(1,0,0,0);\n"
        "INSERT INTO sfx_message_configuration_state VALUES(1,1);\n"
        "INSERT INTO sfx_system_sysop SELECT 1,(SELECT a.account_id FROM sfx_installation_state i JOIN sfx_accounts a ON a.account_id=i.staged_sysop_account_id JOIN sfx_security_levels l ON l.security_level=a.security_level JOIN sfx_security_policy p WHERE i.singleton_id=1 AND a.account_state='active' AND l.level_state='active' AND a.security_level>=p.sysop_threshold),1;\n";
    return result;
}();
std::vector<SfxBaselineSchemaObject> local_message_manifest() {
    std::vector<SfxBaselineSchemaObject> result;
    for (const auto& object : local_message_definitions)
        result.push_back({object.kind, object.name, stored_definition_sha256(object.ddl)});
    return result;
}
