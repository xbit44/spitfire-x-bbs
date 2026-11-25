// Additive schema25. Older published wire sources retain their prior body;
// new color-aware publications freeze the exact rendition for every retry.
const std::string ftn_wire_body_migration_sql=R"sql(
CREATE TABLE sfx_ftn_wire_bodies (
 message_id INTEGER NOT NULL,
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 body BLOB NOT NULL CHECK(length(body)<=1048576),
 PRIMARY KEY(message_id,network),
 FOREIGN KEY(message_id,network) REFERENCES sfx_ftn_message_intents(message_id,network) ON DELETE CASCADE
) STRICT;
CREATE TRIGGER sfx_ftn_wire_bodies_update BEFORE UPDATE ON sfx_ftn_wire_bodies
BEGIN SELECT RAISE(ABORT,'Published FTN body is immutable'); END;
CREATE TRIGGER sfx_ftn_wire_bodies_delete BEFORE DELETE ON sfx_ftn_wire_bodies
WHEN NOT EXISTS(SELECT 1 FROM sfx_local_messages m WHERE m.message_id=OLD.message_id AND
 (EXISTS(SELECT 1 FROM sfx_message_delete_scope WHERE message_id=m.message_id) OR
 (m.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope WHERE echo_id=m.echo_id))))
BEGIN SELECT RAISE(ABORT,'Published FTN body deletion outside authorized purge'); END;
)sql";
std::vector<SfxBaselineSchemaObject> ftn_wire_body_manifest(){
    static const auto manifest=[] {
        auto fixed=ftn_mail_predecessor();
        for(const auto* sql:{&ftn_mail_migration_sql,&optional_subject_migration_sql,&file_group_deletion_migration_sql,&ftn_wire_body_migration_sql})
            if(sqlite3_exec(fixed.get(),sql->c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("FTN wire body fixed predecessor");
        std::vector<SfxBaselineSchemaObject> actual;
        if(!collect_schema_manifest(fixed.get(),&actual))throw std::runtime_error("FTN wire body manifest collection");
        auto result=file_group_deletion_manifest();
        for(const auto& object:actual)if(object.name.find("sfx_ftn_wire_bodies")==0)result.push_back(object);
        return result;
    }();return manifest;
}
