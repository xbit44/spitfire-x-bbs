// Read-only actual shipping Config after the populated normal Setup update.
#define main retained_current_config_main
#include "ftn_installed_ui_tests.cpp"
#undef main
int main(int argc,char** argv){Product panel,config;try{
    check(argc==2,"fresh PNG directory");const auto out=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(out),"preserve prior evidence");std::filesystem::create_directories(out);
    const auto root=win32::installation::canonical_root(L"C:\\Program Files\\SFX-Section14-Test-20261030-r4");
    check(!win32::installation::elevated(),"ordinary owner");win32::installation::verify_runtime_installation(root,win32::installation::operator_sid());
    const auto before=win32::offline_ftn_configuration(root,win32::installation::operator_sid());check(before&&before.value,"populated saved configuration");
    panel=launch(root/L"Bin/SFXControlPanel.exe",L"");panel.window=wait_window(panel.pid,kWindowClass);const auto deadline=GetTickCount64()+20000;while(foreign_text(GetDlgItem(panel.window,IDC_MAIN_SUBHEADING)).find(L"READY")==std::wstring::npos&&GetTickCount64()<deadline)Sleep(20);check(foreign_text(GetDlgItem(panel.window,IDC_MAIN_SUBHEADING)).find(L"READY")!=std::wstring::npos,"updated shipping Runtime ready");
    config=launch(root/L"Bin/SFXConfig.exe",L"--ftn");config.window=wait_window(config.pid,L"SFXConfigWindow");const auto loaded=GetTickCount64()+20000;while(!visible_text_contains(config.window,L"Saved FTN settings loaded.")&&GetTickCount64()<loaded)Sleep(20);check(visible_text_contains(config.window,L"Saved FTN settings loaded.")&&!visible_text_contains(config.window,L"Transport integration is pending"),"current truthful loaded status");render_foreign_png(config.window,out/L"current-config-loaded.png");PostMessageW(config.window,WM_CLOSE,0,0);check(WaitForSingleObject(config.process.get(),20000)==WAIT_OBJECT_0,"normal Config close");config.window=nullptr;close_panel(panel);
    const auto after=win32::offline_ftn_configuration(root,win32::installation::operator_sid());check(after&&after.value&&app::encode_ftn_configuration(*after.value)==app::encode_ftn_configuration(*before.value),"read-only current view preserves configuration");std::cout<<"PASS "<<checks<<" updated actual Config and Runtime normal lifecycle checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';if(config.window)PostMessageW(config.window,WM_CLOSE,0,0);try{close_panel(panel);}catch(...){}return 1;}}
