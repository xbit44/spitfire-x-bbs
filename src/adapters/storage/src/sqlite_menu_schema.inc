// Additive shipping migration 3; the frozen baseline and journal remain exact.
const std::string menu_preferences_ddl = R"SQL(CREATE TABLE sfx_account_menu_preferences (
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    color INTEGER NOT NULL DEFAULT 0 CHECK(color BETWEEN 0 AND 15),
    hotkey INTEGER NOT NULL DEFAULT 0 CHECK(hotkey IN (0,1)),
    lightbar INTEGER NOT NULL DEFAULT 1 CHECK(lightbar IN (0,1)),
    revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>=1),
    modified_utc_seconds INTEGER NOT NULL DEFAULT 0 CHECK(modified_utc_seconds>=0)
) STRICT)SQL";
const std::string menu_default_trigger_ddl = R"SQL(CREATE TRIGGER sfx_accounts_default_menu_preferences
AFTER INSERT ON sfx_accounts WHEN NEW.account_state<>'tombstoned'
BEGIN
    INSERT INTO sfx_account_menu_preferences(account_id) VALUES(NEW.account_id);
END)SQL";
const std::string menu_migration_sql = menu_preferences_ddl + ";\n" +
    menu_default_trigger_ddl + ";\n" +
    "INSERT INTO sfx_account_menu_preferences(account_id) SELECT account_id FROM sfx_accounts "
    "WHERE account_state<>'tombstoned';\n";
std::vector<SfxBaselineSchemaObject> menu_manifest() {
    return {
        {SfxBaselineSchemaObjectKind::table,"sfx_account_menu_preferences",stored_definition_sha256(menu_preferences_ddl)},
        {SfxBaselineSchemaObjectKind::trigger,"sfx_accounts_default_menu_preferences",stored_definition_sha256(menu_default_trigger_ddl)}
    };
}
