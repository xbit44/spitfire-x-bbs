#define main retained_management_catalog_main
#define Clock RetainedManagementClock
#include "linux_management_catalog_tests.cpp"
#undef Clock
#undef main
#include "sfx/archive/archive_engine.hpp"
#include "sfx/platform/linux/scheduler_files.hpp"
#include "linux_backup_job.hpp"
#include "sfx/platform/linux/installation_layout.hpp"
#include "../../src/apps/product_version.h"

namespace {
struct Clock final:app::IClock {
    std::atomic<std::int64_t> offset{};
    std::int64_t unix_seconds()const noexcept override{return RetainedManagementClock{}.unix_seconds()+offset.load();}
    std::uint64_t monotonic_millis()const noexcept override{return RetainedManagementClock{}.monotonic_millis();}
    void advance_to(std::int64_t value){offset.store(value-RetainedManagementClock{}.unix_seconds());}
};
void invalid_snapshot_requests(const std::filesystem::path& root){
    sfx::archive::ManagedBackupRequest baseline;baseline.installation_root=root;baseline.transaction_id=std::string(32,'A');
    baseline.physical_layout=sfx::archive::PhysicalLayout::linux_lowercase_v1;
    baseline.destination_directory=root/"backup";
    baseline.database_snapshot=root/"work/no-database.sqlite";baseline.linux_journal_snapshot_set=true;
    baseline.linux_journal_snapshots={{"Logs/CALLERS.LOG",root/"work/no-callers.snapshot"},{"Logs/HEYSYSOP.LOG",root/"work/no-attention.snapshot"}};
    const auto invalid=[&](sfx::archive::ManagedBackupRequest request){const auto result=sfx::archive::create_managed_backup(request);
        check(result.result.error==sfx::archive::Error::invalid_request,"invalid coordinated member capability refused before archive creation");};
    auto request=baseline;request.linux_journal_snapshot_set=false;invalid(request);
    request=baseline;request.database_snapshot.clear();invalid(request);
    request=baseline;request.operational_log_snapshot=root/"work/legacy.snapshot";invalid(request);
    request=baseline;request.linux_journal_snapshots[1].archive_path="Config/SFXConfig";invalid(request);
    request=baseline;request.linux_journal_snapshots[1].native_path=root/"data/sfx.db";invalid(request);
    request=baseline;request.linux_journal_snapshots[1].archive_path="Logs/CALLERS.LOG";invalid(request);
    request=baseline;request.linux_journal_snapshots[1].native_path=request.linux_journal_snapshots[0].native_path;invalid(request);
    request=baseline;request.linux_journal_snapshots[1].archive_path="Logs/SFXRuntime.log";invalid(request);
    check(!std::filesystem::exists(baseline.database_snapshot)&&
        !std::filesystem::exists(baseline.linux_journal_snapshots[0].native_path)&&
        !std::filesystem::exists(baseline.linux_journal_snapshots[1].native_path),"invalid request validation creates no private snapshots");
}
void cancel_captured_database(const std::filesystem::path& root){
    host::InstallationLock owner;check(owner.acquire(root,".sfx-runtime.lock"),"test retains normal installation ownership after host closes");
    host::LinuxLocalStoragePathPolicy paths;sfx::adapters::storage::SqliteStorageSettings settings;settings.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;settings.data_directory=root/"data";
    sfx::adapters::storage::SqliteStorage storage(settings,paths);check(storage.start().succeeded(),"cancellation fixture opens actual supported SQLite");
    host::LinuxSecureIdentifierGenerator identifiers;auto bindings=host::LinuxFilePayloadBindings::open(root);
    auto operational=std::make_shared<rt::AlphaOperationalJournal>(root/"logs/sfxruntime.log",std::to_wstring(geteuid()),identifiers.generate());
    auto durable=std::make_shared<rt::AlphaDurableJournal>(storage,identifiers,root,std::to_wstring(geteuid()),"UTC",operational);
    check(durable->start(),"actual journal owners started for capture cancellation");
    app::ScheduledRun run;run.job_id=app::kBackupJobId;run.run_id=std::string(31,'A')+"1";
    const auto snapshot=root/"work"/host::layout::owned_name("scheduler-database-"+run.run_id+".sqlite");
    check(!std::filesystem::exists(snapshot),"fresh deterministic cancellation marker");
    const auto result=rt::execute_linux_backup(root,storage,bindings,{},operational,durable,"capture-cancellation-fixture",run,[&]{return std::filesystem::exists(snapshot);});
    check(result.first==app::ScheduledJobResult::failed&&result.second=="backup_journal_capture_cancelled_work_preserved","cancel after actual DB cut refuses precisely without retry");
    check(std::filesystem::exists(snapshot)&&storage.validate_backup(snapshot).succeeded(),"cancelled capture retains genuine valid DB work for diagnosis");
    std::string detail;check(durable->capture_backup_files([&]{return operational->capture_backup_files([]{return true;});},detail),"cancellation released both writer fences");
    durable->stop();check(operational->flush(),"writers resume and flush cancellation reporting");durable.reset();operational.reset();
    check(storage.shutdown().succeeded(),"cancellation fixture closes normally");
}
app::AlphaControlResponse job_command(rt::LinuxRuntimeHost& runtime,app::LocalControlMessageType type,std::vector<std::uint8_t> request,app::LocalControlMessageType response){
    const auto decoded=app::decode_control_response(exchange(runtime,type,std::move(request),response));check(decoded.has_value(),"typed job reply decodes");return *decoded;
}
app::AlphaSchedulerStatus finish_backup(rt::LinuxRuntimeHost& runtime,std::size_t expected){
    for(unsigned i=0;i<1000;++i){auto status=runtime.status().scheduler;
        if(!status.active&&!status.queued&&status.history.size()>=expected){
            if(status.last_result!=app::ScheduledJobResult::success)throw std::runtime_error("actual native backup outcome: "+status.diagnostic_code+" / "+status.last_detail);
            check(status.last_result==app::ScheduledJobResult::success,"native Runtime job reports actual completed archive");return status;}
        std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    throw std::runtime_error("native Runtime backup did not finish");
}
}
int main(int argc,char** argv){try{
    check(argc==3&&geteuid()!=0,"prepared native board/timezone, unprivileged");const std::filesystem::path root=argv[1];
    check(host::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"native timezone initialized");Clock clock;
    rt::LinuxRuntimeHost runtime(root,clock,[]{return false;});check(runtime.open().code==app::LocalControlResponseCode::ok,"actual host opens stopped");
    const auto before=wire_status(runtime);check(before.scheduler.configuration_loaded&&!before.scheduler.enabled&&!before.scheduler.active&&!runtime.bbs_owned(),"management exposes default-Off backup without starting BBS");
    invalid_snapshot_requests(root);
    app::AlphaSchedulerSettingsRequest setting;setting.expected_revision=before.scheduler.configuration_revision;setting.enabled=false;setting.backup_destination=(root/"backup/Owner selected").string();
    using T=app::LocalControlMessageType;
    check(job_command(runtime,T::scheduler_settings_request,app::encode_scheduler_settings_request(setting),T::scheduler_settings_response).code==app::LocalControlResponseCode::ok,"actual typed custom destination save");
    check(job_command(runtime,T::scheduler_settings_request,app::encode_scheduler_settings_request(setting),T::scheduler_settings_response).code!=app::LocalControlResponseCode::ok,"actual typed stale schedule save refused");
    check(job_command(runtime,T::scheduler_run_request,app::encode_scheduler_run_request({}),T::scheduler_run_response).code==app::LocalControlResponseCode::ok,"actual typed stopped backup admitted");
    const auto completed=finish_backup(runtime,1);check(!runtime.bbs_owned(),"manual stopped backup never starts caller/FTN listeners");
    const auto directory=root/"backup/Owner selected";std::vector<std::filesystem::path> archives;
    for(const auto& entry:std::filesystem::directory_iterator(directory))if(entry.path().extension()==".zip")archives.push_back(entry.path());
    check(archives.size()==1,"exact one archive in explicitly selected destination");
    auto inspection=sfx::archive::inspect_backup(archives.front());check(inspection.result.ok(),"actual Runtime archive independently verifies");
    check(inspection.metadata.backup_type=="manual"&&inspection.metadata.sfx_version==SFX_PRODUCT_VERSION_SHORT_STRING,"ordinary manual archive records actual compiled SFX version");
    const auto member=[&](const char* path){return std::any_of(inspection.files.begin(),inspection.files.end(),[&](const auto& row){return row.path==path;});};
    check(member("Data/SFX.DB")&&member("Config/file-identities-linux.v1")&&!member("Config/.file-identities-linux.lock"),"database and immutable Linux identity map captured, process lock excluded");
    check(member("Logs/CALLERS.LOG")&&member("Logs/HEYSYSOP.LOG"),"actual manual archive contains coordinated active durable journals");
    check(!member("Config/file-identities.v1"),"Linux map does not impersonate Windows identity member");
    check(std::filesystem::is_empty(root/"work"),"confirmed snapshot/index/identity work cleaned");
    const auto saved=host::read_scheduler_configuration_file(root);check(saved.value&&saved.value->backup_destination==directory.string(),"actual POSIX destination durable readback");
    check(runtime.start_bbs().code==app::LocalControlResponseCode::ok,"start actual BBS after backup retired");
    const auto second=clock.unix_seconds();while(clock.unix_seconds()==second)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(job_command(runtime,T::scheduler_run_request,app::encode_scheduler_run_request({}),T::scheduler_run_response).code==app::LocalControlResponseCode::ok,"actual typed online backup admitted");
    (void)finish_backup(runtime,2);check(runtime.bbs_owned()&&runtime.status().lifecycle.active_work.caller_sessions==0,"online backup retains BBS ownership");
    check(job_command(runtime,T::message_maintenance_run_request,app::encode_message_maintenance_kind(app::MessageMaintenanceKind::integrity),T::message_maintenance_run_response).code==app::LocalControlResponseCode::ok,"actual typed online integrity check admitted");
    app::MessageMaintenanceStatus integrity;
    for(unsigned i=0;i<1000;++i){const auto value=app::decode_message_maintenance_status(exchange(runtime,T::message_maintenance_query_request,app::encode_message_maintenance_kind(app::MessageMaintenanceKind::integrity),T::message_maintenance_query_response));check(value.has_value(),"typed maintenance status decodes");integrity=*value;if(!integrity.state.active&&integrity.latest)break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    check(integrity.available&&integrity.latest&&integrity.latest->outcome==app::MessageMaintenanceOutcome::success&&!integrity.shared_verification_required,"actual typed integrity result from SQLite");
    setting.expected_revision=runtime.status().scheduler.configuration_revision;setting.enabled=true;
    check(job_command(runtime,T::scheduler_settings_request,app::encode_scheduler_settings_request(setting),T::scheduler_settings_response).code==app::LocalControlResponseCode::ok,"actual typed daily backup enabled in owned fixture");
    const auto due=runtime.status().scheduler.next_run_utc;check(due>clock.unix_seconds(),"real daily schedule supplies future due slot");
    clock.advance_to(due);
    (void)finish_backup(runtime,3);
    std::size_t scheduled{};
    for(const auto& entry:std::filesystem::directory_iterator(directory)){
        if(entry.path().extension()!=".zip"||entry.path().filename().string().rfind("sfx-scheduled-backup-",0)!=0)continue;
        const auto verified=sfx::archive::inspect_backup(entry.path());
        check(verified.result.ok()&&verified.metadata.backup_type=="scheduled"&&verified.metadata.sfx_version==SFX_PRODUCT_VERSION_SHORT_STRING,"actual due scheduled archive records current version and independently verifies");++scheduled;
    }
    check(scheduled==1,"one genuine due scheduled archive; manual cases not relabeled");
    check(runtime.stop_bbs().response.code==app::LocalControlResponseCode::ok,"normal BBS stop joins jobs before FTN/store");
    check(!runtime.bbs_owned()&&runtime.status().scheduler.history.size()==3,"stopped management retains durable backup history");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"management shuts down after worker retirement");
    check(runtime.open().code==app::LocalControlResponseCode::ok&&!runtime.bbs_owned()&&runtime.status().scheduler.history.size()==3,"reopen remains stopped with recovered actual archive history");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"reopened management closes cleanly");
    cancel_captured_database(root);
    std::cout<<"PASS "<<checks<<" actual native Runtime typed backup and maintenance checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
