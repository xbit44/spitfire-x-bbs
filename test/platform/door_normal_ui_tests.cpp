#define main retained_command_review_main
#define render retained_command_review_render
#include "door_command_line_early.cpp"
#undef render
#undef main
#include "door_native_render.hpp"
#include <new>
namespace {
void pump_door_review(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}GdiFlush();}
void select_door_tab(State& s,unsigned tab){
    TabCtrl_SetCurSel(s.doors.tabs,tab);NMHDR notification{s.doors.tabs,IDC_DOOR_TABS,TCN_SELCHANGE};
    check(door_configuration_notify(s,&notification).has_value(),"actual native tab notification handled");pump_door_review();
}
struct ReviewKey {std::wstring path;~ReviewKey(){RegDeleteKeyW(HKEY_CURRENT_USER,path.c_str());}};
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh early UI evidence path");const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"preserve prior UI evidence");std::filesystem::create_directories(output);unsigned renders{};
    for(unsigned theme=0;theme<2;++theme)for(const int dpi:{96,144,192}){
        State s;s.dpi=dpi;s.instance=GetModuleHandleW(nullptr);s.root=LR"(C:\sfxtst\normal-door-ui-not-installed)";
        const auto tag=std::to_string(theme)+"-"+std::to_string(dpi);ReviewKey key{L"Software\\Spitfire-X BBS\\Tests\\NormalDoorReview\\"+output.parent_path().filename().wstring()+L"-"+std::to_wstring(theme)+L"-"+std::to_wstring(dpi)};
        HKEY h{};DWORD disposition{};check(RegCreateKeyExW(HKEY_CURRENT_USER,key.path.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&h,&disposition)==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY,"fresh private theme preference");const DWORD value=theme;check(RegSetValueExW(h,L"Theme",0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value))==ERROR_SUCCESS,"private theme selection");RegCloseKey(h);s.appearance.~DesktopAppearance();new(&s.appearance)win32::DesktopAppearance(key.path);
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"native control classes");const int width=MulDiv(1080,dpi,96),height=MulDiv(860,dpi,96);
        s.window=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"SFX normal Door early review",WS_POPUP|WS_CLIPCHILDREN,-30000,-30000,width,height,nullptr,nullptr,s.instance,nullptr);check(s.window&&SetWindowSubclass(s.window,synthetic_root,91,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s),"real SFXConfig native editor");requested_fonts(s);door_catalog_fixture=snapshot;door_change_fixture=change;seed();
        show_page(s,26);refresh_door_configuration_page(s);remember_page(s);s.appearance.apply_controls(s.window);ShowWindow(s.window,SW_SHOWNOACTIVATE);layout(s,width,height);pump_door_review();
        auto& d=s.doors;d.menu=2;d.navigation_placement.reset();door_fill_lists(s);door_show_view(s,DoorPage::entries);remember_page(s);press(s,IDC_DOOR_NEW);const auto id=d.door;const auto number=d.entry;
        check(d.view==DoorPage::settings&&TabCtrl_GetItemCount(d.tabs)==4,"ordinary Add Door opens four actual tabs");check(door_checked(d.enabled)&&door_checked(d.multiple)&&door_text(d.level)=="10"&&door_text(d.execution_level)=="10","new slot defaults preserved");
        door_set(d.name,"Outpost Trader");door_set(d.code,"OUTPOST");door_set(d.startup,"D:\\Games\\Outpost");door_set(d.command_line,"otrader /D%N");SendMessageW(d.drop_file,CB_SETCURSEL,3,0);
        const auto capture=[&](const char* name){layout(s,width,height);SendMessageW(s.pages[26],WM_VSCROLL,SB_TOP,0);pump_door_review();render(s,output/(tag+"-"+name+".bmp"));++renders;};
        capture("01-program");check(!IsWindowVisible(d.security_mode)&&!IsWindowVisible(d.cleanup)&&!IsWindowVisible(d.entry_number),"other tab fields hidden");
        select_door_tab(s,1);check(IsWindowVisible(d.security_mode)&&!IsWindowVisible(d.command_line),"Access tab shows only its controls");door_check(d.multiple,false);capture("02-access");
        select_door_tab(s,2);door_set(d.cleanup,"cleanup.exe");SendMessageW(d.automatic_event,CB_SETCURSEL,0,0);capture("03-automation");
        select_door_tab(s,3);door_set(d.entry_title,"Play Outpost Trader");door_set(d.entry_description,"A local DOS Door");capture("04-menu-entry");check(page_dirty(s),"tab switches retain shared unsaved draft");
        check(save_door_configuration_page(s),"all four tabs save through versioned protected operations");const auto* saved=app::find_door(catalog,id);check(saved&&saved->kind==app::DoorExecutionKind::local&&saved->program.command_line=="otrader /D%N"&&saved->code=="OUTPOST"&&!saved->allow_multiple&&saved->program.cleanup_command=="cleanup.exe","hidden tab values saved exactly");
        open_entry(s,number);check(door_text(d.command_line)=="otrader /D%N"&&door_text(d.cleanup)=="cleanup.exe"&&!door_checked(d.multiple),"reopening preserves complete local Door");
        door_set(d.code,app::effective_door_code(catalog.doors.front()));const auto writes=changes;check(!save_door_configuration_page(s)&&changes==writes&&GetWindowTextLengthW(d.error)>0,"duplicate code leaves draft with field error, no write");capture("05-code-error");door_set(d.code,"OUTPOST");
        door_set(d.command_line,"rlogin example.invalid -p -q -P");door_configuration_command(s,IDC_DOOR_COMMAND_LINE,EN_KILLFOCUS);door_show_view(s,DoorPage::settings);check(IsWindowVisible(d.advanced_button)&&!IsWindowEnabled(d.program_type),"same command field detects built-in connection");
        check(save_door_configuration_page(s)&&app::find_door(catalog,id)->rlogin.prompt_credential,"retained automatic caller password option saved");
        open_entry(s,number);capture("06-rlogin");
        DestroyWindow(s.window);DeleteObject(s.heading_font);DeleteObject(s.ui_font);
    }
    std::cout<<"PASS "<<checks<<" early native UI checks; "<<renders<<" actual native renders\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<" after "<<checks<<" checks\n";return 1;}}
