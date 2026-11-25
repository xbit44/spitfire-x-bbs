// S13-EARLY-01: actual unchanged product chooser, not a chooser-return callback.
// This owns every window it drives; no capture helper, global input or guessed
// common-dialog control IDs. UIA selects semantic controls in its own HWND.
#define main retained_event_sounds_ui_main
#include "event_sounds_ui_tests.cpp"
#undef main
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
                    const auto candidate=FindWindowW(L"#32770",L"Choose Event Sound");DWORD actual{};
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
        while(!done && GetTickCount64()<end){pump();Sleep(10);}
        check(done,"native picker driver completed within bounded GUI pump");
        worker.join();check(ok,failure.empty()?"real Windows chooser accepted action":failure.c_str());
    }
};
void real_picker(HWND dialog,EventSoundsDialog& model,std::size_t row,const std::wstring& file,bool cancel,bool probe=false){
    check(!model.browse,"production chooser has no injected return callback");
    const auto button=sound_field(dialog,IDC_SOUND_BROWSE_FIRST+static_cast<int>(row));
    SetForegroundWindow(dialog);SetFocus(button);
    std::cout<<"PICKER before action selected="<<model.selected<<" focus="<<GetDlgCtrlID(GetFocus())<<std::endl;
    PickerDriver driver;driver.start(file,cancel);
    // Actual native Browse button and product command handler.
    SendMessageW(button,BM_CLICK,0,0);driver.finish();
    const auto focus=GetFocus();
    std::cout<<"PICKER after action selected="<<model.selected<<" focus="<<(focus?GetDlgCtrlID(focus):0)<<std::endl;
    check(focus && (focus==dialog || IsChild(dialog,focus)),"chooser returns focus to owned Event Sounds window");
    check(focus==button,"chooser restores keyboard focus to the clicked event's Browse button");
    if(probe && model.selected!=row)std::cout<<"FINDING selected event changed: expected="<<row<<" actual="<<model.selected<<std::endl;
    else check(model.selected==row,"chooser retains intended event selection");
    check(SendMessageW(sound_field(dialog,IDC_SOUND_ROW_FIRST+static_cast<int>(row)),BM_GETCHECK,0,0)==BST_CHECKED,"returned event radio remains checked");
}
constexpr UINT picker_walk_message=WM_APP+0x713;
constexpr const wchar_t* picker_context_property=L"SFX-S13-EARLY-01-TestContext";
struct ModalWalk {EventSoundsDialog& model;std::function<void(HWND)> run;std::exception_ptr failure;};
INT_PTR CALLBACK picker_modal_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
    auto* context=reinterpret_cast<ModalWalk*>(GetPropW(window,picker_context_property));
    if(message==WM_INITDIALOG){
        context=reinterpret_cast<ModalWalk*>(lparam);SetPropW(window,picker_context_property,context);
        const auto result=event_sounds_proc(window,message,wparam,reinterpret_cast<LPARAM>(&context->model));
        PostMessageW(window,picker_walk_message,0,0);return result;
    }
    if(message==picker_walk_message && context){
        try{context->run(window);}catch(...){context->failure=std::current_exception();}
        EndDialog(window,IDCANCEL);return TRUE;
    }
    const auto result=event_sounds_proc(window,message,wparam,lparam);
    if(message==WM_NCDESTROY)RemovePropW(window,picker_context_property);return result;
}
void modal_walk(EventSoundsDialog& model,std::function<void(HWND)> run){
    ModalWalk context{model,std::move(run),{}};
    const auto result=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_EVENT_SOUNDS),nullptr,picker_modal_proc,reinterpret_cast<LPARAM>(&context));
    check(result!=-1,"actual modal DialogBoxParam host");if(context.failure)std::rethrow_exception(context.failure);
}
}

