// Actual guarded installed restore; postings use controlled host storage sessions.
// This does not claim a real caller, Config click, or owner/hub acceptance.
#include "fixture_paths.hpp"
#ifdef SFX_CURRENT_WORKSPACE_RESTORE
#include "ftn_current_installed_client.inc"
#else
#define main retained_ftn_restore_fixture_main
#include "ftn_installed_tests.cpp"
#undef main
#endif
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/archive/backup_service.hpp"
#include <iomanip>
#include <sstream>
#include <map>
#include <cstring>

namespace restore_fixture {
namespace db = sfx::adapters::storage;
namespace arc = sfx::archive;
const std::filesystem::path root=std::filesystem::path(L"C:/Program Files/SFX-Section14-Test-20261030-r4").make_preferred();
app::Identifier128 identifier(){win32::WindowsSecureIdentifierGenerator generator;return generator.generate();}
#ifdef SFX_CURRENT_WORKSPACE_RESTORE
bool resume_partial_pair{};
app::Identifier128 recorded_identifier(const char* hex){app::Identifier128 out;for(unsigned i=0;i<16;++i)out.bytes[i]=static_cast<std::uint8_t>(std::stoul(std::string(hex+i*2,2),nullptr,16));return out;}
#endif
std::int64_t now(){return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
app::FtnSerialState history(){
    const auto owner=win32::current_process_user_sid_string();win32::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner).succeeded(),"exclusive installed history inspection");
    win32::WindowsLocalStoragePathPolicy paths(owner);db::SqliteStorageSettings settings;settings.data_directory=root/L"Data";
    db::SqliteStorage storage(settings,paths);const auto state=storage.verified_ftn_serial_history(root/L"Data"/L"SFX.DB");
    check(state&&state.value&&!state.value->predecessor,"verified current installed serial history");return *state.value;
}
app::FtnSerialState post_pair(const std::filesystem::path& data=root/L"Data"){
    const auto owner=win32::current_process_user_sid_string();win32::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner).succeeded(),"exclusive installed controlled session");
    check(data==root/L"Data"||data==root/L"Work"/L"FTN-Higher-History"/L"Data","closed installed or owned snapshot branch only");
    win32::WindowsLocalStoragePathPolicy paths(owner);db::SqliteStorageSettings settings;settings.data_directory=data;
    db::SqliteStorage storage(settings,paths);check(storage.start().succeeded(),"actual installed native-path SQLite writer");
    const auto account=storage.find_account_by_name("SSH Fixture");
    const auto policy=storage.security_policy();check(account&&account.value&&policy&&policy.value,"normal bootstrap account and published policy");
    const auto before=storage.ftn_serial_state({true,{}});check(before&&before.value&&!before.value->recovery_required,"known installed allocator before postings");
    app::StartSessionRequest start;start.session_id=identifier();start.correlation_id=identifier();start.account_id=account.value->account_id;
    start.node_id=1;start.started_utc_seconds=now();start.journal_transport="Telnet";start.journal_source_ip="127.0.0.1";start.journal_ansi=0;
    bool partial{};
#ifdef SFX_CURRENT_WORKSPACE_RESTORE
    partial=resume_partial_pair;
    if(partial){check(data==root/L"Data"&&before.value->high_water==21&&account.value->account_id==1&&account.value->revision==1&&policy.value->revision==1,"exact partial original session and serial checkpoint");start.session_id=recorded_identifier("D9C3421B4D6F79928D2AD37D76554D15");}
    else
#endif
    check(storage.record_session_started(start).succeeded(),"controlled host session through supported storage API, no SQL actor fabrication");
    app::MessageActor actor{start.session_id,account.value->account_id,account.value->revision,policy.value->revision};
    app::MessageTarget target;target.kind=app::MessageKind::private_ftn;
    target.remote=app::MessageRemoteTarget{"own-network","42:100/1@own-network","42:100/2@own-network","Synthetic Remote"};
    std::optional<app::MessageId> parent;std::string parent_wire;
#ifdef SFX_CURRENT_WORKSPACE_RESTORE
    if(partial){const auto saved=storage.pending_ftn_scan({true,{}},"own-network",22,1);check(saved&&saved.value&&saved.value->size()==1,"bounded original committed source verification");const auto& first=saved.value->front();check(first.message.value==23&&first.operation==recorded_identifier("19C8B7778CCE66F7146556317C26D2B3")&&first.msgid=="42:100/1@own-network 00000015"&&first.reply.empty(),"preserved exact original operation and wire identity; no duplicate posting");parent=first.message;parent_wire=first.msgid;}
