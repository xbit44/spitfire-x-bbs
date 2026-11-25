// Actual Config controls, synthetic in-memory catalog. No installed IPC.
#define SFX_FTN_UI_NO_MAIN 1
#include "ftn_ui_tests.cpp"
int main(){try{
    INITCOMMONCONTROLSEX classes{sizeof(classes),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&classes)!=FALSE,"native classes");
    ftn_model={};ftn_model.message_catalog_revision=ftn_model.file_catalog_revision=1;ftn_model.settings.mode=app::FtnMailerMode::external_binkd;ftn_model.settings.external_executable="Bin/binkd.exe";
    ftn_query_fixture=ftn_ui_query;ftn_change_fixture=ftn_ui_change;message_query_fixture=ftn_message_query;file_query_fixture=ftn_file_query;
    State s;s.instance=GetModuleHandleW(nullptr);s.dpi=96;s.system_recovery_only=true;s.configuration.saved.bbs_name="Synthetic External BinkD Review";
    s.window=CreateWindowExW(0,L"STATIC",L"SFX External Config - synthetic review",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,100,70,1060,900,nullptr,nullptr,s.instance,nullptr);
    check(s.window&&SetWindowSubclass(s.window,ftn_native_root,120,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s),"actual Config controls");requested_fonts(s);s.appearance.apply_controls(s.window);
    RECT area{};GetClientRect(s.window,&area);layout(s,area.right,area.bottom);show_page(s,29);refresh_ftn(s);remember_page(s);
    ShowWindow(s.window,SW_SHOWNOACTIVATE);
    MSG message{};while(IsWindow(s.window)&&GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(s.window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    std::cout<<"Closed synthetic visible Config review\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
