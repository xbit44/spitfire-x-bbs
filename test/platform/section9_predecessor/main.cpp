// Test coordinator linked ONLY to accepted Section8D source/libraries/resources.
// New root only, normal Setup consent, actual old shipping Runtime, no SQL seed.
#include "setup_engine.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include <objbase.h>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>
namespace ins=sfx::platform::win32::installation;
namespace win=sfx::platform::win32;namespace app=sfx::application;namespace setup=sfx::apps::setup;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;ins::require(ok,why);std::cout<<"PASS "<<why<<std::endl;}
struct Runtime {
    ins::Handle process;win::NamedPipeConnection connection;std::uint32_t sequence{};
    explicit Runtime(const ins::Path& root,bool bootstrap){
        ins::verify_runtime_installation(root,ins::operator_sid());std::array<std::uint8_t,app::kLocalControlNonceBytes> nonce{};check(win::fill_secure_random(nonce.data(),nonce.size()),"fresh owner nonce");
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE read{},write{};check(CreatePipe(&read,&write,&sa,0),"owner channel");ins::Handle reader(read),writer(write);check(SetHandleInformation(write,HANDLE_FLAG_INHERIT,0),"writer private");
        SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);std::vector<std::uint8_t> buffer(size);auto* attributes=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        check(InitializeProcThreadAttributeList(attributes,1,0,&size),"native attributes");struct Release{PPROC_THREAD_ATTRIBUTE_LIST p;~Release(){DeleteProcThreadAttributeList(p);}} release{attributes};
        check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&read,sizeof(read),nullptr,nullptr),"exact inherited owner handle");
        const auto exe=root/L"Bin"/L"SFXRuntime.exe";auto command=L"\""+exe.native()+L"\" --owner-handle="+std::to_wstring(reinterpret_cast<ULONG_PTR>(read));
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;startup.lpAttributeList=attributes;PROCESS_INFORMATION child{};
        check(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup.StartupInfo,&child),"actual accepted installed Runtime process");process.reset(child.hProcess);ins::Handle thread(child.hThread);reader.reset();DWORD written{},pid=GetCurrentProcessId();
        check(WriteFile(write,&pid,sizeof(pid),&written,nullptr)&&written==sizeof(pid)&&WriteFile(write,nonce.data(),static_cast<DWORD>(nonce.size()),&written,nullptr)&&written==nonce.size(),"owner PID and nonce bound");writer.reset();
        connection=win::connect_local_control_server(win::installation_object_names(root).pipe_name,child.dwProcessId,30000);check(connection.valid(),"installed authenticated predecessor pipe");
        const auto hello=app::decode_control_response(exchange(app::LocalControlMessageType::hello,app::LocalControlMessageType::hello_response,app::encode_hello_payload(nonce)));SecureZeroMemory(nonce.data(),nonce.size());
        if(hello)std::cout<<"START "<<hello->diagnostic_code<<std::endl;
        check(hello&&hello->code==(bootstrap?app::LocalControlResponseCode::setup_required:app::LocalControlResponseCode::ok),"accepted IPC32 expected startup state");
    }
    ~Runtime(){connection.disconnect();if(process&&WaitForSingleObject(process.get(),30000)!=WAIT_OBJECT_0)std::cerr<<"FAIL owned predecessor exit unconfirmed; do not force-stop\n";}
    std::vector<std::uint8_t> exchange(app::LocalControlMessageType type,app::LocalControlMessageType expected,std::vector<std::uint8_t> payload={}){
        app::LocalControlFrame request,response;request.type=type;request.request_id=++sequence;request.payload=std::move(payload);const auto ok=connection.write_frame(request)&&connection.read_frame(response)&&response.type==expected&&response.request_id==sequence;SecureZeroMemory(request.payload.data(),request.payload.size());check(ok,"matched accepted predecessor response");return response.payload;
    }
    void stop(){const auto response=app::decode_control_response(exchange(app::LocalControlMessageType::shutdown_request,app::LocalControlMessageType::shutdown_response));check(response&&response->code==app::LocalControlResponseCode::ok,"normal predecessor shutdown response");connection.disconnect();DWORD code{};check(WaitForSingleObject(process.get(),30000)==WAIT_OBJECT_0&&GetExitCodeProcess(process.get(),&code)&&code==0,"normal predecessor exit0");}
};
std::string token(){std::string out;for(const auto c:ins::unique_id())out+=static_cast<char>(c);return out;}
void initialize(const ins::Path& root){
    Runtime runtime(root,true);app::AlphaBootstrapRequest request;struct Erase{app::AlphaBootstrapRequest& request;~Erase(){app::erase_bootstrap_secrets(request);}} erase{request};
    const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& query)->std::optional<std::vector<std::uint8_t>>{return runtime.exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(query));});
    check(status&&status->security_policy_revision>0&&status->security_policy_hash.size()==64,"authoritative old bootstrap policy, same as normal wizard");
    request.configuration.security_policy_revision=status->security_policy_revision;request.configuration.security_policy_hash=status->security_policy_hash;
    request.configuration.bbs_name="Section9 Genuine 8D Predecessor";request.configuration.bbs_time_zone="Pacific Standard Time";request.configuration.logical_nodes=4;
    request.configuration.telnet.bind_address="127.0.0.1";request.configuration.telnet.port=26010;request.configuration.telnet.bot_gate_enabled=false;request.configuration.botgate.enabled=false;
    request.configuration.ssh.enabled=true;request.configuration.ssh.bind_address="127.0.0.1";request.configuration.ssh.port=26011;
    request.sysop_name="Section9 Predecessor Sysop";request.sysop_password="Sfx-Aa1-"+token();request.system_password="Sfx-Zz2-"+token();
    auto encoded=app::encode_bootstrap_request(request);check(!encoded.empty(),"valid complete predecessor bootstrap request");
    const auto response=app::decode_control_response(runtime.exchange(app::LocalControlMessageType::bootstrap_request,app::LocalControlMessageType::bootstrap_response,std::move(encoded)));
    if(response)std::cout<<"BOOTSTRAP "<<response->diagnostic_code<<std::endl;
    check(response&&response->code==app::LocalControlResponseCode::ok,"actual accepted shipping first-run bootstrap");runtime.stop();
}
void proxy(const ins::Path& root){
    Runtime runtime(root,false);std::cout<<"READY Predecessor"<<std::endl;
    for(std::string line;std::getline(std::cin,line)&&line!="stop";){
        struct Erase{std::string& line;~Erase(){std::fill(line.begin(),line.end(),'\0');}} erase{line};check(line.size()<=2*app::kLocalControlMaximumPayloadBytes+80,"bounded coordinator input");
        std::istringstream input(line);unsigned type{},expected{};std::string encoded;input>>type>>expected>>encoded;
        check(type>=1&&type<=62&&expected>=1&&expected<=62&&(encoded=="-"||encoded.size()%2==0),"closed old IPC types");std::vector<std::uint8_t> payload;
        if(encoded!="-"){check(encoded.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos,"hex input");for(std::size_t i=0;i<encoded.size();i+=2)payload.push_back(static_cast<std::uint8_t>(std::stoul(encoded.substr(i,2),nullptr,16)));}
        std::fill(encoded.begin(),encoded.end(),'\0');const auto response=runtime.exchange(static_cast<app::LocalControlMessageType>(type),static_cast<app::LocalControlMessageType>(expected),std::move(payload));
        std::ostringstream out;out<<std::hex<<std::setfill('0');for(const auto byte:response)out<<std::setw(2)<<static_cast<unsigned>(byte);std::cout<<"RESPONSE "<<out.str()<<std::endl;
    }runtime.stop();
}
}
int wmain(int argc,wchar_t** argv)try{
    if(argc==4&&std::wstring(argv[1])==L"--worker"){const std::wstring pid=argv[3];ins::require(!pid.empty()&&pid.size()<=10&&pid.find_first_not_of(L"0123456789")==std::wstring::npos,"canonical worker PID");const auto value=std::stoull(pid);ins::require(value>0&&value<=MAXDWORD&&std::to_wstring(value)==pid,"bounded worker PID");return setup::worker(argv[2],static_cast<DWORD>(value));}
    check(argc==3,"explicit predecessor action and fixture");const std::wstring action=argv[1];const auto root=ins::canonical_root(argv[2]);
    check(!ins::elevated()&&root==ins::Path(L"C:\\Program Files\\SFX-Section9-Test-20261017-r5"),"ordinary exact new predecessor fixture only");
    if(action==L"--runtime"){proxy(root);return 0;}
    if(action==L"--initialize"){initialize(root);return 0;}
    check(action==L"--fresh"&&!std::filesystem::exists(root),"fresh predecessor never adopts existing data");
    setup::verify_payload();const auto plan=setup::inspect_plan(root,false);check(plan.operation==setup::Operation::fresh,"genuine accepted Setup reviewed plan");
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);check(SUCCEEDED(com),"normal Setup COM");struct Com{~Com(){CoUninitialize();}} release;
    const auto started=GetTickCount64();check(setup::execute(plan,{false,false},nullptr,[](const auto& message){std::wcout<<message<<std::endl;},[started]{return GetTickCount64()-started<600000;}),"actual accepted Setup with normal Windows consent");
    const auto owner=ins::operator_sid();ins::verify_runtime_installation(root,owner);const auto receipt=ins::read_receipt(root,owner);
    check(receipt.committed&&receipt.program_count==7&&receipt.generation==1,"genuine accepted seven-program generation1");
    std::cout<<"PASS predecessor fresh checks="<<checks<<"; no new Section9 migration yet\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL predecessor after "<<checks<<": "<<e.what()<<'\n';return 1;}
