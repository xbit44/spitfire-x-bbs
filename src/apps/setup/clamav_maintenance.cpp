#include "clamav_maintenance.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/clamav_update.hpp"
#include <shellapi.h>
#include <sddl.h>
#include <algorithm>
#include <chrono>

namespace sfx::apps::setup {
namespace {
namespace ins=platform::win32::installation;namespace win=platform::win32;using ins::require;using Path=std::filesystem::path;
constexpr wchar_t prefix[]=L"\\\\.\\pipe\\SFX-ClamAV-Install-";
bool nonce(const std::wstring& s){return s.size()==32&&s.find_first_not_of(L"0123456789ABCDEF")==std::wstring::npos;}
std::string ascii(const std::wstring& s){require(!s.empty()&&s.find_first_not_of(L" !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~")==std::wstring::npos,"clamav_maintenance_argument");std::string out;for(const auto c:s)out+=static_cast<char>(c);return out;}
std::wstring wide(const std::string& s){return {s.begin(),s.end()};}
struct Channel {
    ins::Handle pipe,peer;ClamAvDownloadPump pump;ULONGLONG started{GetTickCount64()};
    void tick(){require(!peer||WaitForSingleObject(peer.get(),0)==WAIT_TIMEOUT,"clamav_maintenance_peer_exited");require(GetTickCount64()-started<900000,"clamav_maintenance_timeout");require(!pump||pump(),"clamav_maintenance_cancelled");}
    void transfer(void* raw,DWORD bytes,bool writing){auto* p=static_cast<BYTE*>(raw);const auto start=GetTickCount64();while(bytes){require(!pump||pump(),"clamav_maintenance_cancelled");DWORD done{};const auto ok=writing?WriteFile(pipe.get(),p,bytes,&done,nullptr):ReadFile(pipe.get(),p,bytes,&done,nullptr);if(!ok){const auto e=GetLastError();require(e==ERROR_NO_DATA||e==ERROR_PIPE_LISTENING,"clamav_maintenance_connection_lost");}require(done<=bytes,"clamav_maintenance_packet");p+=done;bytes-=done;if(!done){tick();require(GetTickCount64()-start<60000,"clamav_maintenance_connection_timeout");Sleep(20);}}}
    void send(const std::string& s){require(!s.empty()&&s.size()<=4096,"clamav_maintenance_packet");auto n=static_cast<DWORD>(s.size());transfer(&n,4,true);transfer(const_cast<char*>(s.data()),n,true);}
    std::string receive(){DWORD n{};transfer(&n,4,false);require(n&&n<=4096,"clamav_maintenance_packet");std::string s(n,'\0');transfer(s.data(),n,false);require(std::all_of(s.begin(),s.end(),[](char c){return c=='\n'||(c>=32&&c<127);}),"clamav_maintenance_packet");return s;}
};
ins::Receipt reviewed(const Path& root,const std::wstring& owner){const auto r=ins::read_receipt(root,owner);ins::verify_structure(r);require(r.committed&&r.program_count==ins::current_program_count,"clamav_maintenance_update_sfx_first");return r;}
void initial_definitions(const Path& root,const ins::Receipt& receipt,const ClamAvDownloadProgress& progress,const ClamAvDownloadPump& pump){
    win::initialize_clamav_data(root,receipt.owner);progress(L"Checking initial definitions. Daily updates remain owned by Runtime.");
    const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto result=win::update_clamav_definitions(root,receipt.owner,receipt.id,now,[&]{return pump&&!pump();},[&](std::uint64_t){if(pump)(void)pump();},180000,win::ClamAvUpdateMode::manual);
    if(!result.succeeded)throw std::runtime_error("Portable ClamAV is installed. Definitions are not confirmed ready: "+result.diagnostic+". Use Update definitions now to retry; scanning errors hold uploads privately.");
    progress(L"Portable ClamAV installed; definitions verified. No scanning-policy choice was changed.");
}
}
bool install_portable_clamav(const Path& input,HWND window,const ClamAvDownloadProgress& progress,const ClamAvDownloadPump& pump){
    require(win::clamav_available(),"optional_component_unavailable_on_this_os");
    require(!ins::elevated(),"clamav_maintenance_ordinary_coordinator_required");const auto root=ins::canonical_root(input);const auto owner=ins::operator_sid();
    const auto token=ins::process_token();const auto anchors=ins::protect_ancestors(root,token.get());const auto receipt=reviewed(root,owner);
    win::OfflineInstallationGuard guard;require(guard.acquire(root,owner).succeeded(),"clamav_maintenance_stop_bbs_first");
    const bool upgrading=win::clamav_upgrade_required(root,owner,receipt.id);
    if(win::clamav_installed(root)&&!upgrading){win::verify_clamav_installation(root,owner,receipt.id);initial_definitions(root,receipt,progress,pump);return true;}
    auto downloaded=DownloadedClamAv::fetch(progress,pump);
    const auto current=reviewed(root,owner);require(ins::encode_receipt(current)==ins::encode_receipt(receipt),"clamav_maintenance_review_changed");
    const auto worker=root/L"Bin"/ins::programs[ins::maintenance_program_index];auto pinned=ins::open_object(worker,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
    require(ins::file_sha256(worker)==receipt.program_hashes[ins::maintenance_program_index],"clamav_maintenance_image_changed");
    const auto name=std::wstring(prefix)+ins::unique_id();void* raw{};require(ConvertStringSecurityDescriptorToSecurityDescriptorW((L"D:P(A;;GA;;;"+owner+L")(A;;GA;;;SY)(A;;GA;;;BA)").c_str(),SDDL_REVISION_1,&raw,nullptr),"clamav_maintenance_pipe_security");ins::Descriptor sd(raw);SECURITY_ATTRIBUTES sa{sizeof(sa),sd.get(),FALSE};
    Channel channel;channel.pump=pump;channel.pipe.reset(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4100,4100,5000,&sa));require(channel.pipe.get()!=INVALID_HANDLE_VALUE,"clamav_maintenance_pipe_create");
    const auto args=L"--worker "+name+L" "+std::to_wstring(GetCurrentProcessId());SHELLEXECUTEINFOW launch{};launch.cbSize=sizeof(launch);launch.hwnd=window;launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;launch.lpVerb=L"runas";launch.lpFile=worker.c_str();launch.lpParameters=args.c_str();launch.nShow=SW_HIDE;progress(L"Waiting for Windows administrator approval...");
    if(!ShellExecuteExW(&launch)){if(GetLastError()==ERROR_CANCELLED)return false;throw std::runtime_error("clamav_maintenance_launch_failed");}channel.peer.reset(launch.hProcess);require(bool(channel.peer),"clamav_maintenance_worker_missing");
    const auto start=GetTickCount64();for(;;){channel.tick();if(ConnectNamedPipe(channel.pipe.get(),nullptr)||GetLastError()==ERROR_PIPE_CONNECTED)break;require(GetLastError()==ERROR_PIPE_LISTENING&&GetTickCount64()-start<60000,"clamav_maintenance_connect");Sleep(20);}
    ULONG client{};require(GetNamedPipeClientProcessId(channel.pipe.get(),&client)&&client==GetProcessId(channel.peer.get()),"clamav_maintenance_worker_identity");ins::verify_installed_program_process(receipt,client,ins::maintenance_program_index);
    channel.send(ascii(root.native()));const auto bytes=ins::encode_receipt(receipt);channel.send({bytes.begin(),bytes.end()});channel.send(ascii(downloaded->path().native()));
    progress(L"Installing the verified Portable ClamAV component...");for(;;){const auto reply=channel.receive();if(reply=="BUSY"){channel.send("CONTINUE");continue;}require(reply=="INSTALLED","clamav_maintenance_install_failed");break;}
    channel.send("ACK");require(WaitForSingleObject(channel.peer.get(),10000)==WAIT_OBJECT_0,"clamav_maintenance_worker_exit_unconfirmed");DWORD exit{};require(GetExitCodeProcess(channel.peer.get(),&exit)&&exit==0,"clamav_maintenance_worker_failed");
    win::verify_clamav_installation(root,owner,receipt.id);
    if(upgrading&&win::inspect_clamav_status(root,owner,receipt.id).definitions_verified)
        progress(L"Portable ClamAV 1.4.7 installed. Existing definitions are verified. The scanning policy was preserved.");
    else initial_definitions(root,receipt,progress,pump);
    return true;
}
int clamav_maintenance_worker(const std::wstring& name,DWORD parent)noexcept{
    Channel channel;try{
        require(win::clamav_available(),"optional_component_unavailable_on_this_os");
        require(ins::elevated()&&parent&&name.rfind(prefix,0)==0&&nonce(name.substr(std::wstring(prefix).size())),"clamav_maintenance_worker_arguments");channel.peer.reset(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent));require(bool(channel.peer),"clamav_maintenance_parent_unavailable");
        const auto token=ins::process_token(parent);const auto owner=ins::token_sid(token.get());require(owner==ins::operator_sid(),"clamav_maintenance_other_credential_refused");
        channel.pipe.reset(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr));require(channel.pipe.get()!=INVALID_HANDLE_VALUE,"clamav_maintenance_pipe_open");DWORD mode=PIPE_READMODE_BYTE|PIPE_NOWAIT;require(SetNamedPipeHandleState(channel.pipe.get(),&mode,nullptr,nullptr),"clamav_maintenance_pipe_mode");ULONG server{};require(GetNamedPipeServerProcessId(channel.pipe.get(),&server)&&server==parent,"clamav_maintenance_parent_identity");
        const auto root=ins::canonical_root(wide(channel.receive()));const auto expected=channel.receive();const auto package=ins::canonical_root(wide(channel.receive()));const auto anchors=ins::protect_ancestors(root,token.get());const auto receipt=reviewed(root,owner);const auto raw=ins::encode_receipt(receipt);require(expected==std::string(raw.begin(),raw.end()),"clamav_maintenance_receipt_changed");
        const auto self=ins::module_path();require(!_wcsicmp(self.c_str(),(root/L"Bin"/ins::programs[ins::maintenance_program_index]).c_str()),"clamav_maintenance_wrong_worker");ins::verify_installed_program_process(receipt,GetCurrentProcessId(),ins::maintenance_program_index);
        const auto names=win::installation_object_names(root);ins::Handle lifetime(OpenMutexW(SYNCHRONIZE,FALSE,names.mutex_name.c_str()));require(bool(lifetime),"clamav_maintenance_guard_missing");
        win::verify_clamav_package(package);auto last=GetTickCount64();const auto tick=[&]{channel.tick();if(GetTickCount64()-last>=500){channel.send("BUSY");require(channel.receive()=="CONTINUE","clamav_maintenance_cancelled");last=GetTickCount64();}};
        win::provision_clamav_engine(root,owner,receipt.id,package,tick);channel.send("INSTALLED");require(channel.receive()=="ACK","clamav_maintenance_ack_missing");return 0;
    }catch(...){if(channel.pipe&&channel.pipe.get()!=INVALID_HANDLE_VALUE)try{channel.send("FAILED");}catch(...){}return 20;}
}
} // namespace sfx::apps::setup
