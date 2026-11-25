// Schema27 adds an account-owned selection and a partial toss-work index.
// It never changes message contents, read pointers or existing receipt states.
const std::string message_location_migration_sql=R"SQL(
CREATE TABLE sfx_caller_message_location (
 account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id) ON DELETE CASCADE,
 echo_id INTEGER NOT NULL REFERENCES sfx_local_echoes(echo_id) ON DELETE CASCADE
) STRICT;
CREATE INDEX sfx_ftn_toss_pending ON sfx_ftn_ingress(receipt_id)
 WHERE state IN(1,2) AND (lower(substr(wire_name,-4)) IN('.pkt','.tic') OR
 (substr(wire_name,-4,1)='.' AND lower(substr(wire_name,-3,2)) IN('su','mo','tu','we','th','fr','sa') AND lower(substr(wire_name,-1)) GLOB '[0-9a-z]'));
)SQL";
std::vector<SfxBaselineSchemaObject> message_location_manifest(){
    static const auto manifest=[] {
        auto fixed=ftn_mail_predecessor();
        for(const auto* sql:{&ftn_mail_migration_sql,&optional_subject_migration_sql,&file_group_deletion_migration_sql,&ftn_wire_body_migration_sql,&ftn_subject_migration_sql,&message_location_migration_sql})
            if(sqlite3_exec(fixed.get(),sql->c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("message location fixed manifest");
        std::vector<SfxBaselineSchemaObject> all;
        if(!collect_schema_manifest(fixed.get(),&all))throw std::runtime_error("message location manifest collection");
        auto result=ftn_subject_manifest();
        for(const auto* name:{"sfx_caller_message_location","sfx_ftn_toss_pending"}){
            const auto added=std::find_if(all.begin(),all.end(),[&](const auto& object){return object.name==name;});
            if(added==all.end())throw std::runtime_error("message location manifest missing object");
            result.push_back(*added);
        }return result;
    }();return manifest;
}
