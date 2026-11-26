#include "fixture_paths.hpp"
#include "linux_runtime_host.hpp"
#include "sfx/platform/linux/core_restore.hpp"
#include "sfx/platform/linux/installation_layout.hpp"
#include "sfx/platform/linux/bbs_health.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "../../src/apps/product_version.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace app=sfx::application;
namespace native=sfx::platform::linux_host;
namespace rt=sfx::apps::runtime;
namespace fs=std::filesystem;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F> bool refused(F fn){try{fn();return false;}catch(const std::exception&){return true;}}
struct Clock final:app::IClock{
    std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    std::uint64_t monotonic_millis()const noexcept override{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
};
void save(const fs::path& path,const std::string& value){std::ofstream file(path,std::ios::binary|std::ios::trunc);file<<value;file.close();check(bool(file)&&chmod(path.c_str(),0600)==0,"owned synthetic state saved");}
std::string read(const fs::path& path){std::ifstream file(path,std::ios::binary);check(bool(file),"read retained synthetic state");return {std::istreambuf_iterator<char>(file),{}};}
// Bound this restore fixture's unrelated definition-download/Work footprint.
// Journal safety now comes from the production coordinated capture boundary;
// there is no byte-stability sleep or journal-drain assumption before backup.
void settle_fixture(rt::LinuxRuntimeHost& runtime){
    const auto before=runtime.file_configuration_snapshot({});check(before.code==app::LocalAccountResultCode::ok,"fixture reads unchanged upload-scan policy");
    runtime.pause_clamav_automatic(true);
    const auto active=runtime.clamav_status();
    if(active.active||active.queued){check(active.operation!=0,"fixture observes exact optional updater operation");(void)runtime.cancel_clamav_update(active.operation);}
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    bool idle{};while(std::chrono::steady_clock::now()<deadline){const auto status=runtime.clamav_status();
        if(!status.active&&!status.queued){idle=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    check(idle,"owned fixture updater reaches observed idle without changing scan policy");
    const auto after=runtime.file_configuration_snapshot({});check(after.code==app::LocalAccountResultCode::ok&&after.policy.scan_uploads==before.policy.scan_uploads&&after.policy.revision==before.policy.revision,"fixture pause/cancel preserves actual saved upload-scan choice and revision");
}
void next_second(Clock& clock){const auto before=clock.unix_seconds();while(clock.unix_seconds()==before)std::this_thread::sleep_for(std::chrono::milliseconds(10));}
sfx::archive::Timestamp preservation(unsigned second){auto value=sfx::archive::current_timestamp();value.local="2026-11-09T06:07:"+std::to_string(second);value.filename_component="20261109-0607"+std::to_string(second);return value;}
struct SavedObject {
    fs::path path;struct stat before{};
    explicit SavedObject(fs::path value):path(std::move(value)){check(lstat(path.c_str(),&before)==0,"capture existing collision object");}
    void unchanged()const{struct stat after{};check(lstat(path.c_str(),&after)==0&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_mode==after.st_mode&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec,"existing collision identity bytes mode mtime ctime unchanged");}
};
}
int main(int argc,char** argv){try{
    check((argc==3||argc==4)&&geteuid()!=0,"prepared root and timezone, normal account");const fs::path root=argv[1];Clock clock;
    check(native::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"native time rules initialized");
    if(argc==4){
        const std::string action=argv[3];const bool fault=action=="--diagnose-log-capture-failure";
        check((action=="--diagnose-backup"||fault)&&root==(sfx::test::fixture_account_home() / "sfx-tests/beta1-20261112-r1/installed-recovery-fresh-r2/board"),"only retained synthetic backup failure diagnostic");
        const auto log=root/"logs/sfxruntime.log";const auto original=read(log);
        if(fault)check(chmod(log.c_str(),0640)==0,"synthetic capture refusal mode on exact retained log");
        struct RestoreMode{fs::path path;bool active;~RestoreMode(){if(active)(void)chmod(path.c_str(),0600);}} restore_mode{log,fault};
        rt::LinuxRuntimeHost diagnostic(root,clock,[]{return false;});check(diagnostic.open().code==app::LocalControlResponseCode::ok,"retained test host opens stopped");
        const auto before=diagnostic.status().scheduler.history.size();check(diagnostic.run_scheduler_now({}).code==app::LocalControlResponseCode::ok,"retained test backup requested");
        for(unsigned i=0;i<3000;++i){const auto status=diagnostic.status().scheduler;if(!status.active&&status.history.size()>before){
            if(fault){
                check(status.last_result==app::ScheduledJobResult::failed&&status.last_detail.rfind("backup_journal_snapshot_failed: Logs/SFXRuntime.log:",0)==0,"actual native capture failure identifies exact journal and retains failed work");
                check(diagnostic.shutdown().code==app::LocalControlResponseCode::ok,"capture failure host closes normally");
                check(chmod(log.c_str(),0600)==0,"restore only synthetic capture fixture mode");restore_mode.active=false;check(read(log)==original,"unsafe capture retains exact original log bytes");
                std::cout<<"PASS actual native operational-log capture failure preservation\n";return 0;
            }
            if(status.last_result!=app::ScheduledJobResult::success)throw std::runtime_error(status.last_detail);
            check(diagnostic.shutdown().code==app::LocalControlResponseCode::ok,"diagnostic management closes normally");std::cout<<"PASS retained backup diagnostic\n";return 0;
        }std::this_thread::sleep_for(std::chrono::milliseconds(10));}throw std::runtime_error("native backup diagnostic timed out");
    }
    const auto probe=root/"config/restore-probe.txt",file=root/"files/general/owner-file.txt",intake=root/"data/fileintake/owner-intake.txt";
    save(probe,"at backup\n");save(file,"owner file never replaced\n");save(intake,"owner intake never replaced\n");
    rt::LinuxRuntimeHost runtime(root,clock,[]{return false;});check(runtime.open().code==app::LocalControlResponseCode::ok,"actual host opens stopped");
    settle_fixture(runtime);
    check(!native::run_bbs_health_check(root).stopped,"health refuses inspection while management owns installation");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"close before native payload preparation");
    const auto database_before=read(root/"data/sfx.db"),map_before=read(root/"config/file-identities-linux.v1"),config_before=read(root/"config/sfx.conf");
    const auto health=native::run_bbs_health_check(root);
    check(health.stopped&&health.saved_report&&health.text().find("OK - Database integrity")!=std::string::npos,"native stopped health verifies actual database and saves dated report");
    check(health.text().find("Needs attention - Node")==std::string::npos&&health.text().find("Needs attention - FTN settings")==std::string::npos,"fresh optional Node folder and unconfigured FTN seed are not false damage findings");
    check(read(root/"data/sfx.db")==database_before&&read(root/"config/file-identities-linux.v1")==map_before&&read(root/"config/sfx.conf")==config_before,"health preserves exact database settings and identity-map bytes");
    const auto cancelled_health=native::run_bbs_health_check(root,[]{return true;});
    check(cancelled_health.stopped&&cancelled_health.text().find("Could not check - Remaining board checks")!=std::string::npos&&cancelled_health.text().find("OK - Database integrity")==std::string::npos,"cancelled health never claims database validation");
    app::FilePayloadIdentity expected_payload;
    {auto bindings=native::LinuxFilePayloadBindings::open(root);auto directory=native::FilePayloadDirectory::private_store(root/"ftn",bindings);auto payload=directory.create("managed.pkt",128);
        const std::string bytes="synthetic managed FTN payload\n";payload.append(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());payload.flush();expected_payload=payload.identity();}
    const auto executable=root/"doors/native-restore-fixture",plain=root/"doors/ordinary-door-data.txt";
    const std::string program="#!/bin/sh\nexit 37\n";save(executable,program);check(chmod(executable.c_str(),0700)==0,"synthetic native executable owner mode");save(plain,"ordinary nonexecutable Door data\n");
    const auto upper=root/"doors/Game/NetMail",lower=root/"doors/Game/netmail";fs::create_directories(upper);fs::create_directories(lower);
    for(const auto& directory:{root/"doors/Game",upper,lower})check(chmod(directory.c_str(),0700)==0,"private case-distinct native Door directories");
    save(upper/"1.MSG","network A message\n");save(lower/"1.MSG","network B message\n");
    check(runtime.open().code==app::LocalControlResponseCode::ok,"reopen populated native file map before backup");
    settle_fixture(runtime);
    check(runtime.run_scheduler_now({}).code==app::LocalControlResponseCode::ok,"actual backup queued");
    bool done{};for(unsigned i=0;i<1000;++i){const auto status=runtime.status().scheduler;if(!status.active&&!status.history.empty()){
        if(status.last_result!=app::ScheduledJobResult::success)throw std::runtime_error(status.last_detail);
        done=true;break;}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    check(done,"actual backup completes");fs::path archive;
    for(const auto& entry:fs::directory_iterator(root/"backup/scheduled"))if(entry.path().extension()==".zip"){check(archive.empty(),"one source archive");archive=entry.path();}
    const auto captured=sfx::archive::inspect_backup(archive);
    check(captured.result.ok()&&captured.metadata.sfx_version==SFX_PRODUCT_VERSION_SHORT_STRING,"source restore archive records actual current product version");
    for(const auto* leaf:{"Logs/CALLERS.LOG","Logs/HEYSYSOP.LOG"})check(std::any_of(captured.files.begin(),captured.files.end(),[&](const auto& row){return row.path==leaf;}),"restore archive contains coordinated durable journal");
    check(refused([&]{(void)native::LinuxCoreRestore::preview(root,archive);}),"restore refuses while management owns installation");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"management closes before core restore");save(probe,"after backup\n");
    const auto history=[&](bool raise){
        native::LinuxLocalStoragePathPolicy paths;sfx::adapters::storage::SqliteStorageSettings settings;settings.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;settings.data_directory=root/"data";
        sfx::adapters::storage::SqliteStorage storage(settings,paths);check(bool(storage.start()),"actual stopped native history opens");
        if(raise){auto old=*storage.ftn_serial_state({true,{}}).value;old.high_water=4000000000ULL;check(bool(storage.preserve_ftn_serial_history({true,{}},old)),"verified higher pre-restore floor saved");}
        const auto state=storage.ftn_serial_state({true,{}});check(bool(state)&&state.value.has_value(),"native restore history reads");check(bool(storage.shutdown()),"native history writer settles");return *state.value;
    };
    (void)history(true);
    auto restore=native::LinuxCoreRestore::preview(root,archive,{},SFX_PRODUCT_VERSION_SHORT_STRING);check(restore->inspection().result.ok()&&!restore->confirmation_token().empty(),"verified preview holds stopped guard and confirmation token");
    {rt::LinuxRuntimeHost contender(root,clock,[]{return false;});check(contender.open().code!=app::LocalControlResponseCode::ok,"preview excludes a competing management owner");}
    const auto wrong=restore->execute("wrong token");check(!wrong.restore.result.ok()&&!wrong.restore.live_state_changed&&read(probe)=="after backup\n","unconfirmed restore cannot alter live state");
    const auto restored=restore->execute(restore->confirmation_token());
    if(!restored.restore.result.ok())throw std::runtime_error("native core restore: "+restored.restore.result.detail);
    check(restored.restore.live_state_changed&&!restored.restore.rollback_attempted,"confirmed core restore commits");
    check(read(upper/"1.MSG")=="network A message\n"&&read(lower/"1.MSG")=="network B message\n","case-distinct native per-network Door paths both round-trip exact bytes");
    check(restored.file_prepare_required,"same-root populated core restore explicitly requires Prepare");
    struct stat executed{},data_mode{};check(lstat(executable.c_str(),&executed)==0&&(executed.st_mode&07777)==0700&&lstat(plain.c_str(),&data_mode)==0&&(data_mode.st_mode&07777)==0600&&read(executable)==program,"verified native execute permission and exact script bytes restored; plain data stays private nonexecutable");
    const auto child=fork();check(child>=0,"start exact restored native fixture");if(child==0){execl(executable.c_str(),executable.c_str(),static_cast<char*>(nullptr));_exit(127);}int child_status{};check(waitpid(child,&child_status,0)==child&&WIFEXITED(child_status)&&WEXITSTATUS(child_status)==37,"restored native executable actually runs");
    check(read(probe)=="at backup\n"&&read(restored.restore.retained_previous_state/"config/restore-probe.txt")=="after backup\n","restored core and exact prior-state retention both verified");
    check(fs::exists(restored.restore.pre_restore_archive)&&sfx::archive::inspect_backup(restored.restore.pre_restore_archive).result.ok(),"pre-restore archive retained and verified");
    check(sfx::archive::inspect_backup(restored.restore.pre_restore_archive).metadata.sfx_version==SFX_PRODUCT_VERSION_SHORT_STRING,"pre-restore safety archive identifies current host build, independently of restored source version");
    check(read(file)=="owner file never replaced\n"&&read(intake)=="owner intake never replaced\n","core restore never changes Files or durable intake");
    check(!restore->execute(restore->confirmation_token()).restore.result.ok(),"consumed confirmation cannot repeat restore");restore.reset();
    const auto retained_history=history(false);check(retained_history.high_water==4000000000ULL&&!retained_history.recovery_required&&!restored.ftn_recovery_required,"ordinary older healthy restore retains higher captured floor without inventing history hold");
    const auto restore_open=runtime.open();check(restore_open.code==app::LocalControlResponseCode::recovery_required&&restore_open.diagnostic_code=="linux_file_map_prepare_required"&&!runtime.bbs_owned(),"durable same-root restored map requires explicit Prepare after reopen");
    check(runtime.prepare_copied_file_bindings().code==app::LocalControlResponseCode::ok&&!runtime.bbs_owned(),"explicit restored-core Prepare produces stopped ready board");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"restored management closes cleanly");
    {auto bindings=native::LinuxFilePayloadBindings::open(root);auto directory=native::FilePayloadDirectory::private_store(root/"ftn",bindings);const auto payload=directory.read("managed.pkt").identity();check(payload.volume==expected_payload.volume&&payload.file_id==expected_payload.file_id&&payload.bytes==expected_payload.bytes&&payload.sha256==expected_payload.sha256,"explicit Prepare preserves managed FTN logical identity and exact hash");}
    save(probe,"before failed attempt\n");next_second(clock);
    auto failed=native::LinuxCoreRestore::preview(root,archive);
    const auto rolled=failed->execute(failed->confirmation_token(),{},[]{return sfx::archive::Result{sfx::archive::Error::publication_failed,"injected native restored prerequisite failure"};});
    check(!rolled.restore.result.ok()&&rolled.restore.rollback_attempted&&rolled.restore.rollback_succeeded,"failed restored prerequisite rolls back actual core publication");
    check(read(probe)=="before failed attempt\n"&&read(file)=="owner file never replaced\n"&&read(intake)=="owner intake never replaced\n","rollback restores exact prior core and preserves file payloads");
    check(!rolled.restore.retained_failed_state.empty()&&fs::exists(rolled.restore.retained_failed_state),"failed publication evidence retained");failed.reset();
    check(runtime.open().code==app::LocalControlResponseCode::ok&&!runtime.bbs_owned()&&runtime.shutdown().code==app::LocalControlResponseCode::ok,"rolled-back board opens and closes actual stopped management");
    auto cancelled=native::LinuxCoreRestore::preview(root,archive);const auto stopped=cancelled->execute(cancelled->confirmation_token(),[]{return true;});
    check(!stopped.restore.result.ok()&&!stopped.restore.live_state_changed&&read(probe)=="before failed attempt\n","cancel before publication keeps exact live state");cancelled.reset();
    const auto prior_archive=root/"backup/upgrades"/sfx::archive::managed_backup_filename(sfx::archive::BackupKind::pre_restore,preservation(31),sfx::archive::PhysicalLayout::linux_lowercase_v1);
    save(prior_archive,"pre-existing archive bytes\n");check(chmod(prior_archive.c_str(),0640)==0,"deliberately retained prior archive mode");SavedObject prior_saved(prior_archive);
    auto collision=native::LinuxCoreRestore::preview(root,archive);const auto refused_preservation=collision->execute(collision->confirmation_token(),{},{},preservation(31));
    check(!refused_preservation.restore.result.ok()&&!refused_preservation.restore.live_state_changed&&!refused_preservation.restore.pre_restore_archive_created,"preservation collision has no creation provenance or live changes");
    prior_saved.unchanged();check(read(prior_archive)=="pre-existing archive bytes\n"&&read(probe)=="before failed attempt\n","prior archive collision preserves old bytes and live state");collision.reset();
    auto prior_collision=native::LinuxCoreRestore::preview(root,archive);const auto prior_folder=root/"backup/upgrades"/native::layout::owned_name("restore-previous-"+prior_collision->confirmation_token().substr(0,32));
    check(mkdir(prior_folder.c_str(),0750)==0,"pre-existing prior-state folder collision");save(prior_folder/"owner.txt","old previous-state bytes\n");SavedObject previous_saved(prior_folder);
    const auto prior_result=prior_collision->execute(prior_collision->confirmation_token(),{},{},preservation(32));
    check(!prior_result.restore.result.ok()&&prior_result.restore.rollback_attempted&&prior_result.restore.rollback_succeeded&&!prior_result.restore.retained_previous_state_created&&prior_result.restore.retained_failed_state_created,"prior-state destination collision rolls back and never claims existing folder");
    previous_saved.unchanged();check(read(prior_folder/"owner.txt")=="old previous-state bytes\n"&&read(probe)=="before failed attempt\n","prior-state collision preserves old folder bytes and restored live state");prior_collision.reset();
    auto failed_collision=native::LinuxCoreRestore::preview(root,archive);const auto failed_folder=root/"backup/upgrades"/native::layout::owned_name("restore-failed-"+failed_collision->confirmation_token().substr(0,32));
    check(mkdir(failed_folder.c_str(),0750)==0,"pre-existing failed-state folder collision");save(failed_folder/"owner.txt","old failed-state bytes\n");SavedObject failed_saved(failed_folder);
    const auto failed_result=failed_collision->execute(failed_collision->confirmation_token(),{},[]{return sfx::archive::Result{sfx::archive::Error::publication_failed,"injected final check"};},preservation(33));
    check(!failed_result.restore.result.ok()&&failed_result.restore.rollback_attempted&&!failed_result.restore.rollback_succeeded&&!failed_result.restore.retained_failed_state_created,"failed-state retention collision remains explicitly incomplete without false provenance");
    failed_saved.unchanged();check(read(failed_folder/"owner.txt")=="old failed-state bytes\n"&&read(probe)=="before failed attempt\n","failed-state collision preserves original folder and actual restored prior core");failed_collision.reset();
    const auto tamper=root/"backup/changed-source.zip";fs::copy_file(archive,tamper);check(chmod(tamper.c_str(),0600)==0,"isolated source copy private");
    auto stale=native::LinuxCoreRestore::preview(root,tamper);{std::ofstream output(tamper,std::ios::binary|std::ios::app);output<<"changed after preview";}
    const auto rejected=stale->execute(stale->confirmation_token());check(!rejected.restore.result.ok()&&!rejected.restore.live_state_changed,"source changed after preview is refused");stale.reset();
    const auto downloads=root.parent_path()/"Downloads";check(mkdir(downloads.c_str(),0755)==0&&chmod(downloads.c_str(),0755)==0,"ordinary owned read-only source folder");
    const auto selected=downloads/"selected.zip";fs::copy_file(archive,selected);check(chmod(selected.c_str(),0600)==0,"private archive in ordinary source folder");
    SavedObject downloads_saved(downloads),selected_saved(selected);
    auto external=native::LinuxCoreRestore::preview(root,selected);check(external->inspection().result.ok(),"private archive in owned 0755 folder previews successfully");external.reset();downloads_saved.unchanged();selected_saved.unchanged();
    check(chmod(downloads.c_str(),0775)==0,"synthetic group-writable source folder");
    check(refused([&]{(void)native::LinuxCoreRestore::preview(root,selected);}),"group-writable source parent refused");check(chmod(downloads.c_str(),0755)==0,"restore synthetic source folder mode");
    const auto link=downloads/"linked.zip";fs::create_symlink(selected,link);
    check(refused([&]{(void)native::LinuxCoreRestore::preview(root,link);}),"symlink archive refused");
    const auto parent_link=root.parent_path()/"Downloads-link";fs::create_directory_symlink(downloads,parent_link);
    check(refused([&]{(void)native::LinuxCoreRestore::preview(root,parent_link/"selected.zip");}),"symlink source parent refused");
    auto renamed=native::LinuxCoreRestore::preview(root,selected);fs::rename(selected,downloads/"original.zip");fs::copy_file(archive,selected);check(chmod(selected.c_str(),0600)==0,"replacement archive remains private");
    const auto replacement=renamed->execute(renamed->confirmation_token());check(!replacement.restore.result.ok()&&!replacement.restore.live_state_changed,"source name replaced with identical bytes refused");renamed.reset();
    const auto copy=root.parent_path()/"copied-board";check(!fs::exists(copy),"fresh copy fixture");fs::copy(root,copy,fs::copy_options::recursive);
    rt::LinuxRuntimeHost moved(copy,clock,[]{return false;});const auto opened=moved.open();
    check(opened.code==app::LocalControlResponseCode::recovery_required&&opened.diagnostic_code=="linux_file_map_prepare_required","whole-folder copy explicitly requests Prepare on new native identity");
    const auto prepared=moved.prepare_copied_file_bindings();if(prepared.code!=app::LocalControlResponseCode::ok)throw std::runtime_error("copied restore prepare: "+prepared.diagnostic_code);
    check(!moved.bbs_owned()&&moved.configuration().has_value(),"explicit copied-folder Prepare preserves ready stopped board");
    check(read(copy/"config/restore-probe.txt")=="before failed attempt\n"&&read(copy/"files/general/owner-file.txt")=="owner file never replaced\n","copy/Prepare preserves owner core and file contents");
    check(moved.shutdown().code==app::LocalControlResponseCode::ok,"copied board closes without test services");
    const std::string pending_text="SFX_COMPACTION_PENDING=1\nbackup="+archive.string()+"\ntemporary=preserved-native-test-work\n";
    save(copy/"config/compaction-pending.v1",pending_text);
    const auto guarded_db=read(copy/"data/sfx.db"),guarded_map=read(copy/"config/file-identities-linux.v1");
    const auto guard_health=native::run_bbs_health_check(copy);
    check(guard_health.stopped&&guard_health.saved_report&&guard_health.text().find("Needs attention - Interrupted compaction")!=std::string::npos&&guard_health.text().find("Do not delete the marker")!=std::string::npos,"health reports pending compaction with verified Restore guidance");
    check(read(copy/"config/compaction-pending.v1")==pending_text&&read(copy/"data/sfx.db")==guarded_db&&read(copy/"config/file-identities-linux.v1")==guarded_map,"pending health preserves guard database and map exactly");
    const auto blocked_open=moved.open();check(blocked_open.code==app::LocalControlResponseCode::recovery_required&&blocked_open.diagnostic_code=="interrupted_compaction_requires_verified_recovery","copied interrupted board remains in recovery");
    check(moved.prepare_copied_file_bindings().code!=app::LocalControlResponseCode::ok&&read(copy/"config/compaction-pending.v1")==pending_text,"Prepare cannot bypass pending physical recovery");
    check(moved.shutdown().code==app::LocalControlResponseCode::ok,"release pending owner for verified Restore");
    auto guard_rollback=native::LinuxCoreRestore::preview(copy,archive);
    const auto guard_failed=guard_rollback->execute(guard_rollback->confirmation_token(),{},[]{return sfx::archive::Result{sfx::archive::Error::publication_failed,"injected pending-guard restored prerequisite failure"};},preservation(34));
    check(!guard_failed.restore.result.ok()&&guard_failed.restore.rollback_succeeded&&read(copy/"config/compaction-pending.v1")==pending_text&&read(copy/"data/sfx.db")==guarded_db,"failed restore rolls back and preserves pending guard and exact database");
    guard_rollback.reset();
    check(refused([&]{(void)native::LinuxCoreRestore::preview(copy,guard_failed.restore.pre_restore_archive);}),"retained archive with unresolved compaction cannot be chosen as healthy restore source");
    auto guard_restore=native::LinuxCoreRestore::preview(copy,archive);
    const auto guard_recovered=guard_restore->execute(guard_restore->confirmation_token(),{},{},preservation(35));
    if(!guard_recovered.restore.result.ok())throw std::runtime_error("guarded core restore: "+guard_recovered.restore.result.detail);
    check(guard_recovered.restore.live_state_changed&&!fs::exists(copy/"config/compaction-pending.v1")&&read(guard_recovered.restore.retained_previous_state/"config/compaction-pending.v1")==pending_text,"verified restore removes live guard through core replacement and retains original evidence");
    check(read(copy/"config/restore-probe.txt")=="at backup\n"&&read(copy/"files/general/owner-file.txt")=="owner file never replaced\n","guard recovery restores selected core while preserving live Files");
    guard_restore.reset();const auto after_guard=moved.open();check(after_guard.code==app::LocalControlResponseCode::recovery_required&&after_guard.diagnostic_code=="linux_file_map_prepare_required","restored relocated board requires explicit file Prepare");
    check(moved.prepare_copied_file_bindings().code==app::LocalControlResponseCode::ok&&!moved.bbs_owned(),"verified restore then explicit Prepare reaches stopped ready state without manual marker deletion");
    check(moved.shutdown().code==app::LocalControlResponseCode::ok,"recovered relocated board closes cleanly");
    std::cout<<"PASS "<<checks<<" native stopped core restore, rollback, retained evidence and whole-folder Prepare checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