#endif
    for(unsigned index=partial?2:1;index<=2;++index){
        const auto context=storage.begin_message(actor,target,parent);check(context&&context.value,"real installed NetMail context");
        app::CommitMessageRequest draft{actor,identifier(),*context.value,"Installed restore fixture","Controlled session; no actual caller claim.\n"};
        const auto committed=storage.commit_message(draft);check(committed&&committed.value,"actual allocator commits new NetMail or reply");
        const auto repeat=storage.commit_message(draft);check(repeat&&repeat.value&&repeat.value->id==committed.value->id,"same operation retry retains exact message");
        const auto intents=storage.pending_ftn_scan({true,{}},"own-network",committed.value->id.value-1,1);check(intents&&intents.value,"actual committed scan intent in bounded exact-ID page");
        const auto found=std::find_if(intents.value->begin(),intents.value->end(),[&](const auto& value){return value.message==committed.value->id;});
        check(found!=intents.value->end(),"committed identity remains in actual scan ledger");
        std::ostringstream expected;expected<<"42:100/1@own-network "<<std::hex<<std::setw(8)<<std::setfill('0')<<(before.value->high_water+index-(partial?1:0));
        check(found->msgid==expected.str()&&found->reply==parent_wire,"exact new serial and parent REPLY bytes");
        parent=committed.value->id;parent_wire=found->msgid;
    }
    app::FinalizeSessionRequest finish;finish.session_id=start.session_id;finish.completed_utc_seconds=now();finish.audit_event_id=identifier();
    check(storage.finalize_session(finish).succeeded(),"controlled session finalized through supported API");
    const auto state=storage.ftn_serial_state({true,{}});check(state&&state.value&&state.value->high_water==before.value->high_water+(partial?1:2),"exact remaining allocations and no retry allocation");
#ifdef SFX_CURRENT_WORKSPACE_RESTORE
    resume_partial_pair=false;
