// Bounded own-VM coordinator: production Setup/worker, normal Windows consent,
// actual installed Runtime owner protocol. Follow-up fault seeding is restricted
// to one fresh disposable r14 transaction and is never a production test hook.
#include "setup_engine.hpp"
#include "clamav_maintenance.hpp"
#include "sfx/platform/win32/clamav_resources.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/clamav_update.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <objbase.h>
#include <shellapi.h>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>
namespace ins=sfx::platform::win32::installation;
namespace win=sfx::platform::win32;namespace app=sfx::application;namespace arc=sfx::archive;namespace setup=sfx::apps::setup;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;ins::require(ok,why);std::cout<<"PASS "<<why<<std::endl;}
struct Runtime {
    ins::Handle process;win::NamedPipeConnection connection;std::uint32_t sequence{};
    explicit Runtime(const ins::Path& root,bool bootstrap=false){
        ins::verify_runtime_installation(root,ins::operator_sid());std::array<std::uint8_t,app::kLocalControlNonceBytes> nonce{};check(win::fill_secure_random(nonce.data(),nonce.size()),"fresh owner nonce");
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE read{},write{};check(CreatePipe(&read,&write,&sa,0),"owner channel");ins::Handle reader(read),writer(write);check(SetHandleInformation(write,HANDLE_FLAG_INHERIT,0),"writer private");
        SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);std::vector<std::uint8_t> buffer(size);auto* attributes=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        check(InitializeProcThreadAttributeList(attributes,1,0,&size),"native attributes");struct Release{PPROC_THREAD_ATTRIBUTE_LIST p;~Release(){DeleteProcThreadAttributeList(p);}} release{attributes};
        check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&read,sizeof(read),nullptr,nullptr),"exact inherited owner handle");
        const auto exe=root/L"Bin"/L"SFXRuntime.exe";auto command=L"\""+exe.native()+L"\" --owner-handle="+std::to_wstring(reinterpret_cast<ULONG_PTR>(read));
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;startup.lpAttributeList=attributes;PROCESS_INFORMATION child{};
        check(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup.StartupInfo,&child),"installed hook-free Runtime process");process.reset(child.hProcess);ins::Handle thread(child.hThread);reader.reset();DWORD written{},pid=GetCurrentProcessId();
        check(WriteFile(write,&pid,sizeof(pid),&written,nullptr)&&written==sizeof(pid)&&WriteFile(write,nonce.data(),static_cast<DWORD>(nonce.size()),&written,nullptr)&&written==nonce.size(),"owner PID and nonce bound");writer.reset();
        connection=win::connect_local_control_server(win::installation_object_names(root).pipe_name,child.dwProcessId,30000);check(connection.valid(),"installed authenticated pipe");
        const auto hello=app::decode_control_response(exchange(app::LocalControlMessageType::hello,app::LocalControlMessageType::hello_response,app::encode_hello_payload(nonce)));SecureZeroMemory(nonce.data(),nonce.size());
        if(hello)std::cout<<"START "<<hello->diagnostic_code<<std::endl;
        check(hello&&hello->code==(bootstrap?app::LocalControlResponseCode::setup_required:app::LocalControlResponseCode::ok),"installed current IPC expected startup state");
    }
    ~Runtime(){connection.disconnect();if(process&&WaitForSingleObject(process.get(),30000)!=WAIT_OBJECT_0)std::cerr<<"FAIL owned Runtime exit unconfirmed; do not force-stop\n";}
    std::vector<std::uint8_t> exchange(app::LocalControlMessageType type,app::LocalControlMessageType expected,std::vector<std::uint8_t> payload={}){
        app::LocalControlFrame request,response;request.type=type;request.request_id=++sequence;request.payload=std::move(payload);const auto ok=connection.write_frame(request)&&connection.read_frame(response)&&response.type==expected&&response.request_id==sequence;SecureZeroMemory(request.payload.data(),request.payload.size());check(ok,"matched installed response");return response.payload;
    }
    void stop(){const auto response=app::decode_control_response(exchange(app::LocalControlMessageType::shutdown_request,app::LocalControlMessageType::shutdown_response));check(response&&response->code==app::LocalControlResponseCode::ok,"normal Runtime shutdown response");connection.disconnect();DWORD code{};check(WaitForSingleObject(process.get(),30000)==WAIT_OBJECT_0&&GetExitCodeProcess(process.get(),&code)&&code==0,"normal Runtime exit0");}
};
std::string token(){std::string out;for(const auto c:ins::unique_id())out+=static_cast<char>(c);return out;}
void initialize(const ins::Path& root,unsigned number){
    Runtime runtime(root,true);app::AlphaBootstrapRequest request;struct Erase{app::AlphaBootstrapRequest& request;~Erase(){app::erase_bootstrap_secrets(request);}} erase{request};
    const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& query)->std::optional<std::vector<std::uint8_t>>{return runtime.exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(query));});
    check(status&&status->security_policy_revision>0&&status->security_policy_hash.size()==64,"authoritative bootstrap policy, same as normal wizard");
    request.configuration.security_policy_revision=status->security_policy_revision;request.configuration.security_policy_hash=status->security_policy_hash;
    request.configuration.bbs_name="Section9 Disposable Qualification";request.configuration.bbs_time_zone="Pacific Standard Time";request.configuration.logical_nodes=4;
    request.configuration.telnet.bind_address="127.0.0.1";request.configuration.telnet.port=static_cast<std::uint16_t>(26000+number*2);request.configuration.telnet.bot_gate_enabled=false;request.configuration.botgate.enabled=false;
    request.configuration.ssh.enabled=true;request.configuration.ssh.bind_address="127.0.0.1";request.configuration.ssh.port=static_cast<std::uint16_t>(26001+number*2);
    request.sysop_name="Section9 Fixture Sysop";request.sysop_password="Sfx-Aa1-"+token();request.system_password="Sfx-Zz2-"+token();
    auto encoded=app::encode_bootstrap_request(request);check(!encoded.empty(),"valid complete current bootstrap request");
    const auto response=app::decode_control_response(runtime.exchange(app::LocalControlMessageType::bootstrap_request,app::LocalControlMessageType::bootstrap_response,std::move(encoded)));
    if(response)std::cout<<"BOOTSTRAP "<<response->diagnostic_code<<std::endl;
    check(response&&response->code==app::LocalControlResponseCode::ok,"actual installed first-run bootstrap");runtime.stop();
}
void proxy(const ins::Path& root){
    Runtime runtime(root);std::cout<<"READY Section9"<<std::endl;
    for(std::string line;std::getline(std::cin,line)&&line!="stop";){
        struct Erase{std::string& line;~Erase(){std::fill(line.begin(),line.end(),'\0');}} erase{line};check(line.size()<=2*app::kLocalControlMaximumPayloadBytes+80,"bounded coordinator input");
        std::istringstream input(line);unsigned type{},expected{};std::string encoded;input>>type>>expected>>encoded;
        check(type>=1&&type<=90&&expected>=1&&expected<=90&&(encoded=="-"||encoded.size()%2==0),"closed IPC types through Event Sounds requests");std::vector<std::uint8_t> payload;
        if(encoded!="-"){check(encoded.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos,"hex input");for(std::size_t i=0;i<encoded.size();i+=2)payload.push_back(static_cast<std::uint8_t>(std::stoul(encoded.substr(i,2),nullptr,16)));}
        std::fill(encoded.begin(),encoded.end(),'\0');const auto response=runtime.exchange(static_cast<app::LocalControlMessageType>(type),static_cast<app::LocalControlMessageType>(expected),std::move(payload));
        std::ostringstream out;out<<std::hex<<std::setfill('0');for(const auto byte:response)out<<std::setw(2)<<static_cast<unsigned>(byte);std::cout<<"RESPONSE "<<out.str()<<std::endl;
    }runtime.stop();
}
}
#include "section9_manual_events_checks.inc"
#include "section9_clamav_overlap_checks.inc"
#include "section13_sound_checks.inc"
int wmain(int argc,wchar_t** argv)try{
    if(argc==4&&std::wstring(argv[1])==L"--worker"){const std::wstring pid=argv[3];ins::require(!pid.empty()&&pid.size()<=10&&pid.find_first_not_of(L"0123456789")==std::wstring::npos,"canonical worker PID");const auto value=std::stoull(pid);ins::require(value>0&&value<=MAXDWORD&&std::to_wstring(value)==pid,"bounded worker PID");return setup::worker(argv[2],static_cast<DWORD>(value));}
    if(argc==4&&std::wstring(argv[1])==L"--followup-stage-fault"){
        const auto root=ins::canonical_root(argv[2]);const std::wstring transaction=argv[3];
        check(ins::elevated()&&root==ins::Path(L"C:\\Program Files\\SFX-Section9-Test-20261017-r14")&&transaction.size()==32&&transaction.find_first_not_of(L"0123456789ABCDEF")==std::wstring::npos,"exact disposable protected-write fault worker");
        const auto owner=ins::operator_sid();const auto receipt=ins::read_receipt(root,owner);check(receipt.committed&&receipt.generation==1,"fresh fault generation only");ins::verify_structure(receipt);
        const auto descriptor=ins::structural_descriptor(owner);ins::create_file(root/L"Bin"/(L"SFXRuntime.exe.new-"+transaction),{'o','w','n','e','d','-','f','a','u','l','t'},descriptor.get());return 0;
    }
    check(argc==3||argc==4,"bounded action/root/optional official ZIP");const std::wstring action=argv[1];const auto root=ins::canonical_root(argv[2]);const std::wstring prefix=L"SFX-Section9-Test-20261017-r";const auto name=root.filename().native();
    const bool sounds_fixture=name==L"SFX-Section13-Test-20261028-r1";
    const bool ftn_stage2=name==L"SFX-Section14-Test-20261029-r3";const bool ftn_fixture=name==L"SFX-Section14-Test-20261029-r1"||ftn_stage2;
    check(!ins::elevated()&&root.parent_path()==ins::Path(L"C:\\Program Files")&&(sounds_fixture||ftn_fixture||name.rfind(prefix,0)==0),"ordinary exact disposable fixture only");const auto suffix=ftn_stage2?std::wstring(L"32"):ftn_fixture?std::wstring(L"31"):sounds_fixture?std::wstring(L"1"):name.substr(prefix.size());check(!suffix.empty()&&suffix.size()<=2&&suffix.find_first_not_of(L"0123456789")==std::wstring::npos,"closed fixture number");const auto number=static_cast<unsigned>(std::stoul(suffix));check(number>=1&&number<=32&&std::to_wstring(number)==suffix,"canonical fixture number");
    if(action==L"--runtime"){check(argc==3,"no extra runtime arguments");proxy(root);return 0;}
    if(action==L"--sound-checks"){check(argc==3,"no extra sound arguments");event_sound_checks(root);return 0;}
    if(action==L"--notice-checks"){check(argc==3,"no extra Events qualification arguments");manual_events_checks(root);return 0;}
    if(action==L"--overlap-checks"){check(argc==3,"no overlap path/engine overrides");clamav_overlap_checks(root);return 0;}
    if(action==L"--initial-overlap-checks"){check(argc==3,"no initial-overlap overrides");clamav_initial_overlap_checks(root);return 0;}
    if(action==L"--update-faults"){
        check(argc==3,"no updater fault path overrides");const auto owner=ins::operator_sid();const auto receipt=ins::read_receipt(root,owner);ins::verify_structure(receipt);const auto before=win::inspect_clamav_status(root,owner,receipt.id);check(before.verified&&before.definitions_verified,"genuine ready definitions before bounded updater faults");
        const auto definitions=win::FilePayloadDirectory::private_store(root/L"Data"/L"ClamAV",owner);std::vector<std::pair<std::string,app::FilePayloadIdentity>> original;for(const auto& database_name:definitions.names())if(database_name!="freshclam.dat")original.push_back({database_name,definitions.read(database_name).identity()});
        const auto verify=[&]{for(const auto& file:original)check(definitions.read(file.first).identity().sha256==file.second.sha256,"existing validated definition bytes preserved after updater fault");const auto status=win::inspect_clamav_status(root,owner,receipt.id);check(status.definitions_verified&&status.last_success==before.last_success,"failed/cancelled attempt preserves ready fingerprints and prior successful check");};
        const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();const auto started=GetTickCount64();const auto cancelled=win::update_clamav_definitions(root,owner,receipt.id,now,[started]{return GetTickCount64()-started>=250;},{},180000,win::ClamAvUpdateMode::manual);
        std::cout<<"MID_CANCEL "<<cancelled.cancelled<<" "<<cancelled.diagnostic<<std::endl;check(cancelled.cancelled&&!cancelled.succeeded&&cancelled.process_reaped&&cancelled.cleanup_complete,"actual in-progress update staging cancel preserves data and cleans exact scratch");verify();
        const auto timeout=win::update_clamav_definitions(root,owner,receipt.id,now+1,{},{},100,win::ClamAvUpdateMode::manual);std::cout<<"DEADLINE "<<timeout.diagnostic<<std::endl;check(!timeout.succeeded&&timeout.diagnostic=="clamav_update_timeout"&&timeout.process_reaped&&timeout.cleanup_complete,"actual short total deadline retains definitions and closes owned scratch");verify();
        bool vendor_seen{};const auto child_cancel=win::update_clamav_definitions(root,owner,receipt.id,now+2,[&]{return vendor_seen;},[&](std::uint64_t){vendor_seen=true;},180000,win::ClamAvUpdateMode::manual);
        std::cout<<"CHILD_CANCEL "<<vendor_seen<<" "<<child_cancel.diagnostic<<" reaped="<<child_cancel.process_reaped<<std::endl;check(vendor_seen&&child_cancel.cancelled&&!child_cancel.succeeded&&child_cancel.process_reaped&&child_cancel.cleanup_complete,"actual started vendor child cancellation positively reaps child before cleanup");verify();return 0;
    }
    if(action==L"--initialize"){check(argc==3,"no extra bootstrap arguments");initialize(root,number);return 0;}
    if(action==L"--component"){
        check(argc==3,"no arbitrary component package/target arguments");const auto owner=ins::operator_sid();const auto before=ins::read_receipt(root,owner);const auto policy=win::offline_file_configuration_snapshot(root,owner,{});check(policy.code==app::LocalAccountResultCode::ok,"current saved scan policy before install-later");
        const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);check(SUCCEEDED(com),"normal maintenance COM");struct Com{~Com(){CoUninitialize();}} release;
        const auto started=GetTickCount64();check(setup::install_portable_clamav(root,nullptr,[](const auto& text){std::wcout<<text<<std::endl;},[started]{return GetTickCount64()-started<850000;}),"actual installed trusted component maintenance with ordinary automatic download/normal consent");
        const auto after=ins::read_receipt(root,owner);check(ins::encode_receipt(before)==ins::encode_receipt(after),"component-only action leaves core program generation exact");const auto saved=win::offline_file_configuration_snapshot(root,owner,{});check(saved.code==app::LocalAccountResultCode::ok&&saved.policy.scan_uploads==policy.policy.scan_uploads&&saved.policy.revision==policy.policy.revision,"install-later preserves saved scanning choice/revision");const auto status=win::inspect_clamav_status(root,owner,after.id);check(status.verified&&status.definitions_verified&&status.last_success>0,"actual component and initial definitions status verified");return 0;
    }
    if(action==L"--updater-checks"){
        check(argc==3,"no manual-update override arguments");const auto owner=ins::operator_sid();const auto receipt=ins::read_receipt(root,owner);ins::verify_structure(receipt);const auto initial=win::inspect_clamav_status(root,owner,receipt.id);check(initial.verified&&initial.definitions_present,"existing verified engine and present real definitions");const auto policy=win::offline_file_configuration_snapshot(root,owner,{});check(policy.code==app::LocalAccountResultCode::ok,"scan policy readable before updater tests");
        win::WindowsSecureIdentifierGenerator ids;app::FileConfigurationChange off;off.operation=ids.generate();off.expected_catalog_revision=policy.policy.catalog_revision;off.policy=policy.policy;off.policy.scan_uploads=false;check(win::offline_change_file_configuration(root,owner,off).code==app::LocalAccountResultCode::ok,"explicit scan-Off saved through normal revisioned policy before updater tests");const auto off_policy=win::offline_file_configuration_snapshot(root,owner,{});check(off_policy.code==app::LocalAccountResultCode::ok&&!off_policy.policy.scan_uploads,"saved scan-Off confirmed");
        const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto cancel=win::update_clamav_definitions(root,owner,receipt.id,now,[]{return true;},{},180000,win::ClamAvUpdateMode::manual);check(cancel.cancelled&&!cancel.attempted&&cancel.process_reaped,"pre-launch manual cancel creates no vendor child/attempt");
        const auto automatic=win::update_clamav_definitions(root,owner,receipt.id,now);check(automatic.process_reaped&&automatic.cleanup_complete&&(automatic.succeeded||automatic.diagnostic=="clamav_update_already_attempted_today"),"actual automatic attempt or persisted same-day guard");
        const auto guard=win::update_clamav_definitions(root,owner,receipt.id,now);check(!guard.attempted&&guard.diagnostic=="clamav_update_already_attempted_today"&&guard.cleanup_complete,"automatic same-day guard still refuses duplicate automatic launch");
        const auto state=ins::read_file(root/L"Config"/L"ClamAV"/L"update-state.dat",80);
        const auto manual=win::update_clamav_definitions(root,owner,receipt.id,now,{},{},180000,win::ClamAvUpdateMode::manual);std::cout<<"MANUAL attempted="<<manual.attempted<<" success="<<manual.succeeded<<" diagnostic="<<manual.diagnostic<<std::endl;
        check(manual.attempted&&manual.diagnostic!="clamav_update_already_attempted_today"&&manual.process_reaped&&manual.cleanup_complete,"same-day manual request reaches supervised real vendor without bypassing vendor result");check(manual.succeeded||manual.diagnostic=="clamav_update_vendor_cooldown","real vendor success or explicitly respected cooldown only");check(ins::read_file(root/L"Config"/L"ClamAV"/L"update-state.dat",80)==state,"manual request does not delete or reset automatic-day state");
        const auto status=win::inspect_clamav_status(root,owner,receipt.id);check(status.verified&&status.history_available&&status.last_attempt==now&&status.last_mode==win::ClamAvUpdateMode::manual&&status.last_result==manual.diagnostic,"durable status records actual manual time/mode/outcome");check(status.definitions_verified&&status.last_success>0,"validated definitions remain ready after successful check or vendor cooldown");
        const auto saved=win::offline_file_configuration_snapshot(root,owner,{});check(saved.code==app::LocalAccountResultCode::ok&&!saved.policy.scan_uploads&&saved.policy.revision==off_policy.policy.revision,"manual/automatic updater never changes scan-Off choice/revision");
        Runtime runtime(root);const auto live=win::update_clamav_definitions(root,owner,receipt.id,now+1,{},{},180000,win::ClamAvUpdateMode::manual);check(live.attempted&&live.process_reaped&&live.cleanup_complete&&(live.succeeded||live.diagnostic=="clamav_update_vendor_cooldown"),"actual manual update works while installed Runtime is running with scanning Off");runtime.stop();const auto after=win::offline_file_configuration_snapshot(root,owner,{});check(after.code==app::LocalAccountResultCode::ok&&!after.policy.scan_uploads&&after.policy.revision==off_policy.policy.revision,"Runtime startup preserves explicit Off and updater independence");app::FileConfigurationChange restore;restore.operation=ids.generate();restore.expected_catalog_revision=after.policy.catalog_revision;restore.policy=after.policy;restore.policy.scan_uploads=policy.policy.scan_uploads;check(win::offline_change_file_configuration(root,owner,restore).code==app::LocalAccountResultCode::ok,"restore original fixture scan choice through normal revisioned policy");return 0;
    }
    if(action==L"--metadata"){
        check(argc==3,"no extra metadata arguments");const auto owner=ins::operator_sid();ins::verify_runtime_installation(root,owner);
        check(!std::filesystem::exists(root/L"Config"/L"SFX.conf")&&!std::filesystem::exists(root/L"Data"/L"SFX.DB"),"genuine pre-bootstrap fixture only");
        const auto inspect=[&]{return win::alpha_installation_has_persistent_state(root,owner,[&](const auto& path){return win::clamav_bootstrap_metadata(root,path,owner);});};
        const auto initial=inspect();check(initial&&initial.value&&!*initial.value,"verified optional component does not suppress first-run setup");
        const auto conservative=win::alpha_installation_has_persistent_state(root,owner);check(conservative&&conservative.value&&*conservative.value,"no blanket folder-name exemption without component verifier");
        auto folder=win::FilePayloadDirectory::private_store(root/L"Data"/L"ClamAV",owner);const auto fault_name="section9-unexpected-bootstrap.txt";check(folder.absent(fault_name),"unique bounded metadata fault fixture");
        {auto file=folder.create(fault_name,1);const std::uint8_t byte=1;file.append(&byte,1);(void)file.identity();}
        const auto unknown=inspect();check(unknown&&unknown.value&&*unknown.value,"unknown component child remains persistent evidence");
        {auto file=folder.read(fault_name,true);file.remove_exact();}check(folder.absent(fault_name),"only exact new one-byte fault fixture removed");
        const auto restored=inspect();check(restored&&restored.value&&!*restored.value,"known component classification restored without touching BBS data");
        for(const auto* role:{L"FileIntake",L"FileRemoved"}){
            auto intake=win::FilePayloadDirectory::private_store(root/L"Data"/role,owner);
            check(intake.absent(fault_name),"unique new intake metadata fault");
            {auto file=intake.create(fault_name,1);const std::uint8_t byte=1;file.append(&byte,1);(void)file.identity();}
            const auto occupied=inspect();check(occupied&&occupied.value&&*occupied.value,"nonempty intake/removal directory blocks first-run reset");
            {auto file=intake.read(fault_name,true);file.remove_exact();}
            const auto empty=inspect();check(empty&&empty.value&&!*empty.value,"only exact empty private role is layout metadata");
        }
        return 0;
    }
    if(action==L"--definitions"){
        check(argc==3,"no extra definitions arguments");const auto owner=ins::operator_sid();ins::verify_runtime_installation(root,owner);const auto receipt=ins::read_receipt(root,owner);
        win::verify_clamav_installation(root,owner,receipt.id);check(true,"installed protected engine identity");
        win::initialize_clamav_data(root,owner);check(true,"installed private definitions configuration");
        const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto result=win::update_clamav_definitions(root,owner,receipt.id,now);
        std::cout<<"UPDATE attempted="<<result.attempted<<" succeeded="<<result.succeeded<<" reaped="<<result.process_reaped<<" cleanup="<<result.cleanup_complete<<" exit="<<result.exit_code<<" diagnostic="<<result.diagnostic<<std::endl;
        check(result.succeeded&&result.process_reaped&&result.cleanup_complete,"actual installed vendor definitions update");return 0;
    }
    const bool defer_clamav=action==L"--maintenance-clamav-deferred";
    const bool no_backup=action==L"--maintenance-no-backup",cancel_backup=action==L"--maintenance-cancel-backup",cancel_retention=action==L"--maintenance-cancel-retention",write_fault=action==L"--maintenance-write-fault";
    const bool fresh=action==L"--fresh";check(fresh||action==L"--maintenance"||defer_clamav||no_backup||cancel_backup||cancel_retention||write_fault,"closed Setup action");if(fresh)check(!std::filesystem::exists(root),"fresh root never adopted");else ins::verify_runtime_installation(root,ins::operator_sid());
    setup::verify_payload();auto plan=setup::inspect_plan(root,false);check(plan.operation==(fresh?setup::Operation::fresh:setup::Operation::maintenance),"genuine Setup reviewed plan");if(no_backup)plan.backup.clear();setup::Options options{false,false,{}};if(argc==4){if(std::wstring(argv[3])==L"--auto")options.automatic_clamav=true;else options.clamav_package=ins::canonical_root(argv[3]);}
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);check(SUCCEEDED(com),"normal Setup COM");struct Com{~Com(){CoUninitialize();}} release;
    bool defer_initial_definitions{};
    if(defer_clamav)check(!win::clamav_installed(root)&&!options.clamav_package.empty(),"deferred fixture starts with BBS installed and no engine, pinned local package only");
    if(write_fault){
        check(number==14&&argc==3&&plan.previous&&plan.previous->generation==1,"fresh explicit write-fault fixture only");
        wchar_t module[32768]{};check(GetModuleFileNameW(nullptr,module,32768)>0,"same fault helper image");
        const auto parameters=L"--followup-stage-fault \""+root.native()+L"\" "+plan.transaction;
        SHELLEXECUTEINFOW launch{};launch.cbSize=sizeof(launch);launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;launch.lpVerb=L"runas";launch.lpFile=module;launch.lpParameters=parameters.c_str();launch.nShow=SW_HIDE;
        check(ShellExecuteExW(&launch)&&launch.hProcess,"normal Windows consent for owned fault fixture");ins::Handle child(launch.hProcess);DWORD code{};
        check(WaitForSingleObject(child.get(),60000)==WAIT_OBJECT_0&&GetExitCodeProcess(child.get(),&code)&&!code,"owned fault seed reaped");
    }
    bool cancelled{},failed{},complete{};std::wstring last_message;std::string failure;
    const auto started=GetTickCount64();try{
        complete=setup::execute(plan,options,nullptr,[&](const auto& message){last_message=message;std::wcout<<message<<std::endl;
            if(defer_clamav&&message.find(L"Checking optional ClamAV definitions")!=std::wstring::npos)defer_initial_definitions=true;
            if(cancel_backup&&message.find(L"Writing backup ZIP")!=std::wstring::npos)cancelled=true;
            if(cancel_retention&&message.find(L"Checking retained upgrade backups")!=std::wstring::npos)cancelled=true;
        },[&]{return !cancelled&&!defer_initial_definitions&&GetTickCount64()-started<600000;});
    }catch(const std::exception& e){failed=true;failure=e.what();std::cout<<"RETAINED SETUP CHECK: "<<failure<<'\n';}
    if(cancel_backup||write_fault){
        check(failed&&!complete,"requested precommit failure never reports completion");const auto owner=ins::operator_sid();ins::verify_runtime_installation(root,owner);const auto receipt=ins::read_receipt(root,owner);
        check(plan.previous&&receipt.committed&&receipt.program_hashes==plan.previous->program_hashes&&receipt.generation==plan.previous->generation+(write_fault?1:0),"precommit failure preserves or restores exact previous programs and usable receipt");
        if(cancel_backup)check(cancelled&&failure.find("cancel")!=std::string::npos&&!std::filesystem::exists(plan.backup),"backup cancellation leaves no published success");
        if(write_fault)check(failure=="setup_update_failed_previous_generation_restored"&&arc::inspect_backup(plan.backup).result.ok(),"actual staged write failure rolls back with verified backup retained");
        std::cout<<"PASS native Setup precommit failure checks="<<checks<<'\n';return 0;
    }
    check(!failed&&complete,"actual Setup with normal Windows consent completed");
    if(cancel_retention)check(cancelled&&last_message.find(L"Update committed and verified.")==0&&last_message.find(L"did not finish")!=std::wstring::npos,"postcommit cancellation truthfully distinguishes installed update from incomplete retention");
    const auto owner=ins::operator_sid();ins::verify_runtime_installation(root,owner);const auto receipt=ins::read_receipt(root,owner);
    check(receipt.committed&&receipt.program_count==ins::current_program_count&&receipt.generation==(fresh?1:plan.previous->generation+1),"matched current-program committed receipt");
    if(!options.clamav_package.empty()||options.automatic_clamav){win::verify_clamav_installation(root,owner,receipt.id);check(true,"exact protected optional ClamAV installation");}
    if(defer_clamav){const auto status=win::inspect_clamav_status(root,owner,receipt.id);check(defer_initial_definitions&&status.verified&&!status.definitions_verified,"normal first-definition cancellation leaves EXISTING verified engine, no false ready status");}
    std::wcout<<L"FIXTURE "<<root.native()<<L" GENERATION "<<receipt.generation<<std::endl;std::cout<<"PASS installed Setup checks="<<checks<<"; bootstrap/runtime/GUI/transfer are separate checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL Section9 installation after "<<checks<<": "<<e.what()<<'\n';return 1;}
