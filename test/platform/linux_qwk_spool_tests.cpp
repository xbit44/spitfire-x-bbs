// Actual native sealed file + production writer, trusted synthetic actors.
// No installed Runtime/session/Telnet/SSH/reader acceptance is claimed.
#include "linux_qwk_reply_spool.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include <iostream>
#include <algorithm>
#include <stdexcept>

namespace app=sfx::application;
namespace db=sfx::adapters::storage;
namespace internal=db::internal;
namespace host=sfx::platform::linux_host;
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
    check(geteuid()!=0,"native qualification runs as normal user");
    host::LinuxSecureIdentifierGenerator identifiers;const auto random=identifiers.generate();
    std::string name="qwk-";for(auto byte:random.bytes){const char* hex="0123456789abcdef";name+=hex[byte>>4];name+=hex[byte&15];}
    const auto root=parent/name;check(std::filesystem::create_directory(root)&&chmod(root.c_str(),0700)==0,"new private fixture");
    const auto job=[&](const wchar_t* name){const auto path=root/name;check(std::filesystem::create_directory(path)&&chmod(path.c_str(),0700)==0,"new private packet job");return path;};
    db::SqliteStorageSettings settings;settings.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;settings.data_directory=root/L"data";check(std::filesystem::create_directory(settings.data_directory)&&chmod(settings.data_directory.c_str(),0700)==0,"private database directory");settings.utc_now=[]{return std::int64_t{1791920000};};host::LinuxLocalStoragePathPolicy path_policy;db::SqliteStorage storage(settings,path_policy);
    const auto startup=storage.start();if(!startup)std::cerr<<"Native fixture startup: "<<startup.operation<<'\n';
    check(startup.succeeded()&&storage.shutdown().succeeded(),"native test production migration then stopped synthetic setup");const auto database=settings.data_directory/L"sfx.db";
    {
        internal::SqliteConnection c(database,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        c.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(1,'Sysop','sysop',999,'active',1,1,1700000000,1700000000),(2,'Alice','alice',10,'active',1,1,1700000000,1700000000),(3,'Bob','bob',10,'active',1,1,1700000000,1700000000);INSERT INTO sfx_account_statistics(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_profiles(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_name_reservations SELECT name_key,account_id,1,display_name,1700000000,NULL FROM sfx_accounts;UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash;UPDATE sfx_installation_state SET staged_sysop_account_id=1,setup_state='complete',completed_utc_seconds=1700000000","trusted synthetic native fixture");
        internal::SqliteStatement q(c.get(),"INSERT INTO sfx_account_sessions VALUES(?1,3,3,1700000000,0,1700000000,NULL,NULL,?1)","trusted synthetic Bob session");q.bind(1,id(3));q.step();
    }
    check(storage.start().succeeded(),"native populated writer restart");const app::MessageActor bob{id(3),3,1,1};const auto policy=storage.qwk_caller_policy(bob);check(static_cast<bool>(policy),"ordinary native test board identity");
    auto packet=decoded(policy.value->board_id);const auto first_job=job(L"success");
    const auto pinned_job=job(L"host-pinned");auto pinned_directory=host::open_owned_directory(pinned_job);
    auto pinned_source=sfx::apps::runtime::seal_linux_qwk_reply_spool(pinned_job,packet,512,4096,{},nullptr,pinned_directory.get());
    check(pinned_source&&(*pinned_source.value)->verify_identity().succeeded(),"same native host directory pin accepted");
    pinned_source.value.reset();check(std::filesystem::exists(pinned_job/"reply.spool"),"unconfirmed pinned source preserved");
    const auto wrong_job=job(L"wrong-pin");
    check(!sfx::apps::runtime::seal_linux_qwk_reply_spool(wrong_job,packet,512,4096,{},nullptr,pinned_directory.get())&&!std::filesystem::exists(wrong_job/"reply.spool"),"different directory pin refuses before creating a file");
    auto sealed=sfx::apps::runtime::seal_linux_qwk_reply_spool(first_job,packet,512,4096);if(!sealed)std::cerr<<"Native seal: "<<sealed.status.operation<<'\n';check(sealed&&sealed.value,"actual protected native spool sealed read-only");auto source=*sealed.value;
    check(source->verify_identity().succeeded()&&source->rewind().succeeded(),"actual immutable file/header readback");
    const auto one=source->next(),two=source->next(),end=source->next();check(one&&one.value->has_value()&&two&&two.value->has_value()&&end&&!end.value->has_value()&&one.value->value().message.body==packet.records[0].message.body,"actual framed fields/exact EOF preserved");
    struct stat sealed_mode{};check(stat((first_job/"reply.spool").c_str(),&sealed_mode)==0&&(sealed_mode.st_mode&07777)==0400,"sealed spool read-only to normal account");
    host::FileDescriptor writer(open((first_job/"reply.spool").c_str(),O_WRONLY|O_CLOEXEC));
    check(!writer,"normal account cannot reopen sealed file for writing without an observable permission change");
    auto imported=storage.import_qwk_reply(bob,source);if(!imported)std::cerr<<"Native import: "<<imported.status.operation<<'\n';
    check(imported&&imported.value->messages==2&&!imported.value->replayed,"actual sealed native source reaches one atomic production import");
    check(scalar(database,"SELECT count(*) FROM sfx_local_messages")==2&&scalar(database,"SELECT count(*) FROM sfx_qwk_rep_receipts")==1&&scalar(database,"SELECT count(*) FROM sfx_message_arrival_intents")==1,"actual file import messages/receipt/private arrival agree");
    check(storage.import_qwk_reply(bob,source).value->replayed&&scalar(database,"SELECT count(*) FROM sfx_local_messages")==2,"actual native source replay does not duplicate");
    check(storage.checkpoint().succeeded(),"writer barrier releases completed import captures before cleanup assertion");
    source->confirm_completed();
    source.reset();sealed.value.reset();check(!std::filesystem::exists(first_job/L"reply.spool"),"last sealed source closes/deletes exact owned temporary file");
    auto altered=decoded(policy.value->board_id,"Altered spool");altered.records[1].message.body+="mutated after codec";const auto bad_job=job(L"bad-logical");auto bad_source=sfx::apps::runtime::seal_linux_qwk_reply_spool(bad_job,altered,512,4096);
    check(bad_source&&!storage.import_qwk_reply(bob,*bad_source.value)&&scalar(database,"SELECT count(*) FROM sfx_local_messages")==2&&scalar(database,"SELECT count(*) FROM sfx_qwk_rep_receipts")==1,"actual sealed mismatched logical digest rejects entire batch before writes");bad_source.value.reset();check(std::filesystem::exists(bad_job/L"reply.spool"),"failed import preserves sealed input for review");
    bool revoked=false;const auto cancel_job=job(L"cancel");auto cancel_source=sfx::apps::runtime::seal_linux_qwk_reply_spool(cancel_job,packet,512,4096,[&]{return revoked;});check(static_cast<bool>(cancel_source),"actual cancellation-linked source sealed");revoked=true;
    check(!storage.import_qwk_reply(bob,*cancel_source.value)&&scalar(database,"SELECT count(*) FROM sfx_local_messages")==2,"actual host revocation cancels sealed source without posts");cancel_source.value.reset();check(std::filesystem::exists(cancel_job/L"reply.spool"),"revoked/unconfirmed import preserves its sealed input");
    const auto immediate_job=job(L"pre-cancel");check(!sfx::apps::runtime::seal_linux_qwk_reply_spool(immediate_job,packet,512,4096,[]{return true;})&&!std::filesystem::exists(immediate_job/L"reply.spool"),"cancel before creation leaves no spool");
    unsigned cancel_calls{};const auto partial_job=job(L"partial-seal");const auto partial=sfx::apps::runtime::seal_linux_qwk_reply_spool(partial_job,packet,512,4096,[&]{return ++cancel_calls==8;});
    check(!partial&&std::filesystem::exists(partial_job/L"reply.spool")&&!host::read_private_file(host::open_owned_directory(partial_job).get(),"reply.spool",8192).empty(),"cancelled partial sealing retains exact newly written evidence");
    const auto existing_job=job(L"existing");host::publish_private_file(host::open_owned_directory(existing_job).get(),"reply.spool",{'K','E','E','P'},false);
    check(!sfx::apps::runtime::seal_linux_qwk_reply_spool(existing_job,packet,512,4096)&&host::read_private_file(host::open_owned_directory(existing_job).get(),"reply.spool",4)==std::vector<std::uint8_t>({'K','E','E','P'}),"create-new refuses and preserves unknown pre-existing spool");
    const auto oversize_job=job(L"oversize");check(!sfx::apps::runtime::seal_linux_qwk_reply_spool(oversize_job,packet,app::kQwkCompressedBytes+1,4096)&&!std::filesystem::exists(oversize_job/L"reply.spool"),"native factory cannot bypass packet byte bounds");
    #include "linux_qwk_spool_checks.inc"
    check(storage.shutdown().succeeded(),"actual native component writer stopped");std::cout<<"FIXTURE "<<root.string()<<"\nPASS "<<checks<<" native sealed-spool checks; NOT installed Runtime/Telnet/SSH/NCM/owner acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native spool after "<<checks<<": "<<e.what()<<'\n';return 1;}}
