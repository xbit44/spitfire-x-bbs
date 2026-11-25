// Current fixed-root workspace test driver, normal authenticated installed owner IPC.
// No sibling Runtime substitution, helper staging, bootstrap or root overrides.
#include "sfx/platform/win32/backup_collections.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include "sfx/application/ftn_retention.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include <chrono>
#include <iostream>
#include <algorithm>
namespace ins=sfx::platform::win32::installation;
namespace win=sfx::platform::win32;namespace win32=win;namespace app=sfx::application;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;ins::require(ok,why);}
struct RuntimeSession {
    ins::Handle process;win::NamedPipeConnection connection;std::uint32_t sequence{};app::AlphaStatusTracker tracker;
    explicit RuntimeSession(const ins::Path& root,bool bootstrap=false){
        ins::verify_runtime_installation(root,ins::operator_sid());std::array<std::uint8_t,app::kLocalControlNonceBytes> nonce{};check(win::fill_secure_random(nonce.data(),nonce.size()),"fresh owner nonce");
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE read{},write{};check(CreatePipe(&read,&write,&sa,0),"owner channel");ins::Handle reader(read),writer(write);check(SetHandleInformation(write,HANDLE_FLAG_INHERIT,0),"writer private");
        SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);std::vector<std::uint8_t> buffer(size);auto* attributes=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        check(InitializeProcThreadAttributeList(attributes,1,0,&size),"native attributes");struct Release{PPROC_THREAD_ATTRIBUTE_LIST p;~Release(){DeleteProcThreadAttributeList(p);}} release{attributes};
        check(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&read,sizeof(read),nullptr,nullptr),"exact inherited owner handle");
        const auto exe=root/L"Bin"/L"SFXRuntime.exe";auto command=L"\""+exe.native()+L"\" --owner-handle="+std::to_wstring(reinterpret_cast<ULONG_PTR>(read));
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=SW_HIDE;startup.lpAttributeList=attributes;PROCESS_INFORMATION child{};
        check(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup.StartupInfo,&child),"installed hook-free RuntimeSession process");process.reset(child.hProcess);ins::Handle thread(child.hThread);reader.reset();DWORD written{},pid=GetCurrentProcessId();
        check(WriteFile(write,&pid,sizeof(pid),&written,nullptr)&&written==sizeof(pid)&&WriteFile(write,nonce.data(),static_cast<DWORD>(nonce.size()),&written,nullptr)&&written==nonce.size(),"owner PID and nonce bound");writer.reset();
        connection=win::connect_local_control_server(win::installation_object_names(root).pipe_name,child.dwProcessId,30000);check(connection.valid(),"installed authenticated pipe");
        const auto hello=app::decode_control_response(exchange(app::LocalControlMessageType::hello,app::LocalControlMessageType::hello_response,app::encode_hello_payload(nonce)));SecureZeroMemory(nonce.data(),nonce.size());
        if(hello)std::cout<<"START "<<hello->diagnostic_code<<std::endl;
        check(hello&&hello->code==(bootstrap?app::LocalControlResponseCode::setup_required:app::LocalControlResponseCode::ok),"installed current IPC expected startup state");check(tracker.bind_authenticated_epoch(hello->runtime_epoch),"authenticated current epoch");
    }
    ~RuntimeSession(){connection.disconnect();if(process&&WaitForSingleObject(process.get(),30000)!=WAIT_OBJECT_0)std::cerr<<"FAIL owned RuntimeSession exit unconfirmed; do not force-stop\n";}
    std::vector<std::uint8_t> exchange(app::LocalControlMessageType type,app::LocalControlMessageType expected,std::vector<std::uint8_t> payload={}){
        app::LocalControlFrame request,response;request.type=type;request.request_id=++sequence;request.payload=std::move(payload);const auto ok=connection.write_frame(request)&&connection.read_frame(response)&&response.type==expected&&response.request_id==sequence;SecureZeroMemory(request.payload.data(),request.payload.size());check(ok,"matched installed response");return response.payload;
    }
    void stop(){const auto response=app::decode_control_response(exchange(app::LocalControlMessageType::shutdown_request,app::LocalControlMessageType::shutdown_response));check(response&&response->code==app::LocalControlResponseCode::ok,"normal RuntimeSession shutdown response");connection.disconnect();DWORD code{};check(WaitForSingleObject(process.get(),30000)==WAIT_OBJECT_0&&GetExitCodeProcess(process.get(),&code)&&code==0,"normal RuntimeSession exit0");}
};
struct RuntimeClient {
    ins::Path root=ins::canonical_root(L"C:\\Program Files\\SFX-Section14-Test-20261030-r4");
    std::unique_ptr<RuntimeSession> session;
    RuntimeClient(){
        check(!ins::elevated(),"normal owner only");
        const auto receipt=ins::read_receipt(root,ins::operator_sid());
        check(receipt.generation==4&&receipt.id==L"CD32AD93BBF78EB4D11959A6F89BBF21","exact current generation4 fixture");
    }
    bool launch(std::string&){check(!session,"previous RuntimeSession fully released");session=std::make_unique<RuntimeSession>(root);return true;}
    bool child_alive()const{return session&&WaitForSingleObject(session->process.get(),0)==WAIT_TIMEOUT;}
    std::optional<app::AlphaRuntimeStatus> status(){auto result=app::read_status_snapshot(0,[this](const app::AlphaStatusRequest& q)->std::optional<std::vector<std::uint8_t>>{return session->exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(q));});check(result&&session->tracker.accept(*result),"current installed paged status and epoch");return result;}
    app::FtnRuntimeResponse ftn_runtime(const app::FtnRuntimeRequest& q){auto bytes=session->exchange(app::LocalControlMessageType::ftn_runtime_request,app::LocalControlMessageType::ftn_runtime_response,app::encode_ftn_runtime_request(q));return app::decode_ftn_runtime_response(bytes).value_or(app::FtnRuntimeResponse{});}
    app::FtnConfigurationResponse request(const app::FtnConfigurationRequest& q,bool hatch){
        auto bytes=app::encode_ftn_configuration_request(q);if(bytes.empty())return {};
        const auto response=session->exchange(hatch?app::LocalControlMessageType::ftn_hatch_catalog_request:app::LocalControlMessageType::ftn_configuration_request,hatch?app::LocalControlMessageType::ftn_hatch_catalog_response:app::LocalControlMessageType::ftn_configuration_response,std::move(bytes));
        return app::decode_ftn_configuration_response(response).value_or(app::FtnConfigurationResponse{});
    }
    auto ftn_configuration(const app::FtnConfigurationRequest& q){return request(q,false);}
    auto ftn_hatch_catalog(const app::FtnConfigurationRequest& q){return request(q,true);}
    app::FileManagementJobStatus file_management(const app::FileManagementTask& task){
        auto response=session->exchange(app::LocalControlMessageType::file_management_start_request,app::LocalControlMessageType::file_management_start_response,app::encode_file_management_task(task));
        auto result=app::decode_file_management_status(response).value_or(app::FileManagementJobStatus{});
        const auto deadline=GetTickCount64()+30000;
        while(result.code==app::LocalAccountResultCode::ok&&result.state==app::FileManagementJobState::running&&GetTickCount64()<deadline){
            Sleep(5);response=session->exchange(app::LocalControlMessageType::file_management_status_request,app::LocalControlMessageType::file_management_status_response,app::encode_file_management_job(task.job));
            result=app::decode_file_management_status(response).value_or(app::FileManagementJobStatus{});
        }
        return result;
    }
    bool restart(std::string& why){if(session){session->stop();session.reset();}return launch(why);}
    bool shutdown(std::string&){session->stop();session.reset();return true;}
};
app::FtnTransferToken token(){win32::WindowsSecureIdentifierGenerator ids;return ids.generate().bytes;}
auto exchange(RuntimeClient& r,const app::FtnConfigurationRequest& q,bool hatch=false){return hatch?r.ftn_hatch_catalog(q):r.ftn_configuration(q);}
std::vector<std::uint8_t> read_bytes(RuntimeClient& r,bool hatch=false){app::FtnConfigurationRequest q;q.token=token();std::vector<std::uint8_t> bytes;std::int64_t revision{};std::uint32_t total{};do{const auto p=exchange(r,q,hatch);check(p.code==app::FtnConfigurationCode::ok&&!p.bytes.empty()&&p.offset==bytes.size(),"real protected read page");if(!revision){revision=p.revision;total=p.total;}check(p.revision==revision&&p.total==total&&p.bytes.size()<=app::kFtnConfigurationPageBytes,"bounded pinned page");bytes.insert(bytes.end(),p.bytes.begin(),p.bytes.end());q.offset=static_cast<std::uint32_t>(bytes.size());q.revision=revision;}while(bytes.size()<total);q.operation=app::FtnConfigurationOperation::cancel;q.offset=0;check(exchange(r,q,hatch).code==app::FtnConfigurationCode::ok,"release read snapshot");return bytes;}
auto read(RuntimeClient& r){const auto c=app::decode_ftn_configuration(read_bytes(r));check(c.has_value(),"typed actual owner snapshot");return *c;}
auto upload(RuntimeClient& r,const app::FtnConfiguration& c,app::FtnTransferToken id,bool retry=false,app::FtnConfigurationOperation finish=app::FtnConfigurationOperation::commit){const auto bytes=app::encode_ftn_configuration(c);check(!bytes.empty(),"valid staged catalog");app::FtnConfigurationRequest q;q.token=id;q.operation=app::FtnConfigurationOperation::begin;q.revision=c.revision;q.total=static_cast<std::uint32_t>(bytes.size());auto p=exchange(r,q);if(p.code!=app::FtnConfigurationCode::ok)return p;q.operation=app::FtnConfigurationOperation::append;for(std::size_t at=0;at<bytes.size();at+=app::kFtnConfigurationPageBytes){q.offset=static_cast<std::uint32_t>(at);const auto end=std::min(bytes.size(),at+app::kFtnConfigurationPageBytes);q.bytes={bytes.begin()+at,bytes.begin()+end};p=exchange(r,q);check(p.code==app::FtnConfigurationCode::ok&&p.offset==end,"real ordered upload");if(retry)check(exchange(r,q).offset==end,"real duplicate page idempotent");}q.operation=finish;q.offset=0;q.bytes.clear();return exchange(r,q);}
}
