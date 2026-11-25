// Additive migration21; accepted migration1-20 byte strings remain unchanged.
const std::string ftn_migration_sql=R"SQL(
CREATE TABLE sfx_ftn_configuration (
 singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
 revision INTEGER NOT NULL CHECK(revision>=1),
 definition BLOB CHECK(definition IS NULL OR length(definition) BETWEEN 1 AND 4194304)
) STRICT;
INSERT INTO sfx_ftn_configuration VALUES(1,1,NULL);
CREATE TABLE sfx_ftn_configuration_receipts (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
 request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
 result_revision INTEGER NOT NULL CHECK(result_revision>=2)
) STRICT;
)SQL";
std::vector<SfxBaselineSchemaObject> ftn_manifest(){
    auto result=private_chat_manifest();sqlite3* raw=nullptr;
    if(sqlite3_open(":memory:",&raw)!=SQLITE_OK){if(raw)sqlite3_close(raw);throw std::runtime_error("FTN manifest database");}
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);
    if(sqlite3_exec(raw,ftn_migration_sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("FTN manifest DDL");
    std::vector<SfxBaselineSchemaObject> objects;if(!collect_schema_manifest(raw,&objects))throw std::runtime_error("FTN manifest collection");
    result.insert(result.end(),objects.begin(),objects.end());return result;
}