#ifndef SFX_OWNER_SOUND_TEST
int main(int argc,char** argv){
    // A stuck provider/secondary warning cannot survive this owned process.
    std::atomic<bool> finished{};
    std::thread watchdog([&]{for(int i=0;i<400 && !finished;++i)Sleep(100);if(!finished){std::cerr<<"FAIL: bounded native picker watchdog (40 seconds); owned process exiting"<<std::endl;ExitProcess(97);}});
    int code=1;
    try {
        const std::string option=argc==3?argv[2]:"--row=3";
        const bool numbered=option.size()==7 && option.substr(0,6)=="--row=" && option[6]>='0' && option[6]<='9';
        check(argc==2 || (argc==3 && (option=="--row-zero" || option=="--probe-row-three" || numbered)),"fresh owned picker evidence directory required");
        const auto repo=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
        const auto out=std::filesystem::absolute(argv[1]).lexically_normal();const auto relative=out.lexically_relative(repo/L"build");
        check(!relative.empty() && relative.native().find(L"..")==std::wstring::npos && out!=repo/L"build" && !std::filesystem::exists(out),"new bounded picker evidence root");
        std::filesystem::create_directories(out/L"Config");
        const auto sid=win32::current_process_user_sid_string();win32::WindowsStorageDaclPolicy policy(sid);
        check(policy.apply(out/L"Config",win32::StorageDaclObjectKind::directory).succeeded(),"private real configuration folder");
        const auto previous=out/L"previous.wav",chosen=out/L"chosen local tone.wav";write_wave(previous);write_wave(chosen);
        const auto before_previous=win32::read_event_sound(previous.wstring()).bytes,before_chosen=win32::read_event_sound(chosen.wstring()).bytes;
        check(!before_previous.empty()&&!before_chosen.empty(),"existing valid absolute local PCM fixtures including spaces");
        const std::size_t intended=numbered?static_cast<std::size_t>(option[6]-'0'):option=="--row-zero"?0:3;
        const bool probe=option=="--probe-row-three";
        app::OperationalConfiguration baseline;baseline.bbs_time_zone="Pacific Standard Time";baseline.security_policy_revision=1;baseline.security_policy_hash=std::string(64,'a');
        baseline.event_sounds.paths[intended]=*wide_to_utf8(previous.wstring());
        check(win32::publish_operational_configuration_file(out,sid,baseline).succeeded(),"publish isolated baseline through real configuration publisher");
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&controls)!=FALSE,"actual native common controls");
        const auto configure=[&](EventSoundsDialog& model){
            model.load=[&]()->std::optional<app::AlphaConfigurationSnapshot>{const auto disk=win32::read_operational_configuration_file(out);if(!disk.configuration)return {};return app::AlphaConfigurationSnapshot{*disk.configuration,*disk.configuration};};
            model.save=[&](const app::AlphaConfigurationUpdateRequest& r){const auto disk=win32::read_operational_configuration_file(out);app::AlphaConfigurationUpdateResult result;
                check(disk.configuration && r.expected_revision==disk.configuration->configuration_revision && r.configuration.configuration_revision==r.expected_revision+1,"Save uses last real saved revision");
                const auto saved=win32::publish_operational_configuration_file(out,sid,r.configuration);result.code=saved?app::LocalControlResponseCode::ok:app::LocalControlResponseCode::configuration_failure;result.detail=saved?"Applied":"Refused - configuration could not be saved";
                if(saved)++saves;return result;
            };
        };
        EventSoundsDialog model;configure(model);
        modal_walk(model,[&](HWND dialog){
        check(dialog!=nullptr,"exact native product resource and unchanged procedure");
        sound_select(dialog,model,intended);State capture{dialog,g_appearance};
        real_picker(dialog,model,intended,chosen.wstring(),true,probe);
        check(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST+intended))==previous.wstring(),"actual chooser Cancel retains prior row path");
        std::cout<<"PASS actual chooser Cancel retains previous path and returns focus; selection separately checked"<<std::endl;
        real_picker(dialog,model,intended,chosen.wstring(),false,probe);
        if(probe){
            std::cout<<"PROBE intended row="<<intended<<" path="<<wide_to_utf8(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST+static_cast<int>(intended)))).value_or("?")<<std::endl;
            std::cout<<"PROBE first row path="<<wide_to_utf8(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST))).value_or("?")<<std::endl;
            render_png(capture,out/L"actual-open-row-selection-probe.png");
        }
        check(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST+intended))==chosen.wstring(),"actual chooser Open delivers exact absolute WAV path to intended row");
        for(std::size_t row=0;row<app::kSoundEventCount;++row)if(row!=intended)check(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST+static_cast<int>(row))).empty(),"Open leaves other assignments unchanged");
        check(!model.player.owns_buffer(),"real Browse/Open does not preview");render_png(capture,out/L"actual-open-intended-row.png");
        std::cout<<"PASS actual production Browse/Open absolute existing WAV into intended row"<<std::endl;
        // Keyboard returns through the normal dialog manager, not a synthetic
        // command that bypasses child-container Tab navigation.
        const auto edit=sound_field(dialog,IDC_SOUND_PATH_FIRST+intended);SetFocus(edit);
        MSG tab{};tab.hwnd=edit;tab.message=WM_KEYDOWN;tab.wParam=VK_TAB;
        check(IsDialogMessageW(dialog,&tab) && GetFocus()==sound_field(dialog,IDC_SOUND_BROWSE_FIRST+intended),"keyboard Tab from returned row enters its Browse button");
        const auto test=GetDlgItem(dialog,IDC_SOUND_TEST);SetFocus(test);
        SendMessageW(test,WM_KEYDOWN,VK_SPACE,0);SendMessageW(test,WM_KEYUP,VK_SPACE,0);pump();
        check(model.player.owns_buffer(),"Space on native Test button starts actual asynchronous local playback");
        const auto stop=GetDlgItem(dialog,IDC_SOUND_STOP);SetFocus(stop);SendMessageW(stop,WM_KEYDOWN,VK_SPACE,0);SendMessageW(stop,WM_KEYUP,VK_SPACE,0);pump();
        check(!model.player.owns_buffer(),"Space on Stop ends preview and releases owned buffer");
        SendMessageW(GetDlgItem(dialog,IDOK),BM_CLICK,0,0);check(saves==1,"actual native Save button publishes once");
        check(window_text(GetDlgItem(dialog,IDC_SOUND_STATUS)).find(L"assignments saved")!=std::wstring::npos,"Save has truthful success feedback");render_png(capture,out/L"actual-saved.png");
        real_picker(dialog,model,intended,previous.wstring(),true);
        check(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST+intended))==chosen.wstring(),"actual chooser Cancel after Save retains saved new path");
        });
        EventSoundsDialog reopened;configure(reopened);
        modal_walk(reopened,[&](HWND again){
        check(again && window_text(sound_field(again,IDC_SOUND_PATH_FIRST+intended))==chosen.wstring(),"new native dialog reopens exact persisted chooser result");
        State reopened_capture{again,g_appearance};render_png(reopened_capture,out/L"actual-reopened.png");});
        check(win32::read_event_sound(previous.wstring()).bytes==before_previous && win32::read_event_sound(chosen.wstring()).bytes==before_chosen,"both supplied WAVs remain byte-exact");
        check(!model.player.owns_buffer()&&!reopened.player.owns_buffer(),"both dialog lifetimes end with no preview");
        std::cout<<"PASS actual chooser row="<<intended<<' '<<checks<<" chooser/focus/keyboard/Test/Stop/Save/reopen checks. Controlled Runtime callbacks; real picker/player/publisher. Does not waive other-row failures, audible owner or installed IPC acceptance."<<std::endl;code=0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<std::endl;}
    finished=true;watchdog.join();return code;
}
#endif
