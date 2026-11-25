#include "sfx/application/qwk_storage.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>

namespace candidate {
#include "../../src/adapters/storage/src/sqlite_qwk_schema.inc"
}
namespace {
unsigned checks{};
void check(bool c,const char* why){++checks;if(!c)throw std::runtime_error(why);}
struct DbFree{void operator()(sqlite3* db)const{sqlite3_close(db);}};
struct StatementFree{void operator()(sqlite3_stmt* q)const{sqlite3_finalize(q);}};
using Db=std::unique_ptr<sqlite3,DbFree>;
void exec(sqlite3* db,const std::string& sql){if(sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));}
bool refuses(sqlite3* db,const std::string& sql){return sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK;}
std::int64_t scalar(sqlite3* db,const std::string& sql){sqlite3_stmt* raw=nullptr;if(sqlite3_prepare_v2(db,sql.c_str(),-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error(std::string("scalar prepare: ")+sqlite3_errmsg(db));std::unique_ptr<sqlite3_stmt,StatementFree> q(raw);if(sqlite3_step(raw)!=SQLITE_ROW)throw std::runtime_error(std::string("scalar row: ")+sqlite3_errmsg(db));return sqlite3_column_int64(raw,0);}
std::map<std::string,std::vector<std::string>> tables(sqlite3* db){
    std::map<std::string,std::vector<std::string>> result;sqlite3_stmt* raw=nullptr;
    if(sqlite3_prepare_v2(db,"SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sfx_qwk_%' ORDER BY name",-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("table list");
    std::unique_ptr<sqlite3_stmt,StatementFree> list(raw);
    while(sqlite3_step(raw)==SQLITE_ROW){const std::string name(reinterpret_cast<const char*>(sqlite3_column_text(raw,0)));sqlite3_stmt* rows=nullptr;
        const auto sql="SELECT * FROM \""+name+"\"";if(sqlite3_prepare_v2(db,sql.c_str(),-1,&rows,nullptr)!=SQLITE_OK)throw std::runtime_error("table read");
        std::unique_ptr<sqlite3_stmt,StatementFree> data(rows);auto& values=result[name];
        while(sqlite3_step(rows)==SQLITE_ROW){std::string row;
            for(int col=0;col<sqlite3_column_count(rows);++col){const auto type=sqlite3_column_type(rows,col);row+=static_cast<char>(type);
                std::string field;
                if(type==SQLITE_INTEGER)field=std::to_string(sqlite3_column_int64(rows,col));
                else if(type==SQLITE_FLOAT){const auto number=sqlite3_column_double(rows,col);field.assign(reinterpret_cast<const char*>(&number),sizeof(number));}
                else if(type==SQLITE_BLOB || type==SQLITE_TEXT){const auto count=sqlite3_column_bytes(rows,col);if(count)field.assign(static_cast<const char*>(sqlite3_column_blob(rows,col)),static_cast<std::size_t>(count));}
                row+=std::to_string(field.size());row+=':';row+=field;row+=';';
            }values.push_back(std::move(row));
        }std::sort(values.begin(),values.end());
    }return result;
}
}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Specify the approved stopped owned schema13 baseline; read-only clone only.");
    const auto source_uri=std::string("file:")+argv[1]+"?mode=ro&immutable=1";
    sqlite3* raw=nullptr;check(sqlite3_open_v2(source_uri.c_str(),&raw,SQLITE_OPEN_READONLY|SQLITE_OPEN_URI,nullptr)==SQLITE_OK,"owned stopped predecessor opened immutable read-only; no sidecar creation");Db source(raw);
    check(scalar(source.get(),"PRAGMA user_version")==13,"exact schema13 predecessor");
    check(sqlite3_open(":memory:",&raw)==SQLITE_OK,"disposable RAM database opened");Db db(raw);
    auto* copy=sqlite3_backup_init(db.get(),"main",source.get(),"main");check(copy!=nullptr,"read-only baseline backup initialized");
    const int copied=sqlite3_backup_step(copy,-1),finished=sqlite3_backup_finish(copy);check(copied==SQLITE_DONE && finished==SQLITE_OK,"exact in-memory SQLite clone");source.reset();
    exec(db.get(),"PRAGMA foreign_keys=ON");const auto before=tables(db.get());
    exec(db.get(),candidate::qwk_migration_sql);check(tables(db.get())==before,"all prior table row multisets unchanged by candidate14 SQL");
    check(scalar(db.get(),"SELECT count(*) FROM pragma_foreign_key_check")==0,"candidate foreign-key integrity");
    check(scalar(db.get(),"SELECT maximum_messages FROM sfx_qwk_policy")==10000,"approved combined-count seed");
    check(scalar(db.get(),"SELECT count(*) FROM sfx_qwk_policy WHERE word_wrap_default=0")==1,"explicit owner-selected Off seeded, not an unresolved default");
    check(scalar(db.get(),"SELECT length(board_id)=8 AND substr(board_id,1,3)='SFX' FROM sfx_qwk_policy")==1,"one stable safe board ID generated");
    check(scalar(db.get(),"SELECT count(*) FROM sfx_qwk_conference_mapping")==scalar(db.get(),"SELECT count(*) FROM sfx_local_echoes"),"existing stable echoes mapped without message/menu rewrite");
    check(scalar(db.get(),"SELECT count(*) FROM sfx_qwk_caller_preferences")==0,"migration doesn't overwrite/force caller preferences");
    for(const auto id:{"SYSOP","NETMAIL","CON","PRN","AUX","NUL","COM1","LPT9","CLOCK$","123","ABCDEFGHI","a","A.B","A/B","A B"})check(refuses(db.get(),std::string("UPDATE sfx_qwk_policy SET board_id='")+id+"'"),"SQL board-ID constraints enforce safe identity");
    for(const auto n:{"0","65536","-1","NULL"})check(refuses(db.get(),std::string("UPDATE sfx_qwk_policy SET maximum_messages=")+n),"SQL rejects count sentinels/overflow");
    const auto account=scalar(db.get(),"SELECT min(account_id) FROM sfx_accounts");check(account>0,"owned test account exists");const auto a=std::to_string(account);
    exec(db.get(),"INSERT INTO sfx_qwk_caller_preferences VALUES("+a+",0,0,0,1,0,1,1,1)");
    check(refuses(db.get(),"UPDATE sfx_qwk_caller_preferences SET encoding=1,full_headers=0"),"UTF8/header dependency enforced at storage boundary");
    exec(db.get(),"UPDATE sfx_qwk_caller_preferences SET encoding=1");check(scalar(db.get(),"SELECT encoding FROM sfx_qwk_caller_preferences")==1,"explicit valid preference preserved");
    const auto mapped=scalar(db.get(),"SELECT min(conference) FROM sfx_qwk_conference_mapping");
    check(refuses(db.get(),"DELETE FROM sfx_qwk_conference_mapping WHERE conference="+std::to_string(mapped)),"retired mappings cannot be deleted/reused");
    check(refuses(db.get(),"UPDATE sfx_qwk_conference_mapping SET echo_id=999999 WHERE conference="+std::to_string(mapped)),"old REP conference cannot be redirected");
    exec(db.get(),"INSERT INTO sfx_qwk_message_reference VALUES(1,1,zeroblob(16))");
    check(refuses(db.get(),"DELETE FROM sfx_qwk_message_reference"),"retired message references retained content-free");
    check(refuses(db.get(),"UPDATE sfx_qwk_message_reference SET message_id=2"),"classic reference identity immutable");
    check(refuses(db.get(),"INSERT INTO sfx_qwk_message_reference VALUES(10000000,2,randomblob(16))"),"classic number representability enforced");
    exec(db.get(),"INSERT INTO sfx_qwk_delivery_receipts VALUES(zeroblob(16),"+a+",zeroblob(32),zeroblob(32),16777216,67108864,65535,1,0)");
    check(refuses(db.get(),"UPDATE sfx_qwk_delivery_receipts SET delivered=1"),"delivery receipt immutable");
    check(refuses(db.get(),"DELETE FROM sfx_qwk_delivery_receipts"),"delivery reconciliation survives ordinary cleanup");
    check(refuses(db.get(),"INSERT INTO sfx_qwk_delivery_receipts VALUES(randomblob(16),"+a+",zeroblob(32),zeroblob(32),16777217,1,1,0,0)"),"compressed delivery cap checked durably");
    check(refuses(db.get(),"INSERT INTO sfx_qwk_delivery_receipts VALUES(randomblob(16),"+a+",zeroblob(32),zeroblob(32),1,67108865,1,0,0)"),"expanded delivery cap checked durably");
    exec(db.get(),"INSERT INTO sfx_qwk_rep_receipts VALUES("+a+",zeroblob(32),'SFX1',2,1,1,0,0)");
    check(refuses(db.get(),"INSERT INTO sfx_qwk_rep_receipts VALUES("+a+",zeroblob(32),'SFX1',2,1,1,0,0)"),"logical receipt unique within stable caller");
    check(refuses(db.get(),"UPDATE sfx_qwk_rep_receipts SET messages=2"),"REP receipt immutable");
    check(refuses(db.get(),"DELETE FROM sfx_qwk_rep_receipts"),"REP receipt durable");
    check(refuses(db.get(),"INSERT INTO sfx_qwk_rep_receipts VALUES("+a+",randomblob(32),'SFX1',2,0,1,0,0)"),"atomic receipt counts must account for every record");
    exec(db.get(),"BEGIN;INSERT INTO sfx_qwk_rep_receipts VALUES("+a+",randomblob(32),'SFX1',1,1,0,0,0)");
    check(scalar(db.get(),"SELECT count(*) FROM sfx_qwk_rep_receipts")==2,"receipt staged inside one transaction");exec(db.get(),"ROLLBACK");
    check(scalar(db.get(),"SELECT count(*) FROM sfx_qwk_rep_receipts")==1,"rollback leaves no partial receipt");
    check(scalar(db.get(),"SELECT count(*) FROM pragma_foreign_key_check")==0,"final candidate foreign keys clean");
    std::cout<<"PASS qwk schema "<<checks<<" checks; RAM clone of schema13, not shipping migration/import acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL qwk schema after "<<checks<<": "<<e.what()<<'\n';return 1;}}
