// Qualification of the EXISTING pinned SQLite, not a dependency upgrade.
#include <sqlite3.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
namespace {
unsigned checks{};void check(bool v,const char* what){++checks;if(!v)throw std::runtime_error(what);}
void sql(sqlite3* db,const char* statement){char* error{};const auto rc=sqlite3_exec(db,statement,nullptr,nullptr,&error);const auto message=error?std::string(error):std::string{};sqlite3_free(error);if(rc!=SQLITE_OK)throw std::runtime_error(message);++checks;}
std::string value(sqlite3* db,const char* statement){sqlite3_stmt* raw{};check(sqlite3_prepare_v2(db,statement,-1,&raw,nullptr)==SQLITE_OK,"probe query prepares");std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> q(raw,sqlite3_finalize);check(sqlite3_step(raw)==SQLITE_ROW,"probe query has row");const auto* bytes=sqlite3_column_text(raw,0);return bytes?reinterpret_cast<const char*>(bytes):"";}
}
int main(){try{
    check(std::string(sqlite3_libversion())=="3.53.4","actual approved SQLite version");sqlite3* raw{};check(sqlite3_open(":memory:",&raw)==SQLITE_OK,"isolated memory database");std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    sql(raw,"PRAGMA foreign_keys=ON; CREATE TABLE accounts(id INTEGER PRIMARY KEY) STRICT; INSERT INTO accounts VALUES(1); CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT,author INTEGER NOT NULL REFERENCES accounts(id),body BLOB NOT NULL,parent INTEGER REFERENCES messages(id),UNIQUE(id,author)) STRICT; CREATE TABLE sources(message INTEGER PRIMARY KEY REFERENCES messages(id),body BLOB NOT NULL) STRICT; CREATE INDEX author_lookup ON messages(author); CREATE TRIGGER immutable_message BEFORE UPDATE ON messages BEGIN SELECT RAISE(ABORT,'immutable'); END; INSERT INTO messages VALUES(10,1,x'616263',NULL); INSERT INTO sources VALUES(10,x'616263'); UPDATE sqlite_sequence SET seq=1000 WHERE name='messages';");
    const auto trigger=value(raw,"SELECT sql FROM sqlite_schema WHERE name='immutable_message'");const auto index=value(raw,"SELECT sql FROM sqlite_schema WHERE name='author_lookup'");
    sql(raw,"BEGIN IMMEDIATE; ALTER TABLE messages ALTER author DROP NOT NULL;");
    check(value(raw,"SELECT [notnull] FROM pragma_table_info('messages') WHERE name='author'")=="0","pinned library supports official DROP NOT NULL");
    check(value(raw,"PRAGMA foreign_keys")=="1","FK enforcement stays enabled inside migration");sql(raw,"ROLLBACK");
    check(value(raw,"SELECT [notnull] FROM pragma_table_info('messages') WHERE name='author'")=="1","failed migration restores original constraint");
    sql(raw,"BEGIN IMMEDIATE; ALTER TABLE messages ALTER author DROP NOT NULL; COMMIT;");
    check(value(raw,"PRAGMA foreign_keys")=="1","FK enforcement enabled after commit");
    check(value(raw,"SELECT seq FROM sqlite_sequence WHERE name='messages'")=="1000","sequence high-water untouched");
    check(value(raw,"SELECT hex(body) FROM messages WHERE id=10")=="616263"&&value(raw,"SELECT hex(body) FROM sources WHERE message=10")=="616263","parent and referencing content preserved");
    check(value(raw,"SELECT sql FROM sqlite_schema WHERE name='immutable_message'")==trigger&&value(raw,"SELECT sql FROM sqlite_schema WHERE name='author_lookup'")==index,"existing trigger/index definitions unchanged");
    check(sqlite3_exec(raw,"INSERT INTO messages(author,body) VALUES(999,x'00')",nullptr,nullptr,nullptr)==SQLITE_CONSTRAINT,"local missing-account FK still rejects");
    sql(raw,"INSERT INTO messages(author,body) VALUES(NULL,x'00')");check(value(raw,"SELECT id FROM messages WHERE author IS NULL")=="1001","remote nullable author without caller impersonation or ID reuse");
    check(value(raw,"SELECT count(*) FROM pragma_foreign_key_check")=="0","full FK consistency");
    check(sqlite3_exec(raw,"UPDATE messages SET body=x'01' WHERE id=10",nullptr,nullptr,nullptr)==SQLITE_CONSTRAINT,"existing immutable-message trigger still enforces");
    std::cout<<"PASS "<<checks<<" pinned SQLite3.53.4 nullable-author migration probe checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
