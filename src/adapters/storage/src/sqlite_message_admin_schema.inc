// Additive migration 9. Never alter the accepted migration 8 definitions.
const std::vector<MessageSchemaDefinition> message_admin_definitions = [] {
    std::vector<MessageSchemaDefinition> result{
        {SfxBaselineSchemaObjectKind::table,"sfx_message_purge_scope",
         "CREATE TABLE sfx_message_purge_scope (echo_id INTEGER PRIMARY KEY REFERENCES sfx_local_echoes(echo_id)) STRICT"},
        {SfxBaselineSchemaObjectKind::table,"sfx_message_purged_receipts",
         "CREATE TABLE sfx_message_purged_receipts (operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),actor_account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),message_id INTEGER NOT NULL UNIQUE CHECK(message_id>0)) STRICT"},
        {SfxBaselineSchemaObjectKind::trigger,"sfx_message_purged_receipts_immutable_update",
         "CREATE TRIGGER sfx_message_purged_receipts_immutable_update BEFORE UPDATE ON sfx_message_purged_receipts BEGIN SELECT RAISE(ABORT,'immutable purge receipt'); END"},
        {SfxBaselineSchemaObjectKind::trigger,"sfx_message_purged_receipts_immutable_delete",
         "CREATE TRIGGER sfx_message_purged_receipts_immutable_delete BEFORE DELETE ON sfx_message_purged_receipts BEGIN SELECT RAISE(ABORT,'immutable purge receipt'); END"},
        {SfxBaselineSchemaObjectKind::trigger,"sfx_local_messages_immutable_delete",
         "CREATE TRIGGER sfx_local_messages_immutable_delete BEFORE DELETE ON sfx_local_messages WHEN OLD.kind<>0 OR NOT EXISTS(SELECT 1 FROM sfx_message_purge_scope WHERE echo_id=OLD.echo_id) BEGIN SELECT RAISE(ABORT,'message deletion outside echo purge'); END"},
        {SfxBaselineSchemaObjectKind::trigger,"sfx_message_receipts_immutable_delete",
         "CREATE TRIGGER sfx_message_receipts_immutable_delete BEFORE DELETE ON sfx_message_commit_receipts WHEN NOT EXISTS(SELECT 1 FROM sfx_local_messages m JOIN sfx_message_purge_scope p USING(echo_id) JOIN sfx_message_purged_receipts t ON t.operation_id=OLD.operation_id AND t.message_id=m.message_id AND t.actor_account_id=OLD.actor_account_id AND t.request_fingerprint=OLD.request_fingerprint WHERE m.message_id=OLD.message_id AND m.kind=0) BEGIN SELECT RAISE(ABORT,'receipt deletion outside echo purge'); END"},
        {SfxBaselineSchemaObjectKind::trigger,"sfx_local_messages_immutable_update",
         "CREATE TRIGGER sfx_local_messages_immutable_update BEFORE UPDATE ON sfx_local_messages WHEN NOT (NEW.parent_message_id IS NULL AND EXISTS(SELECT 1 FROM sfx_local_messages m JOIN sfx_message_purge_scope p USING(echo_id) WHERE m.message_id=OLD.parent_message_id AND m.kind=0) AND NEW.message_id IS OLD.message_id AND NEW.kind IS OLD.kind AND NEW.echo_id IS OLD.echo_id AND NEW.area_number IS OLD.area_number AND NEW.author_account_id IS OLD.author_account_id AND NEW.recipient_account_id IS OLD.recipient_account_id AND NEW.subject IS OLD.subject AND NEW.body IS OLD.body AND NEW.encoding IS OLD.encoding AND NEW.body_lines IS OLD.body_lines AND NEW.arrival_ordinal IS OLD.arrival_ordinal AND NEW.created_utc_seconds IS OLD.created_utc_seconds) BEGIN SELECT RAISE(ABORT,'immutable message'); END"}
    };
    for(const auto& d:local_message_definitions) if(std::string(d.name)=="sfx_message_configuration_audit") {
        static const std::string ddl=[](const char* old){std::string sql(old);const auto at=sql.find("BETWEEN 0 AND 4");sql.replace(at,15,"BETWEEN 0 AND 8");return sql;}(d.ddl);
        result.push_back({d.kind,d.name,ddl.c_str()});
    }
    return result;
}();
const std::string message_admin_migration_sql=[] {
    std::string sql;
    sql+="DROP TRIGGER sfx_local_messages_immutable_delete;\nDROP TRIGGER sfx_local_messages_immutable_update;\nDROP TRIGGER sfx_message_receipts_immutable_delete;\n";
    sql+="ALTER TABLE sfx_message_configuration_audit RENAME TO sfx_message_configuration_audit_v8;\n";
    for(const auto& d:message_admin_definitions)sql+=std::string(d.ddl)+";\n";
    sql+="INSERT INTO sfx_message_configuration_audit SELECT * FROM sfx_message_configuration_audit_v8;\nDROP TABLE sfx_message_configuration_audit_v8;\n";
    return sql;
}();
std::vector<SfxBaselineSchemaObject> message_admin_manifest() {
    auto result=local_message_manifest();
    for(const auto& d:message_admin_definitions) {
        result.erase(std::remove_if(result.begin(),result.end(),[&](const auto& old){return old.name==d.name;}),result.end());
        result.push_back({d.kind,d.name,stored_definition_sha256(d.ddl)});
    }
    return result;
}
