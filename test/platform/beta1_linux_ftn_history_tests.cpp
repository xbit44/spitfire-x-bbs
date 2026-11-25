// Real stopped Linux installation; all fault edits are confined to a new beta fixture.
#include "fixture_paths.hpp"
#include "sfx/platform/linux/core_restore.hpp"
#include "sfx/platform/linux/ftn_native_session.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include <sodium.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
namespace app=sfx::application;namespace host=sfx::platform::linux_host;namespace store=sfx::adapters::storage;namespace sql=store::internal;namespace fs=std::filesystem;
namespace {
unsigned checks{};
void check(bool yes,const char* why){++checks;if(!yes)throw std::runtime_error(why);}
template<class F>bool refused(F f){try{f();return false;}catch(const std::exception&){return true;}}
app::Identifier128 id(unsigned n){app::Identifier128 v;v.bytes[0]=0xb1;v.bytes[15]=static_cast<std::uint8_t>(n);return v;}
std::int64_t scalar(const fs::path& p,const char* q){sql::SqliteConnection db(p,SQLITE_OPEN_READONLY,std::chrono::milliseconds(500),false);return db.scalar_int64(q,"inspect synthetic native fixture");}
void edit(const fs::path& p,const char* q){sql::SqliteConnection db(p,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(500),false);db.exec(q,"explicit synthetic native fault");}
void write(const fs::path& p,std::string_view bytes){std::ofstream out(p,std::ios::binary);out<<bytes;out.close();check(bool(out)&&chmod(p.c_str(),0600)==0,"private synthetic file written");}
std::string read(const fs::path& p){std::ifstream in(p,std::ios::binary);check(bool(in),"read native fixture file");return {std::istreambuf_iterator<char>(in),{}};}
}
int main(int argc,char** argv)try{
 check((argc==2||argc==3)&&geteuid()==1001,"dedicated native test account and prepared board required");const fs::path root=argv[1];
 check(root==(sfx::test::fixture_account_home() / "sfx-tests/beta1-20261112-r1/installed-fresh-r1/board")||root==(sfx::test::fixture_account_home() / "sfx-tests/beta1-20261112-r1/installed-recovery-fresh-r2/board"),"only exact disposable beta boards");
 if(argc==3){check(std::string(argv[2])=="--diagnose-existing","explicit retained failure diagnostic only");auto recovery=host::preview_ftn_history_recovery(root);const auto result=recovery->execute(recovery->confirmation_token());std::cout<<"DIAGNOSTIC code="<<static_cast<unsigned>(result.status.code)<<" operation="<<result.status.operation<<" receipt="<<bool(result.receipt)<<" report="<<result.report_written<<'\n';return result.status?0:1;}
 const auto file=root/"Data/SFX.DB";host::LinuxLocalStoragePathPolicy paths;store::SqliteStorageSettings settings;settings.data_directory=root/"Data";
 const std::string bytes="exact synthetic pending FTN bytes\r\n";app::FtnOutboundJob pending;pending.id=id(1);pending.operation=id(2);pending.network="TEST";pending.link="PEER";pending.file={"pending.pkt",bytes.size(),1700000000};
 crypto_hash_sha256(pending.digest.data(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());const auto name=host::ftn_native_spool_name(pending);
 {
  host::InstallationLock lock;check(lock.acquire(root,".sfx-runtime.lock"),"hold native stopped installation");store::SqliteStorage db(settings,paths);check(bool(db.start()),"validated native store opens");
  auto config=*db.ftn_configuration({true,{}}).value;app::FtnNetwork network;network.code="TEST";network.name="Synthetic recovery";network.address="2:100/1@test";network.enabled=true;config.networks.push_back(network);
  app::FtnLink link;link.network="TEST";link.code="PEER";link.name="Offline synthetic peer";link.address="2:100/2@test";link.host="127.0.0.1";link.enabled=true;config.links.push_back(link);
  check(bool(db.change_ftn_configuration({true,{}},id(3).bytes,config)),"normal test network configured");config=*db.ftn_configuration({true,{}}).value;
  auto held=pending;held.id=id(4);held.operation=id(5);held.file.name="held.pkt";check(bool(db.queue_ftn_jobs({true,{}},config.revision,{pending,held})),"pending and separate held test jobs queued");
  check(bool(db.preserve_ftn_serial_history({true,{}},app::FtnSerialState{100,true,id(6)}))&&bool(db.shutdown()),"higher verified floor retained and writer closed");
  edit(file,"UPDATE sfx_ftn_outbound SET state=4,reason='expected_held' WHERE wire_name='held.pkt';UPDATE sfx_ftn_serial_allocator SET recovery_required=1");
  for(const auto& p:{root/"FTN/Outbound",root/"FTN/Outbound/TEST",root/"FTN/External"})if(!fs::exists(p))check(mkdir(p.c_str(),0700)==0,"private native spool folder");
 }
 check(refused([&]{(void)host::preview_ftn_history_recovery(root);}),"missing required pending bytes refuse recovery");
 check(scalar(file,"SELECT recovery_required FROM sfx_ftn_serial_allocator")==1&&scalar(file,"SELECT count(*) FROM sfx_ftn_serial_recoveries")==0,"failed preview retains hold with no receipt");
 {
  host::InstallationLock lock;check(lock.acquire(root,".sfx-runtime.lock"),"stopped owner available after refusal");auto bindings=host::LinuxFilePayloadBindings::open(root);auto directory=host::FilePayloadDirectory::private_store(root/"FTN/Outbound/TEST",bindings);
  {auto payload=directory.create(name,bytes.size());payload.append(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());payload.flush();check(payload.identity().sha256==pending.digest,"actual required payload exact");}bindings->refresh();
 }
 {auto preview=host::preview_ftn_history_recovery(root);check(preview->preview().pending_outbound==1&&preview->preview().held_outbound==1,"native preview separates expected held work");host::InstallationLock competitor;check(!competitor.acquire(root,".sfx-runtime.lock"),"preview excludes competing owner");check(!preview->execute("unconfirmed").receipt,"wrong confirmation cannot commit");}
 check(scalar(file,"SELECT recovery_required FROM sfx_ftn_serial_allocator")==1,"cancel leaves history held");const auto busy=root/"FTN/External/fixture.bsy";write(busy,"busy");
 check(refused([&]{(void)host::preview_ftn_history_recovery(root);}),"independent stock busy fence refused");check(fs::remove(busy),"remove only own synthetic fence");fs::path report;
 {auto recovery=host::preview_ftn_history_recovery(root);report=root/"Logs"/("FTN-Recovery-"+recovery->confirmation_token()+".log");write(report,"OWNER");const auto result=recovery->execute(recovery->confirmation_token());check(result.status&&result.receipt&&!result.report_written,"recovery commits with report conflict pending");check(result.receipt->old_floor==100&&result.receipt->new_floor==100&&result.receipt->held_outbound==1,"native durable receipt retains floor and held count");check(read(report)=="OWNER","conflict bytes preserved");}
 check(scalar(file,"SELECT recovery_required FROM sfx_ftn_serial_allocator")==0&&scalar(file,"SELECT count(*) FROM sfx_ftn_serial_recoveries")==1,"native recovery generation commits once");check(fs::remove(report),"remove only verified test report conflict");write(busy,"busy");
 {auto retry=host::preview_ftn_history_recovery(root);const auto result=retry->execute(retry->confirmation_token());check(result.status&&result.receipt&&result.report_written&&result.receipt->generation==1,"export-only retry succeeds despite later fence");}
 check(fs::remove(busy),"retire only own later fence");check(scalar(file,"SELECT count(*) FROM sfx_ftn_serial_recoveries")==1&&scalar(file,"SELECT count(*) FROM sfx_ftn_outbound")==2&&scalar(file,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=4 AND reason='expected_held'")==1,"all queue states retained and no second recovery");
 check(read(root/"FTN/Outbound/TEST"/name)==bytes,"required queued bytes unchanged");std::cout<<"PASS "<<checks<<" actual stopped Linux FTN recovery checks; board remains stopped\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
