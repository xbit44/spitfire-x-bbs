// Actual native sealed file + production writer, trusted synthetic actors.
// No installed Runtime/session/Telnet/SSH/reader acceptance is claimed.
#include "alpha_qwk_reply_spool.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include <iostream>
#include <algorithm>
#include <stdexcept>

namespace app=sfx::application;
namespace db=sfx::adapters::storage;
namespace internal=db::internal;
namespace inst=sfx::platform::win32::installation;
namespace {
unsigned checks{};
void check(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
app::Identifier128 id(unsigned n){app::Identifier128 r;r.bytes[0]=0x8d;r.bytes[15]=static_cast<std::uint8_t>(n);return r;}
std::int64_t scalar(const std::filesystem::path& file,const char* sql){internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);return c.scalar_int64(sql,"native owned spool invariant");}
std::string reply(std::uint16_t conference,std::string to,std::string subject){
    std::string record(256,' ');const auto field=[&](std::size_t at,std::size_t n,std::string_view value){record.replace(at,std::min(n,value.size()),value.substr(0,n));};
    record[0]=conference?' ':'*';field(1,7,std::to_string(conference));field(8,8,"10-13-26");field(16,5,"14:00");field(21,25,to);field(46,25,"Bob");field(71,25,subject);field(116,6,"2");record[122]=static_cast<char>(0xe1);record[123]=static_cast<char>(conference);record[124]=static_cast<char>(conference>>8);field(128,128,"Native spool body");record[145]=static_cast<char>(0xe3);return record;
}
app::QwkReplyPacket decoded(std::string board,std::string subject="Native spool"){
    std::string first(128,' ');first.replace(0,board.size(),board);auto result=app::decode_qwk_reply(board,{{board+".MSG",first+reply(2,"All Callers",subject)+reply(0,"Alice",subject)}});
    check(static_cast<bool>(result)&&result.records.size()==2,"actual codec validates synthetic REP before native sealing");return result;
}
}
int main(int argc,char** argv){try{
    check(argc==2,"new native fixture parent required");const auto parent=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(parent);
    const auto root=parent/inst::unique_id();const auto owner=inst::operator_sid();const auto security=inst::mutable_descriptor(owner);inst::create_directory(root,security.get());
    const auto job=[&](const wchar_t* name){const auto path=root/name;inst::create_directory(path,security.get());return path;};
    db::SqliteStorageSettings settings;settings.data_directory=root/L"Data";inst::create_directory(settings.data_directory,security.get());settings.utc_now=[]{return std::int64_t{1791920000};};sfx::test::FakeStoragePathPolicy path_policy;db::SqliteStorage storage(settings,path_policy);
    const auto startup=storage.start();if(!startup)std::cerr<<"Native fixture startup: "<<startup.operation<<'\n';
    check(startup.succeeded()&&storage.shutdown().succeeded(),"native test production migration then stopped synthetic setup");const auto database=settings.data_directory/L"SFX.DB";
    {
        internal::SqliteConnection c(database,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        c.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(1,'Sysop','sysop',999,'active',1,1,1700000000,1700000000),(2,'Alice','alice',10,'active',1,1,1700000000,1700000000),(3,'Bob','bob',10,'active',1,1,1700000000,1700000000);INSERT INTO sfx_account_statistics(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_profiles(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_name_reservations SELECT name_key,account_id,1,display_name,1700000000,NULL FROM sfx_accounts;UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash;UPDATE sfx_installation_state SET staged_sysop_account_id=1,setup_state='complete',completed_utc_seconds=1700000000","trusted synthetic native fixture");
        internal::SqliteStatement q(c.get(),"INSERT INTO sfx_account_sessions VALUES(?1,3,3,1700000000,0,1700000000,NULL,NULL,?1)","trusted synthetic Bob session");q.bind(1,id(3));q.step();
    }
    check(storage.start().succeeded(),"native populated writer restart");const app::MessageActor bob{id(3),3,1,1};const auto policy=storage.qwk_caller_policy(bob);check(static_cast<bool>(policy),"ordinary native test board identity");
    auto packet=decoded(policy.value->board_id);const auto first_job=job(L"success");
    const auto pinned_job=job(L"host-pinned");auto pinned_directory=inst::open_object(pinned_job,true,DELETE|FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
    const auto attribute_job=job(L"attribute-unconfirmed");auto attribute_reopen=sfx::apps::runtime::seal_qwk_reply_spool(attribute_job,owner,packet,512,4096);
    check(attribute_reopen&&(*attribute_reopen.value)->verify_identity().succeeded(),"native Windows permits this directory read-attributes reopen; sharing conflict is not inferred");
    attribute_reopen.value.reset();check(std::filesystem::exists(attribute_job/L"reply.spool"),"unconfirmed factory-only source is retained when its last reader closes");
    auto pinned_source=sfx::apps::runtime::seal_qwk_reply_spool(pinned_job,owner,packet,512,4096,{},nullptr,pinned_directory.get());
    check(pinned_source&&(*pinned_source.value)->verify_identity().succeeded(),"host DELETE directory pin duplicates without broadening sharing");
    check(!MoveFileExW(pinned_job.c_str(),(root/L"moved-host-job").c_str(),0),"duplicated host job remains unrenameable");pinned_source.value.reset();
    check(std::filesystem::exists(pinned_job/L"reply.spool"),"duplicated directory source preserves unconfirmed import input");pinned_directory.reset();
    auto sealed=sfx::apps::runtime::seal_qwk_reply_spool(first_job,owner,packet,512,4096);if(!sealed)std::cerr<<"Native seal: "<<sealed.status.operation<<'\n';check(sealed&&sealed.value,"actual protected native spool sealed read-only");auto source=*sealed.value;
    check(source->verify_identity().succeeded()&&source->rewind().succeeded(),"actual immutable file/header readback");
    const auto one=source->next(),two=source->next(),end=source->next();check(one&&one.value->has_value()&&two&&two.value->has_value()&&end&&!end.value->has_value()&&one.value->value().message.body==packet.records[0].message.body,"actual framed fields/exact EOF preserved");
    inst::Handle writer(CreateFileW((first_job/L"reply.spool").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));const auto write_error=GetLastError();
    check(writer.get()==INVALID_HANDLE_VALUE&&write_error==ERROR_SHARING_VIOLATION,"actual sealed source denies external write sharing");writer.reset();
    check(!MoveFileExW((first_job/L"reply.spool").c_str(),(first_job/L"moved.spool").c_str(),0),"actual source file cannot be renamed during ownership");
    check(!MoveFileExW(first_job.c_str(),(root/L"moved-job").c_str(),0),"actual source private directory remains pinned");
    auto imported=storage.import_qwk_reply(bob,source);if(!imported)std::cerr<<"Native import: "<<imported.status.operation<<'\n';
    check(imported&&imported.value->messages==2&&!imported.value->replayed,"actual sealed native source reaches one atomic production import");
    check(scalar(database,"SELECT count(*) FROM sfx_local_messages")==2&&scalar(database,"SELECT count(*) FROM sfx_qwk_rep_receipts")==1&&scalar(database,"SELECT count(*) FROM sfx_message_arrival_intents")==1,"actual file import messages/receipt/private arrival agree");
    check(storage.import_qwk_reply(bob,source).value->replayed&&scalar(database,"SELECT count(*) FROM sfx_local_messages")==2,"actual native source replay does not duplicate");
    check(storage.checkpoint().succeeded(),"writer barrier releases completed import captures before cleanup assertion");
    source->confirm_completed();
    source.reset();sealed.value.reset();check(!std::filesystem::exists(first_job/L"reply.spool"),"last sealed source closes/deletes exact owned temporary file");
    auto altered=decoded(policy.value->board_id,"Altered spool");altered.records[1].message.body+="mutated after codec";const auto bad_job=job(L"bad-logical");auto bad_source=sfx::apps::runtime::seal_qwk_reply_spool(bad_job,owner,altered,512,4096);
    check(bad_source&&!storage.import_qwk_reply(bob,*bad_source.value)&&scalar(database,"SELECT count(*) FROM sfx_local_messages")==2&&scalar(database,"SELECT count(*) FROM sfx_qwk_rep_receipts")==1,"actual sealed mismatched logical digest rejects entire batch before writes");bad_source.value.reset();check(std::filesystem::exists(bad_job/L"reply.spool"),"failed import preserves sealed input for review");
    bool revoked=false;const auto cancel_job=job(L"cancel");auto cancel_source=sfx::apps::runtime::seal_qwk_reply_spool(cancel_job,owner,packet,512,4096,[&]{return revoked;});check(static_cast<bool>(cancel_source),"actual cancellation-linked source sealed");revoked=true;
    check(!storage.import_qwk_reply(bob,*cancel_source.value)&&scalar(database,"SELECT count(*) FROM sfx_local_messages")==2,"actual host revocation cancels sealed source without posts");cancel_source.value.reset();check(std::filesystem::exists(cancel_job/L"reply.spool"),"revoked/unconfirmed import preserves its sealed input");
    const auto immediate_job=job(L"pre-cancel");check(!sfx::apps::runtime::seal_qwk_reply_spool(immediate_job,owner,packet,512,4096,[]{return true;})&&!std::filesystem::exists(immediate_job/L"reply.spool"),"cancel before creation leaves no spool");
    unsigned cancel_calls{};const auto partial_job=job(L"partial-seal");const auto partial=sfx::apps::runtime::seal_qwk_reply_spool(partial_job,owner,packet,512,4096,[&]{return ++cancel_calls==8;});
    check(!partial&&std::filesystem::exists(partial_job/L"reply.spool")&&!inst::read_file(partial_job/L"reply.spool",8192).empty(),"cancelled partial sealing retains exact newly written evidence");
    const auto existing_job=job(L"existing");inst::create_file(existing_job/L"reply.spool",{'K','E','E','P'},security.get());
    check(!sfx::apps::runtime::seal_qwk_reply_spool(existing_job,owner,packet,512,4096)&&inst::read_file(existing_job/L"reply.spool",4)==std::vector<std::uint8_t>({'K','E','E','P'}),"create-new refuses and preserves unknown pre-existing spool");
    const auto oversize_job=job(L"oversize");check(!sfx::apps::runtime::seal_qwk_reply_spool(oversize_job,owner,packet,app::kQwkCompressedBytes+1,4096)&&!std::filesystem::exists(oversize_job/L"reply.spool"),"native factory cannot bypass packet byte bounds");
    check(storage.shutdown().succeeded(),"actual native component writer stopped");std::cout<<"FIXTURE "<<root.string()<<"\nPASS "<<checks<<" native sealed-spool checks; NOT installed Runtime/Telnet/SSH/NCM/owner acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native spool after "<<checks<<": "<<e.what()<<'\n';return 1;}}
