// Additive migration16. Accepted migration1..15 SQL remains immutable.
const std::string door_migration_sql = R"SQL(
CREATE TABLE sfx_door_policy (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT;
INSERT INTO sfx_door_policy VALUES(1,1);
CREATE TABLE sfx_door_definitions (
    door_id INTEGER PRIMARY KEY CHECK(door_id BETWEEN 1 AND 999),
    definition BLOB NOT NULL CHECK(length(definition) BETWEEN 1 AND 10000)
) STRICT;
CREATE TABLE sfx_door_menus (
    menu_id INTEGER PRIMARY KEY CHECK(menu_id BETWEEN 0 AND 999),
    name TEXT NOT NULL CHECK(length(CAST(name AS BLOB)) BETWEEN 1 AND 64)
) STRICT;
INSERT INTO sfx_door_menus VALUES(0,'Door Menu');
CREATE TABLE sfx_door_entries (
    menu_id INTEGER NOT NULL REFERENCES sfx_door_menus(menu_id),
    local_number INTEGER NOT NULL CHECK(local_number BETWEEN 1 AND 99),
    kind INTEGER NOT NULL CHECK(kind IN(0,1)),
    target_id INTEGER NOT NULL CHECK(target_id BETWEEN 1 AND 999),
    title TEXT NOT NULL CHECK(length(CAST(title AS BLOB)) BETWEEN 1 AND 64),
    description TEXT NOT NULL CHECK(length(CAST(description AS BLOB))<=256),
    PRIMARY KEY(menu_id,local_number)
) STRICT;
CREATE TABLE sfx_door_configuration_receipts (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    fingerprint BLOB NOT NULL CHECK(length(fingerprint)=32),
    actor_account_id INTEGER,
    revision INTEGER NOT NULL CHECK(revision>=2),
    action INTEGER NOT NULL CHECK(action BETWEEN 0 AND 6),
    identity INTEGER NOT NULL CHECK(identity BETWEEN 0 AND 999),
    created_utc_seconds INTEGER NOT NULL CHECK(created_utc_seconds>=0)
) STRICT;
)SQL";
std::vector<SfxBaselineSchemaObject> door_manifest() {
    auto result=file_manifest();
    sqlite3* raw=nullptr;
    if(sqlite3_open(":memory:",&raw)!=SQLITE_OK){if(raw)sqlite3_close(raw);throw std::runtime_error("Door manifest database");}
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);
    if(sqlite3_exec(raw,door_migration_sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("Door manifest DDL");
    std::vector<SfxBaselineSchemaObject> objects;
    if(!collect_schema_manifest(raw,&objects))throw std::runtime_error("Door manifest collection");
    result.insert(result.end(),objects.begin(),objects.end());return result;
}

// Additive migration17. Previously accepted migration SQL stays byte-exact.
const std::string door_security_migration_sql=R"SQL(
CREATE TABLE sfx_door_menu_requirements (
    menu_id INTEGER PRIMARY KEY REFERENCES sfx_door_menus(menu_id) ON DELETE CASCADE,
    access_mode INTEGER NOT NULL CHECK(access_mode BETWEEN 0 AND 2),
    access_level INTEGER NOT NULL CHECK(access_level BETWEEN 1 AND 999),
    access_sysop INTEGER NOT NULL CHECK(access_sysop IN(0,1)),
    execution_mode INTEGER NOT NULL CHECK(execution_mode BETWEEN 0 AND 2),
    execution_level INTEGER NOT NULL CHECK(execution_level BETWEEN 1 AND 999),
    execution_sysop INTEGER NOT NULL CHECK(execution_sysop IN(0,1))
) STRICT;
INSERT INTO sfx_door_menu_requirements SELECT menu_id,0,1,0,0,1,0 FROM sfx_door_menus;
)SQL";
std::vector<SfxBaselineSchemaObject> door_security_manifest(){
    auto result=door_manifest();sqlite3* raw=nullptr;
    if(sqlite3_open(":memory:",&raw)!=SQLITE_OK){if(raw)sqlite3_close(raw);throw std::runtime_error("Door requirements manifest database");}
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);
    if(sqlite3_exec(raw,(door_migration_sql+door_security_migration_sql).c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("Door requirements manifest DDL");
    std::vector<SfxBaselineSchemaObject> objects;if(!collect_schema_manifest(raw,&objects))throw std::runtime_error("Door requirements manifest collection");
    for(const auto& object:objects)if(object.name=="sfx_door_menu_requirements")result.push_back(object);
    return result;
}

// Migration18 changes only the bound of the versioned Door record. The entries
// table has polymorphic targets, not a foreign key to this replaced table.
const std::string door_program_migration_sql=R"SQL(
CREATE TABLE sfx_door_definitions_v3 (
    door_id INTEGER PRIMARY KEY CHECK(door_id BETWEEN 1 AND 999),
    definition BLOB NOT NULL CHECK(length(definition) BETWEEN 1 AND 32768)
) STRICT;
INSERT INTO sfx_door_definitions_v3 SELECT door_id,definition FROM sfx_door_definitions;
DROP TABLE sfx_door_definitions;
ALTER TABLE sfx_door_definitions_v3 RENAME TO sfx_door_definitions;
)SQL";
std::vector<SfxBaselineSchemaObject> door_program_manifest(){
    auto result=door_security_manifest();sqlite3* raw=nullptr;
    if(sqlite3_open(":memory:",&raw)!=SQLITE_OK){if(raw)sqlite3_close(raw);throw std::runtime_error("Door program manifest database");}
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);
    if(sqlite3_exec(raw,(door_migration_sql+door_security_migration_sql+door_program_migration_sql).c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("Door program manifest DDL");
    std::vector<SfxBaselineSchemaObject> objects;if(!collect_schema_manifest(raw,&objects))throw std::runtime_error("Door program manifest collection");
    for(const auto& object:objects)if(object.name=="sfx_door_definitions")for(auto& existing:result)if(existing.name==object.name)existing=object;
    return result;
}