#endif
    check(storage.shutdown().succeeded(),"installed controlled writer stopped");return *state.value;
}
std::filesystem::path higher_history_archive(){
    const auto owner=win32::current_process_user_sid_string();
    const auto branch=root/L"Work"/L"FTN-Higher-History";
    {
        win32::OfflineInstallationGuard guard;check(guard.acquire(root,owner).succeeded(),"exclusive consistent branch snapshot");
        check(!std::filesystem::exists(branch),"new named branch never adopts or overwrites prior state");
        auto security=win32::installation::mutable_descriptor(owner);
        win32::installation::create_directory(branch,security.get());win32::installation::create_directory(branch/L"Data",security.get());
        win32::WindowsLocalStoragePathPolicy paths(owner);db::SqliteStorageSettings settings;settings.data_directory=root/L"Data";
        db::SqliteStorage storage(settings,paths);check(storage.start().succeeded(),"installed writer for consistent native snapshot");
        check(storage.create_backup(branch/L"Data"/L"SFX.DB").status.succeeded(),"supported SQLite backup creates same-lineage branch");
        check(storage.shutdown().succeeded(),"installed writer stopped before branch-only allocations");
    }
    const auto current=history(),higher=post_pair(branch/L"Data");
    check(higher.lineage==current.lineage&&higher.high_water==current.high_water+2,"branch uses genuine new allocations, no arbitrary floor or SQL change");
    const auto unchanged=history();check(unchanged.high_water==current.high_water&&unchanged.lineage==current.lineage,"branch allocation leaves installed floor unchanged");
    win32::OfflineInstallationGuard guard;check(guard.acquire(root,owner).succeeded(),"exclusive higher-history archive capture");
    const auto receipt=win32::installation::read_receipt(root,owner);win32::installation::verify_ftn_directory(receipt);
    arc::ManagedBackupRequest request;request.installation_root=root;request.destination_directory=root/L"Backup"/L"Scheduled";
    request.kind=arc::BackupKind::manual;request.ftn_layout=receipt.ftn_layout;request.source_generation=std::to_string(receipt.generation);
    const auto transaction=win32::installation::unique_id();for(const auto value:transaction){check((value>=L'0'&&value<=L'9')||(value>=L'A'&&value<=L'F'),"branch archive transaction ID");request.transaction_id.push_back(static_cast<char>(value));}
    Sleep(1100);request.created=arc::current_timestamp();request.database_snapshot=branch/L"Data"/L"SFX.DB";
    const auto saved=arc::create_managed_backup(request);check(saved.result.ok()&&arc::inspect_backup(saved.archive_path).result.ok(),"verified managed archive with genuine higher-history snapshot; no retention pruning");
    return saved.archive_path;
}
arc::RestoreResult restore_guarded(const std::filesystem::path& archive,bool fail,bool expected_hold,
    win32::OfflineInstallationGuard& guard,bool& publication_called){
    const auto owner=win32::current_process_user_sid_string();
    check(guard.acquired(),"continuously held normal exclusive restore guard");
    const auto receipt=win32::installation::read_receipt(root,owner);check(receipt.ftn_layout,"actual receipt9 FTN capability");
    win32::installation::verify_ftn_directory(receipt);
    const auto serial=win32::OfflineFtnSerialRestore::capture(root,owner,guard);
    arc::RestoreRequest request;request.installation_root=root;request.archive_path=archive;
    const auto transaction=win32::installation::unique_id();
    for(const auto value:transaction){check((value>=L'0'&&value<=L'9')||(value>=L'A'&&value<=L'F'),"exact hexadecimal transaction ID");request.transaction_id.push_back(static_cast<char>(value));}
    request.source_generation=std::to_string(receipt.generation);request.ftn_layout=receipt.ftn_layout;request.pre_restore_created=arc::current_timestamp();
    request.secure_live_directory=[&](const auto& path){const auto status=win32::WindowsStorageDaclPolicy(owner).apply(path,win32::StorageDaclObjectKind::directory);return status?arc::Result{}:arc::Result{arc::Error::publication_failed,status.operation};};
    request.secure_live_file=[&](const auto& path){const auto status=win32::WindowsStorageDaclPolicy(owner).apply(path,win32::StorageDaclObjectKind::file);return status?arc::Result{}:arc::Result{arc::Error::publication_failed,status.operation};};
    request.verify_live_publication=[&]{
        publication_called=true;
        if(!guard.acquired())return arc::Result{arc::Error::publication_failed,"exclusive guard lost"};
        const auto published=serial->publish();if(!published)return arc::Result{arc::Error::publication_failed,published.operation};
        if(fail)return arc::Result{arc::Error::publication_failed,"test-only failure after serial preservation before restore commit"};
        const auto invalidated=win32::invalidate_restored_message_verification(root,owner);return invalidated?arc::Result{}:arc::Result{arc::Error::publication_failed,invalidated.operation};
    };
    const auto result=arc::restore_backup(request);
    if(result.result.ok()&&!fail)check(serial->recovery_required()==expected_hold,"successful guarded restore reports expected history classification");
    std::cout<<"PRESERVED previous="<<result.retained_previous_state.u8string()<<" failed="<<result.retained_failed_state.u8string()<<" pre_restore="<<result.pre_restore_archive.u8string()<<'\n';
    return result;
}
arc::RestoreResult restore(const std::filesystem::path& archive,bool fail,bool expected_hold=false){
    win32::OfflineInstallationGuard guard;
    check(guard.acquire(root,win32::current_process_user_sid_string()).succeeded(),"normal exclusive restore guard");
    bool publication_called=false;return restore_guarded(archive,fail,expected_hold,guard,publication_called);
}
#include "ftn_installed_predecessor_checks.inc"
#include "ftn_installed_missing_current_checks.inc"
void finish_held_recovery(const std::filesystem::path& higher_archive){
    RuntimeClient runtime;std::string why;struct Stop{RuntimeClient& r;~Stop(){std::string reason;if(r.child_alive())r.shutdown(reason);}}stop{runtime};
    held_originators();
    check(runtime.restart(why),"unrelated local BBS starts while FTN history held");
    auto held_config=read(runtime);held_config.settings.mode=app::FtnMailerMode::native;
    held_config.settings.listener_enabled=false;held_config.settings.tls_listener_enabled=false;
    for(auto& link:held_config.links){check(link.host=="127.0.0.1","only previously approved synthetic loopback links");link.poll_minutes=0;}
    check(upload(runtime,held_config,token()).code==app::FtnConfigurationCode::ok,"normal owner mode save cannot clear FTN hold");
    app::FtnRuntimeResponse ftn;const auto deadline=GetTickCount64()+10000;
    do{ftn=runtime.ftn_runtime({});if(ftn.serial_recovery_required)break;Sleep(25);}while(GetTickCount64()<deadline);
    check(ftn.code==app::FtnConfigurationCode::ok&&ftn.serial_recovery_required&&!ftn.classic_listening&&!ftn.tls_listening,"actual installed IPC status exposes persistent FTN hold");
    app::FtnRuntimeRequest action;action.action=app::FtnRuntimeAction::scan;check(runtime.ftn_runtime(action).code==app::FtnConfigurationCode::unavailable,"held actual engine refuses Scan Now");
    action.action=app::FtnRuntimeAction::poll;action.network="own-network";action.link="hub";check(runtime.ftn_runtime(action).code==app::FtnConfigurationCode::unavailable,"held actual engine refuses eligible known link Poll Now");
    check(runtime.restart(why),"actual shipping Runtime restart while history held");
    const auto local_status=runtime.status();check(local_status&&local_status->state==app::AlphaRuntimePresentationState::ready,"unrelated local BBS remains Ready after restart");
    const auto restart_deadline=GetTickCount64()+10000;do{ftn=runtime.ftn_runtime({});if(ftn.serial_recovery_required)break;Sleep(25);}while(GetTickCount64()<restart_deadline);
    check(ftn.serial_recovery_required,"FTN hold survives actual shipping restart");check(runtime.shutdown(why),"held fixture stopped and preserved");
    check(history().recovery_required,"durable hold remains; no self-clear or reset");
    missing_current_last(higher_archive);
}
}
int main(int argc,char** argv){try{
    using namespace restore_fixture;
    check(argc==2&&(std::string(argv[1])=="--final-restore"
#ifdef SFX_CURRENT_WORKSPACE_RESTORE
        ||std::string(argv[1])=="--verify-client"||std::string(argv[1])=="--resume-rejected-session"||std::string(argv[1])=="--resume-committed-post"||std::string(argv[1])=="--resume-held"||std::string(argv[1])=="--resume-missing"
#endif
        )&&!win32::installation::elevated(),"explicit final restore/hold row on ordinary exact installed r4 only");
#ifdef SFX_CURRENT_WORKSPACE_RESTORE
    const auto bound=win32::installation::read_receipt(root,win32::installation::operator_sid());
    check(bound.generation==4&&bound.id==L"CD32AD93BBF78EB4D11959A6F89BBF21","fixed generation4 installed restore target; no caller root override");
#else
    check(win32::alpha_installation_root()==root,"installed helper belongs to exact isolated r4");
#endif
    win32::installation::verify_runtime_installation(root,win32::current_process_user_sid_string());
#ifdef SFX_CURRENT_WORKSPACE_RESTORE
    if(std::string(argv[1])=="--resume-missing"){
        const auto higher=root/L"Backup"/L"Scheduled"/L"SFX-backup-20261030-154025.zip";
        check(win32::installation::file_sha256(root/L"Data"/L"SFX.DB")=="E91369C68C201D1697D3BF0363AC26A6B4C08694C2CA680B8656860A893E08CE"&&win32::installation::file_sha256(higher)=="E0443525DB728B2577DC656F1A5CF60A14D6CEB181784F7C5DD8229101B9975F","exact post-status held checkpoint before first missing-current fault");
        missing_current_last(higher);std::cout<<"PASS "<<checks<<" remaining installed missing-current fault/settlement checks; prior455 restore and140 held/status checks bound separately; FIXTURE RETAINED HELD\n";return 0;
    }
    if(std::string(argv[1])=="--resume-held"){
        const auto higher=root/L"Backup"/L"Scheduled"/L"SFX-backup-20261030-154025.zip";
        check(win32::installation::file_sha256(root/L"Data"/L"SFX.DB")=="C9D39633C60FB06F66BB1D3EA34E56AF9D30A60AB62E80FC2C01D1144260DF70"&&win32::installation::file_sha256(higher)=="E0443525DB728B2577DC656F1A5CF60A14D6CEB181784F7C5DD8229101B9975F","exact preserved held checkpoint and higher archive");
        const auto state=history();check(state.recovery_required&&state.high_water==28,"already held serial state preserved");auto security=win32::installation::mutable_descriptor(win32::current_process_user_sid_string());const std::string text="Continue held originator/status/missing-current tail only; all prior restore cases remain preserved and are not repeated.\n";win32::installation::create_file(root/L"Work"/L"FTN-FINAL-RESTORE-HELD-CONTINUATION.txt",{text.begin(),text.end()},security.get());
        finish_held_recovery(higher);std::cout<<"PASS "<<checks<<" remaining installed held-originator/Runtime/status/restart/missing-current checks; prior455 successful checks bound separately; FIXTURE RETAINED HELD\n";return 0;
    }
    if(std::string(argv[1])=="--verify-client"){
        RuntimeClient runtime;std::string why;check(runtime.launch(why),"current fixed-root owner client launch");
        const auto original=app::encode_ftn_configuration(read(runtime));
        check(runtime.status()->state==app::AlphaRuntimePresentationState::ready&&!runtime.ftn_runtime({}).serial_recovery_required,"current Ready and known FTN history");
        check(runtime.shutdown(why)&&!runtime.child_alive(),"normal stopped owner session released");
        check(runtime.restart(why),"same client restarts after shutdown");
        check(runtime.status()->state==app::AlphaRuntimePresentationState::ready&&app::encode_ftn_configuration(read(runtime))==original,"restart authenticates new epoch and preserves configuration");
        check(runtime.shutdown(why),"verified client exits normally");std::cout<<"PASS "<<checks<<" current installed owner client read/status/shutdown/restart checks; no restore executed\n";return 0;
    }
#endif
    const auto scheduler=win32::read_scheduler_configuration_file(root,win32::current_process_user_sid_string());
    check(scheduler.state==win32::ConfigurationFileState::missing||(scheduler.value&&scheduler.value->backup_destination.empty()&&scheduler.value->retained_backups>=1),"test backup uses only new installation default collection");
    const auto collection=root/L"Backup"/L"Scheduled";
    if(std::filesystem::exists(collection))check(std::filesystem::is_empty(collection),"no existing collection content can be pruned by test backup");
    const auto marker=root/L"Work"/L"FTN-FINAL-RESTORE-STARTED.txt";
#ifdef SFX_CURRENT_WORKSPACE_RESTORE
    const bool rejected_session_resume=std::string(argv[1])=="--resume-rejected-session";
    if(std::string(argv[1])=="--resume-committed-post"){
        check(win32::installation::file_sha256(marker)=="A85DAA059BE8548BDB057502B0D938D3A3AD7241299485F27C0CE895BCB8C019"&&win32::installation::file_sha256(root/L"Data"/L"SFX.DB")=="7312BDD758986F19B01847A503D69E88E443EAA42E0E192AD2FE629CA4ED2B84","exact one committed original checkpoint before any backup or restore");
        check(!std::filesystem::exists(root/L"Work"/L"FTN-Higher-History"),"no prior higher branch");auto security=win32::installation::mutable_descriptor(win32::current_process_user_sid_string());const std::string text="Continue original controlled session at bounded scan-read and reply; retain first message, both old markers and all failed evidence.\n";win32::installation::create_file(root/L"Work"/L"FTN-FINAL-RESTORE-SCAN-CONTINUATION.txt",{text.begin(),text.end()},security.get());resume_partial_pair=true;
    }else if(rejected_session_resume){
        check(win32::installation::file_sha256(marker)=="A85DAA059BE8548BDB057502B0D938D3A3AD7241299485F27C0CE895BCB8C019"&&win32::installation::file_sha256(root/L"Data"/L"SFX.DB")=="D9DC8B76D6CD1B5653A0026DB3A0218C7438A563B73B1C3FD7502053F22CCE82","exact preserved rejected-session checkpoint; no prior restore replay");
        check(!std::filesystem::exists(root/L"Work"/L"FTN-Higher-History"),"no prior branch created before rejected session");
        auto security=win32::installation::mutable_descriptor(win32::current_process_user_sid_string());
        const std::string text="Continue once from rejected lowercase transport validation, before any session/backup/restore. Preserve original marker and all failures.\n";
        win32::installation::create_file(root/L"Work"/L"FTN-FINAL-RESTORE-SESSION-CONTINUATION.txt",{text.begin(),text.end()},security.get());
    }else
#endif
    {
        check(!std::filesystem::exists(marker),"final restore row runs once, never reset after failure");
        auto security=win32::installation::mutable_descriptor(win32::current_process_user_sid_string());const std::string text="Final isolated restore qualification started. Preserve failures and do not repeat or clear holds.\n";win32::installation::create_file(marker,{text.begin(),text.end()},security.get());
    }
    const auto initial=history();check(!initial.recovery_required&&initial.high_water<=100000,"known bounded serial history after all positive caller rows");
    const auto early=post_pair();
    const auto backup=win32::offline_create_backup(root,win32::current_process_user_sid_string());
    check(backup.status.succeeded()&&!backup.archive.empty()&&backup.pruned==0,"normal isolated backup created and no existing archive pruned");
    check(arc::inspect_backup(backup.archive).result.ok(),"actual archive independently verifies");
    const auto later=post_pair();check(later.high_water==early.high_water+2&&later.lineage==early.lineage,"genuine later history from new allocations");
    const auto restored=restore(backup.archive,false);check(restored.result.ok()&&restored.live_state_changed&&!restored.rollback_attempted,"normal current installed known-history restore completes");
    const auto preserved=history();check(preserved.high_water==later.high_water&&preserved.lineage==later.lineage&&!preserved.recovery_required,"restoring older content preserves later floor and lineage");
    const auto newer=post_pair();check(newer.high_water==later.high_water+2,"post-restore new messages allocate above retained floor");
    const auto higher_archive=higher_history_archive();
    Sleep(1100);
    const auto higher_restore=restore(higher_archive,false);check(higher_restore.result.ok()&&higher_restore.live_state_changed,"actual installed restore accepts verified higher same-lineage history");
    const auto higher=history();check(higher.high_water==newer.high_water+2&&higher.lineage==newer.lineage&&!higher.recovery_required,"higher restored floor is never reduced to current lower floor");
    const auto latest=post_pair();check(latest.high_water==higher.high_water+2,"new serials above restored higher floor");
    Sleep(1100); // archive names use whole-second timestamps; no overwrite.
    const auto rolled=restore(backup.archive,true);check(!rolled.result.ok()&&rolled.rollback_attempted&&rolled.rollback_succeeded,"post-publication prerequisite failure uses actual existing restore rollback");
    const auto after=history();check(after.high_water==latest.high_water&&after.lineage==latest.lineage&&!after.recovery_required,"rollback retains exact prior serial state");
    RuntimeClient runtime;std::string why;struct Stop{RuntimeClient& r;~Stop(){std::string reason;if(r.child_alive())r.shutdown(reason);}}stop{runtime};
    check(runtime.launch(why),"actual shipping Runtime reopens restored installed state");const auto status=runtime.status();check(status&&status->bootstrap_complete,"shipping owner IPC reports restored bootstrap");
    check(runtime.shutdown(why),"shipping Runtime stops normally after restore");
    const auto predecessor=predecessor_archive();const auto before_predecessor=history();
    Sleep(1100);const auto transitioned=restore(predecessor,false);check(transitioned.result.ok(),"actual guarded derived predecessor-store restore");
    const auto new_history=history();check(!new_history.recovery_required&&new_history.high_water==before_predecessor.high_water&&new_history.lineage.valid()&&new_history.lineage!=before_predecessor.lineage,"verified predecessor initializes new history while conserving trustworthy current floor");
    check(runtime.restart(why),"actual shipping Runtime opens restored predecessor-store transition");
    const auto predecessor_status=runtime.status();check(predecessor_status&&predecessor_status->bootstrap_complete&&predecessor_status->state==app::AlphaRuntimePresentationState::ready,"predecessor accounts/config pass normal shipping startup");
    check(runtime.shutdown(why),"predecessor Runtime stops normally");
    Sleep(1100);const auto divergent=restore(higher_archive,false,true);check(divergent.result.ok(),"normal guarded divergent established-history restore commits a hold");
    const auto held=history();check(held.recovery_required&&held.lineage==latest.lineage,"divergent history remains held without rekey or floor bypass");
    finish_held_recovery(higher_archive);
    std::cout<<"PASS "<<checks<<" installed guarded known/higher/predecessor/divergent restore and rollback; controlled storage sessions, actual shipping status/restart; missing-current outcome reported separately; FIXTURE RETAINED HELD\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
