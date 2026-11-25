// New bounded roots only; genuine production Setup/UAC and receipt-verified
// hook-free installed Runtime. No trust/ACL/entrypoint bypass or old fixture edit.
#include "setup_engine.hpp"
#include "../../src/apps/runtime/alpha_runtime_host.hpp"
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/backup_collections.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/archive/backup_service.hpp"
#include <objbase.h>
#include <iostream>
#include <sstream>
#include <limits>
#include <iomanip>
#include <algorithm>
#include <set>

namespace inst=sfx::platform::win32::installation;
namespace win=sfx::platform::win32;
namespace app=sfx::application;
namespace arc=sfx::archive;
namespace setup=sfx::apps::setup;
namespace fs=std::filesystem;
namespace {
unsigned checks{};
void check(bool ok,const char* label){inst::require(ok,label);++checks;std::cout<<"PASS "<<label<<std::endl;}
app::Identifier128 operation(unsigned n){app::Identifier128 id;id.bytes[0]=0x8c;for(unsigned i=0;i<4;++i)id.bytes[15-i]=static_cast<std::uint8_t>(n>>(i*8));return id;}
const std::wstring owner(){return inst::operator_sid();}
fs::path qualified_root(const wchar_t* argument){
    const auto root=fs::absolute(argument).lexically_normal();const auto name=root.filename().native();
    const bool nested=root.parent_path()==fs::path(L"C:\\sfxbuilds") && name.rfind(L"section8c-qualified-r",0)==0;
    // The preferred nested parent must satisfy the EXISTING trusted-ancestor
    // policy. A newly operator-owned writable parent cannot be silently fixed.
    // Direct C: uses the existing volume-root distinction and normal Setup UAC.
    const bool direct=root.parent_path()==fs::path(L"C:\\") && name.rfind(L"sfxbuilds-section8c-qualified-r",0)==0;
    check((nested || direct) && name.size()<80 &&
          name.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789-")==std::wstring::npos && name.find(L"-20261011")!=std::wstring::npos,
          "closed new Section8C fixture path, never live or predecessor");return root;
}
void perform_setup(const fs::path& root,bool fresh){
    check(!inst::elevated(),"ordinary production Setup coordinator");if(fresh)check(!fs::exists(root),"fresh named root absent");else inst::verify_runtime_installation(root,owner());
    setup::verify_payload();const auto plan=setup::inspect_plan(root,false);
    check(plan.operation==(fresh?setup::Operation::fresh:setup::Operation::maintenance),"genuine Setup fresh/update/repair plan");
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);check(SUCCEEDED(com),"Setup COM initialization");struct Com{~Com(){CoUninitialize();}} release;
    check(setup::execute(plan,{false,false},nullptr,[](const std::wstring& value){std::wcout<<value<<std::endl;},[]{return true;}),"actual Windows Setup completion");
    inst::verify_runtime_installation(root,owner());const auto receipt=inst::read_receipt(root,owner());
    check(receipt.committed && receipt.generation==(fresh?1:plan.previous->generation+1),"exact committed Setup generation");
    std::wcout<<L"GENERATION "<<receipt.generation<<L" ID "<<receipt.id<<std::endl;
}
void bootstrap(const fs::path& root){
    sfx::apps::runtime::AlphaRuntimeHost host(root);check(host.start().code==app::LocalControlResponseCode::setup_required,"genuine fresh first run");
    app::AlphaBootstrapRequest request;request.configuration.bbs_name="Section8C Qualification";request.configuration.bbs_time_zone="Pacific Standard Time";
    request.configuration.logical_nodes=4;request.configuration.telnet.bind_address="127.0.0.1";request.configuration.telnet.port=25401;
    request.configuration.telnet.bot_gate_enabled=false;request.configuration.botgate.enabled=false;
    request.configuration.ssh.enabled=true;request.configuration.ssh.bind_address="127.0.0.1";request.configuration.ssh.port=25402;
    request.sysop_name="SSH Fixture";request.sysop_password="Sfx-test-password-123";request.system_password="Sfx-system-password-456";
    check(host.bootstrap(request).code==app::LocalControlResponseCode::ok,"supported first-run bootstrap, no SQL account seeding");
    for(const auto* name:{"Caller One","Caller Two"}){app::LocalAccountRequest create;create.action=app::LocalAccountAction::create_user;create.name=name;create.expected_revision=1;
        create.security_level=10;create.purge_allowed=true;create.password=create.confirmation="Sfx-test-password-123";
        check(host.administer_account(create).code==app::LocalAccountResultCode::ok,"supported disposable caller creation");}
    const auto retention=host.message_maintenance_status(app::MessageMaintenanceKind::retention),integrity=host.message_maintenance_status(app::MessageMaintenanceKind::integrity);
    check(retention.available && retention.configuration.enabled && retention.configuration.local_hour==0 && retention.configuration.local_minute==0,"actual initialized retention On midnight");
    check(integrity.available && integrity.configuration.enabled && integrity.configuration.local_weekday==std::optional<std::uint8_t>{std::uint8_t{0}} && integrity.configuration.local_hour==0 && integrity.configuration.local_minute==30,"actual initialized integrity On Sunday0030");
    const auto catalog=host.message_configuration_snapshot({});const auto echoes=host.message_configuration_snapshot({app::MessageGroupId{1},{}});
    check(catalog.code==app::LocalAccountResultCode::ok && catalog.catalog.unread_mail_age_days==std::optional<std::uint32_t>{365} && echoes.catalog.echoes.size()==2,"fresh unread age365 and two starter echoes");
    for(const auto& echo:echoes.catalog.echoes)check(echo.retention.maximum_count==std::optional<std::uint32_t>{1000} && !echo.retention.maximum_age_days,"actual starter retention1000Unlimited");
    check(!host.status().scheduler.enabled,"accepted fresh backup schedule remains Off");check(host.shutdown().code==app::LocalControlResponseCode::ok,"fresh host closes cleanly");
}
struct InstalledRuntime final {
    inst::Handle process;
    win::NamedPipeConnection connection;
    std::uint32_t sequence{};
    DWORD child_pid{};
    bool legacy{};
    explicit InstalledRuntime(const fs::path& root,bool legacy_mode=false):legacy(legacy_mode){
        check(!inst::elevated(),"ordinary installed shipping Runtime coordinator");inst::verify_runtime_installation(root,owner());
        std::array<std::uint8_t,app::kLocalControlNonceBytes> nonce{};check(win::fill_secure_random(nonce.data(),nonce.size()),"fresh inherited owner nonce");
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE read{},write{};check(CreatePipe(&read,&write,&sa,0),"normal inherited owner channel");inst::Handle reader(read),writer(write);
        check(SetHandleInformation(write,HANDLE_FLAG_INHERIT,0),"writer not inherited");SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<std::uint8_t> memory(size);auto* attributes=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
        check(InitializeProcThreadAttributeList(attributes,1,0,&size),"native process attributes");struct Attributes{PPROC_THREAD_ATTRIBUTE_LIST p;~Attributes(){DeleteProcThreadAttributeList(p);}} release{attributes};
        check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&read,sizeof(read),nullptr,nullptr),"exact inherited handle allowlist");
        const auto exe=root/L"Bin"/L"SFXRuntime.exe";auto command=L"\""+exe.native()+L"\" --owner-handle="+std::to_wstring(reinterpret_cast<ULONG_PTR>(read));
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;startup.lpAttributeList=attributes;
        PROCESS_INFORMATION created{};check(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup.StartupInfo,&created),"receipt-verified installed hook-free Runtime launch");
        process=inst::Handle(created.hProcess);inst::Handle thread(created.hThread);child_pid=created.dwProcessId;reader.reset();DWORD written{};const auto pid=GetCurrentProcessId();
        check(WriteFile(write,&pid,sizeof(pid),&written,nullptr) && written==sizeof(pid),"ordinary owner process binding");
        check(WriteFile(write,nonce.data(),static_cast<DWORD>(nonce.size()),&written,nullptr) && written==nonce.size(),"normal nonce transfer");writer.reset();
        connection=win::connect_local_control_server(win::installation_object_names(root).pipe_name,child_pid,15000);check(connection.valid(),"actual authenticated installed channel");
        const auto hello=app::decode_control_response(exchange(app::LocalControlMessageType::hello,app::LocalControlMessageType::hello_response,app::encode_hello_payload(nonce)));SecureZeroMemory(nonce.data(),nonce.size());
        check(hello && (hello->code==app::LocalControlResponseCode::ok || legacy && hello->code==app::LocalControlResponseCode::setup_required),legacy?"exact predecessor IPC30 owner hello":"shipping ready hello IPC31");
    }
    ~InstalledRuntime(){connection.disconnect();if(process && WaitForSingleObject(process.get(),15000)!=WAIT_OBJECT_0)std::cerr<<"FAIL owned Runtime still exiting PID "<<child_pid<<std::endl;}
    std::vector<std::uint8_t> exchange(app::LocalControlMessageType type,app::LocalControlMessageType expected,std::vector<std::uint8_t> payload={}){
        app::LocalControlFrame request,response;request.type=type;request.request_id=++sequence;request.payload=std::move(payload);
        if(legacy){
            // Test-only exact predecessor connection. Shipping still rejects
            // IPC30. Normal inherited nonce, SID and server PID binding remain.
            check(static_cast<unsigned>(type)<=52 && static_cast<unsigned>(expected)<=52,"closed predecessor protocol types");request.version={30,0};
            check(connection.write_frame(request),"exact predecessor frame write");
            const auto read_all=[&](void* data,DWORD length){auto* p=static_cast<unsigned char*>(data);while(length){DWORD n{};if(!ReadFile(connection.native_handle(),p,length,&n,nullptr)||!n)return false;p+=n;length-=n;}return true;};
            std::array<unsigned char,app::kLocalControlFrameHeaderBytes> header{};check(read_all(header.data(),static_cast<DWORD>(header.size())),"bounded predecessor frame header");
            const auto u16=[&](std::size_t at){return static_cast<unsigned>(header[at])|static_cast<unsigned>(header[at+1])<<8;};
            const auto u32=[&](std::size_t at){return static_cast<std::uint32_t>(header[at])|static_cast<std::uint32_t>(header[at+1])<<8|static_cast<std::uint32_t>(header[at+2])<<16|static_cast<std::uint32_t>(header[at+3])<<24;};
            const auto bytes=u32(16);check(u32(0)==app::kLocalControlFrameMagic && u16(4)==30 && u16(6)==0 && u16(8)==static_cast<unsigned>(expected) && u16(10)==0 && u32(12)==sequence && bytes<=app::kLocalControlMaximumPayloadBytes,"strict exact predecessor response identity/size");
            response.payload.resize(bytes);check(!bytes || read_all(response.payload.data(),bytes),"bounded predecessor payload");
        }else check(connection.write_frame(request) && connection.read_frame(response) && response.type==expected && response.request_id==sequence,"exact installed IPC exchange");return response.payload;
    }
    void stop(){const auto result=app::decode_control_response(exchange(app::LocalControlMessageType::shutdown_request,app::LocalControlMessageType::shutdown_response));
        check(result && result->code==app::LocalControlResponseCode::ok,"shipping orderly shutdown acknowledged");connection.disconnect();DWORD code{};
        check(WaitForSingleObject(process.get(),15000)==WAIT_OBJECT_0 && GetExitCodeProcess(process.get(),&code) && code==0,"owned shipping Runtime exited normally");std::cout<<"STOPPED clean"<<std::endl;}
};
void proxy(const fs::path& root,bool legacy=false){
    InstalledRuntime runtime(root,legacy);std::cout<<"READY Section8C"<<std::endl;
    for(std::string line;std::getline(std::cin,line) && line!="stop";){
        check(line.size()<=2*app::kLocalControlMaximumPayloadBytes+80,"bounded test-only command line");std::istringstream input(line);unsigned type{},expected{};std::string encoded;
        input>>type>>expected>>encoded;check(type>=1 && type<=58 && expected>=1 && expected<=58 && (encoded=="-" || encoded.size()%2==0),"closed known protocol types and hex data");
        std::vector<std::uint8_t> bytes;if(encoded!="-"){
            check(encoded.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos,"hex protocol bytes");
            for(std::size_t i=0;i<encoded.size();i+=2){const auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a'+10);return static_cast<unsigned>(c-'A'+10);};bytes.push_back(static_cast<std::uint8_t>(digit(encoded[i])*16+digit(encoded[i+1])));}
        }
        const auto response=runtime.exchange(static_cast<app::LocalControlMessageType>(type),static_cast<app::LocalControlMessageType>(expected),std::move(bytes));
        std::ostringstream out;out<<std::hex<<std::setfill('0');for(const auto byte:response)out<<std::setw(2)<<static_cast<unsigned>(byte);std::cout<<"RESPONSE "<<out.str()<<std::endl;
    }
    runtime.stop();
}
void legacy_bootstrap(const fs::path& root){
    InstalledRuntime runtime(root,true);app::AlphaBootstrapRequest request;
    const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& query)->std::optional<std::vector<std::uint8_t>>{return runtime.exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(query));});
    check(status && status->security_policy_revision>0 && status->security_policy_hash.size()==64,"actual predecessor authoritative bootstrap policy binding, same as normal wizard");
    request.configuration.security_policy_revision=status->security_policy_revision;request.configuration.security_policy_hash=status->security_policy_hash;
    request.configuration.bbs_name="Section8C Predecessor";request.configuration.bbs_time_zone="Pacific Standard Time";
    request.configuration.logical_nodes=4;request.configuration.telnet.bind_address="127.0.0.1";request.configuration.telnet.port=25401;
    request.configuration.telnet.bot_gate_enabled=false;request.configuration.botgate.enabled=false;request.configuration.ssh.enabled=true;
    request.configuration.ssh.bind_address="127.0.0.1";request.configuration.ssh.port=25402;
    request.sysop_name="SSH Fixture";request.sysop_password="Sfx-test-password-123";request.system_password="Sfx-system-password-456";
    auto encoded=app::encode_bootstrap_request(request);check(!encoded.empty(),"complete valid predecessor bootstrap frame");const auto result=app::decode_control_response(runtime.exchange(app::LocalControlMessageType::bootstrap_request,app::LocalControlMessageType::bootstrap_response,encoded));
    std::cout<<"BOOTSTRAP_RESULT code="<<(result?static_cast<unsigned>(result->code):999U)<<" diagnostic="<<(result?result->diagnostic_code:"invalid response")<<std::endl;
    SecureZeroMemory(encoded.data(),encoded.size());check(result && result->code==app::LocalControlResponseCode::ok,"actual predecessor supported bootstrap, no schema/account substitution");
    for(const auto* name:{"Caller One","Caller Two"}){app::LocalAccountRequest account;account.action=app::LocalAccountAction::create_user;account.name=name;account.expected_revision=1;account.security_level=10;account.purge_allowed=true;account.password=account.confirmation="Sfx-test-password-123";
        auto input=app::encode_account_request(account);const auto added=app::decode_account_result(runtime.exchange(app::LocalControlMessageType::account_command_request,app::LocalControlMessageType::account_command_response,input));
        SecureZeroMemory(input.data(),input.size());check(added && added->code==app::LocalAccountResultCode::ok,"actual predecessor supported caller creation");}
    runtime.stop();
}
void offline_controls(const fs::path& root){
    inst::verify_runtime_installation(root,owner());const auto before=win::offline_message_configuration_snapshot(root,owner(),{});check(before.code==app::LocalAccountResultCode::ok,"genuine stopped catalog snapshot");
    auto retention=win::offline_message_maintenance_status(root,owner(),app::MessageMaintenanceKind::retention);check(retention.available,"stopped real maintenance snapshot");
    app::MessageMaintenanceScheduleRequest settings{app::MessageMaintenanceKind::retention,retention.configuration.revision,false,23,51,{}};
    check(win::offline_configure_message_schedule(root,owner(),settings).code==app::LocalControlResponseCode::ok,"actual offline CAS saved Off/custom time, no run");
    check(win::offline_configure_message_schedule(root,owner(),settings).code!=app::LocalControlResponseCode::ok,"same stale revision refused");
    auto current=win::offline_message_configuration_snapshot(root,owner(),{});app::MessageConfigurationChange change;change.operation=operation(1);change.action=app::MessageConfigurationAction::group;change.expected_configuration_revision=current.catalog.revision;change.group.name="Section8C Delete Group";change.group.order=3;
    const auto group=win::offline_change_message_configuration(root,owner(),change);check(group.code==app::LocalAccountResultCode::ok,"new stopped group via genuine owner port");
    for(unsigned i=0;i<2;++i){current=win::offline_message_configuration_snapshot(root,owner(),{});change={};change.operation=operation(2+i);change.action=app::MessageConfigurationAction::echo;change.expected_configuration_revision=current.catalog.revision;change.echo.group={group.result.entity_id};change.echo.name="Section8C Delete Echo "+std::to_string(i);change.echo.order=i;
        const auto echo=win::offline_change_message_configuration(root,owner(),change);check(echo.code==app::LocalAccountResultCode::ok,"new stopped echo via genuine owner port");}
    const auto after=win::offline_message_configuration_snapshot(root,owner(),{});const auto target=app::MessageDeletionTarget{app::MessageDeletionKind::group,group.result.entity_id,group.result.entity_revision};std::string diagnostic;
    {
        auto context=win::OfflineMessageDeletion::begin(root,owner(),target,diagnostic);check(context && context->preview().echo_count==2 && context->preview().message_count==0,"held actual stopped group preview includes complete two-echo set");
        win::RuntimeInstanceGuard attempted;check(!attempted.acquire(root,owner()),"concurrent Runtime ownership cannot start during preview/final No");
        win::OfflineInstallationGuard other;check(!other.acquire(root,owner()),"another offline transaction cannot enter held preview");
    }
    current=win::offline_message_configuration_snapshot(root,owner(),{});check(current.catalog.revision==after.catalog.revision,"dropping held context cancels all config/content deletion");
    {
        auto context=win::OfflineMessageDeletion::begin(root,owner(),target,diagnostic);check(context!=nullptr,"new deliberately selected deletion backup attempt");
        const auto failed=context->backup([]{return true;});check(!failed.status && !context->commit(),"requested cancelled backup refuses same deletion attempt, no implicit No");
    }
    {
        auto context=win::OfflineMessageDeletion::begin(root,owner(),target,diagnostic);check(context!=nullptr,"new deliberate no-backup attempt permitted");
        const auto committed=context->commit();check(committed && committed.value && committed.value->echo_count==2 && committed.value->message_count==0,"confirmed no-backup whole group deletes exact set atomically");
        const auto repeated=context->commit();check(repeated && repeated.value && repeated.value->replayed,"held same operation reconciles removed group");
    }
    current=win::offline_message_configuration_snapshot(root,owner(),{});check(current.catalog.groups.size()==before.catalog.groups.size(),"unrelated starter groups/private infrastructure preserved");
    const auto compacted=win::offline_compact_messages(root,owner());check(compacted.status.succeeded() && compacted.logical_content_verified && compacted.physical_compaction_committed && !compacted.verified_backup.empty(),"actual guarded compaction AFTER new verified managed backup");
    check(compacted.verified_backup.parent_path()==root/L"Backup"/L"Scheduled" && arc::inspect_backup(compacted.verified_backup).result.ok(),"compaction honors existing default ordinary backup collection");
    check(!win::read_scheduler_configuration_file(root,owner()).value->enabled,"manual prerequisite never enables scheduled backup");
    check(fs::is_empty(root/L"Work"),"all owned offline operations closed, Work empty");
    std::cout<<"PASS Section8C actual offline controls checks="<<checks<<std::endl;
}
app::MessageDeletionTarget public_echo_target(const fs::path& root){
    const auto current=win::offline_message_configuration_snapshot(root,owner(),{app::MessageGroupId{1},{}});
    check(current.code==app::LocalAccountResultCode::ok,"actual stopped stable public target snapshot");
    const auto selected=std::find_if(current.catalog.echoes.begin(),current.catalog.echoes.end(),[](const auto& e){return e.id.value==2;});
    check(selected!=current.catalog.echoes.end(),"explicit current Public Chat ID2 exists");
    return {app::MessageDeletionKind::echo,selected->id.value,selected->revision};
}
void backup_controls(const fs::path& root){
    inst::verify_runtime_installation(root,owner());std::string diagnostic;
    const auto target=public_echo_target(root);auto context=win::OfflineMessageDeletion::begin(root,owner(),target,diagnostic);
    check(context!=nullptr,"optional backup holds exact populated echo context");
    const auto saved=context->backup();check(saved.status.succeeded() && !saved.archive.empty(),"requested Yes creates fresh verified managed backup");
    check(arc::inspect_backup(saved.archive).result.ok(),"actual optional archive independently inspected");
    const auto preview=context->preview();context.reset(); // Final No after requested backup.
    auto unchanged=win::OfflineMessageDeletion::begin(root,owner(),target,diagnostic);
    check(unchanged && unchanged->preview().message_count==preview.message_count && unchanged->preview().permanent_count==preview.permanent_count,
          "final No after successful backup preserves exact populated content");
    std::cout<<"BACKUP_ARCHIVE "<<saved.archive.u8string()<<std::endl;
}
void restore_controls(const fs::path& root,const wchar_t* basename){
    inst::verify_runtime_installation(root,owner());win::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner()).succeeded(),"actual stopped restore exclusive owner guard");
    const fs::path name(basename);const auto native=name.native();
    check(name==name.filename() && name.extension()==L".zip" && native.rfind(L"SFX-backup-",0)==0 && native.size()<100 &&
          native.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-.")==std::wstring::npos,"closed existing ordinary backup basename");
    const auto selected=root/L"Backup"/L"Scheduled"/name;
    check(arc::inspect_backup(selected).result.ok(),"actual selected immutable owner-data archive verifies");
    arc::RestoreRequest request;request.installation_root=root;request.archive_path=selected;
    const auto token=inst::unique_id();for(const auto c:token){check((c>=L'0'&&c<=L'9')||(c>=L'A'&&c<=L'F'),"restore token exact hex");request.transaction_id.push_back(static_cast<char>(c));}
    request.source_generation=std::to_string(inst::read_receipt(root,owner()).generation);request.pre_restore_created=arc::current_timestamp();
    request.secure_live_directory=[&](const fs::path& path){const auto s=win::WindowsStorageDaclPolicy(owner()).apply(path,win::StorageDaclObjectKind::directory);return s?arc::Result{}:arc::Result{arc::Error::publication_failed,s.operation};};
    request.secure_live_file=[&](const fs::path& path){const auto s=win::WindowsStorageDaclPolicy(owner()).apply(path,win::StorageDaclObjectKind::file);return s?arc::Result{}:arc::Result{arc::Error::publication_failed,s.operation};};
    // Exact same production prerequisite used by SFXConfig under its held guard.
    request.verify_live_publication=[&]{if(!guard.acquired())return arc::Result{arc::Error::publication_failed,"restore guard lost"};
        const auto s=win::invalidate_restored_message_verification(root,owner());return s?arc::Result{}:arc::Result{arc::Error::publication_failed,s.operation};};
    const auto restored=arc::restore_backup(request);
    std::cout<<"RESTORE_RESULT error="<<static_cast<unsigned>(restored.result.error)<<" detail="<<restored.result.detail
             <<" changed="<<restored.live_state_changed<<" rollback="<<restored.rollback_attempted<<" rollback_ok="<<restored.rollback_succeeded
             <<" previous="<<restored.retained_previous_state.u8string()<<" failed="<<restored.retained_failed_state.u8string()<<std::endl;
    check(restored.result.ok() && restored.live_state_changed && !restored.rollback_attempted,"actual supported protected mutable-data restore commits");
    check(arc::inspect_backup(restored.pre_restore_archive).result.ok(),"fresh pre-restore backup independently verified");
    inst::verify_runtime_installation(root,owner());
    const auto state=win::read_scheduler_state_file(root,owner(),app::kMessageIntegrityJobId);
    check(state.value && state.value->requires_fresh_check,"restored historical clean check is not current verification");
    std::cout<<"RESTORE_PREVIOUS "<<restored.retained_previous_state.u8string()<<"\nRESTORE_PREREQUISITE "<<restored.pre_restore_archive.u8string()<<std::endl;
}
void refuse_active_controls(const fs::path& root){
    inst::verify_runtime_installation(root,owner());win::OfflineInstallationGuard guard;
    check(!guard.acquire(root,owner()),"actual running Runtime refuses offline ownership");std::string diagnostic;
    check(!win::OfflineMessageDeletion::begin(root,owner(),{app::MessageDeletionKind::echo,2,1},diagnostic),"active Runtime refuses destructive preview without forcing callers off");
    const auto compact=win::offline_compact_messages(root,owner());check(!compact.status && compact.verified_backup.empty() && !compact.physical_compaction_committed,"active Runtime refuses compaction before backup/mutation");
}
void delete_final_group(const fs::path& root){
    inst::verify_runtime_installation(root,owner());const auto before=win::offline_message_configuration_snapshot(root,owner(),{});
    check(before.code==app::LocalAccountResultCode::ok && before.catalog.groups.size()==1,"explicit sole starter group selected for disposable deletion");
    const auto group=before.catalog.groups.front();std::string diagnostic;
    const app::MessageDeletionTarget target{app::MessageDeletionKind::group,group.id.value,group.revision};
    {
        auto cancelled=win::OfflineMessageDeletion::begin(root,owner(),target,diagnostic);
        check(cancelled && cancelled->preview().echo_count==2 && cancelled->preview().message_count>=2 && cancelled->preview().permanent_count>=1,"exact populated starter/final group counts include Permanent");
        win::RuntimeInstanceGuard restart;check(!restart.acquire(root,owner()),"concurrent Runtime restart refused throughout populated final No preview");
    }
    const auto after_no=win::offline_message_configuration_snapshot(root,owner(),{});
    check(after_no.catalog.revision==before.catalog.revision,"final No leaves starter group configuration untouched");
    {
        auto selected=win::OfflineMessageDeletion::begin(root,owner(),target,diagnostic);check(selected!=nullptr,"new explicit final Yes context");
        const auto expected=selected->preview();const auto done=selected->commit();
        check(done && done.value && done.value->message_count==expected.message_count && done.value->permanent_count==expected.permanent_count && done.value->echo_count==expected.echo_count,
              "explicit no-backup final Yes removes entire exact populated group including Permanent");
        check(selected->commit().value->replayed,"same-token uncertain completion reconciles once after final group gone");
    }
    const auto after=win::offline_message_configuration_snapshot(root,owner(),{});
    check(after.code==app::LocalAccountResultCode::ok && after.catalog.groups.empty(),"no remaining public groups is supported, never silently reseeded");
    check(fs::is_empty(root/L"Work"),"populated destructive contexts closed with empty Work");
}
void backup_extra_controls(const fs::path& root){
    inst::verify_runtime_installation(root,owner());const auto saved=win::read_scheduler_configuration_file(root,owner());check(saved.value.has_value(),"accepted existing backup settings available");
    auto original=*saved.value;
    const auto publish=[&](const std::string& destination){win::OfflineInstallationGuard guard;check(guard.acquire(root,owner()).succeeded(),"actual guarded backup destination publication");
        const auto prior=win::read_scheduler_configuration_file(root,owner());check(prior.value.has_value(),"current backup revision available");auto value=*prior.value;++value.revision;value.backup_destination=destination;
        check(win::publish_scheduler_configuration_file(root,owner(),value).succeeded(),"saved custom destination changes no schedule/retention policy");};
    const auto refused_test_selection=root/L"Section8C-Custom-Backup";
    if(original.backup_destination==refused_test_selection.u8string()){
        // Correct ONLY the exact choice made by this retained test's failed
        // attempt. It was rightly refused inside a live role; not product repair.
        check(!original.enabled && original.local_hour==2 && original.local_minute==0 && original.retained_backups==10 && !fs::exists(refused_test_selection),"exact prior failed test selection, all genuine saved choices otherwise unchanged");
        publish("");original.backup_destination.clear();
    }
    const auto custom=root/L"Backup"/L"Scheduled"/L"Section8C-Custom-Backup";check(!fs::exists(custom),"fresh suitable custom destination only");publish(custom.u8string());
    std::string last_backup_stamp;
    std::string diagnostic;const auto target=public_echo_target(root);
    {auto context=win::OfflineMessageDeletion::begin(root,owner(),target,diagnostic);check(context!=nullptr,"held optional custom backup context");const auto backup=context->backup();
        check(backup.status.succeeded() && backup.archive.parent_path()==custom && arc::inspect_backup(backup.archive).result.ok(),"optional Yes honors suitable saved custom destination");last_backup_stamp=backup.archive.stem().u8string().substr(std::string("SFX-backup-").size());}
    // These are distinct manual operations with second-resolution filenames.
    // Observe the next civil second; never alter the clock or retry product work.
    const auto naming_deadline=GetTickCount64()+3000;while(arc::current_timestamp().filename_component==last_backup_stamp){check(GetTickCount64()<naming_deadline,"bounded next distinct backup name");Sleep(10);}
    const auto compacted=win::offline_compact_messages(root,owner());
    check(compacted.status.succeeded() && compacted.logical_content_verified && compacted.physical_compaction_committed && compacted.verified_backup.parent_path()==custom,"actual compaction fresh mandatory backup honors custom destination");
    const auto unavailable=root/L"Absent-Backup-Parent"/L"Destination";check(!fs::exists(unavailable.parent_path()),"genuinely unavailable parent, not a permission bypass");publish(unavailable.u8string());
    {auto context=win::OfflineMessageDeletion::begin(root,owner(),target,diagnostic);check(context!=nullptr,"new requested unavailable-backup attempt");const auto rejected=context->backup();check(!rejected.status && !context->commit(),"backup failure refuses same deletion attempt without silent No/fallback");}
    const auto blocked=win::offline_compact_messages(root,owner());check(!blocked.status && blocked.verified_backup.empty() && !blocked.physical_compaction_committed,"unavailable mandatory backup refuses compaction before mutation");
    publish(original.backup_destination);
    {auto context=win::OfflineMessageDeletion::begin(root,owner(),target,diagnostic);check(context!=nullptr,"new deliberate cancellation attempt");const auto cancelled=context->backup([]{return true;});check(!cancelled.status && !context->commit(),"cancelled requested backup remains a failed prerequisite");}
    const auto after=win::read_scheduler_configuration_file(root,owner());check(after.value && after.value->enabled==original.enabled && after.value->local_hour==original.local_hour && after.value->local_minute==original.local_minute && after.value->retained_backups==original.retained_backups && after.value->backup_destination==original.backup_destination,"original Off/custom-time/destination/retention choices preserved");
    std::set<fs::path> old_markers;const auto ordinary=root/L"Backup"/L"Scheduled";
    for(const auto& item:fs::directory_iterator(ordinary))if(item.path().extension()==L".sfx-owned")old_markers.insert(item.path());
    bool verified_cancel{};const auto stopped=win::offline_compact_messages(root,owner(),[&]{for(const auto& item:fs::directory_iterator(ordinary))if(item.path().extension()==L".sfx-owned" && !old_markers.count(item.path())){verified_cancel=true;return true;}return false;});
    fs::path retained_archive;for(const auto& item:fs::directory_iterator(ordinary))if(item.path().extension()==L".sfx-owned" && !old_markers.count(item.path())){auto name=item.path().native();name.resize(name.size()-std::wstring(L".sfx-owned").size());retained_archive=fs::path(name);}
    std::cout<<"CANCEL_RESULT seen_publication="<<verified_cancel<<" status="<<stopped.status.succeeded()<<" committed="<<stopped.physical_compaction_committed<<" operation="<<stopped.status.operation<<std::endl;
    // A marker exists BEFORE its final hash-bound ownership verification. The
    // callback deliberately interrupts that prerequisite; do not mislabel it a
    // completed verified backup or weaken the product's conservative result.
    check(verified_cancel && !stopped.status && stopped.verified_backup.empty() && !stopped.physical_compaction_committed && !retained_archive.empty() && arc::inspect_backup(retained_archive).result.ok(),"cancellation during final backup verification retains valid archive, refuses incomplete prerequisite and physical compaction");
    check(fs::is_empty(root/L"Work"),"custom/failure/cancel operations closed with empty Work");
}
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc==4 && std::wstring(argv[1])==L"--worker"){
            const std::wstring number=argv[3];check(!number.empty() && number.size()<=10 && number.find_first_not_of(L"0123456789")==std::wstring::npos,"closed worker PID");const auto pid=std::stoull(number);
            check(pid && pid<=std::numeric_limits<DWORD>::max() && std::to_wstring(pid)==number,"exact worker PID");return setup::worker(argv[2],static_cast<DWORD>(pid));}
        check(argc==3 || (argc==4 && std::wstring(argv[1])==L"--restore"),"explicit bounded action/new fixture root required");const auto root=qualified_root(argv[2]);const std::wstring action=argv[1];
#ifdef SFX_SECTION8C_LEGACY_SEED
        if(action==L"--fresh"){perform_setup(root,true);legacy_bootstrap(root);}
        else if(action==L"--runtime")proxy(root,true);
        else check(false,"legacy test profile is fresh/source-runtime only; never new offline or migration");
#else
        if(action==L"--fresh"){perform_setup(root,true);bootstrap(root);}
        else if(action==L"--maintenance")perform_setup(root,false);
        else if(action==L"--runtime")proxy(root);
        else if(action==L"--offline")offline_controls(root);
        else if(action==L"--backup")backup_controls(root);
        else if(action==L"--restore")restore_controls(root,argv[3]);
        else if(action==L"--refuse-active")refuse_active_controls(root);
        else if(action==L"--delete-final")delete_final_group(root);
        else if(action==L"--backup-extra")backup_extra_controls(root);
        else check(false,"unknown Section8C qualification action");
#endif
        std::cout<<"PASS Section8C action checks="<<checks<<std::endl;return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL Section8C qualification: "<<error.what()<<std::endl;return 1;}
}
