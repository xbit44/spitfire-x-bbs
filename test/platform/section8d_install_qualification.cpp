// Test-only coordinator. Actual shipping Setup engine/worker/payload, normal
// Windows consent, no structural writes/trust bypass or predecessor mutation.
#include "setup_engine.hpp"
#include "../../src/apps/runtime/alpha_runtime_host.hpp"
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/archive/backup_service.hpp"
#include <objbase.h>
#include <iostream>
#include <limits>
#include <sstream>
#include <iomanip>
namespace inst=sfx::platform::win32::installation;
namespace setup=sfx::apps::setup;
namespace app=sfx::application;
unsigned checks{};
void check(bool ok,const char* label){++checks;inst::require(ok,label);std::cout<<"PASS "<<label<<std::endl;}
struct InstalledRuntime final {
    inst::Handle process;
    sfx::platform::win32::NamedPipeConnection connection;
    std::uint32_t sequence{};
    DWORD child_pid{};
    explicit InstalledRuntime(const inst::Path& root){
        check(!inst::elevated(),"ordinary installed shipping Runtime coordinator");inst::verify_runtime_installation(root,inst::operator_sid());
        std::array<std::uint8_t,app::kLocalControlNonceBytes> nonce{};check(sfx::platform::win32::fill_secure_random(nonce.data(),nonce.size()),"fresh inherited owner nonce");
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE read{},write{};check(CreatePipe(&read,&write,&sa,0),"normal inherited owner channel");inst::Handle reader(read),writer(write);
        check(SetHandleInformation(write,HANDLE_FLAG_INHERIT,0),"writer not inherited");SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);
        std::vector<std::uint8_t> memory(size);auto* attributes=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
        check(InitializeProcThreadAttributeList(attributes,1,0,&size),"native process attributes");struct Attributes{PPROC_THREAD_ATTRIBUTE_LIST p;~Attributes(){DeleteProcThreadAttributeList(p);}} release{attributes};
        check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&read,sizeof(read),nullptr,nullptr),"exact inherited handle allowlist");
        const auto exe=root/L"Bin"/L"SFXRuntime.exe";auto command=L"\""+exe.native()+L"\" --owner-handle="+std::to_wstring(reinterpret_cast<ULONG_PTR>(read));
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;startup.lpAttributeList=attributes;
        PROCESS_INFORMATION created{};check(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup.StartupInfo,&created),"receipt-verified installed hook-free Runtime launch");
        process=inst::Handle(created.hProcess);inst::Handle thread(created.hThread);child_pid=created.dwProcessId;reader.reset();DWORD written{};const auto pid=GetCurrentProcessId();
        check(WriteFile(write,&pid,sizeof(pid),&written,nullptr)&&written==sizeof(pid),"ordinary owner process binding");
        check(WriteFile(write,nonce.data(),static_cast<DWORD>(nonce.size()),&written,nullptr)&&written==nonce.size(),"normal nonce transfer");writer.reset();
        connection=sfx::platform::win32::connect_local_control_server(sfx::platform::win32::installation_object_names(root).pipe_name,child_pid,15000);check(connection.valid(),"actual authenticated installed channel");
        const auto hello=app::decode_control_response(exchange(app::LocalControlMessageType::hello,app::LocalControlMessageType::hello_response,app::encode_hello_payload(nonce)));SecureZeroMemory(nonce.data(),nonce.size());
        check(hello&&hello->code==app::LocalControlResponseCode::ok,"shipping ready hello IPC32");
    }
    ~InstalledRuntime(){connection.disconnect();if(process&&WaitForSingleObject(process.get(),15000)!=WAIT_OBJECT_0)std::cerr<<"FAIL owned Runtime still exiting PID "<<child_pid<<std::endl;}
    std::vector<std::uint8_t> exchange(app::LocalControlMessageType type,app::LocalControlMessageType expected,std::vector<std::uint8_t> payload={}){
        app::LocalControlFrame request,response;request.type=type;request.request_id=++sequence;request.payload=std::move(payload);
        const auto ok=connection.write_frame(request)&&connection.read_frame(response)&&response.type==expected&&response.request_id==sequence;
        SecureZeroMemory(request.payload.data(),request.payload.size());check(ok,"exact installed IPC32 exchange");return response.payload;
    }
    void stop(){const auto result=app::decode_control_response(exchange(app::LocalControlMessageType::shutdown_request,app::LocalControlMessageType::shutdown_response));
        check(result&&result->code==app::LocalControlResponseCode::ok,"shipping orderly shutdown acknowledged");connection.disconnect();DWORD code{};
        check(WaitForSingleObject(process.get(),15000)==WAIT_OBJECT_0&&GetExitCodeProcess(process.get(),&code)&&code==0,"owned shipping Runtime exited normally");std::cout<<"STOPPED clean"<<std::endl;}
};
void proxy(const inst::Path& root){
    InstalledRuntime runtime(root);std::cout<<"READY Section8D"<<std::endl;
    for(std::string line;std::getline(std::cin,line)&&line!="stop";){
        struct Erase{std::string& s;~Erase(){std::fill(s.begin(),s.end(),'\0');}} erase{line};
        check(line.size()<=2*app::kLocalControlMaximumPayloadBytes+80,"bounded test-only command line");std::istringstream input(line);unsigned type{},expected{};std::string encoded;input>>type>>expected>>encoded;
        check(type>=1&&type<=62&&expected>=1&&expected<=62&&(encoded=="-"||encoded.size()%2==0),"closed known protocol types and hex data");std::vector<std::uint8_t> bytes;
        if(encoded!="-"){check(encoded.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos,"hex protocol bytes");for(std::size_t i=0;i<encoded.size();i+=2){const auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');if(c>='a'&&c<='f')return static_cast<unsigned>(c-'a'+10);return static_cast<unsigned>(c-'A'+10);};bytes.push_back(static_cast<std::uint8_t>(digit(encoded[i])*16+digit(encoded[i+1])));}}
        std::fill(encoded.begin(),encoded.end(),'\0');const auto response=runtime.exchange(static_cast<app::LocalControlMessageType>(type),static_cast<app::LocalControlMessageType>(expected),std::move(bytes));
        std::ostringstream out;out<<std::hex<<std::setfill('0');for(const auto byte:response)out<<std::setw(2)<<static_cast<unsigned>(byte);std::cout<<"RESPONSE "<<out.str()<<std::endl;
    }
    runtime.stop();
}
void backup_current(const inst::Path& root){
    namespace win=sfx::platform::win32;namespace arc=sfx::archive;
    const auto owner=inst::operator_sid();inst::verify_runtime_installation(root,owner);
    const auto current=win::offline_message_configuration_snapshot(root,owner,{app::MessageGroupId{1},{}});
    check(current.code==app::LocalAccountResultCode::ok,"current stopped message configuration");
    const auto echo=std::find_if(current.catalog.echoes.begin(),current.catalog.echoes.end(),[](const auto& e){return e.id.value==2;});
    check(echo!=current.catalog.echoes.end(),"exact existing public echo2, no replacement target");
    std::string diagnostic;auto context=win::OfflineMessageDeletion::begin(root,owner,{app::MessageDeletionKind::echo,2,echo->revision},diagnostic);
    check(context!=nullptr,"stopped owner guard for optional backup only, NEVER deletion commit");
    const auto before=context->preview();const auto saved=context->backup();
    check(saved.status.succeeded()&&!saved.archive.empty()&&arc::inspect_backup(saved.archive).result.ok(),"actual current-schema full managed backup verified");
    check(context->preview().message_count==before.message_count,"backup makes no message deletion");
    std::cout<<"BACKUP_ARCHIVE "<<saved.archive.u8string()<<std::endl;
}
void restore_current(const inst::Path& root,const wchar_t* basename){
    namespace win=sfx::platform::win32;namespace arc=sfx::archive;
    const auto owner=inst::operator_sid();inst::verify_runtime_installation(root,owner);win::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner).succeeded(),"current stopped exclusive restore guard");
    const inst::Path name(basename);const auto native=name.native();
    check(name==name.filename()&&name.extension()==L".zip"&&native.rfind(L"SFX-backup-",0)==0&&native.size()<100&&native.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-.")==std::wstring::npos,"closed ordinary backup basename");
    const auto selected=root/L"Backup"/L"Scheduled"/name;check(arc::inspect_backup(selected).result.ok(),"immutable selected current backup verifies");
    arc::RestoreRequest request;request.installation_root=root;request.archive_path=selected;
    const auto token=inst::unique_id();for(const auto c:token){check((c>=L'0'&&c<=L'9')||(c>=L'A'&&c<=L'F'),"exact restore token");request.transaction_id.push_back(static_cast<char>(c));}
    request.source_generation=std::to_string(inst::read_receipt(root,owner).generation);request.pre_restore_created=arc::current_timestamp();
    request.secure_live_directory=[&](const inst::Path& path){const auto s=win::WindowsStorageDaclPolicy(owner).apply(path,win::StorageDaclObjectKind::directory);return s?arc::Result{}:arc::Result{arc::Error::publication_failed,s.operation};};
    request.secure_live_file=[&](const inst::Path& path){const auto s=win::WindowsStorageDaclPolicy(owner).apply(path,win::StorageDaclObjectKind::file);return s?arc::Result{}:arc::Result{arc::Error::publication_failed,s.operation};};
    request.verify_live_publication=[&]{if(!guard.acquired())return arc::Result{arc::Error::publication_failed,"restore guard lost"};const auto s=win::invalidate_restored_message_verification(root,owner);return s?arc::Result{}:arc::Result{arc::Error::publication_failed,s.operation};};
    const auto restored=arc::restore_backup(request);
    std::cout<<"RESTORE_RESULT error="<<static_cast<unsigned>(restored.result.error)<<" detail="<<restored.result.detail<<std::endl;
    check(restored.result.ok()&&restored.live_state_changed&&!restored.rollback_attempted,"actual current protected mutable-data restore commits");
    check(arc::inspect_backup(restored.pre_restore_archive).result.ok(),"new pre-restore backup verifies");inst::verify_runtime_installation(root,owner);
    std::cout<<"RESTORE_PREVIOUS "<<restored.retained_previous_state.u8string()<<"\nRESTORE_PREREQUISITE "<<restored.pre_restore_archive.u8string()<<std::endl;
}
int wmain(int argc,wchar_t** argv)try{
    if(argc==4&&std::wstring(argv[1])==L"--worker"){
        const std::wstring pid=argv[3];inst::require(!pid.empty()&&pid.size()<=10&&pid.find_first_not_of(L"0123456789")==std::wstring::npos,"closed helper PID");const auto value=std::stoull(pid);inst::require(value>0&&value<=MAXDWORD&&std::to_wstring(value)==pid,"canonical helper PID");return setup::worker(argv[2],static_cast<DWORD>(value));
    }
    check(argc==3||(argc==4&&std::wstring(argv[1])==L"--restore"),"explicit bounded action and new owned fixture");const std::wstring action=argv[1];const auto root=inst::canonical_root(argv[2]);
check(root==inst::Path(L"C:\\sfxbuilds-section8d-qwk-keys-r1-20261017")||root==inst::Path(L"C:\\sfxbuilds-section8d-qwk-polish-r1-20261017")||root==inst::Path(L"C:\\sfxbuilds-section8d-presentation-r1-20261015")||root==inst::Path(L"C:\\sfxbuilds-section8d-qualified-r1-20261013")||root==inst::Path(L"C:\\sfxbuilds-section8d-qualified-r2-20261014")||root==inst::Path(L"C:\\sfxbuilds-section8d-qualified-r3-20261014")||root==inst::Path(L"C:\\sfxbuilds-section8d-qualified-r4-20261014")||root==inst::Path(L"C:\\sfxbuilds-section8d-qualified-r5-20261014")||root==inst::Path(L"C:\\sfxbuilds-section8c-qualified-r90-8d-upgrade-20261011")||root==inst::Path(L"C:\\sfxbuilds-section8c-qualified-r91-8d-upgrade-20261011")||root==inst::Path(L"C:\\sfxbuilds-section8c-qualified-r92-8d-upgrade-20261011"),"exact disposable8D fixtures plus new QWK presentation root; no live root");check(!inst::elevated(),"ordinary production Setup coordinator");
    if(action==L"--runtime"){proxy(root);return 0;}
    if(action==L"--backup"){backup_current(root);return 0;}
    if(action==L"--restore"){restore_current(root,argv[3]);return 0;}
    const bool fresh=action==L"--fresh";check(fresh||action==L"--maintenance","closed normal lifecycle action");if(fresh)check(!std::filesystem::exists(root),"fresh root absent, never adopt");else inst::verify_runtime_installation(root,inst::operator_sid());
    const auto expected_ans=fresh?std::string("57A1CAD9E90195BBFA7807A5C083834F8D2F28EA7C21EE9AC8FBBBC02927ADE6"):inst::file_sha256(root/L"Display"/L"qwk.ans");
    const auto expected_asc=fresh?std::string("0E950DD9EDC83DCF474A99C3E5BEEB545E4C347DF9AEAD9C632D72B7FC6308CD"):inst::file_sha256(root/L"Display"/L"qwk.asc");
    setup::verify_payload();const auto plan=setup::inspect_plan(root,false);check(plan.operation==(fresh?setup::Operation::fresh:setup::Operation::maintenance),"actual Setup reviewed plan");
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);check(SUCCEEDED(com),"normal Setup COM");struct Com{~Com(){CoUninitialize();}} release;
    const auto started=GetTickCount64();check(setup::execute(plan,{false,false},nullptr,[](const std::wstring& text){std::wcout<<text<<std::endl;},[started]{return GetTickCount64()-started<300000;}),"actual protected Setup completion with normal Windows consent");
    const auto owner=inst::operator_sid();inst::verify_runtime_installation(root,owner);const auto receipt=inst::read_receipt(root,owner);
    check(receipt.committed&&receipt.program_count==7&&receipt.generation==(fresh?1:plan.previous->generation+1),"actual committed seven-program generation");
    check(inst::file_sha256(root/L"Bin"/L"Tools"/L"sexyz.exe")=="E884051DC99FA4D56DF3770CD2680EC40A22C32048440354CCE8D4E9E0D36FE8"&&inst::file_sha256(root/L"Bin"/L"Tools"/L"SFXQWKTransfer.exe")=="073DF149ED5D973CF30C89F900EDBC81DB8BEF28B81F56B5B0742E238C35334D","both distinct helper identities exact");
    check(inst::file_sha256(root/L"Display"/L"qwk.ans")==expected_ans&&inst::file_sha256(root/L"Display"/L"qwk.asc")==expected_asc,"fresh approved Lakota stock or existing custom QWK assets exact");
    if(fresh){
        sfx::apps::runtime::AlphaRuntimeHost host(root);check(host.start().code==app::LocalControlResponseCode::setup_required,"actual fresh first-run readiness");
        app::AlphaBootstrapRequest request;struct Erase{app::AlphaBootstrapRequest& request;~Erase(){app::erase_bootstrap_secrets(request);}} erase{request};
        request.configuration.bbs_name="Section8D Qualification";request.configuration.bbs_time_zone="Pacific Standard Time";request.configuration.logical_nodes=4;
        request.configuration.telnet.bind_address="127.0.0.1";request.configuration.telnet.port=25221;request.configuration.telnet.bot_gate_enabled=false;request.configuration.botgate.enabled=false;
        request.configuration.ssh.enabled=true;request.configuration.ssh.bind_address="127.0.0.1";request.configuration.ssh.port=25222;
        request.sysop_name="Packet Fixture Sysop";const auto random=inst::unique_id();std::string ascii_random;for(const auto ch:random){inst::require(ch>=L'0'&&ch<=L'F',"ASCII-only random fixture password suffix");ascii_random.push_back(static_cast<char>(ch));}request.sysop_password="Sfx-Aa1-"+ascii_random;request.system_password="Sfx-Zz2-"+ascii_random;std::fill(ascii_random.begin(),ascii_random.end(),'\0');
        check(host.bootstrap(request).code==app::LocalControlResponseCode::ok,"supported first-run API bootstrap, no SQL user seeding or GUI authentication automation");
        const auto policy=host.qwk_policy_snapshot();check(policy.code==app::LocalAccountResultCode::ok&&app::valid_qwk_board_id(policy.policy.board_id)&&policy.policy.maximum_messages==10000&&policy.policy.word_wrap_default==false,"actual new policy identity/count, owner-selected export Off default");
        check(host.shutdown().code==app::LocalControlResponseCode::ok,"normal initialized host stopped");
    }
    std::wcout<<L"FIXTURE "<<root.native()<<L" GENERATION "<<receipt.generation<<std::endl;std::cout<<"PASS "<<checks<<" actual Setup/basic initialized lifecycle checks; not visible UI/caller/NCM/owner acceptance\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL installation after "<<checks<<": "<<e.what()<<'\n';return 1;}
