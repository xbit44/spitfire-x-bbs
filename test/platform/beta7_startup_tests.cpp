// Exercise the shipping worker/handoff/close code with real native windows,
// threads and an exact owned child. Trust/Prepare/dialog ports are synthetic;
// no installed board, service, real prompts or public listeners are touched.
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/portable_installation.hpp"
#include "sfx/platform/win32/portable_preparation.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include <commctrl.h>
#include <atomic>
#include <thread>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <vector>
namespace fixture {
DWORD ui{};std::atomic<bool> release_verify{false},entered_verify{false},fail_verify{false};
std::atomic<bool> entered_creation{false},release_creation{true};
bool first_start{},prepare_confirm{},fail_creation{},fail_bootstrap{},destroyed{},protocol_peer{};
unsigned checks{},verification_calls{},preparation_calls{},dialog_calls{},notices{},wrong_thread{};
int dialog_answer=IDCANCEL;DWORD child_pid{};HANDLE child{};std::filesystem::path fixture_child;
void (*during_dialog)(HWND){};
std::filesystem::path root;
std::filesystem::path base_root;
std::wstring dialog_detail;
std::wstring last_notice;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void on_ui(){if(GetCurrentThreadId()!=ui)++wrong_thread;}
HRESULT WINAPI task_dialog(const TASKDIALOGCONFIG* config,int* selected,int*,BOOL*){on_ui();++dialog_calls;if(config->pszContent)dialog_detail=config->pszContent;if(during_dialog)during_dialog(config->hwndParent);*selected=dialog_answer;return S_OK;}
int WINAPI notice(HWND,LPCWSTR text,LPCWSTR,UINT){on_ui();++notices;last_notice=text?text:L"";return IDOK;}
BOOL WINAPI create_child(LPCWSTR,LPWSTR original,LPSECURITY_ATTRIBUTES process_security,LPSECURITY_ATTRIBUTES thread_security,BOOL inherit,DWORD flags,LPVOID environment,LPCWSTR directory,LPSTARTUPINFOW startup,LPPROCESS_INFORMATION process){
    entered_creation.store(true);while(!release_creation.load())Sleep(5);
    if(fail_creation){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    std::wstring command=L"\""+fixture_child.native()+L"\"";
    if(protocol_peer){const std::wstring arguments=original;const auto owner=arguments.find(L" --owner-handle=");if(owner==arguments.npos)return FALSE;command+=arguments.substr(owner)+L" --fixture-root=\""+root.native()+L"\"";}
    std::vector<wchar_t> writable(command.begin(),command.end());writable.push_back(0);
    const auto result=CreateProcessW(fixture_child.c_str(),writable.data(),process_security,thread_security,inherit,flags,environment,directory,startup,process);
    if(result){child_pid=process->dwProcessId;DuplicateHandle(GetCurrentProcess(),process->hProcess,GetCurrentProcess(),&child,SYNCHRONIZE,FALSE,0);}return result;
}
BOOL WINAPI write(HANDLE handle,LPCVOID bytes,DWORD count,LPDWORD written,LPOVERLAPPED overlapped){if(fail_bootstrap){*written=0;SetLastError(ERROR_BROKEN_PIPE);return FALSE;}return WriteFile(handle,bytes,count,written,overlapped);}
}
namespace sfx::platform::win32::installation {
void beta7_verify(const Path& root,const std::wstring&){fixture::entered_verify.store(true);++fixture::verification_calls;while(!fixture::release_verify.load())Sleep(5);if(fixture::fail_verify.load())throw std::runtime_error("synthetic_integrity_failure");if(!std::filesystem::is_directory(root/L"Work"))throw std::runtime_error("synthetic_installation_mutable_boundary_invalid");}
std::optional<PreparedFirstStart> beta7_first_start(const Path&,const std::wstring&){return fixture::first_start?std::optional<PreparedFirstStart>{{L"fixture",L"first"}}:std::nullopt;}
std::optional<ServiceMode> beta7_service_mode(const Path&){return {};}
}
namespace sfx::platform::win32 {
std::filesystem::path beta7_root(){return fixture::root;}
PortablePreparationResult beta7_prepare(std::size_t,HWND=nullptr){fixture::on_ui();++fixture::preparation_calls;if(fixture::prepare_confirm){fixture::fail_verify.store(false);std::filesystem::create_directories(fixture::root/L"Work");}return {fixture::prepare_confirm,!fixture::prepare_confirm,{}};}
application::StorageStatus beta7_preferences(const std::filesystem::path&,const std::wstring&,const application::ControlPanelUiPreferences&){return application::StorageStatus::success();}
}
#define verify_runtime_installation beta7_verify
#define alpha_installation_root beta7_root
#define read_prepared_first_start beta7_first_start
#define read_service_mode beta7_service_mode
#define offer_portable_preparation beta7_prepare
#define publish_control_panel_ui_preferences beta7_preferences
#define TaskDialogIndirect fixture::task_dialog
#define MessageBoxW fixture::notice
#define CreateProcessW fixture::create_child
#define WriteFile fixture::write
#define wWinMain retained_beta7_production_entry
#include "../../src/apps/control_panel/main.cpp"
#undef wWinMain
#undef WriteFile
#undef CreateProcessW
#undef MessageBoxW
#undef TaskDialogIndirect
#undef publish_control_panel_ui_preferences
#undef offer_portable_preparation
#undef read_service_mode
#undef read_prepared_first_start
#undef verify_runtime_installation
#undef alpha_installation_root
namespace {
struct TestState:AppState {
    ~TestState(){
        fixture::release_verify=true;fixture::release_creation=true;
        if(startup){startup->cancelled.store(true);if(startup->thread.joinable())startup->thread.join();startup->runtime.cancel_owned_start();}
        if(IsWindow(window))DestroyWindow(window);
    }
};
LRESULT CALLBACK test_window(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
    auto* state=reinterpret_cast<AppState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){state=reinterpret_cast<AppState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);state->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));}
    if(state&&message==kStartupMessage){complete_startup(*state);return 0;}
    if(state&&(message==kStartupNodesMessage||message==WM_COMMAND||message==WM_NOTIFY||message==WM_CONTEXTMENU||
               (message==WM_TIMER&&wparam==kStartupActivityTimer)))
        return window_proc(window,message,wparam,lparam);
    if(state&&message==WM_CLOSE){close_runtime(*state,false);return 0;}
    if(message==WM_DESTROY){fixture::destroyed=true;return 0;}
    return DefWindowProcW(window,message,wparam,lparam);
}
void pump_until(const std::function<bool()>& condition){
    const auto until=GetTickCount64()+7000;
    while(!condition()&&GetTickCount64()<until){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(5);}
    fixture::check(condition(),"bounded UI pump reaches requested workflow checkpoint");
}
void initialize(AppState& state,bool with_work=true){
    fixture::root=fixture::base_root;
    if(with_work)std::filesystem::create_directories(fixture::root/L"Work");
    else{fixture::root/=L"missing-work-"+sfx::platform::win32::installation::unique_id();std::filesystem::create_directory(fixture::root);}
    fixture::destroyed=false;fixture::release_verify=false;fixture::entered_verify=false;fixture::entered_creation=false;fixture::release_creation=true;
    fixture::fail_verify=false;fixture::first_start=false;fixture::prepare_confirm=false;fixture::fail_creation=false;fixture::fail_bootstrap=false;fixture::protocol_peer=false;fixture::child_pid=0;
    if(fixture::child){CloseHandle(fixture::child);fixture::child=nullptr;}
    state.startup_pending=true;state.window=CreateWindowExW(0,L"SFXBeta7StartupFixture",L"Isolated startup workflow",WS_OVERLAPPEDWINDOW,0,0,640,400,nullptr,nullptr,GetModuleHandleW(nullptr),&state);
    state.preferences.modem_lights=false;
    state.subheading=CreateWindowExW(0,L"STATIC",L"",WS_CHILD,0,0,600,40,state.window,nullptr,nullptr,nullptr);
    state.status_bar=CreateWindowExW(0,STATUSCLASSNAMEW,L"",WS_CHILD,0,40,600,20,state.window,nullptr,nullptr,nullptr);
    state.setup_button=CreateWindowExW(0,L"BUTTON",L"Setup",WS_CHILD,0,60,100,20,state.window,nullptr,nullptr,nullptr);
    state.disable_all_nodes_button=CreateWindowExW(0,L"BUTTON",L"Disable All Nodes",WS_CHILD|WS_DISABLED,105,60,140,20,state.window,nullptr,nullptr,nullptr);
    state.enable_all_nodes_button=CreateWindowExW(0,L"BUTTON",L"Enable All Nodes",WS_CHILD|WS_DISABLED,250,60,140,20,state.window,nullptr,nullptr,nullptr);
    state.tab=CreateWindowExW(0,WC_TABCONTROLW,L"",WS_CHILD,0,90,600,200,state.window,nullptr,nullptr,nullptr);
    TCITEMW tab{};tab.mask=TCIF_TEXT;tab.pszText=const_cast<wchar_t*>(L"Nodes");TabCtrl_InsertItem(state.tab,0,&tab);
    state.node_list=CreateWindowExW(0,WC_LISTVIEWW,L"",WS_CHILD|LVS_REPORT,0,100,600,180,state.window,nullptr,nullptr,nullptr);
    state.original_node_list_proc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(state.node_list,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(node_list_proc)));
    state.list_header=ListView_GetHeader(state.node_list);
    state.original_header_proc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(state.list_header,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(list_header_proc)));
    for(int n=0;n<6;++n){LVCOLUMNW column{};column.mask=LVCF_WIDTH|LVCF_TEXT;column.cx=90;column.pszText=const_cast<wchar_t*>(L"Column");ListView_InsertColumn(state.node_list,n,&column);}
    const auto menu=CreateMenu();AppendMenuW(menu,MF_STRING,ID_FILE_EXIT,L"Exit");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(CreatePopupMenu()),L"BBS");AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(CreatePopupMenu()),L"Help");SetMenu(state.window,menu);
}
void fixture_configuration(std::uint16_t count){
    const win32::WindowsStorageDaclPolicy security(win32::current_process_user_sid_string());
    std::filesystem::create_directories(fixture::root/L"Config");
    fixture::check(bool(security.apply(fixture::root,win32::StorageDaclObjectKind::directory)),"private fresh fixture root");
    fixture::check(bool(security.apply(fixture::root/L"Config",win32::StorageDaclObjectKind::directory)),"private fresh fixture Config");
    app::OperationalConfiguration configuration;configuration.logical_nodes=count;configuration.bbs_time_zone="UTC";
    configuration.security_policy_revision=1;configuration.security_policy_hash=std::string(64,'a');
    const auto published = win32::publish_operational_configuration_file(fixture::root, win32::current_process_user_sid_string(), configuration);
    fixture::check(published.succeeded(), ("publish protected fixture configuration: " + published.operation).c_str());
}
void offline_rows(const AppState& state,std::uint16_t count){
    fixture::check(ListView_GetItemCount(state.node_list)==count,"configured preview row count exact");
    for(int n=0;n<count;++n){
        LVITEMW item{};item.mask=LVIF_PARAM;item.iItem=n;
        fixture::check(ListView_GetItem(state.node_list,&item)&&item.lParam==n+1,"configured preview identities remain one based");
        wchar_t text[64]{};ListView_GetItemText(state.node_list,n,1,text,64);
        fixture::check(std::wstring(text)==L"OFFLINE","preview status is always Offline");
        for(int column=2;column<6;++column){ListView_GetItemText(state.node_list,n,column,text,64);fixture::check(!text[0],"preview has no stale caller, connection, activity or details");}
    }
    fixture::check(!state.have_status&&state.status.nodes.empty(),"preview does not publish Runtime state");
}
}
int main(int argc,char** argv){try{
    using fixture::check;fixture::ui=GetCurrentThreadId();check(argc==3,"disposable child executable and fresh root required");fixture::fixture_child=std::filesystem::absolute(argv[1]);
    fixture::root=std::filesystem::absolute(argv[2]);fixture::base_root=fixture::root;check(!std::filesystem::exists(fixture::root),"preserve previous startup fixture");std::filesystem::create_directories(fixture::root);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES|ICC_LISTVIEW_CLASSES|ICC_TAB_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSW window{};window.hInstance=GetModuleHandleW(nullptr);window.lpfnWndProc=test_window;window.lpszClassName=L"SFXBeta7StartupFixture";check(RegisterClassW(&window)!=0,"native workflow test window registers");
    {
        TestState state;
        initialize(state);
        state.have_status=true;
        state.status.state=app::AlphaRuntimePresentationState::ready;
        state.status.nodes.resize(3);
        auto& enabled=state.status.nodes[0];enabled.number=1;enabled.available=true;enabled.enabled=true;
        auto& disabled=state.status.nodes[1];disabled.number=2;disabled.available=true;
        auto& unavailable=state.status.nodes[2];unavailable.number=3;unavailable.enabled=true;
        const auto buttons=[&](bool disable,bool enable) {
            update_bulk_node_buttons(state);
            return (IsWindowEnabled(state.disable_all_nodes_button)!=FALSE)==disable &&
                (IsWindowEnabled(state.enable_all_nodes_button)!=FALSE)==enable;
        };
        check(buttons(false,false),"starting disables bulk controls even with stale Ready status");
        state.startup_pending=false;
        check(buttons(true,true),"mixed current availability supports both explicit actions without selection");
        enabled.enabled=false;
        check(buttons(false,true),"unavailable enabled row cannot admit Disable All");
        enabled.enabled=disabled.enabled=true;
        check(buttons(true,false),"already enabled configured rows do not offer Enable All");
        state.status.state=app::AlphaRuntimePresentationState::stopped;
        check(buttons(false,false),"stopped Runtime does not offer bulk changes");
        state.status.state=app::AlphaRuntimePresentationState::ready;
        state.have_status=false;
        check(buttons(false,false),"lost status does not offer bulk changes");
        DestroyWindow(state.window);
    }
    {TestState state;initialize(state);
        fixture_configuration(3);
        check(startup_configured_node_count(fixture::root)==3,"protected configuration exposes only its validated configured count");
        {std::ofstream invalid(fixture::root/L"Config"/L"SFX.conf");invalid<<"invalid";}
        check(!startup_configured_node_count(fixture::root),"invalid preview has no invented default");
        fixture_configuration(3);
        check(!startup_configured_node_count(fixture::root/L"absent"),"missing preview has no invented default");
        startup_controls(state,false);
        for(const auto count:{1,7,128}){SendMessageW(state.window,kStartupNodesMessage,count,0);offline_rows(state,static_cast<std::uint16_t>(count));}
        check(IsWindowEnabled(state.node_list)&&IsWindowEnabled(state.list_header)&&!IsWindowEnabled(state.setup_button),"display-only list/header retain native painting while actions stay disabled");
        const auto notices=fixture::notices;
        ListView_SetItemState(state.node_list,0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        for(const auto input:{WM_LBUTTONDOWN,WM_LBUTTONDBLCLK,WM_RBUTTONDOWN,WM_RBUTTONUP,WM_CONTEXTMENU,WM_KEYDOWN,WM_NCLBUTTONDOWN,WM_VSCROLL,WM_HSCROLL})SendMessageW(state.node_list,input,VK_RETURN,0);
        NMHDR clicked{state.node_list,0,NM_DBLCLK};SendMessageW(state.window,WM_NOTIFY,0,reinterpret_cast<LPARAM>(&clicked));
        PostMessageW(state.window,WM_COMMAND,IDC_OPEN_NODE,0);PostMessageW(state.window,WM_COMMAND,ID_BBS_CONFIGURE,0);
        MSG pending{};while(PeekMessageW(&pending,nullptr,0,0,PM_REMOVE))DispatchMessageW(&pending);
        check(!ListView_GetSelectedCount(state.node_list)&&state.open_node_views.empty()&&!fixture::child_pid&&fixture::notices==notices,"mouse/key/double-click/context/selection/posted commands cannot act while pending");
        synchronize_ownership_menu(state,true);check(GetMenuState(GetSubMenu(GetMenu(state.window),1),ID_BBS_SERVICE_START,MF_BYCOMMAND)!=static_cast<UINT>(-1),"verified Service mode adds its own actions after early window creation");
        synchronize_ownership_menu(state,false);check(GetMenuState(GetSubMenu(GetMenu(state.window),1),ID_BBS_SERVICE_START,MF_BYCOMMAND)==static_cast<UINT>(-1),"Desktop selection retires Service-only menu actions without service operations");
        begin_startup(state,StartupWork::Phase::verification);pump_until([&]{return fixture::entered_verify.load()&&ListView_GetItemCount(state.node_list)==3;});
        offline_rows(state,3);
        wchar_t phase_text[256]{};GetWindowTextW(state.subheading,phase_text,256);
        check(std::wstring(phase_text)==L"SFX: checking program files...","visible activity names the actual verification phase");
        check(!state.startup_dot_count, "program checks leave Runtime dots inactive");
        check(!IsWindowEnabled(state.setup_button)&&!state.have_status,"controls and readiness stay held during delayed verification");
        const auto before=GetTickCount64();SendMessageW(state.window,WM_CLOSE,0,0);check(GetTickCount64()-before<200&&!fixture::destroyed&&state.startup_close,"Close remains responsive while verification worker owns blocked work");
        fixture::release_verify=true;pump_until([]{return fixture::destroyed;});check(!fixture::child_pid,"cancelled verification never creates Runtime");}
    {TestState state;initialize(state);fixture::fail_verify=true;fixture::release_verify=true;const auto before=fixture::preparation_calls;
        begin_startup(state,StartupWork::Phase::verification);pump_until([]{return fixture::destroyed;});check(fixture::preparation_calls==before+1&&!fixture::child_pid,"failed trust offers UI-thread Prepare and cancelled consent leaves Runtime stopped");}
    {TestState state;initialize(state);fixture::fail_verify=true;fixture::prepare_confirm=true;fixture::fail_creation=true;fixture::release_verify=true;const auto before=fixture::verification_calls;
        begin_startup(state,StartupWork::Phase::verification);pump_until([&]{return state.startup&&state.startup->done.load()&&!state.startup->thread.joinable();});
        check(fixture::verification_calls==before+2&&state.startup_pending&&!state.have_status,"confirmed Prepare re-verifies before launch and launch failure never publishes ready");
        offline_rows(state,3);wchar_t failed[256]{};GetWindowTextW(state.subheading,failed,256);
        check(std::wstring(failed).find(L"Startup failed")!=std::wstring::npos,"failed startup retains Offline rows and truthful visible failure");close_runtime(state,false);}
    {TestState state;initialize(state);fixture::first_start=true;fixture::release_verify=true;fixture::dialog_answer=IDCANCEL;const auto before=fixture::dialog_calls;
        fixture::during_dialog = [](HWND window) {
            const auto* state = reinterpret_cast<const AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            fixture::check(state && !state->startup_dot_count, "first-run prompt has no Runtime activity");
        };
        begin_startup(state,StartupWork::Phase::verification);pump_until([]{return fixture::destroyed;});check(fixture::dialog_calls==before+1&&!fixture::child_pid,"first-start Close stays on UI thread and preserves stopped board");
        fixture::during_dialog = {};}
    {TestState state;initialize(state,false);fixture::release_verify=true;const auto before=fixture::preparation_calls;
        begin_startup(state,StartupWork::Phase::verification);pump_until([]{return fixture::destroyed;});
        check(fixture::preparation_calls==before+1&&!fixture::child_pid&&!std::filesystem::exists(fixture::root/L"Work"),"missing Work reaches UI-thread Prepare cancel without early directory creation or Runtime");}
    {TestState state;initialize(state,false);fixture::release_verify=true;fixture::prepare_confirm=true;fixture::protocol_peer=true;const auto before=fixture::preparation_calls;
        begin_startup(state,StartupWork::Phase::verification);pump_until([&]{return !state.startup_pending;});
        check(fixture::preparation_calls==before+1&&state.have_status&&state.runtime.child_alive(),"missing Work Prepare-confirm re-verifies before positive Runtime handoff without a disk font dependency");
        check(!FindResourceW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(204),RT_RCDATA)&&load_embedded_cp437_font(GetModuleHandleW(nullptr),IDR_MOEBIUS_CP437_FONT).has_value(),"obsolete captured font absent; exact Moebius CP437 resource retained");
        close_runtime(state,false);pump_until([]{return fixture::destroyed;});}
    for(bool bootstrap_failure:{false,true}){TestState state;initialize(state);fixture::release_verify=true;fixture::release_creation=bootstrap_failure;fixture::fail_bootstrap=bootstrap_failure;
        SendMessageW(state.window,kStartupNodesMessage,3,0);
        begin_startup(state,StartupWork::Phase::runtime);
        const auto check_dots = [&state](unsigned count) {
            wchar_t caption[256]{};
            GetWindowTextW(state.subheading, caption, _countof(caption));
            check(caption == L"SFX: opening Runtime — checking configuration and protection" +
                std::wstring(count, L'.'), "native status text has the expected trailing dots");
        };
        check_dots(1);
        pump_until([]{return fixture::entered_creation.load();});
        offline_rows(state,3);wchar_t starting[256]{};GetWindowTextW(state.subheading,starting,256);
        check(std::wstring(starting).find(L"SFX: opening Runtime")!=std::wstring::npos,"runtime phase is visible while rows remain Offline");
        if(!bootstrap_failure){
            for (const unsigned count : {2u, 3u, 1u}) {
                pump_until([&]{return state.startup_dot_count == count;});
                check_dots(count);
            }
            SendMessageW(state.window,WM_CLOSE,0,0);
            SendMessageW(state.window,WM_TIMER,kStartupActivityTimer,0);
            wchar_t cancelling[256]{};
            GetWindowTextW(state.subheading,cancelling,_countof(cancelling));
            check(!state.startup_dot_count && std::wstring(cancelling).find(L"Cancelling startup") == 0,
                "cancel stops the dots and a queued timer leaves the cancellation text intact");
            fixture::release_creation=true;pump_until([]{return fixture::destroyed;});
        } else {
            pump_until([&]{return state.startup&&state.startup->done.load()&&!state.startup->thread.joinable();});
            check(!state.startup_dot_count, "immediate startup failure stops the dots");
            close_runtime(state,false);
        }
        check(fixture::child&&WaitForSingleObject(fixture::child,0)==WAIT_OBJECT_0,"exact suspended-cancel/bootstrap-failure child is reaped without Runtime parent job");}
    {TestState state;initialize(state);fixture::release_verify=true;fixture::protocol_peer=true;
        begin_startup(state,StartupWork::Phase::verification);pump_until([&]{return !state.startup_pending;});
        check(state.have_status&&state.status.state==app::AlphaRuntimePresentationState::ready&&state.runtime.child_alive(),"positive real local-control peer publishes complete authenticated handoff on UI thread");
        check(!state.startup_dot_count, "Ready and metrics leave startup dots inactive");
        check(ListView_GetItemCount(state.node_list)==4&&state.status.nodes.front().state==sfx::core::NodeState::ready,"authoritative handoff replaces preview count and row states");
        wchar_t ready[64]{};ListView_GetItemText(state.node_list,0,1,ready,64);check(std::wstring(ready)==L"READY","actual confirmed Ready row replaces Offline");
        check(IsWindowEnabled(state.setup_button),"controls become available only after positive handoff");
        close_runtime(state,false);pump_until([]{return fixture::destroyed;});
        check(fixture::child&&WaitForSingleObject(fixture::child,0)==WAIT_OBJECT_0,"positive handoff then Close reaps exact synthetic peer through orderly shutdown");}
    {TestState state;initialize(state);state.startup_pending=false;state.have_status=true;state.status.lifecycle.active_work.caller_sessions=1;
        state.startup=std::make_unique<StartupWork>();auto& work=*state.startup;work.phase=StartupWork::Phase::metrics;
        work.thread=std::thread([&]{while(!work.cancelled.load())Sleep(5);work.done.store(true);PostMessageW(state.window,kStartupMessage,0,0);});
        fixture::dialog_answer=IDCANCEL;check(!close_runtime(state,true)&&!state.startup_close&&!work.cancelled.load(),"declined caller shutdown leaves optional metrics and Runtime ownership alone");
        check(fixture::dialog_detail.find(L"active caller work")!=std::wstring::npos,"metrics-pending close retains active-caller warning");
        fixture::dialog_answer=100;close_runtime(state,true);pump_until([]{return fixture::destroyed;});}
    {TestState state;initialize(state);state.startup_pending=false;
        state.startup=std::make_unique<StartupWork>();auto& work=*state.startup;work.phase=StartupWork::Phase::metrics;
        work.thread=std::thread([&]{while(!work.cancelled.load())Sleep(5);work.done.store(true);PostMessageW(state.window,kStartupMessage,0,0);});
        fixture::during_dialog=[](HWND window){
            auto& modal_state=*reinterpret_cast<AppState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
            modal_state.startup->cancelled.store(true);
            pump_until([&]{return !modal_state.startup;});
        };
        fixture::dialog_answer=100;
        const auto before=fixture::dialog_calls;
        const bool closed=close_runtime(state,true);
        fixture::during_dialog=nullptr;
        check(closed&&fixture::destroyed&&!state.startup,"confirmation closes safely even when metrics complete in the modal loop");
        check(fixture::dialog_calls==before+1,"metrics completing during the shutdown modal does not require a second confirmation");}
    check(!fixture::wrong_thread,"all Prepare/first-start/error/close dialogs stay on UI thread");
    bool font_left{};for(const auto& item:std::filesystem::recursive_directory_iterator(fixture::base_root))font_left=font_left||(item.is_regular_file()&&item.path().extension()==L".fon");
    check(!font_left,"verification cancellation/failure and orderly close retire only their own private font resources/files");
    if(fixture::child)CloseHandle(fixture::child);
    std::cout<<"PASS "<<fixture::checks<<" real-window/thread/exact-child startup workflow checks; synthetic trust and UI ports, no installed/service/visual acceptance\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<fixture::checks<<": "<<error.what()<<'\n';return 1;}}
