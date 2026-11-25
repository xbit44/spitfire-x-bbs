#include "linux_runtime_host.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <algorithm>
#include <iostream>
#include <unistd.h>
namespace app=sfx::application;namespace rt=sfx::apps::runtime;namespace native=sfx::platform::linux_host;
namespace {
unsigned checks{};
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Clock final:app::IClock{
 std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
 std::uint64_t monotonic_millis()const noexcept override{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
};
struct Revocations final:app::ISessionRevocationSink{bool revoke(const app::SessionRevocationCommand&)override{return true;}};
}
int main(int argc,char** argv){try{
 check(argc==4&&geteuid()!=0,"prepared private root timezone and test mode required");const std::filesystem::path root=argv[1];
 check(native::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"time rules initialized");
 if(std::string_view(argv[3])=="seed-messages"){
  Clock clock;Revocations revocations;rt::LinuxRuntimeStore store(root,revocations,clock,[]{return false;});check(store.open().code==app::LocalControlResponseCode::ok,"message seed opens existing stopped fixture");
  auto& database=*store.storage();const auto policy=database.security_policy();check(policy&&policy.value,"message seed policy");
  std::vector<app::MessageActor> actors;
  for(unsigned account=1;account<=2;++account){const auto record=database.find_account_by_id(account);check(record&&record.value,"prepared seed account exists");app::StartSessionRequest start;start.session_id=store.identifiers().generate();start.correlation_id=store.identifiers().generate();start.account_id=account;start.node_id=static_cast<std::uint16_t>(account);start.started_utc_seconds=clock.unix_seconds();check(database.record_session_started(start).succeeded(),"synthetic test session bookkeeping, not caller-authentication proof");actors.push_back({start.session_id,account,record.value->revision,policy.value->revision});}
  const auto post=[&](const app::MessageActor& actor,const app::MessageTarget& target){const auto context=database.begin_message(actor,target);check(context&&context.value,"shared compose context");app::CommitMessageRequest request{actor,store.identifiers().generate(),*context.value,"Deletion fixture","Retain exact message body\n"};const auto message=database.commit_message(request);check(message&&message.value,"shared message commit");return message.value->id;};
  const auto protected_message=post(actors[1],{app::MessageKind::public_echo,{2},{},{}});post(actors[1],{app::MessageKind::public_echo,{2},{},{}});
  check(database.set_message_permanent(actors[0],store.identifiers().generate(),protected_message,false,true).succeeded(),"selected message marked Permanent by shared authorization");
  post(actors[0],{app::MessageKind::public_echo,{1},{},{}});post(actors[1],{app::MessageKind::private_local,{},1,{}});
  for(const auto& actor:actors){app::FinalizeSessionRequest finish;finish.session_id=actor.session;finish.completed_utc_seconds=clock.unix_seconds();finish.audit_event_id=store.identifiers().generate();check(database.finalize_session(finish).succeeded(),"synthetic session finalized before stopped deletion");}
  check(store.close_for_maintenance().code==app::LocalControlResponseCode::ok,"message seed releases storage and installation ownership");
 }else if(std::string_view(argv[3])=="seed-file-deletion"){
  Clock clock;Revocations revocations;rt::LinuxRuntimeStore store(root,revocations,clock,[]{return false;});check(store.open().code==app::LocalControlResponseCode::ok,"file deletion seed opens existing stopped fixture");
  auto& database=*store.storage();const auto folder=root/"files/ResumeDisk";check(std::filesystem::create_directory(folder),"new owned deletion fixture folder");std::filesystem::permissions(folder,std::filesystem::perms::owner_all);
  native::LinuxFileConfigurationPaths paths(root,store.file_bindings(),true,[](native::FileGroupDeletionBoundary boundary){if(boundary==native::FileGroupDeletionBoundary::before_retire)throw std::runtime_error("owned fixture interruption after disk deletion");});
  app::LocalFileAdministration admin(database,app::LocalAccountAuthority::installation_owner,&paths);
  app::FileConfigurationChange change;change.operation=store.identifiers().generate();change.action=app::FileConfigurationAction::group;change.expected_catalog_revision=database.file_configuration({true,{}}).value->policy.catalog_revision;change.group.name="Resume Disk";
  const auto group=admin.execute(change);check(group.code==app::LocalAccountResultCode::ok,"seed group through shared native administration");
  change={};change.operation=store.identifiers().generate();change.action=app::FileConfigurationAction::area;change.expected_catalog_revision=group.result.revision;change.area.area.group={group.result.identity};change.area.area.name="Resume Files";change.area.storage_path="files/ResumeDisk";
  check(admin.execute(change).code==app::LocalAccountResultCode::ok,"seed area through native existing-folder validation");
  {auto directory=native::FilePayloadDirectory::private_store(folder,store.file_bindings());auto output=directory.create("reviewed.txt",64);const std::string bytes="owned deletion resume fixture\n";output.append(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());output.flush();check(output.identity().bytes==bytes.size(),"actual seed payload hashed");}
  const auto config=database.file_configuration({true,{}});check(config&&config.value,"read actual deletion target");const auto selected=std::find_if(config.value->groups.begin(),config.value->groups.end(),[&](const auto& g){return g.id.value==group.result.identity;});check(selected!=config.value->groups.end(),"exact seeded group");
  change={};change.operation=store.identifiers().generate();change.action=app::FileConfigurationAction::begin_group_delete;change.expected_catalog_revision=config.value->policy.catalog_revision;change.group=*selected;change.delete_disk_files=true;
  const auto interrupted=admin.execute(change);check(interrupted.code!=app::LocalAccountResultCode::ok&&!std::filesystem::exists(folder/"reviewed.txt"),"native boundary interrupts after actual reviewed file deletion");
  const auto intent=database.file_group_deletion({true,{}},selected->id);check(intent&&intent.value&&!intent.value->completed&&intent.value->delete_disk_files,"saved exact disk deletion intent survives interruption");
  const auto managed=root/"files/ManagedDelete";check(std::filesystem::create_directory(managed),"separate new managed deletion fixture folder");std::filesystem::permissions(managed,std::filesystem::perms::owner_all);
  {auto directory=native::FilePayloadDirectory::private_store(managed,store.file_bindings());auto output=directory.create("managed.txt",64);const std::string bytes="current managed deletion fixture\n";output.append(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());output.flush();check(output.identity().bytes==bytes.size(),"managed deletion payload has durable portable identity");}
  check(store.close_for_maintenance().code==app::LocalControlResponseCode::ok,"file deletion seed releases store ownership");
 }else if(std::string_view(argv[3])=="seed"){
  native::InstallationLock lock;check(lock.acquire(root,".sfx-runtime.lock"),"seed owns stopped fixture");auto bindings=native::LinuxFilePayloadBindings::open(root);
  auto directory=native::FilePayloadDirectory::private_store(root/"files/general",bindings);auto output=directory.create("deferred-health-fixture.dat",128);
  const std::string bytes="exact native deferred payload\n";output.append(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());output.flush();const auto identity=output.identity();
  check(identity.bytes==bytes.size()&&!identity.file_id.empty(),"actual private payload registered and hashed");
 }else{
  check(std::string_view(argv[3])=="shutdown","known test mode");Clock clock;rt::LinuxRuntimeHost runtime(root,clock,[]{return false;});
  check(runtime.open().code==app::LocalControlResponseCode::ok,"host opens stopped");const auto operation=runtime.request_offline(rt::LinuxOfflineKind::health);check(operation!=0,"owned health accepted");
  check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"shutdown cancels and joins health before host lock");const auto result=runtime.offline_status();
  check(result.operation==operation&&!result.active&&result.completed,"final joined health status retained");check(runtime.request_offline(rt::LinuxOfflineKind::health)==0,"closed host refuses new work");
 }
 std::cout<<"PASS "<<checks<<" native offline host checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
