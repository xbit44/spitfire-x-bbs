// Schema23 permits a genuinely empty subject. Predecessor migrations stay exact.
// Rebuild only the message table, preserving IDs, sequence, rows and all fixed
// indexes/triggers. Never derive executable DDL from an installed database.
const std::string optional_subject_migration_sql=[] {
    auto fixed=ftn_mail_predecessor();
    if(sqlite3_exec(fixed.get(),ftn_mail_migration_sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error("optional subject fixed predecessor");
    sqlite3_stmt* raw{};
    if(sqlite3_prepare_v2(fixed.get(),"SELECT type,name,sql FROM sqlite_schema WHERE sql IS NOT NULL AND ((type='table' AND name='sfx_local_messages') OR (type IN('index','trigger') AND instr(sql,'sfx_local_messages')>0)) ORDER BY type,name",-1,&raw,nullptr)!=SQLITE_OK)
        throw std::runtime_error("optional subject fixed definitions");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> query(raw,sqlite3_finalize);
    std::string drops,recreate,table;
    while(sqlite3_step(raw)==SQLITE_ROW){
        const std::string type(reinterpret_cast<const char*>(sqlite3_column_text(raw,0))),name(reinterpret_cast<const char*>(sqlite3_column_text(raw,1))),ddl(reinterpret_cast<const char*>(sqlite3_column_text(raw,2)));
        if(type=="table")table=ddl;
        else {drops+="DROP "+type+" "+name+";\n";recreate+=ddl+";\n";}
    }
    const std::string constraint="length(subject) BETWEEN 1 AND 70";
    const auto at=table.find(constraint);if(at==std::string::npos)throw std::runtime_error("optional subject predecessor constraint");
    table.replace(at,constraint.size(),"length(subject) BETWEEN 0 AND 70");
    const auto open=table.find('(');if(open==std::string::npos)throw std::runtime_error("optional subject predecessor table");
    table="CREATE TABLE sfx_messages23 "+table.substr(open);
    return drops+"CREATE TABLE sfx_subject_sequence23(seq INTEGER NOT NULL);\n"
        "INSERT INTO sfx_subject_sequence23 SELECT seq FROM sqlite_sequence WHERE name='sfx_local_messages';\n"+table+";\n"
        "INSERT INTO sfx_messages23 SELECT * FROM sfx_local_messages;\n"
        "DROP TABLE sfx_local_messages;\nALTER TABLE sfx_messages23 RENAME TO sfx_local_messages;\n"
        "DELETE FROM sqlite_sequence WHERE name IN('sfx_local_messages','sfx_messages23');\n"
        "INSERT INTO sqlite_sequence(name,seq) SELECT 'sfx_local_messages',seq FROM sfx_subject_sequence23;\n"
        "DROP TABLE sfx_subject_sequence23;\n"+recreate;
}();
std::vector<SfxBaselineSchemaObject> optional_subject_manifest(){
    static const auto manifest=[] {
    auto fixed=ftn_mail_predecessor();
    if(sqlite3_exec(fixed.get(),ftn_mail_migration_sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK ||
       sqlite3_exec(fixed.get(),optional_subject_migration_sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)
        throw std::runtime_error("optional subject fixed manifest");
    std::vector<SfxBaselineSchemaObject> changed;
    if(!collect_schema_manifest(fixed.get(),&changed))throw std::runtime_error("optional subject manifest collection");
    auto result=ftn_mail_manifest();
    for(auto& object:result){const auto found=std::find_if(changed.begin(),changed.end(),[&](const auto& candidate){return candidate.name==object.name;});
        if(found==changed.end())throw std::runtime_error("optional subject manifest missing predecessor object");
        object=*found;}
    return result;
    }();
    return manifest;
}
