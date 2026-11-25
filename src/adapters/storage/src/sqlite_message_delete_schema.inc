// Additive migration 11: individual deletion, never edit migrations 1-10.
const std::vector<MessageSchemaDefinition> message_delete_definitions{
    {SfxBaselineSchemaObjectKind::table,"sfx_message_delete_scope",
     "CREATE TABLE sfx_message_delete_scope (message_id INTEGER PRIMARY KEY REFERENCES sfx_local_messages(message_id) ON DELETE CASCADE) STRICT"},
    {SfxBaselineSchemaObjectKind::table,"sfx_message_delete_receipts",
     "CREATE TABLE sfx_message_delete_receipts (operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),actor_account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),message_id INTEGER NOT NULL UNIQUE CHECK(message_id>0),deleted_utc_seconds INTEGER NOT NULL CHECK(deleted_utc_seconds>=0)) STRICT"},
    {SfxBaselineSchemaObjectKind::trigger,"sfx_message_delete_receipts_immutable_update",
     "CREATE TRIGGER sfx_message_delete_receipts_immutable_update BEFORE UPDATE ON sfx_message_delete_receipts BEGIN SELECT RAISE(ABORT,'immutable message delete receipt'); END"},
    {SfxBaselineSchemaObjectKind::trigger,"sfx_message_delete_receipts_immutable_delete",
     "CREATE TRIGGER sfx_message_delete_receipts_immutable_delete BEFORE DELETE ON sfx_message_delete_receipts BEGIN SELECT RAISE(ABORT,'immutable message delete receipt'); END"},
    {SfxBaselineSchemaObjectKind::trigger,"sfx_local_messages_immutable_delete",
     "CREATE TRIGGER sfx_local_messages_immutable_delete BEFORE DELETE ON sfx_local_messages WHEN NOT (EXISTS(SELECT 1 FROM sfx_message_delete_scope WHERE message_id=OLD.message_id) OR (OLD.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope WHERE echo_id=OLD.echo_id))) BEGIN SELECT RAISE(ABORT,'message deletion outside authorized purge'); END"},
    {SfxBaselineSchemaObjectKind::trigger,"sfx_message_receipts_immutable_delete",
     "CREATE TRIGGER sfx_message_receipts_immutable_delete BEFORE DELETE ON sfx_message_commit_receipts WHEN NOT EXISTS(SELECT 1 FROM sfx_local_messages m JOIN sfx_message_purged_receipts t ON t.operation_id=OLD.operation_id AND t.message_id=m.message_id AND t.actor_account_id=OLD.actor_account_id AND t.request_fingerprint=OLD.request_fingerprint WHERE m.message_id=OLD.message_id AND (EXISTS(SELECT 1 FROM sfx_message_delete_scope WHERE message_id=m.message_id) OR (m.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope WHERE echo_id=m.echo_id)))) BEGIN SELECT RAISE(ABORT,'receipt deletion outside authorized purge'); END"},
    {SfxBaselineSchemaObjectKind::trigger,"sfx_local_messages_immutable_update",
     "CREATE TRIGGER sfx_local_messages_immutable_update BEFORE UPDATE ON sfx_local_messages WHEN NOT (NEW.parent_message_id IS NULL AND EXISTS(SELECT 1 FROM sfx_local_messages m WHERE m.message_id=OLD.parent_message_id AND (EXISTS(SELECT 1 FROM sfx_message_delete_scope WHERE message_id=m.message_id) OR (m.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope WHERE echo_id=m.echo_id)))) AND NEW.message_id IS OLD.message_id AND NEW.kind IS OLD.kind AND NEW.echo_id IS OLD.echo_id AND NEW.area_number IS OLD.area_number AND NEW.author_account_id IS OLD.author_account_id AND NEW.recipient_account_id IS OLD.recipient_account_id AND NEW.subject IS OLD.subject AND NEW.body IS OLD.body AND NEW.encoding IS OLD.encoding AND NEW.body_lines IS OLD.body_lines AND NEW.arrival_ordinal IS OLD.arrival_ordinal AND NEW.created_utc_seconds IS OLD.created_utc_seconds) BEGIN SELECT RAISE(ABORT,'immutable message'); END"}
};
const std::string message_delete_migration_sql=[] {
    std::string sql="DROP TRIGGER sfx_local_messages_immutable_delete;\nDROP TRIGGER sfx_message_receipts_immutable_delete;\nDROP TRIGGER sfx_local_messages_immutable_update;\n";
    for(const auto& d:message_delete_definitions)sql+=std::string(d.ddl)+";\n";
    return sql;
}();
std::vector<SfxBaselineSchemaObject> message_delete_manifest(){
    auto result=message_group_manifest();
    for(const auto& d:message_delete_definitions){
        result.erase(std::remove_if(result.begin(),result.end(),[&](const auto& old){return old.name==d.name;}),result.end());
        result.push_back({d.kind,d.name,stored_definition_sha256(d.ddl)});
    }
    return result;
}
