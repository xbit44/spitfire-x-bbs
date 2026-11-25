// Actual shipping Config positive-count retention Cancel/Save/reopen.
#define main retained_installed_ui_main
#include "ftn_installed_ui_tests.cpp"
#undef main
int main(int argc,char** argv){Product panel,config;try{
    check(argc==2,"fresh retention PNG path required");const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"preserve prior evidence");std::filesystem::create_directories(output);
    const auto root=win32::installation::canonical_root(L"C:\\Program Files\\SFX-Section14-Test-20261030-r4");const auto owner=win32::installation::operator_sid();check(!win32::installation::elevated(),"ordinary installed owner");win32::installation::verify_runtime_installation(root,owner);
    const auto before=win32::offline_ftn_configuration(root,owner);check(before&&before.value,"current offline configuration");
    const auto find_network=[](const app::FtnConfiguration& c){return std::find_if(c.networks.begin(),c.networks.end(),[](const auto& n){return n.code=="TEST";});};
    const auto network=find_network(*before.value);check(network!=before.value->networks.end()&&network->keep_processed&&network->retention_days==0,"existing Forever fixture");
    const auto wait=[&](auto predicate,const char* why){const auto deadline=GetTickCount64()+20000;while(!predicate()&&GetTickCount64()<deadline)Sleep(10);check(predicate(),why);};
    panel=launch(root/L"Bin"/L"SFXControlPanel.exe",L"");panel.window=wait_window(panel.pid,kWindowClass);wait([&]{return foreign_text(GetDlgItem(panel.window,IDC_MAIN_SUBHEADING)).find(L"READY")!=std::wstring::npos;},"shipping Runtime ready");
    config=launch(root/L"Bin"/L"SFXConfig.exe",L"--ftn");config.window=wait_window(config.pid,L"SFXConfigWindow");
    const auto control=[&](int id){HWND h{};wait([&]{h=visible_control(config.window,id);return h!=nullptr;},"actual visible FTN control");return h;};
    const auto click=[&](int id){const auto h=control(id);check(IsWindowEnabled(h),"enabled FTN action");PostMessageW(h,BM_CLICK,0,0);};
    const auto list=control(IDC_FTN_LIST);const auto index=SendMessageW(list,LB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(L"Installed Hatch Fixture"));check(index!=LB_ERR,"named existing network found");SendMessageW(list,LB_SETCURSEL,index,0);click(IDC_FTN_EDIT);
    check(foreign_text(control(IDC_FTN_DAYS))==L"0"&&SendMessageW(control(IDC_FTN_KEEP),BM_GETCHECK,0,0)==BST_CHECKED,"actual native Forever values");check(set_foreign_text(control(IDC_FTN_DAYS),L"1"),"one-day draft entered");
    for(unsigned pass=0;pass<2;++pass){
        click(IDC_FTN_SAVE);const auto dialog=wait_window(config.pid,L"#32770");check(foreign_text(dialog)==L"Review Processed Original Retention","actual positive-count review dialog");
        const auto body=foreign_text(GetDlgItem(dialog,103));check(body.find(L"shorten protection for 2 existing")!=std::wstring::npos&&body.find(L"Installed Hatch Fixture (TEST)")!=std::wstring::npos&&body.find(L"Due now: 0")!=std::wstring::npos&&body.find(L"Expire later: 2")!=std::wstring::npos&&body.find(L"Previously kept Forever: 2")!=std::wstring::npos,"actual retained originals produce exact named positive counts");
        DWORD thread=GetWindowThreadProcessId(dialog,nullptr);GUITHREADINFO focus{sizeof(focus)};check(GetGUIThreadInfo(thread,&focus)&&focus.hwndFocus==GetDlgItem(dialog,IDCANCEL),"review defaults to Cancel");render_foreign_png(dialog,output/(pass?L"retention-save-review.png":L"retention-cancel-review.png"));
        PostMessageW(dialog,WM_COMMAND,pass?IDOK:IDCANCEL,0);wait([&]{return !IsWindow(dialog);},"review dialog closes normally");
        wait([&]{return visible_text_contains(config.window,pass?L"FTN settings saved.":L"Retention review cancelled.");},"truthful actual Save or Cancel feedback");
        if(!pass)check(foreign_text(control(IDC_FTN_DAYS))==L"1","Cancel preserves unsaved draft for review");
    }
    render_foreign_png(config.window,output/L"retention-saved.png");PostMessageW(config.window,WM_CLOSE,0,0);check(WaitForSingleObject(config.process.get(),20000)==WAIT_OBJECT_0,"Config normal close");config.window=nullptr;close_panel(panel);
    const auto saved=win32::offline_ftn_configuration(root,owner);check(saved&&saved.value&&saved.value->revision==before.value->revision+1,"Cancel made no configuration commit; explicit Save made exactly one");const auto applied=find_network(*saved.value);check(applied!=saved.value->networks.end()&&applied->keep_processed&&applied->retention_days==1,"saved one-day retention persisted");
    panel=launch(root/L"Bin"/L"SFXControlPanel.exe",L"");panel.window=wait_window(panel.pid,kWindowClass);wait([&]{return foreign_text(GetDlgItem(panel.window,IDC_MAIN_SUBHEADING)).find(L"READY")!=std::wstring::npos;},"Runtime restart with preserved retained originals");close_panel(panel);
    const auto restarted=win32::offline_ftn_configuration(root,owner);check(restarted&&restarted.value&&app::encode_ftn_configuration(*restarted.value)==app::encode_ftn_configuration(*saved.value),"full saved configuration unchanged through actual restart");
    std::cout<<"PASS "<<checks<<" actual installed positive-count retention UI Cancel/Save/restart checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';if(config.window){const auto dialog=process_window(config.pid,L"#32770");if(dialog)PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);PostMessageW(config.window,WM_CLOSE,0,0);if(WaitForSingleObject(config.process.get(),20000)!=WAIT_OBJECT_0)std::cerr<<"Config exit unconfirmed; preserve unsaved draft\n";}try{close_panel(panel);}catch(...){}return 1;}}
