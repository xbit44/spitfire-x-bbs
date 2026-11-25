// Actual Panel controls, synthetic External snapshot. No Runtime or IPC.
#define main retained_panel_test_main
#include "ftn_panel_tests.cpp"
#undef main
int main(){try{
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&controls)!=FALSE,"native classes");const auto instance=GetModuleHandleW(nullptr);
    for(const auto& type:std::vector<std::pair<const wchar_t*,WNDPROC>>{{kWindowClass,window_proc},{kTerminalClass,terminal_proc},{kMonitorClass,monitor_proc},{kStatisticsClass,statistics_proc}}){WNDCLASSEXW wc{sizeof(wc)};wc.hInstance=instance;wc.lpfnWndProc=type.second;wc.lpszClassName=type.first;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);check(RegisterClassExW(&wc)!=0,"Panel class");}
    AppState s;s.instance=instance;s.restoring_placement=true;s.preferences.system_monitor_visible=false;
    const auto window=CreateWindowExW(0,kWindowClass,L"SFX External Panel - synthetic review",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,150,100,1100,760,nullptr,nullptr,instance,&s);check(window!=nullptr,"Panel created");
    select_panel_page(s.tab,1);show_selected_page(s,true,false);rebuild_ui_fonts(s,window_dpi(window));RECT area{};GetClientRect(window,&area);layout(s,area.right,area.bottom);
    app::FtnRuntimeResponse view;view.code=app::FtnConfigurationCode::ok;view.mode=app::FtnMailerMode::external_binkd;view.capable=true;view.saved_revision=view.active_revision=5;view.diagnostic="ftn_external_processing_active";s.binkp.apply(view);
    ShowWindow(window,SW_SHOWNOACTIVATE);MSG message{};while(IsWindow(window)&&GetMessageW(&message,nullptr,0,0)>0){if(message.message==WM_TIMER)continue;if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    std::cout<<"Closed synthetic visible Panel review\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
