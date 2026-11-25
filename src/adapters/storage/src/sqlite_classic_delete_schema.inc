// Migration 6 changes only the classic-information audit caller-action CHECK.
// Frozen migration 5 and every earlier schema/ledger remain byte-exact.
const std::string classic_delete_audit_ddl=[] {
    auto ddl=classic_audit_ddl;
    const std::string previous="(action=0 AND caller_account_id IS NOT NULL)";
    const auto at=ddl.find(previous);
    if(at==std::string::npos)throw std::logic_error("classic delete audit lineage missing");
    ddl.replace(at,previous.size(),"(action IN (0,2) AND caller_account_id IS NOT NULL)");
    return ddl;
}();
const std::string classic_delete_migration_sql=
    "ALTER TABLE sfx_classic_information_audit RENAME TO sfx_section7_previous_classic_audit;\n"+
    classic_delete_audit_ddl+";\n"
    "INSERT INTO sfx_classic_information_audit SELECT * FROM sfx_section7_previous_classic_audit;\n"
    "DROP TABLE sfx_section7_previous_classic_audit;\n";
