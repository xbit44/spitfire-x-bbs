#include "linux_runtime_jobs.hpp"
#include "sfx/platform/linux/scheduler_files.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

namespace app=sfx::application;
namespace native=sfx::platform::linux_host;
namespace rt=sfx::apps::runtime;
namespace {
unsigned checks{};
std::atomic_bool fail_data_sync{false};
std::string data_directory;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Clock final:app::IClock {
    std::int64_t unix_seconds()const noexcept override{return 1794225600;}
    std::uint64_t monotonic_millis()const noexcept override{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
};
template<class F> void await(F predicate){
    for(unsigned i=0;i<500;++i){if(predicate())return;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    throw std::runtime_error("joined job outcome did not settle");
}
void persistence(const std::filesystem::path& root){
    using S=native::SchedulerFileState;
    check(native::read_scheduler_configuration_file(root).state==S::missing,"missing config distinguished");
    check(native::read_scheduler_state_file(root).state==S::missing,"missing state distinguished");
    check(native::read_scheduler_state_file(root,"../SFX.DB").state==S::invalid,"unknown job never becomes pathname");
    for(const auto* id:{app::kBackupJobId,app::kMessageRetentionJobId,app::kMessageIntegrityJobId}){
        auto config=app::message_maintenance_job(id)?app::default_message_scheduler_configuration(id):app::SchedulerConfiguration{};
        auto state=app::message_maintenance_job(id)?app::default_message_scheduler_state(id):app::SchedulerState{};
        check(bool(native::publish_scheduler_configuration_file(root,config)),"all job configurations publish");
        check(bool(native::publish_scheduler_state_file(root,state)),"all job states publish");
        const auto loaded=native::read_scheduler_configuration_file(root,id);
        check(loaded.value&&app::serialize_scheduler_configuration(*loaded.value)==app::serialize_scheduler_configuration(config),"configuration exact readback");
        const auto loaded_state=native::read_scheduler_state_file(root,id);
        check(loaded_state.value&&app::serialize_scheduler_state(*loaded_state.value)==app::serialize_scheduler_state(state),"state exact readback");
    }
    const auto saved=root/"config/sfxscheduler.conf";
    check(chmod(saved.c_str(),0644)==0,"make only isolated fixture insecure");
    check(native::read_scheduler_configuration_file(root).state==S::invalid,"read refuses shared config");
    check(!native::publish_scheduler_configuration_file(root,{}),"publish never repairs shared target");
    check(chmod(saved.c_str(),0600)==0,"restore own test mode");
    std::filesystem::rename(saved,root/"config/preserved-scheduler.conf");
    check(symlink("preserved-scheduler.conf",saved.c_str())==0,"symlink injection");
    check(native::read_scheduler_configuration_file(root).state==S::invalid&&!native::publish_scheduler_configuration_file(root,{}),"symlink read and replacement refused");
    check(unlink(saved.c_str())==0&&mkfifo(saved.c_str(),0600)==0,"FIFO injection");
    check(native::read_scheduler_configuration_file(root).state==S::invalid,"FIFO rejected without blocking");
    check(unlink(saved.c_str())==0,"remove only owned FIFO");
    std::filesystem::rename(root/"config/preserved-scheduler.conf",saved);
    auto config=*native::read_scheduler_configuration_file(root).value;
    config.backup_destination=(root/"private-backups").string();
    check(bool(native::publish_scheduler_configuration_file(root,config)),"POSIX custom destination saves");
    check(native::read_scheduler_configuration_file(root).value->backup_destination==config.backup_destination,"POSIX destination survives reopen exactly");
    for(const auto* value:{"/","//tmp/backups","/tmp/../backup","/tmp/./backup","/tmp/backups/","/tmp/back\\ups","/tmp/a\nb"})
        check(!app::valid_backup_destination_text(value),"POSIX aliases or control text refused");
    check(app::valid_backup_destination_text("/tmp/Private Backups")&&app::valid_backup_destination_text("C:\\Private Backups"),"native syntax and foreign retained setting remain readable");
}
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){
    if(fail_data_sync.load()){
        char name[4096]{};const auto target="/proc/self/fd/"+std::to_string(fd);
        const auto n=readlink(target.c_str(),name,sizeof(name)-1);
        if(n>0&&std::string(name,static_cast<std::size_t>(n))==data_directory){errno=EIO;return -1;}
    }
    return __real_fsync(fd);
}
int main(int argc,char** argv){try{
    check(argc==3&&geteuid()!=0,"prepared board and private timezone required");
    const std::filesystem::path root=argv[1];data_directory=(root/"data").string();
    check(native::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"native time rules initialized");
    persistence(root);
    native::LinuxLocalStoragePathPolicy paths;sfx::adapters::storage::SqliteStorageSettings settings;settings.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;
    settings.data_directory=root/"data";
    sfx::adapters::storage::SqliteStorage storage(settings,paths,sfx::adapters::storage::sfx_storage_migrations());
    check(bool(storage.start()),"actual prepared SQLite store opens");Clock clock;native::LinuxSecureIdentifierGenerator identifiers;
    std::atomic_uint backups{0};std::atomic_bool block{false},entered{false};std::string captured;
    rt::LinuxRuntimeJobs::Backup backup=[&](const app::ScheduledRun& run,const std::function<bool()>& cancelled){
        ++backups;captured=run.backup_destination;entered.store(true);
        while(block.load()&&!cancelled())std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return rt::LinuxRuntimeJobs::Outcome{cancelled()?app::ScheduledJobResult::failed:app::ScheduledJobResult::success,"test backup capability returned"};
    };
    // This test validates job dispatch, not native archive custody. Real backup
    // capabilities are qualified by the separate backup integration tests.
    rt::LinuxRuntimeJobs::ValidateDestination destination=[](const std::string& value)->std::shared_ptr<void>{
        if(!value.empty()&&value.front()!='/')throw std::runtime_error("foreign_destination_requires_correction");
        return std::make_shared<int>(1);
    };
    rt::LinuxRuntimeJobs jobs(root,storage,identifiers,clock,"Pacific Standard Time",{},backup,destination);
    check(jobs.start(),"joined native jobs start paused");
    check(!jobs.start(),"duplicate owner start refused");
    check(jobs.scheduler_status().configuration_loaded&&!jobs.scheduler_status().enabled&&!jobs.busy()&&backups.load()==0,"opening job owner never runs backup");
    check(jobs.run_message_maintenance_now(app::MessageMaintenanceKind::integrity).code!=app::LocalControlResponseCode::ok,"paused maintenance not admitted");
    auto setting=app::AlphaSchedulerSettingsRequest{};setting.expected_revision=jobs.scheduler_status().configuration_revision;setting.enabled=false;
    setting.backup_destination=(root/"chosen-backups").string();
    check(jobs.configure_scheduler(setting).code==app::LocalControlResponseCode::ok,"save custom native destination");
    check(jobs.configure_scheduler(setting).code!=app::LocalControlResponseCode::ok,"stale schedule revision refused");
    ++setting.expected_revision;setting.backup_destination="C:\\Retained Windows Folder";
    check(jobs.configure_scheduler(setting).code!=app::LocalControlResponseCode::ok,"foreign destination cannot silently select default");
    setting.backup_destination.reset();
    check(jobs.configure_scheduler(setting,17).code==app::LocalControlResponseCode::ok,"idle stopped retention update saves through schedule CAS");++setting.expected_revision;
    check(jobs.scheduled_job(app::kBackupJobId).configuration.retained_backups==17&&jobs.scheduled_job(app::kBackupJobId).paused,"snapshot captures exact saved retention and stopped state");
    check(!jobs.scheduled_job("unrecognized").available,"unknown job never reads arbitrary settings");
    check(jobs.configure_scheduler(setting,0).code!=app::LocalControlResponseCode::ok,"zero backup retention refused");
    block.store(true);entered.store(false);
    check(jobs.run_scheduler_now({}).code==app::LocalControlResponseCode::ok,"manual backup queued");await([&]{return entered.load();});
    check(jobs.busy()&&jobs.run_scheduler_now({}).code!=app::LocalControlResponseCode::ok,"one shared worker refuses concurrent backup");
    const auto working=jobs.scheduled_job(app::kBackupJobId);check(working.state.active&&working.any_active&&!working.queued&&!working.state.active_run_id.empty()&&!working.state.active_scheduled,"working manual job status distinguishes pending queue and retains run identity");
    check(jobs.configure_scheduler(setting,18).code!=app::LocalControlResponseCode::ok,"active job blocks changed retained count even while BBS stopped");
    jobs.pause(false);
    check(jobs.configure_scheduler(setting,18).code!=app::LocalControlResponseCode::ok,"running BBS blocks changed retained count");
    check(jobs.run_message_maintenance_now(app::MessageMaintenanceKind::integrity).code!=app::LocalControlResponseCode::ok,"maintenance cannot overlap backup");
    setting.backup_destination=(root/"future-backups").string();
    check(jobs.configure_scheduler(setting).code==app::LocalControlResponseCode::ok,"schedule edit while run retains captured settings");
    check(captured==(root/"chosen-backups").string(),"active run retained exact selected destination");
    block.store(false);await([&]{return !jobs.busy();});
    check(jobs.scheduler_status().last_result==app::ScheduledJobResult::success&&jobs.scheduler_status().history.size()==1,"completed dispatch has durable history");
    jobs.stop();
    check(jobs.run_scheduler_now({}).code!=app::LocalControlResponseCode::ok,"stopped worker cannot accept work");
    // Simulate a missing integrity-state file on an existing configured board.
    std::filesystem::rename(root/"data/sfxmessageintegrity.state",root/"data/preserved-integrity.state");
    check(jobs.start(false),"reopen actual schedules");
    auto integrity=jobs.message_maintenance_status(app::MessageMaintenanceKind::integrity);
    check(integrity.available&&integrity.shared_verification_required,"missing existing state requires fresh check");
    check(jobs.run_message_maintenance_now(app::MessageMaintenanceKind::retention).code==app::LocalControlResponseCode::ok,"retention admits guarded run");await([&]{return !jobs.busy();});
    const auto held=jobs.message_maintenance_status(app::MessageMaintenanceKind::retention);
    check(held.latest&&held.latest->outcome==app::MessageMaintenanceOutcome::skipped&&held.latest->removed_public==0&&held.latest->expired_unread==0,"real SQLite retention skips with zero removals behind fresh-check guard");
    check(jobs.run_message_maintenance_now(app::MessageMaintenanceKind::integrity).code==app::LocalControlResponseCode::ok,"fresh integrity queued");await([&]{return !jobs.busy();});
    integrity=jobs.message_maintenance_status(app::MessageMaintenanceKind::integrity);
    check(integrity.latest&&integrity.latest->outcome==app::MessageMaintenanceOutcome::success&&!integrity.shared_verification_required,"actual complete fresh integrity releases external guard");
    check(jobs.run_message_maintenance_now(app::MessageMaintenanceKind::retention).code==app::LocalControlResponseCode::ok,"retention queued after fresh integrity");await([&]{return !jobs.busy();});
    const auto maintained=jobs.message_maintenance_status(app::MessageMaintenanceKind::retention);
    check(maintained.latest&&maintained.latest->outcome==app::MessageMaintenanceOutcome::success,"actual retention completes after guard released");
    app::MessageMaintenanceScheduleRequest schedule; schedule.kind=app::MessageMaintenanceKind::retention;schedule.expected_revision=maintained.configuration.revision;
    schedule.enabled=false;schedule.hour=4;schedule.minute=15;
    check(jobs.configure_message_schedule(schedule).code==app::LocalControlResponseCode::ok,"maintenance CAS schedule saves without purge");
    check(jobs.configure_message_schedule(schedule).code!=app::LocalControlResponseCode::ok,"maintenance stale revision refused");
    const auto prior=backups.load();fail_data_sync.store(true);
    check(jobs.run_scheduler_now({}).code!=app::LocalControlResponseCode::ok,"post-rename directory-sync failure not reported queued");fail_data_sync.store(false);
    check(!jobs.scheduler_status().configuration_loaded&&backups.load()==prior,"uncertain admission disables job and never invokes backup");
    check(jobs.run_scheduler_now({}).code!=app::LocalControlResponseCode::ok,"uncertain state never immediately retried");
    jobs.stop();check(jobs.start(),"recovery reopens durable active evidence");
    check(jobs.scheduler_status().last_result==app::ScheduledJobResult::interrupted&&backups.load()==prior,"uncertain published run becomes interrupted without retry");
    block.store(true);entered.store(false);check(jobs.run_scheduler_now({}).code==app::LocalControlResponseCode::ok,"new explicit manual run after recovery");await([&]{return entered.load();});
    jobs.stop();block.store(false);
    const auto stopped=native::read_scheduler_state_file(root);
    check(stopped.value&&!stopped.value->active&&stopped.value->last_result==app::ScheduledJobResult::failed,"stop cancels and joins worker before durable completion");
    check(bool(storage.shutdown()),"SQLite closes only after joined job retirement");
    std::cout<<"PASS "<<checks<<" native scheduler persistence, joined dispatch, actual message maintenance and failure recovery checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
