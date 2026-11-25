// Actual unmodified Message Echo List chooser; test-owned semantic UIA only.
#define SFX_FTN_UI_NO_MAIN
#include "ftn_ui_tests.cpp"
#include <UIAutomation.h>
#include <wrl/client.h>
namespace {
using Microsoft::WRL::ComPtr;
std::wstring automation_text(IUIAutomationElement* element, bool id=false){
    BSTR value{};const auto hr=id?element->get_CurrentAutomationId(&value):element->get_CurrentName(&value);
    const std::wstring text=SUCCEEDED(hr)&&value?value:L"";SysFreeString(value);return text;
}
ComPtr<IUIAutomationElement> semantic_control(IUIAutomation* automation,IUIAutomationElement* root,
    int type,const wchar_t* name){
    VARIANT property{};property.vt=VT_I4;property.lVal=type;ComPtr<IUIAutomationCondition> condition;
    if(FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,property,&condition)))throw std::runtime_error("UIA type condition");
    // The native HWND exists before the shell has published its UIA subtree.
    for(int attempt=0;attempt<250;++attempt){
        ComPtr<IUIAutomationElementArray> elements;
        if(FAILED(root->FindAll(TreeScope_Descendants,condition.Get(),&elements)))throw std::runtime_error("UIA descendant controls");
        int count{};elements->get_Length(&count);ComPtr<IUIAutomationElement> found;
        for(int i=0;i<count;++i){ComPtr<IUIAutomationElement> e;elements->GetElement(i,&e);const auto text=automation_text(e.Get());
            BOOL enabled{},offscreen{};RECT bounds{};e->get_CurrentIsEnabled(&enabled);e->get_CurrentIsOffscreen(&offscreen);e->get_CurrentBoundingRectangle(&bounds);
            if((attempt==249 || text==name) && (type==UIA_EditControlTypeId || text==L"Open" || text==L"Cancel"))
                std::cout<<"UIA control type="<<type<<" name="<<wide_to_utf8(text).value_or("?")<<" id="<<wide_to_utf8(automation_text(e.Get(),true)).value_or("?")<<" enabled="<<enabled<<" offscreen="<<offscreen<<" bounds="<<bounds.left<<','<<bounds.top<<','<<bounds.right<<','<<bounds.bottom<<std::endl;
            if(text==name && enabled && !offscreen && bounds.right>bounds.left && bounds.bottom>bounds.top){
                if(found){BOOL same{};automation->CompareElements(found.Get(),e.Get(),&same);if(!same)throw std::runtime_error("UIA semantic control is ambiguous");}
                else found=e;
            }
        }
        if(found)return found;Sleep(20);
    }
    throw std::runtime_error("UIA semantic control missing after bounded initialization: "+wide_to_utf8(name).value_or("?"));
}
struct PickerDriver {
    std::atomic<bool> done{},ok{};
    std::string failure;
    std::thread worker;
    ~PickerDriver(){if(worker.joinable())worker.join();}
    void start(const std::wstring& path,bool cancel){
        const DWORD pid=GetCurrentProcessId();
        worker=std::thread([this,pid,path,cancel]{
            bool initialized{};HWND picker{};
            try {
                initialized=SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
                if(!initialized)throw std::runtime_error("UIA MTA initialization failed");
                // The production dialog's exact caption plus actual owning PID.
                for(int attempt=0;attempt<250 && !picker;++attempt){
                    const auto candidate=FindWindowW(L"#32770",L"Choose Message Echo List");DWORD actual{};
                    if(candidate)GetWindowThreadProcessId(candidate,&actual);
                    if(actual==pid)picker=candidate;else Sleep(20);
                }
                if(!picker)throw std::runtime_error("owned production chooser not found within five seconds");
                ComPtr<IUIAutomation> automation;
                const auto hr=CoCreateInstance(__uuidof(CUIAutomation),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation));
                if(FAILED(hr))throw std::runtime_error("UIA native client unavailable");
                ComPtr<IUIAutomationElement> root;
                if(FAILED(automation->ElementFromHandle(picker,&root)))throw std::runtime_error("owned chooser UIA root unavailable");
                int actual_pid{};root->get_CurrentProcessId(&actual_pid);
                if(actual_pid!=static_cast<int>(pid))throw std::runtime_error("chooser UIA process mismatch");
                if(!cancel){
                    auto filename=semantic_control(automation.Get(),root.Get(),UIA_EditControlTypeId,L"File name:");
                    ComPtr<IUIAutomationValuePattern> value;
                    if(FAILED(filename->GetCurrentPatternAs(UIA_ValuePatternId,IID_PPV_ARGS(&value))))throw std::runtime_error("actual filename ValuePattern unavailable");
                    const auto replacement=SysAllocStringLen(path.data(),static_cast<UINT>(path.size()));
                    if(!replacement)throw std::runtime_error("UIA absolute-path BSTR allocation failed");
                    const auto changed=value->SetValue(replacement);SysFreeString(replacement);
                    if(FAILED(changed))throw std::runtime_error("actual filename ValuePattern rejected absolute path");
                    BSTR echo{};const auto read=value->get_CurrentValue(&echo);const bool exact=SUCCEEDED(read)&&echo&&path==echo;SysFreeString(echo);
                    if(!exact)throw std::runtime_error("actual filename edit did not retain exact absolute path");
                    std::cout<<"UIA filename verified absolute="<<wide_to_utf8(path).value_or("?")<<std::endl;
                }
                if(cancel){
                    auto action=semantic_control(automation.Get(),root.Get(),UIA_ButtonControlTypeId,L"Cancel");
                    ComPtr<IUIAutomationInvokePattern> invoke;
                    if(FAILED(action->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(&invoke))) || FAILED(invoke->Invoke()))throw std::runtime_error("actual chooser Cancel Invoke failed");
                }else{
                    // IDOK is the documented Win32 default Open command. UIA's
                    // combo arrows also have the name Open, so do not invoke
                    // an ambiguous arrow as if it were the primary action.
                    if(!PostMessageW(picker,WM_COMMAND,IDOK,0))throw std::runtime_error("actual chooser default Open command failed");
                    std::cout<<"WIN32 posted actual chooser IDOK/Open after exact filename readback"<<std::endl;
                }
                ok=true;
            }catch(const std::exception& e){failure=e.what();std::cerr<<"PICKER DRIVER: "<<failure<<std::endl;
                // Test-owned modal only. A process watchdog handles an unresponsive
                // provider instead of leaving a warning on the owner's desktop.
                if(picker)PostMessageW(picker,WM_CLOSE,0,0);
            }
            if(initialized)CoUninitialize();done=true;
        });
    }
    void finish(){
        // Keep the owning GUI thread pumping while UIA's Invoke unwinds.
        const auto end=GetTickCount64()+8000;
        while(!done && GetTickCount64()<end){ftn_pump();Sleep(10);}
        check(done,"native picker driver completed within bounded GUI pump");
        worker.join();check(ok,failure.empty()?"real Windows chooser accepted action":failure.c_str());
    }
};

}

