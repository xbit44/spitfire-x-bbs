// Additive migration 10. Accepted migrations 1-9 remain byte-exact.
const std::string message_group_audit_ddl=[] {
    for(const auto& d:message_admin_definitions)if(std::string(d.name)=="sfx_message_configuration_audit"){
        std::string sql(d.ddl);const auto at=sql.find("BETWEEN 0 AND 8");
        if(at==std::string::npos)throw std::logic_error("group audit predecessor missing");
        sql.replace(at,15,"BETWEEN 0 AND 9");return sql;
    }
    throw std::logic_error("group audit predecessor missing");
}();
const std::string message_group_migration_sql=
    "ALTER TABLE sfx_message_configuration_audit RENAME TO sfx_message_configuration_audit_v9;\n"+
    message_group_audit_ddl+";\nINSERT INTO sfx_message_configuration_audit SELECT * FROM sfx_message_configuration_audit_v9;\nDROP TABLE sfx_message_configuration_audit_v9;\n";
std::vector<SfxBaselineSchemaObject> message_group_manifest(){
    auto result=message_admin_manifest();
    for(auto& object:result)if(object.name=="sfx_message_configuration_audit")object.definition_sha256=stored_definition_sha256(message_group_audit_ddl);
    return result;
}
