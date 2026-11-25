#define wWinMain retained_control_panel_entry
#include "../../src/apps/control_panel/main.cpp"
#undef wWinMain
#include <iostream>
#include <new>
namespace {
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct State {HWND window;win32::DesktopAppearance& appearance;};
void print_children(HWND parent,HDC dc,POINT origin){
    for(auto child=GetWindow(parent,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        if(!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE))continue;
        RECT rect{};GetWindowRect(child,&rect);MapWindowPoints(HWND_DESKTOP,parent,reinterpret_cast<POINT*>(&rect),2);POINT at{origin.x+rect.left,origin.y+rect.top};const auto saved=SaveDC(dc);
        SetViewportOrgEx(dc,at.x,at.y,nullptr);IntersectClipRect(dc,0,0,rect.right-rect.left,rect.bottom-rect.top);SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);print_children(child,dc,at);RestoreDC(dc,saved);
    }
}
}
#include "native_png_capture.hpp"
int main(int argc,char** argv){try{
    check(argc==2,"owned PNG directory required");const std::filesystem::path output=argv[1];check(!std::filesystem::exists(output),"no evidence overwrite");std::filesystem::create_directories(output);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&controls)!=FALSE,"native classes");
    AppState state;state.instance=GetModuleHandleW(nullptr);app::ChatConfiguration c;c.rooms.push_back({2,"Games",true,{}});unsigned sends{};
    NoticeDialog model{state,{}, {},{app::ChatResult::ok,c},[&](const auto& request){++sends;check(!request.target&&request.room==2&&request.text=="Controlled system notice","reviewed destination and body");return app::ChatResult::unavailable;}};
    const auto dialog=CreateDialogParamW(state.instance,MAKEINTRESOURCEW(IDD_CHAT_NOTICE),nullptr,notice_dialog_proc,reinterpret_cast<LPARAM>(&model));check(dialog!=nullptr,"native compose dialog");
    SetWindowPos(dialog,nullptr,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(dialog,SW_SHOWNOACTIVATE);
    MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    State capture{dialog,g_appearance};check(SendDlgItemMessageW(dialog,IDC_NOTICE_SCOPE,CB_GETCURSEL,0,0)==0&&sends==0,"All Callers default sends nothing");render_png(capture,output/"notice-compose.png");
    SendDlgItemMessageW(dialog,IDC_NOTICE_SCOPE,CB_SETCURSEL,2,0);SetDlgItemTextW(dialog,IDC_NOTICE_TEXT,L"Controlled system notice");SendMessageW(dialog,WM_COMMAND,IDOK,0);check(model.reviewed&&sends==0,"Review sends nothing");render_png(capture,output/"notice-review-room.png");
    SendMessageW(dialog,WM_COMMAND,IDOK,0);check(sends==1&&!model.reviewed&&window_text(GetDlgItem(dialog,IDC_NOTICE_TEXT))==L"Controlled system notice","error preserves draft and requires review");render_png(capture,output/"notice-unavailable.png");
    SetDlgItemTextW(dialog,IDC_NOTICE_TEXT,L"Line one\r\nLine two");SendMessageW(dialog,WM_COMMAND,IDOK,0);check(sends==1&&!model.reviewed,"line-control text refused");DestroyWindow(dialog);
    // Actual Control Panel window procedure and controls, without its polling
    // timer or Runtime startup. The displayed node snapshot is synthetic.
    LoadLibraryW(L"Msftedit.dll");
    for(const auto& type:std::vector<std::pair<const wchar_t*,WNDPROC>>{{kWindowClass,window_proc},{kTerminalClass,terminal_proc},{kMonitorClass,monitor_proc},{kStatisticsClass,statistics_proc}}){WNDCLASSEXW wc{sizeof(wc)};wc.hInstance=state.instance;wc.lpfnWndProc=type.second;wc.lpszClassName=type.first;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);check(RegisterClassExW(&wc)!=0,"native panel class");}
    state.restoring_placement=true;state.preferences.system_monitor_visible=false;state.status.state=app::AlphaRuntimePresentationState::ready;state.status.bbs_name="Controlled Chat Review";state.have_status=true;
    app::AlphaNodeStatus node;node.number=1;node.available=node.enabled=true;node.caller_handle="Controlled Alice";node.connection="Telnet";node.activity="Chat Lobby";node.account_id=2;state.status.nodes.push_back(node);
    const auto panel=CreateWindowExW(WS_EX_TOOLWINDOW,kWindowClass,L"Section12 native Panel review",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,-30000,-30000,1100,700,nullptr,nullptr,state.instance,&state);check(panel!=nullptr,"actual native Control Panel");KillTimer(panel,kRefreshTimer);
    SetWindowTextW(state.subheading,L"EARLY REVIEW - synthetic node snapshot; no installed Runtime IPC");refresh_node_rows(state);g_appearance.apply_controls(panel);ShowWindow(panel,SW_SHOWNOACTIVATE);RECT bounds{};GetClientRect(panel,&bounds);layout(state,bounds.right,bounds.bottom);
    while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_TIMER)continue;TranslateMessage(&message);DispatchMessageW(&message);}
    RECT open{},notice{};GetWindowRect(state.open_node_button,&open);GetWindowRect(state.notice_button,&notice);check(notice.left>open.right&&notice.top==open.top,"notice button beside Open Selected");
    check(window_text(state.notice_button)==L"Send System Notice","notice button exact wording");check((GetMenuState(state.node_menu,ID_NODE_ANSWER_PAGE,MF_BYCOMMAND)&MF_GRAYED)!=0&&(GetMenuState(state.node_menu,ID_NODE_CHAT,MF_BYCOMMAND)&MF_GRAYED)!=0,"Page/Chat disabled without valid selected assignment");
    State panel_capture{panel,g_appearance};render_png(panel_capture,output/"control-panel-notice-button.png");DestroyWindow(panel);
    for(unsigned theme=0;theme<2;++theme){
        const auto key=L"Software\\Spitfire-X BBS\\Tests\\Section12Operator-"+output.filename().wstring()+L"-"+std::to_wstring(theme);
        HKEY handle{};DWORD disposition{};check(RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&handle,&disposition)==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY,"new isolated operator theme key");
        const DWORD value=theme;check(RegSetValueExW(handle,L"Theme",0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value))==ERROR_SUCCESS,"isolated operator theme");RegCloseKey(handle);
        g_appearance.~DesktopAppearance();new(&g_appearance)win32::DesktopAppearance(key);
        app::Identifier128 view;view.bytes[0]=41;OperatorChatDialog d{state,view,91};unsigned sent{},closed{};bool reject=true;
        d.exchange=[&](const app::ChatOperatorRequest& request){
            app::ChatOperatorSnapshot result;result.code=app::ChatResult::ok;result.active=true;result.paused=true;result.token=91;
            if(request.action==app::ChatOperatorAction::poll)result.lines={{1,"Controlled caller: private question"},{2,"Sysop: private answer"}};
            if(request.action==app::ChatOperatorAction::send){++sent;check(request.sequence==1&&request.view==view&&request.text=="Retained draft","native send exact identity and retry sequence");if(reject)result.code=app::ChatResult::full;}
            if(request.action==app::ChatOperatorAction::end)++closed;return result;
        };
        const auto chat=CreateDialogParamW(state.instance,MAKEINTRESOURCEW(IDD_OPERATOR_CHAT),nullptr,operator_chat_proc,reinterpret_cast<LPARAM>(&d));check(chat!=nullptr,"actual operator Chat dialog");KillTimer(chat,1);
        SetWindowPos(chat,nullptr,-30000,-30000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(chat,SW_SHOWNOACTIVATE);
        check(d.active&&d.ack==2&&IsWindowEnabled(GetDlgItem(chat,IDOK)),"active private transcript and Send");
        SetDlgItemTextW(chat,IDC_OPERATOR_TEXT,L"Retained draft");SendMessageW(chat,WM_COMMAND,IDOK,0);
        check(sent==1&&window_text(GetDlgItem(chat,IDC_OPERATOR_TEXT))==L"Retained draft"&&d.sent==0,"failed Send keeps exact draft for retry");
        State chat_capture{chat,g_appearance};render_png(chat_capture,output/(theme?"operator-dark.png":"operator-classic.png"));
        reject=false;SendMessageW(chat,WM_COMMAND,IDOK,0);check(sent==2&&d.sent==1&&window_text(GetDlgItem(chat,IDC_OPERATOR_TEXT)).empty()&&d.history.size()==2,"successful retry clears once and poll does not duplicate history");
        SetFocus(GetDlgItem(chat,IDC_OPERATOR_TEXT));MSG tab{};tab.hwnd=GetDlgItem(chat,IDC_OPERATOR_TEXT);tab.message=WM_KEYDOWN;tab.wParam=VK_TAB;
        check(IsDialogMessageW(chat,&tab)&&GetFocus()==GetDlgItem(chat,IDOK),"operator input keyboard Tab reaches Send");
        SendMessageW(chat,WM_CLOSE,0,0);check(closed==1,"window close explicitly ends private session");RegDeleteKeyW(HKEY_CURRENT_USER,key.c_str());
    }
    std::cout<<"PASS "<<checks<<" native notice compose/review checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