int main(int argc,char** argv){try{
    check(argc==2||argc==3,"fresh picker output and optional loopback download origin required");const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"preserve previous picker evidence");std::filesystem::create_directories(output);const auto file=output/L"message-list.na";{std::ofstream stream(file,std::ios::binary);stream<<"ECHO.ONE First description\r\nECHO.TWO Second description\r\n";check(stream.good(),"owned absolute list input");}
    INITCOMMONCONTROLSEX classes{sizeof(classes),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&classes)!=FALSE,"picker native classes");ftn_model={};ftn_model.message_catalog_revision=ftn_model.file_catalog_revision=1;app::FtnNetwork n;n.code="picker-network";n.name="Picker Network";n.address="42:100/1";ftn_model.networks.push_back(n);ftn_query_fixture=ftn_ui_query;ftn_change_fixture=ftn_ui_change;message_query_fixture=ftn_message_query;file_query_fixture=ftn_file_query;
    State s;s.instance=GetModuleHandleW(nullptr);s.dpi=96;s.root=output/L"synthetic-only-not-installed";s.system_recovery_only=true;s.window=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Owned FTN Picker Qualification",WS_POPUP|WS_CAPTION|WS_CLIPCHILDREN,30,30,1000,780,nullptr,nullptr,s.instance,nullptr);check(s.window&&SetWindowSubclass(s.window,ftn_native_root,121,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s),"actual Config picker owner");requested_fonts(s);show_page(s,29);refresh_ftn(s);s.ftn.network=0;s.ftn.draft=*s.ftn.config;ftn_open_import(s);layout(s,1000,750);s.appearance.apply_controls(s.window);ShowWindow(s.window,SW_SHOWNOACTIVATE);ftn_pump();
    const auto browse=GetDlgItem(s.pages[35],IDC_FTN_IMPORT_BROWSE);door_set(s.ftn.import_path,"https://example.invalid/prior.na");SetFocus(browse);
    {PickerDriver driver;driver.start(file.wstring(),false);SendMessageW(browse,BM_CLICK,0,0);driver.finish();}check(door_text(s.ftn.import_path)==file.u8string()&&SendMessageW(s.ftn.import_source,CB_GETCURSEL,0,0)==1,"real Open assigns exact local source");check(GetFocus()==browse,"real Open returns keyboard to Browse");ftn_import_load(s);check(s.ftn.import_list.entries.size()==2&&ftn_model.message_echoes.empty(),"real selected file loads preview without saving");render_png(s,output/L"real-open-list-preview.png");
    const auto previous=door_text(s.ftn.import_path);{PickerDriver driver;driver.start(L"",true);SendMessageW(browse,BM_CLICK,0,0);driver.finish();}check(door_text(s.ftn.import_path)==previous&&s.ftn.import_list.entries.size()==2,"real Cancel preserves source and selected preview");check(GetFocus()==browse,"real Cancel restores keyboard to Browse");MSG tab{};tab.hwnd=browse;tab.message=WM_KEYDOWN;tab.wParam=VK_TAB;check(IsDialogMessageW(s.window,&tab)&&GetFocus()==GetDlgItem(s.pages[35],IDC_FTN_IMPORT_LOAD),"keyboard continues from Browse to Load List");render_png(s,output/L"real-cancel-retained.png");if(argc==3){
    const std::string origin=argv[2];check(origin.rfind("http://127.0.0.1:",0)==0,"owned loopback HTTP fixture only");
    const auto wait=[&]{const auto deadline=GetTickCount64()+10000;while(s.ftn.import_job&&GetTickCount64()<deadline){ftn_pump();Sleep(5);}check(!s.ftn.import_job,"actual native timer completes downloader worker");};
    SendMessageW(s.ftn.import_source,CB_SETCURSEL,0,0);door_set(s.ftn.import_path,origin+"/slow");SendDlgItemMessageW(s.pages[35],IDC_FTN_IMPORT_LOAD,BM_CLICK,0,0);check(ftn_import_busy(s)&&!IsWindowEnabled(browse),"download worker starts while UI remains responsive");SendDlgItemMessageW(s.pages[35],IDC_FTN_IMPORT_CANCEL,BM_CLICK,0,0);wait();check(s.ftn.import_list.entries.size()==2&&ftn_model.message_echoes.empty()&&IsWindowEnabled(browse),"cancel preserves preview/draft and restores Browse");
    door_set(s.ftn.import_path,origin+"/list.na");SendDlgItemMessageW(s.pages[35],IDC_FTN_IMPORT_LOAD,BM_CLICK,0,0);wait();check(s.ftn.import_list.entries.size()==2&&s.ftn.import_list.entries[0].selected&&ftn_model.message_echoes.empty(),"actual native async download loads checked preview without saving");render_png(s,output/L"real-downloaded-preview.png");
}
DestroyWindow(s.window);std::cout<<"PASS "<<checks<<" real Windows FTN Browse/Open/Cancel/focus checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
