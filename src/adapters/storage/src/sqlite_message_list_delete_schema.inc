// Reversible list actions keep their own state. Scope 0 is deletion for
// everyone, and scope 1 records removal by the message's Inbox recipient.
constexpr const char* message_list_delete_table_ddl =
    "CREATE TABLE sfx_message_list_deletion ("
    "message_id INTEGER NOT NULL REFERENCES sfx_local_messages(message_id) ON DELETE CASCADE, "
    "scope INTEGER NOT NULL CHECK(scope IN(0,1)), PRIMARY KEY(message_id,scope)) STRICT";
constexpr const char* active_local_messages_view_ddl =
    "CREATE VIEW sfx_active_local_messages AS SELECT m.* FROM sfx_local_messages m "
    "WHERE NOT EXISTS(SELECT 1 FROM sfx_message_list_deletion d WHERE d.message_id=m.message_id AND d.scope=0)";
const std::string message_list_delete_migration_sql =
    std::string(message_list_delete_table_ddl) + ";\n" + active_local_messages_view_ddl + ";\n";

std::vector<SfxBaselineSchemaObject> message_list_delete_manifest() {
    auto result = door_bulletin_manifest();
    result.push_back({SfxBaselineSchemaObjectKind::table,"sfx_message_list_deletion",
        stored_definition_sha256(message_list_delete_table_ddl)});
    result.push_back({SfxBaselineSchemaObjectKind::view,"sfx_active_local_messages",
        stored_definition_sha256(active_local_messages_view_ddl)});
    return result;
}
