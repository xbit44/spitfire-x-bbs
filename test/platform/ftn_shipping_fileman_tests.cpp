// Actual installed File Manager read-only walkthrough, online and offline.
#define main retained_installed_ui_main
#include "ftn_installed_ui_tests.cpp"
#undef main
int main(int argc,char** argv){Product panel,manager;try{
    check(argc==2,"fresh PNG output required");
    const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"preserve prior evidence");std::filesystem::create_directories(output);
    const auto root=win32::installation::canonical_root(L"C:\\Program Files\\SFX-Section14-Test-20261030-r4");
    check(!win32::installation::elevated(),"ordinary owner only");win32::installation::verify_runtime_installation(root,win32::installation::operator_sid());
    const auto source=root/L"Files"/L"General"/L"installed-hatch.zip";
    const auto source_hash=win32::installation::file_sha256(source);
    const auto wait=[&](auto predicate,const char* why){const auto deadline=GetTickCount64()+20000;while(!predicate()&&GetTickCount64()<deadline)Sleep(10);check(predicate(),why);};
    for(unsigned pass=0;pass<2;++pass){
        const auto folder=output/(pass?L"offline":L"online");std::filesystem::create_directory(folder);
        if(!pass){panel=launch(root/L"Bin"/L"SFXControlPanel.exe",L"");panel.window=wait_window(panel.pid,kWindowClass);wait([&]{return foreign_text(GetDlgItem(panel.window,IDC_MAIN_SUBHEADING)).find(L"READY")!=std::wstring::npos;},"actual shipping Runtime Ready for FileMan");}
        manager=launch(root/L"Bin"/L"SFXFileManager.exe",L"");manager.window=wait_window(manager.pid,L"SFXFileManagerWindow");
        const auto control=[&](int id){return GetDlgItem(manager.window,id);};
        const auto click=[&](int id){wait([&]{return control(id)&&IsWindowVisible(control(id))&&IsWindowEnabled(control(id));},"visible enabled shipping FileMan control");PostMessageW(control(id),BM_CLICK,0,0);};
        wait([&]{return IsWindowEnabled(control(2001));},"shipping FileMan owner connection complete");
        render_foreign_png(manager.window,folder/L"fileman-home.png");
        click(2001);wait([&]{return foreign_text(control(2013)).find(L"choose a File Group")!=std::wstring::npos;},"named File Group view");
        check(ListView_GetItemCount(control(2005))>0,"current saved File Groups present");click(2007);
        wait([&]{return foreign_text(control(2013)).find(L"choose a File Area")!=std::wstring::npos;},"named File Area view");
        check(ListView_GetItemCount(control(2005))>0,"current saved File Areas present");click(2007);
        wait([&]{return foreign_text(control(2013)).find(L"(1 files)")!=std::wstring::npos&&IsWindowEnabled(control(2011));},"actual imported file visible and inspectable");
        render_foreign_png(manager.window,folder/L"fileman-file.png");click(2011);
        const auto dialog=wait_window(manager.pid,L"#32770");
        check(foreign_text(dialog)==L"File Details","actual shipping details modal");
        const auto text=foreign_text(GetDlgItem(dialog,101));
        check(text.find(L"installed-hatch.zip")!=std::wstring::npos&&text.find(L"150001 bytes")!=std::wstring::npos&&text.find(L"Original source DIZ")!=std::wstring::npos&&text.find(L"Sysop import")!=std::wstring::npos,"real saved source metadata and original DIZ displayed");
        check((GetWindowLongPtrW(GetDlgItem(dialog,101),GWL_STYLE)&ES_READONLY)!=0,"actual detail is read-only");
        render_foreign_png(dialog,folder/L"fileman-details.png");PostMessageW(dialog,WM_COMMAND,IDOK,0);
        wait([&]{return !IsWindow(dialog)&&IsWindowEnabled(control(2011));},"modal close returns to original file selection");
        render_foreign_png(manager.window,folder/L"fileman-return.png");
        PostMessageW(manager.window,WM_CLOSE,0,0);check(WaitForSingleObject(manager.process.get(),30000)==WAIT_OBJECT_0,"FileMan exits normally");manager.window=nullptr;DWORD code{};check(GetExitCodeProcess(manager.process.get(),&code)&&code==0,"FileMan exit0");
        if(!pass)close_panel(panel);
        check(win32::installation::file_sha256(source)==source_hash,"original file bytes preserved after actual UI");
    }
    std::cout<<"PASS "<<checks<<" actual installed shipping FileMan online/offline browse/details/return PNG checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';if(manager.window){const auto dialog=process_window(manager.pid,L"#32770");if(dialog)PostMessageW(dialog,WM_CLOSE,0,0);PostMessageW(manager.window,WM_CLOSE,0,0);if(WaitForSingleObject(manager.process.get(),30000)!=WAIT_OBJECT_0)std::cerr<<"FileMan exit unconfirmed\n";}try{close_panel(panel);}catch(...){}return 1;}}
