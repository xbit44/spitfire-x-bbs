// UTC and configured local origination are verified from actual managed sources and native packets.
#define main rc18_retained_engine_main
#include "ftn_engine_tests.cpp"
#undef main
#include <sodium.h>
#include <fstream>
#include "sfx/platform/win32/bbs_log_timestamp.hpp"
namespace {
std::int64_t outbound_count(const std::filesystem::path& path){sfx::adapters::storage::internal::SqliteConnection db(path,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);sfx::adapters::storage::internal::SqliteStatement q(db.get(),"SELECT count(*) FROM sfx_ftn_outbound","observe immutable job count");check(q.row(),"outbound count");return q.int64(0);}
app::Identifier128 old_origin_id(const app::FtnScanIntent& intent,std::uint64_t revision){std::string binding=intent.network+"/PEER/"+std::to_string(revision);binding.append(reinterpret_cast<const char*>(intent.fingerprint.data()),intent.fingerprint.size());const auto bytes="SFX/FTNToss/v1/"+native::ftn_native_artifact_name(intent.operation)+"/0/"+binding;app::FtnDigest d{};check(crypto_hash_sha256(d.data(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size())==0,"derive retained legacy artifact fixture");app::Identifier128 out;std::copy_n(d.begin(),out.bytes.size(),out.bytes.begin());return out;}
std::string read_bytes(const native::FilePayloadDirectory& directory,std::string_view name){auto file=directory.read(std::string(name));const auto bytes=file.read(0,65536);return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};}
}
int main(int argc,char** argv)try{
 check(argc==2||argc==3,"new isolated timezone fixture required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve all previous fixtures");std::filesystem::create_directories(root);
 struct Example {std::int64_t utc;const char* date;const char* offset;};
 std::vector<Example> examples{{1768480496,"15 Jan 26  12:34:56","0000"},{1784118896,"15 Jul 26  12:34:56","0000"}};
 native::WindowsBbsLogTimestamp timezone;
 const std::string zone=argc==3?argv[2]:"";
 if(!zone.empty())check(timezone.configure(zone),"configure actual Windows zone rules");
 if(zone=="Pacific Standard Time")examples={{1768480496,"15 Jan 26  04:34:56","-0800"},
    {1784118896,"15 Jul 26  05:34:56","-0700"},
    {1772963999,"08 Mar 26  01:59:59","-0800"},{1772964000,"08 Mar 26  03:00:00","-0700"},
    {1793523599,"01 Nov 26  01:59:59","-0700"},{1793523600,"01 Nov 26  01:00:00","-0800"}};
 if(zone=="India Standard Time")examples={{1768480496,"15 Jan 26  18:04:56","0530"}};
 const auto* rules=zone.empty()?nullptr:&timezone;
 Fixture f(root/L"Server",1,2);f.ready();f.engine->stop();f.engine.reset();check(f.db->shutdown().succeeded(),"stop isolated store before controllable test clock");
 std::int64_t utc=1768480496;sfx::adapters::storage::SqliteStorageSettings settings;settings.data_directory=f.root/L"Data";settings.utc_now=[&]{return utc;};f.db=std::make_unique<sfx::adapters::storage::SqliteStorage>(settings,f.paths);check(f.db->start().succeeded(),"managed source store with deterministic UTC clock");
 app::FtnMessageEcho echo;echo.network="TEST";echo.tag="TIME.TEST";echo.description="Timezone test";echo.local_echo=2;f.config.message_echoes.push_back(echo);f.config.permissions.push_back({"TEST","PEER","TIME.TEST",false,true,true,false});f.save(62000);
 const app::MessageConfigurationActor owner{true,{}};const app::MessageActor alice{id(2),2,1,1};auto directory=native::FilePayloadDirectory::private_store(f.root/L"FTN/Outbound/TEST",f.receipt.owner);
 // A previously queued, legacy-named packet must remain byte exact and deliverable.
 app::FtnPacket old;old.header.origin=*app::parse_ftn_address("2:100/1@test");old.header.destination=*app::parse_ftn_address("2:100/2@test");old.header.year=2026;old.header.month=0;old.header.day=15;app::FtnPackedMessage old_message;old_message.origin_net=old_message.destination_net=100;old_message.origin_node=1;old_message.destination_node=2;old_message.date="15 Jan 26  11:00:00";old_message.from="Alice";old_message.to="Remote";old_message.subject="Legacy queued";old_message.text="\1MSGID: 2:100/1 00000001\rLegacy packet without timezone\r";old.messages.push_back(old_message);const auto old_bytes=app::encode_ftn_packet(old);check(old_bytes.has_value(),"valid retained old packet fixture");
 app::FtnOutboundJob old_job;old_job.id=id(62001);old_job.operation=id(62002);old_job.network="TEST";old_job.link="PEER";old_job.kind=app::FtnOutboundKind::packet;old_job.file={"old-queued.pkt",old_bytes->size(),static_cast<std::uint64_t>(utc)};const auto old_name=native::ftn_native_artifact_name(old_job.id);{auto out=directory.create(old_name,old_bytes->size());out.append(reinterpret_cast<const std::uint8_t*>(old_bytes->data()),old_bytes->size());old_job.digest=out.identity().sha256;}check(f.db->queue_ftn_jobs(owner,f.config.revision,{old_job}).succeeded(),"retain pre-update queued job and ACK identity");
 unsigned ordinal{};
 for(bool public_echo:{false,true})for(const auto& example:examples){
  utc=example.utc;app::MessageTarget target;if(public_echo){target.kind=app::MessageKind::public_echo;target.echo={2};}else{target.kind=app::MessageKind::private_ftn;target.remote=app::MessageRemoteTarget{"TEST","2:100/1@test","2:100/2@test","AreaFix"};}
  const auto context=f.db->begin_message(alice,target,{});check(bool(context),"ordinary echo/NetMail compose context");const auto saved=f.db->commit_message({alice,id(62100+ordinal),*context.value,"Timestamp subject unchanged","Timezone body unchanged\n"});check(bool(saved),"legitimate local source committed");
  const auto scans=f.db->pending_ftn_scan(owner,"TEST",0,16);check(scans&&scans.value->size()==1&&scans.value->front().created==example.utc,"authored UTC source timestamp retained");const auto intent=scans.value->front();
  std::string prefix_name;const auto prefix=old_bytes->substr(0,19);
  if(!ordinal){prefix_name=native::ftn_native_artifact_name(old_origin_id(intent,f.config.revision));{auto pending=directory.create(prefix_name,old_bytes->size());pending.append(reinterpret_cast<const std::uint8_t*>(prefix.data()),prefix.size());}
   unsigned calls{};check(native::scan_ftn_native_intent(*f.db,f.config,f.receipt,intent,[&]{return ++calls<3;},rules).deferred,"interrupted new publication remains retryable");}
  const auto published=native::scan_ftn_native_intent(*f.db,f.config,f.receipt,intent,[]{return true;},rules);if(!published.processed)std::cerr<<published.reason<<'\n';check(published.processed,"actual native packet publication");
  const auto before=outbound_count(f.root/L"Data/SFX.DB");check(native::scan_ftn_native_intent(*f.db,f.config,f.receipt,intent,[]{return true;},rules).processed&&outbound_count(f.root/L"Data/SFX.DB")==before,"uncertain retry retains one job and MSGID");
  const auto session=id(62200+ordinal),lease=id(62300+ordinal);const auto claimed=f.db->claim_ftn_jobs(owner,"TEST","PEER",session,lease,16,false);check(claimed&&claimed.value->size()==(!ordinal?2U:1U),"exact new and legacy queue claims");
  bool found{};for(const auto& job:*claimed.value){const auto bytes=read_bytes(directory,native::ftn_existing_spool_name(directory,job));if(job.id==old_job.id){check(bytes==*old_bytes&&job.digest==old_job.digest,"old queued wire bytes untouched");}else{found=true;const auto packet=app::decode_ftn_packet(bytes,"test");check(packet.packet&&packet.packet->messages.size()==1,"new packet decoded independently");const auto& m=packet.packet->messages.front();check(m.date==example.date,"wire date agrees with creation instant in configured zone");const auto at=m.text.find(std::string("\1TZUTC: ")+example.offset+'\r');check(at!=std::string::npos&&m.text.find("\1TZUTC:",at+1)==std::string::npos&&at<m.text.find("Timezone body unchanged"),"exactly one matching UTC offset before body");check(m.subject=="Timestamp subject unchanged"&&m.text.find("Timezone body unchanged\r")!=std::string::npos,"subject/body retained");const auto metadata=app::inspect_ftn_message(packet.packet->header,m);check(metadata&&metadata->msgid==intent.msgid&&metadata->reply==intent.reply,"MSGID/REPLY unchanged");if(!prefix_name.empty())check(job.id!=old_origin_id(intent,f.config.revision)&&read_bytes(directory,prefix_name)==prefix,"old interrupted artifact preserved under new publication namespace");}
   check(f.db->finish_ftn_job(owner,job.id,lease,app::FtnOutboundState::delivered,{}).succeeded(),"matched legacy/new ACK delivery retained");}
  check(found&&f.db->pending_ftn_scan(owner,"TEST",0,16).value->empty(),"only the intended source was consumed");++ordinal;
 }
 check(f.db->shutdown().succeeded(),"isolated store closed");std::cout<<"PASS "<<checks<<" origin/date/NetMail/EchoMail/retry/legacy/ACK checks for "<<(zone.empty()?"legacy UTC":zone)<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
