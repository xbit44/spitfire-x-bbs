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

int wmain(int argc,wchar_t** argv)try{
 check(argc==2,"explicit owned beta3 fixture action");
 const auto root=inst::canonical_root(L"C:/Program Files/SFX-Beta3-Test-20261112-r1");
 inst::verify_runtime_installation(root,inst::operator_sid());
 const std::wstring action=argv[1];
 if(action==L"--runtime"){proxy(root);return 0;}
 check(action==L"--bootstrap","closed beta3 first-run action");
 check(!std::filesystem::exists(root/L"Config/SFX.conf"),"unconfigured owned test board only");
 sfx::apps::runtime::AlphaRuntimeHost host(root);
 const auto first=host.start();std::cout<<"FIRST_RUN "<<static_cast<unsigned>(first.code)<<' '<<first.diagnostic_code<<std::endl;
 check(first.code==app::LocalControlResponseCode::setup_required,"normal installed first-run state");
 app::AlphaBootstrapRequest request;
 request.configuration.bbs_name="Beta3 Installed Caller Test";
 request.configuration.bbs_time_zone="Pacific Standard Time";
 request.configuration.logical_nodes=2;
 request.configuration.telnet.bind_address="127.0.0.1";request.configuration.telnet.port=18845;
 request.configuration.telnet.bot_gate_enabled=false;request.configuration.botgate.enabled=false;
 request.configuration.ssh.enabled=true;request.configuration.ssh.bind_address="127.0.0.1";request.configuration.ssh.port=18846;
 request.sysop_name="Sysop Fixture";request.sysop_password="Synthetic-owner-only-123";request.system_password="Synthetic-system-only-456";
 const auto boot=host.bootstrap(request);app::erase_bootstrap_secrets(request);
 check(boot.code==app::LocalControlResponseCode::ok,"normal first-run bootstrap");
 check(host.shutdown().code==app::LocalControlResponseCode::ok,"normal bootstrap shutdown");
 std::cout<<"PASS beta3 installed bootstrap\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL beta3 installed Runtime: "<<e.what()<<'\n';return 1;}
