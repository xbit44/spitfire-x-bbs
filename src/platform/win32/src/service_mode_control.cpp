#include "sfx/platform/win32/service_mode_control.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/service_secret_bridge.hpp"
#include "sfx/platform/win32/portable_installation.hpp"
#include "sfx/platform/win32/portable_preparation.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <sddl.h>
#include <algorithm>
#include <sstream>

namespace sfx::platform::win32 {
namespace {
namespace inst=installation;
using inst::require;using inst::Handle;
constexpr wchar_t prefix[]=L"\\\\.\\pipe\\SFX-Config-Service-";
bool nonce(std::wstring_view s){return s.size()==32&&s.find_first_not_of(L"0123456789ABCDEF")==s.npos;}
std::string ascii(std::wstring_view s){std::string r;for(auto c:s){require(c>=32&&c<127,"service_control_text_invalid");r+=static_cast<char>(c);}return r;}
struct Channel {
    Handle pipe,peer;std::function<void()> pump;
    ULONGLONG timeout{90000};
    ULONGLONG started{GetTickCount64()};
    void tick(){if(pump)pump();require(!peer||WaitForSingleObject(peer.get(),0)==WAIT_TIMEOUT,"service_control_peer_exited");
        require(GetTickCount64()-started<timeout,"service_control_timeout");}
    void transfer(void* bytes,DWORD size,bool write){auto* p=static_cast<BYTE*>(bytes);while(size){
        DWORD done{};const auto ok=write?WriteFile(pipe.get(),p,size,&done,nullptr):ReadFile(pipe.get(),p,size,&done,nullptr);
        if(!ok){const auto e=GetLastError();require(e==ERROR_NO_DATA||e==ERROR_PIPE_LISTENING,"service_control_disconnected");}
        require(done<=size,"service_control_count_invalid");p+=done;size-=done;if(!done){tick();Sleep(20);}
    }}
    void send(const std::string& s){require(!s.empty()&&s.size()<=1024,"service_control_length_invalid");auto n=static_cast<DWORD>(s.size());transfer(&n,4,true);transfer(const_cast<char*>(s.data()),n,true);}
    std::string receive(){DWORD n{};transfer(&n,4,false);require(n&&n<=1024,"service_control_length_invalid");std::string s(n,'\0');transfer(s.data(),n,false);
        require(std::all_of(s.begin(),s.end(),[](char c){return c=='\n'||(c>=32&&c<127);}),"service_control_text_invalid");return s;}
};
LRESULT CALLBACK preparation_progress_proc(HWND window,UINT message,WPARAM wp,LPARAM lp){
    if(message==WM_CLOSE)return 0; // No false cancellation while privileged work may be committing.
    return DefWindowProcW(window,message,wp,lp);
}
struct PreparationProgress {
    HWND window{};
    explicit PreparationProgress(HWND parent){
        const auto instance=GetModuleHandleW(nullptr);WNDCLASSW klass{};klass.hInstance=instance;klass.lpszClassName=L"SFXPreparationProgress";klass.lpfnWndProc=preparation_progress_proc;klass.hCursor=LoadCursorW(nullptr,IDC_WAIT);klass.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        require(RegisterClassW(&klass)||GetLastError()==ERROR_CLASS_ALREADY_EXISTS,"preparation_progress_window_unavailable");
        window=CreateWindowExW(WS_EX_DLGMODALFRAME,klass.lpszClassName,L"Preparing SFX on this computer",WS_CAPTION|WS_POPUP|WS_VISIBLE,
            (GetSystemMetrics(SM_CXSCREEN)-480)/2,(GetSystemMetrics(SM_CYSCREEN)-150)/2,480,150,parent,nullptr,instance,nullptr);
        require(window!=nullptr,"preparation_progress_window_unavailable");
        const auto label=CreateWindowExW(0,L"STATIC",L"Preparing access, saved connections and local registration...\r\n\r\nPlease wait. Your BBS will remain stopped until you choose Start BBS.",WS_CHILD|WS_VISIBLE,20,20,435,85,window,nullptr,instance,nullptr);
        SendMessageW(label,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);UpdateWindow(window);
    }
    ~PreparationProgress(){if(window)DestroyWindow(window);}
    void pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
};
std::string choice(const inst::ServiceMode& m,bool enabled,const inst::Receipt& r,const std::wstring& operation,DWORD policy){return
    ascii(r.id)+"\n"+std::to_string(r.generation)+"\n"+ascii(m.revision)+"\n"+(enabled?"SERVICE":"DESKTOP")+"\n"+std::to_string(policy)+"\n"+ascii(operation);}
struct PanelSearch {const inst::Receipt* receipt;bool found{};};
BOOL CALLBACK find_panel(HWND window,LPARAM parameter){auto& s=*reinterpret_cast<PanelSearch*>(parameter);wchar_t cls[80]{};
    if(GetClassNameW(window,cls,80)&&!wcscmp(cls,L"SFXControlPanelWindow")){
        DWORD pid{};GetWindowThreadProcessId(window,&pid);try{inst::verify_installed_program_process(*s.receipt,pid,inst::control_panel_program_index);s.found=true;}catch(...){}
    }return TRUE;}
void require_stopped(const inst::Receipt& r,const inst::ServiceMode& m){
    require(inst::query_owned_service(m).state==SERVICE_STOPPED,"service_mode_stop_bbs_first");
    PanelSearch search{&r};EnumWindows(find_panel,reinterpret_cast<LPARAM>(&search));
    require(!search.found,"service_mode_close_stopped_control_panel_first");
}
Handle stopped_file_pin(const inst::Path& root){
    const auto file=root/L"Data"/L"SFXRuntime.lock";
    if(GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES){require(GetLastError()==ERROR_FILE_NOT_FOUND,"service_runtime_lock_unavailable");return {};}
    return inst::open_object(file,false,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE);
}
}
ServiceModeChangeResult change_service_mode(const inst::ServiceMode& expected,bool enabled,HWND window,const std::function<void()>& pump,std::optional<DWORD> policy){
    Channel channel;channel.pump=pump;
    ServiceModeChangeResult result;std::uint64_t generation{};
    std::unique_ptr<inst::InstallationVerificationLease> lease;
    std::wstring operation;
    const DWORD wanted_policy=expected.pending?expected.start_policy:!enabled?SERVICE_DISABLED:policy.value_or(SERVICE_AUTO_START);
    try{
        require((enabled&&(wanted_policy==SERVICE_AUTO_START||wanted_policy==SERVICE_DEMAND_START))||(!enabled&&wanted_policy==SERVICE_DISABLED),"service_requested_policy_invalid");
        require(!expected.pending||(enabled==expected.enabled&&(!policy||*policy==expected.start_policy)),"service_pending_choice_must_be_recovered_first");
        operation=expected.pending?expected.revision:inst::unique_id();
        require(!inst::elevated()&&inst::operator_sid()==expected.owner,"service_mode_requires_ordinary_installing_owner");
        const auto r=inst::read_receipt(expected.root,expected.owner);generation=r.generation;
        lease=std::make_unique<inst::InstallationVerificationLease>(r);
        lease->verify_process(r,GetCurrentProcessId(),inst::configuration_program_index);
        require_stopped(r,expected);
        {OfflineInstallationGuard precheck;require(precheck.acquire(expected.root,expected.owner).succeeded(),"service_mode_stop_runtime_or_wait_for_transaction");}
        const auto name=std::wstring(prefix)+inst::unique_id();
        PSECURITY_DESCRIPTOR raw{};const auto sddl=L"D:P(A;;GA;;;"+expected.owner+L")(A;;GA;;;SY)(A;;GA;;;BA)";
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw,nullptr),"service_control_security_failed");
        inst::Descriptor sd(raw);SECURITY_ATTRIBUTES sa{sizeof(sa),sd.get(),FALSE};
        channel.pipe.reset(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,1028,1028,5000,&sa));
        require(channel.pipe.get()!=INVALID_HANDLE_VALUE,"service_control_pipe_failed");
        const auto args=L"--service-mode-worker "+name.substr(std::size(prefix)-1)+L" "+std::to_wstring(GetCurrentProcessId());
        const auto exe=inst::module_path();SHELLEXECUTEINFOW launch{sizeof(launch)};
        launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;launch.hwnd=window;launch.lpVerb=L"runas";
        launch.lpFile=exe.c_str();launch.lpParameters=args.c_str();launch.nShow=SW_HIDE;
        if(!ShellExecuteExW(&launch)){
            if(GetLastError()==ERROR_CANCELLED){ServiceModeChangeResult cancelled;cancelled.cancelled=true;
                cancelled.detail="service_mode_approval_cancelled_draft_retained";return cancelled;}
            throw std::runtime_error("service_mode_admin_launch_failed");
        }
        channel.peer.reset(launch.hProcess);require(channel.peer!=nullptr,"service_control_worker_unavailable");
        for(;;){channel.tick();if(ConnectNamedPipe(channel.pipe.get(),nullptr))break;const auto e=GetLastError();
            if(e==ERROR_PIPE_CONNECTED)break;require(e==ERROR_PIPE_LISTENING,"service_control_connect_failed");Sleep(20);}
        ULONG pid{};require(GetNamedPipeClientProcessId(channel.pipe.get(),&pid)&&pid==GetProcessId(channel.peer.get()),"service_control_worker_mismatch");
        channel.send(choice(expected,enabled,r,operation,wanted_policy));
        auto reply=channel.receive();
        if(reply=="PREPARE"){
            require(enabled,"service_control_unexpected_prepare");prepare_service_secret_bridge(expected.root,expected.owner);
            channel.send("READY");reply=channel.receive();
        }
        result.detail=reply;
        if(reply=="COMMITTED")channel.send("ACK");
    }catch(const std::exception& e){result.detail=e.what();}
    // Closing the private channel releases a worker waiting for ACK. Reap
    // before reconciliation: an in-flight operation is never blindly retried.
    channel.pipe.reset();
    if(channel.peer&&WaitForSingleObject(channel.peer.get(),5000)!=WAIT_OBJECT_0){result.uncertain=true;result.detail="service_worker_still_finishing_reopen_config_before_retry";return result;}
    try {
        const auto r=inst::read_receipt(expected.root,expected.owner);
        const auto actual=inst::read_service_registration(expected.root,true);
        if(r.id==expected.id&&r.generation==generation&&actual&&!actual->pending&&actual->revision==operation&&actual->enabled==enabled){
            const auto status=inst::query_owned_service(*actual);
            if(status.state==SERVICE_STOPPED&&status.start_type==wanted_policy&&actual->start_policy==wanted_policy)
                return {true,false,"service_mode_saved_confirmed_by_operation_requery"};
        }
        result.uncertain=actual&&(actual->pending||actual->revision!=expected.revision);
        if(result.uncertain)result.detail="service_mode_requires_reconciliation_reload_run_mode; "+result.detail;
    }catch(...){result.uncertain=true;result.detail="service_mode_result_unavailable_reopen_config; "+result.detail;}
    return result;
}
PortablePreparationResult offer_portable_preparation(std::size_t role,HWND window){
    Channel channel;channel.timeout=15*60*1000;PortablePreparationResult result;
    std::unique_ptr<PreparationProgress> progress;
    try{
        require(!inst::elevated(),"preparation_open_as_ordinary_current_account");
        const auto root=alpha_installation_root();const auto source=inst::verify_portable_entry(root,role);
        const TASKDIALOG_BUTTON buttons[]{{IDYES,L"Prepare"},{IDCANCEL,L"Cancel"}};
        TASKDIALOGCONFIG dialog{sizeof(dialog)};dialog.hwndParent=window;dialog.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION;
        dialog.pszWindowTitle=L"Spitfire-X BBS";dialog.pszMainInstruction=L"Prepare SFX on this computer";
        const auto explanation = L"Selected folder:\n" + root.native() +
            L"\n\nSFX verified the program but could not verify the existing local registration for this folder. This can happen after a move, copy or restore. "
            L"Prepare updates local access and registration for this copy and preserves the board data. "
            L"The BBS stays stopped until you choose Start BBS. Windows may ask for administrator approval.";
        dialog.pszContent=explanation.c_str();
        dialog.cButtons=2;dialog.pButtons=buttons;dialog.nDefaultButton=IDCANCEL;int selected=IDCANCEL;
        if(FAILED(TaskDialogIndirect(&dialog,&selected,nullptr,nullptr))||selected!=IDYES)return {false,true,"preparation_cancelled_board_stays_stopped"};
        // Both ordinary frontend and consented worker would otherwise keep
        // Windows' implicit no-delete handle on the copied Bin directory.
        PreparationDirectoryScope directory_scope;
        progress=std::make_unique<PreparationProgress>(window);channel.pump=[&]{progress->pump();};
        const auto owner=inst::operator_sid();const auto name=std::wstring(prefix)+inst::unique_id();
        PSECURITY_DESCRIPTOR raw{};const auto sddl=L"D:P(A;;GA;;;"+owner+L")(A;;GA;;;SY)(A;;GA;;;BA)";
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw,nullptr),"preparation_channel_security_failed");
        inst::Descriptor descriptor(raw);SECURITY_ATTRIBUTES attributes{sizeof(attributes),raw,FALSE};
        channel.pipe.reset(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,1028,1028,5000,&attributes));
        require(channel.pipe.get()!=INVALID_HANDLE_VALUE,"preparation_channel_failed");
        const auto arguments=L"--prepare-sfx-worker "+name.substr(std::size(prefix)-1)+L" "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(role);const auto image=inst::module_path();
        SHELLEXECUTEINFOW launch{sizeof(launch)};launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;launch.hwnd=window;launch.lpVerb=L"runas";launch.lpFile=image.c_str();launch.lpParameters=arguments.c_str();launch.nShow=SW_HIDE;
        if(!ShellExecuteExW(&launch)){if(GetLastError()==ERROR_CANCELLED)return {false,true,"preparation_windows_approval_cancelled"};throw std::runtime_error("preparation_windows_approval_failed");}
        channel.peer.reset(launch.hProcess);require(channel.peer!=nullptr,"preparation_worker_unavailable");
        for(;;){channel.tick();if(ConnectNamedPipe(channel.pipe.get(),nullptr))break;const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)break;require(error==ERROR_PIPE_LISTENING,"preparation_channel_connect_failed");Sleep(20);}
        ULONG pid{};require(GetNamedPipeClientProcessId(channel.pipe.get(),&pid)&&pid==GetProcessId(channel.peer.get()),"preparation_worker_process_mismatch");
        channel.send("PREPARE\n"+ascii(source.id));
        auto reply=channel.receive();
        if(reply=="CREDENTIALS"){
            // Legacy readable conversion uses the ordinary current owner's
            // Windows context, never an over-the-shoulder admin DPAPI context.
            const auto credentials=prepare_portable_connection_credentials(root,owner);
            channel.send("READY");reply=channel.receive();
            if(!credentials.issues.empty())result.detail="Some saved connections need attention. Open BBS Health Check for details.";
        }
        if(reply=="ATTENTION"){
            if(!result.detail.empty())result.detail+='\n';
            result.detail+="Some file records could not be verified against this copy. Files and earlier proofs were preserved. Open BBS Health Check before using affected file or FTN work.";
            reply=channel.receive();
        }
        if (reply == "CLAMAV-ATTENTION") {
            if (!result.detail.empty()) result.detail += '\n';
            result.detail += "Portable ClamAV needs attention. Unverified files were preserved. Run matching Setup repair and check SFXConfig > Security > ClamAV.";
            reply = channel.receive();
        }
        if(reply=="COMMITTED"){channel.send("ACK");result.confirmed=true;}
        else result.detail=reply;
    }catch(const std::exception& error){result.detail=error.what();}
    channel.pipe.reset();
    if(channel.peer&&WaitForSingleObject(channel.peer.get(),5000)!=WAIT_OBJECT_0){result.confirmed=false;result.detail="Preparation is still finishing. Keep the BBS stopped and reopen SFX to check the result.";}
    return result;
}
std::optional<int> portable_preparation_worker(std::wstring_view command) noexcept{
    constexpr std::wstring_view option=L"--prepare-sfx-worker ";if(command.substr(0,option.size())!=option)return {};
    Channel channel;channel.timeout=15*60*1000;bool committed=false;
    try{
        require(inst::elevated(),"preparation_worker_windows_approval_required");command.remove_prefix(option.size());
        std::wistringstream input{std::wstring(command)};std::wstring ticket,number,role_text,extra;require(bool(input>>ticket>>number>>role_text)&&!(input>>extra)&&nonce(ticket)&&number.size()<=10&&number.find_first_not_of(L"0123456789")==number.npos&&(role_text==L"0"||role_text==L"4"),"preparation_worker_arguments_invalid");
        const auto parent64=std::stoull(number);require(parent64&&parent64<=MAXDWORD&&std::to_wstring(parent64)==number,"preparation_parent_invalid");const auto parent=DWORD(parent64);const auto role=role_text==L"0"?inst::control_panel_program_index:inst::configuration_program_index;
        const auto root=alpha_installation_root();const auto source=inst::verify_portable_entry(root,role);inst::verify_portable_entry(root,role,parent);
        auto token=inst::process_token(parent);TOKEN_ELEVATION elevation{};DWORD length{};require(GetTokenInformation(token.get(),TokenElevation,&elevation,sizeof(elevation),&length)&&!elevation.TokenIsElevated,"preparation_requires_ordinary_parent");const auto owner=inst::token_sid(token.get());
        channel.peer.reset(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent));require(channel.peer!=nullptr,"preparation_parent_unavailable");
        const auto name=std::wstring(prefix)+ticket;channel.pipe.reset(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr));require(channel.pipe.get()!=INVALID_HANDLE_VALUE,"preparation_channel_unavailable");DWORD mode=PIPE_READMODE_BYTE|PIPE_NOWAIT;require(SetNamedPipeHandleState(channel.pipe.get(),&mode,nullptr,nullptr),"preparation_channel_mode_failed");ULONG pid{};require(GetNamedPipeServerProcessId(channel.pipe.get(),&pid)&&pid==parent,"preparation_parent_channel_mismatch");
        require(channel.receive()=="PREPARE\n"+ascii(source.id),"preparation_source_changed_reopen_sfx");
        prepare_portable_installation(root,owner,[&]{channel.tick();channel.send("CREDENTIALS");require(channel.receive()=="READY","preparation_credentials_not_confirmed");},[&]{channel.send("ATTENTION");},[&]{channel.send("CLAMAV-ATTENTION");});
        committed=true;channel.send("COMMITTED");require(channel.receive()=="ACK","preparation_committed_reopen_sfx_to_confirm");return 0;
    }catch(const std::exception& error){if(committed)return 0;if(channel.pipe&&channel.pipe.get()!=INVALID_HANDLE_VALUE)try{channel.send(std::string("Preparation not confirmed: ")+error.what());}catch(...){}return 15;}
}
std::optional<int> service_mode_worker(std::wstring_view command) noexcept {
    constexpr std::wstring_view option=L"--service-mode-worker ";if(command.substr(0,option.size())!=option)return {};
    Channel channel;bool committed=false;
    try{
        command.remove_prefix(option.size());const auto split=command.find(L' ');
        require(split!=command.npos&&nonce(command.substr(0,split)),"service_worker_arguments_invalid");
        const auto number=command.substr(split+1);require(!number.empty()&&number.size()<=10&&number.find_first_not_of(L"0123456789")==number.npos,"service_worker_pid_invalid");
        const auto pid64=std::stoull(std::wstring(number));require(pid64&&pid64<=MAXDWORD&&std::to_wstring(pid64)==number,"service_worker_pid_invalid");
        const auto parent=static_cast<DWORD>(pid64);const auto name=std::wstring(prefix)+std::wstring(command.substr(0,split));
        const auto root=alpha_installation_root();const auto r=inst::verify_configuration_recovery_entry(root);
        require(inst::operator_sid()==r.owner,"service_worker_other_admin_refused");
        inst::InstallationVerificationLease lease(r);lease.verify_process(r,parent,inst::configuration_program_index);
        auto token=inst::process_token(parent);TOKEN_ELEVATION elevation{};DWORD size{};
        require(GetTokenInformation(token.get(),TokenElevation,&elevation,sizeof(elevation),&size)&&!elevation.TokenIsElevated,"service_worker_requires_ordinary_config_parent");
        channel.peer.reset(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent));require(channel.peer!=nullptr,"service_worker_parent_unavailable");
        channel.pipe.reset(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr));
        require(channel.pipe.get()!=INVALID_HANDLE_VALUE,"service_worker_channel_unavailable");DWORD mode=PIPE_READMODE_BYTE|PIPE_NOWAIT;
        require(SetNamedPipeHandleState(channel.pipe.get(),&mode,nullptr,nullptr),"service_worker_channel_mode_failed");
        ULONG server{};require(GetNamedPipeServerProcessId(channel.pipe.get(),&server)&&server==parent,"service_worker_parent_mismatch");
        const auto message=channel.receive();const auto last=message.rfind('\n');require(last!=message.npos,"service_worker_request_invalid");
        const std::wstring operation(message.begin()+last+1,message.end());require(nonce(operation),"service_worker_operation_invalid");
        const auto m=inst::read_service_registration(root,true);require(m.has_value(),"service_registration_missing_run_setup");
        const bool automatic=message==choice(*m,true,r,operation,SERVICE_AUTO_START);
        const bool enabled=automatic||message==choice(*m,true,r,operation,SERVICE_DEMAND_START);
        const DWORD policy=enabled?(automatic?SERVICE_AUTO_START:SERVICE_DEMAND_START):SERVICE_DISABLED;
        require(enabled||message==choice(*m,false,r,operation,SERVICE_DISABLED),"service_mode_changed_reload_before_retry");
        require(!m->pending||(m->enabled==enabled&&m->start_policy==policy),"service_pending_choice_must_be_recovered_first");
        OfflineInstallationGuard guard;require(guard.acquire(root,r.owner).succeeded(),"service_mode_stop_runtime_or_wait_for_transaction");
        auto lock=stopped_file_pin(root);require_stopped(r,*m);
        if(enabled){channel.send("PREPARE");require(channel.receive()=="READY","service_mode_credentials_not_prepared");}
        channel.tick();lease.verify_process(r,parent,inst::configuration_program_index);require_stopped(r,*m);
        const auto current=inst::read_service_registration(root,true);
        require(current&&current->revision==m->revision&&current->pending==m->pending,"service_mode_changed_reload_before_retry");
        inst::configure_owned_service(root,r.owner,enabled,operation,policy);committed=true;channel.send("COMMITTED");
        require(channel.receive()=="ACK","service_mode_committed_requery_before_retry");return 0;
    }catch(const std::exception& e){
        // A lost acknowledgement cannot undo or relabel a committed result.
        if(committed)return 0;
        if(channel.pipe&&channel.pipe.get()!=INVALID_HANDLE_VALUE)try{channel.send(std::string("Apply not confirmed: ")+e.what());}catch(...){}return 14;}
}
}
