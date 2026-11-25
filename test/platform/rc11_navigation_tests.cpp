#define main retained_rc11_navigation_main
#include "ftn_installed_ui_tests.cpp"
#undef main
#include <fstream>

HWND rc11_page(HWND window){HWND result{};EnumChildWindows(window,[](HWND h,LPARAM p)->BOOL{wchar_t name[80]{};GetClassNameW(h,name,80);if(IsWindowVisible(h)&&std::wstring(name)==L"SFXConfigScrollablePage"){*reinterpret_cast<HWND*>(p)=h;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&result));return result;}
bool rc11_ready(HWND window){const auto page=rc11_page(window);return page&&IsWindowEnabled(page)&&!visible_text_contains(window,L"Loading settings...");}
void rc11_wait(HWND window){const auto end=GetTickCount64()+60000;while(GetTickCount64()<end){DWORD_PTR reply{};if(SendMessageTimeoutW(window,WM_NULL,0,0,SMTO_ABORTIFHUNG,100,&reply)&&rc11_ready(window))return;Sleep(5);}const auto status=foreign_text(GetDlgItem(window,1002));std::wcerr<<L"NAVIGATION STATUS: "<<status<<L'\n';check(false,"navigation becomes usable within bounded wait");}
#ifndef SFX_NETWORK_NAVIGATION_ENTRY
#define SFX_NETWORK_NAVIGATION_ENTRY main
#endif
int SFX_NETWORK_NAVIGATION_ENTRY(int argc,char** argv){Product config,panel;try{
    check(argc>=2&&argc<=4,"fresh navigation output plus optional --online/--baseline");bool online=false,baseline=false;for(int n=2;n<argc;++n){const std::string a=argv[n];if(a=="--online")online=true;else if(a=="--baseline")baseline=true;else check(false,"known navigation option");}
    const auto out=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(out),"preserve prior navigation evidence");std::filesystem::create_directories(out);
    const auto root=win32::installation::canonical_root(
#ifdef SFX_NETWORK_SETUP_QUALIFICATION
network_setup_qualification_root()
#else
L"C:/Program Files/SFX-OwnerFixes-Test-20261031-r3"
#endif
);const auto owner=win32::current_process_user_sid_string();check(!win32::installation::elevated(),"ordinary synthetic installed navigation only");win32::installation::verify_runtime_installation(root,owner);
    const auto original_chat=win32::offline_chat_configuration(root,owner);check(original_chat.code==app::ChatResult::ok&&original_chat.configuration.has_value(),"saved Chat fixture readable before processes");
    if(online){panel=launch(root/L"Bin/SFXControlPanel.exe",L"");panel.window=wait_window(panel.pid,kWindowClass);const auto end=GetTickCount64()+30000;while(!visible_text_contains(panel.window,L"READY")&&GetTickCount64()<end)Sleep(20);check(visible_text_contains(panel.window,L"READY"),"synthetic Runtime ready");}
    config=launch(root/L"Bin/SFXConfig.exe",L"");config.window=wait_window(config.pid,L"SFXConfigWindow");rc11_wait(config.window);const auto navigation=visible_control(config.window,1001);check(navigation!=nullptr,"shipping navigation tree");
    const auto receipt=win32::installation::read_receipt(root,owner);win32::installation::InstallationVerificationLease verified(receipt);
    for(unsigned n=0;n<5;++n)verified.verify_process(receipt,config.pid,win32::installation::configuration_program_index);
    auto changed=receipt;++changed.generation;bool refused=false;try{verified.verify_process(changed,config.pid,win32::installation::configuration_program_index);}catch(...){refused=true;}check(refused,"verification lease refuses another installation generation");
    refused=false;try{verified.verify_process(receipt,config.pid,win32::installation::control_panel_program_index);}catch(...){refused=true;}check(refused,"verification lease rechecks the live process role");
    struct Route{unsigned category,child;const char* name;};
    const std::vector<Route> routes{{2,0,"Files"},{1,0,"Messages"},{7,0,"Accounts"},{4,0,"Chat"},{5,0,"FTN"},{0,0,"Board"},{2,0,"Files-repeat"},{1,0,"Messages-repeat"}};
    const auto node=[&](const Route& route){auto parent=TreeView_GetRoot(navigation);for(unsigned n=0;n<route.category;++n)parent=TreeView_GetNextSibling(navigation,parent);check(parent!=nullptr,"known category exists");auto child=TreeView_GetChild(navigation,parent);for(unsigned n=0;n<route.child;++n)child=TreeView_GetNextSibling(navigation,child);check(child!=nullptr,"known section exists");return child;};
    std::ofstream timings(out/L"navigation.tsv");timings<<"section\tselection_ms\tusable_ms\n";
    for(const auto& route:routes){const auto begin=GetTickCount64();DWORD_PTR result{};check(SendMessageTimeoutW(navigation,TVM_SELECTITEM,TVGN_CARET,reinterpret_cast<LPARAM>(node(route)),SMTO_ABORTIFHUNG,60000,&result)!=0&&result,"shipping section selection");const auto selected=GetTickCount64()-begin;rc11_wait(config.window);const auto ready=GetTickCount64()-begin;timings<<route.name<<'\t'<<selected<<'\t'<<ready<<'\n';timings.flush();if(!baseline)check(selected<500,"section selection returns promptly while worker loads data");check(!visible_text_contains(config.window,L"Settings unavailable")&&!visible_text_contains(config.window,L"could not be read"),"section loaded real available data");}
    if(!baseline){
        TreeView_SelectItem(navigation,node(routes[3]));rc11_wait(config.window);const auto rooms=visible_control(config.window,9000);check(rooms&&SendMessageW(rooms,LB_GETCOUNT,0,0)>0,"Chat navigation loads actual rooms through its authorized relay");SendMessageW(rooms,LB_SETCURSEL,0,0);
        SendMessageW(visible_control(config.window,9002),BM_CLICK,0,0);SendMessageW(visible_control(config.window,9010),BM_CLICK,0,0);check(visible_text_contains(config.window,L"Chat Rooms saved"),"same-value Chat Save uses existing authoritative change service");
        for(const auto& route:{routes[0],routes[3],routes[4]}){DWORD_PTR result{};check(SendMessageTimeoutW(navigation,TVM_SELECTITEM,TVGN_CARET,reinterpret_cast<LPARAM>(node(route)),SMTO_ABORTIFHUNG,500,&result)!=0,"rapid navigation remains responsive");}rc11_wait(config.window);check(visible_text_contains(config.window,L"FTN Networking > Networks")&&visible_control(config.window,11000),"obsolete page completions cannot replace final FTN selection");}
    render_foreign_png(config.window,out/L"navigation-complete.png");PostMessageW(config.window,WM_CLOSE,0,0);check(WaitForSingleObject(config.process.get(),20000)==WAIT_OBJECT_0,"Config closes and joins read worker");config.window=nullptr;close_panel(panel);
    refused=false;try{verified.verify_process(receipt,config.pid,win32::installation::configuration_program_index);}catch(...){refused=true;}check(refused,"verification lease refuses a dead process rather than caching its PID");
    // No batch ownership may survive Config shutdown.
    {win32::OfflineConfigurationReadSession batch(root,owner);app::LocalControlFrame query;query.type=app::LocalControlMessageType::chat_configuration_query_request;query.request_id=1;const auto before=batch.query(query);check(before.has_value(),"offline read batch serves an ordinary authorized query");
        app::LocalControlFrame write;write.type=app::LocalControlMessageType::chat_configuration_change_request;write.request_id=2;write.payload=app::encode_chat_configuration(*original_chat.configuration);check(!batch.query(write),"read batch refuses administrative write dispatch");
        const auto after=batch.query(query);check(after&&after->payload==before->payload,"refused write has no Chat revision or business effect");win32::OfflineInstallationGuard other;check(!other.acquire(root,owner),"read batch retains exclusive stopped-installation ownership until release");}
    const auto final=win32::offline_file_configuration_catalog(root,owner);check(final&&final.value,"offline ownership available after navigation worker exits");
    const auto after_chat=win32::offline_chat_configuration(root,owner);check(after_chat.code==app::ChatResult::ok&&after_chat.configuration.has_value(),"Chat remains readable after actual save and process shutdown");auto chat_values=*after_chat.configuration;chat_values.revision=original_chat.configuration->revision;check(app::encode_chat_configuration(chat_values)==app::encode_chat_configuration(*original_chat.configuration),"navigation/save retains all existing Chat business settings");
    std::cout<<"PASS RC11 shipping navigation "<<checks<<" checks; "<<(online?"online":"offline")<<(baseline?" baseline":" corrected")<<"; timings in navigation.tsv\n";return 0;
}catch(const std::exception& e){if(config.window){PostMessageW(config.window,WM_CLOSE,0,0);WaitForSingleObject(config.process.get(),20000);}try{close_panel(panel);}catch(...){}std::cerr<<"FAIL RC11 navigation "<<checks<<": "<<e.what()<<'\n';return 1;}}
