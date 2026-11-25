// Migration19 adds settings only. Conversation bodies/queues are never durable.
const std::string chat_migration_sql=R"SQL(
CREATE TABLE sfx_chat_configuration (
 singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
 revision INTEGER NOT NULL CHECK(revision>=1),
 definition BLOB CHECK(definition IS NULL OR length(definition) BETWEEN 1 AND 32768)
) STRICT;
INSERT INTO sfx_chat_configuration VALUES(1,1,NULL);
CREATE TABLE sfx_chat_preferences (
 account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
 revision INTEGER NOT NULL CHECK(revision>=1),
 dnd INTEGER NOT NULL CHECK(dnd IN(0,1)),
 ghost INTEGER NOT NULL CHECK(ghost IN(0,1)),
 notices INTEGER NOT NULL CHECK(notices IN(0,1))
) STRICT;
CREATE TABLE sfx_chat_usage_exclusions (
 session_id BLOB PRIMARY KEY REFERENCES sfx_account_sessions(session_id)
) STRICT;
)SQL";
std::vector<SfxBaselineSchemaObject> chat_manifest(){
    auto result=door_program_manifest();sqlite3* raw=nullptr;
    if(sqlite3_open(":memory:",&raw)!=SQLITE_OK){if(raw)sqlite3_close(raw);throw std::runtime_error("Chat manifest database");}
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);
    if(sqlite3_exec(raw,chat_migration_sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("Chat manifest DDL");
    std::vector<SfxBaselineSchemaObject> objects;if(!collect_schema_manifest(raw,&objects))throw std::runtime_error("Chat manifest collection");
    result.insert(result.end(),objects.begin(),objects.end());return result;
}
// Migration20 retains the reviewed migration19 bytes. Only limits and elapsed
// counters are durable; private conversation text never enters these tables.
const std::string private_chat_migration_sql=R"SQL(
CREATE TABLE sfx_security_chat_limits (
 security_level INTEGER PRIMARY KEY REFERENCES sfx_security_levels(security_level),
 minutes_per_chat INTEGER CHECK(minutes_per_chat BETWEEN 1 AND 4294967295),
 minutes_per_day INTEGER CHECK(minutes_per_day BETWEEN 1 AND 4294967295)
) STRICT;
CREATE TABLE sfx_private_chat_seconds (
 session_id BLOB NOT NULL REFERENCES sfx_account_sessions(session_id) ON DELETE CASCADE,
 local_date TEXT NOT NULL CHECK(length(local_date)=10 AND local_date GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'),
 seconds INTEGER NOT NULL CHECK(seconds>=0),
 PRIMARY KEY(session_id,local_date)
) STRICT;
)SQL";
std::vector<SfxBaselineSchemaObject> private_chat_manifest(){
    auto result=chat_manifest();sqlite3* raw=nullptr;
    if(sqlite3_open(":memory:",&raw)!=SQLITE_OK){if(raw)sqlite3_close(raw);throw std::runtime_error("private Chat manifest database");}
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);
    if(sqlite3_exec(raw,private_chat_migration_sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("private Chat manifest DDL");
    std::vector<SfxBaselineSchemaObject> objects;if(!collect_schema_manifest(raw,&objects))throw std::runtime_error("private Chat manifest collection");
    result.insert(result.end(),objects.begin(),objects.end());return result;
}
