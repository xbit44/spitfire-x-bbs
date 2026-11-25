// Actual native controls and owner administration ports; synthetic data only.
#define main retained_door_early_main
#include "door_command_line_early.cpp"
#undef main
#include "native_png_capture.hpp"
#include <new>
namespace {
struct ChatThemeKey {std::wstring value;~ChatThemeKey(){if(!value.empty())RegDeleteKeyW(HKEY_CURRENT_USER,value.c_str());}};
void chat_pump(){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}}
app::ChatHub chat_model;
app::ChatConfigurationResult chat_snapshot(){return {app::ChatResult::ok,chat_model.configuration()};}
app::ChatConfigurationResult chat_change(const app::ChatConfiguration& value){
    const auto code=chat_model.change_configuration(value,[]{return true;});return {code,chat_model.configuration()};
}
}
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("owned output directory required");const std::filesystem::path output=argv[1];std::filesystem::create_directories(output);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"native classes");
    for(const int dpi:{96,144,192})for(unsigned theme=0;theme<2;++theme){
        check(chat_model.configure({}),"fresh Chat model");seed();State s;s.dpi=dpi;s.instance=GetModuleHandleW(nullptr);s.root=L"C:\\sfxtst\\section12-chat-synthetic-only";
        ChatThemeKey key{L"Software\\Spitfire-X BBS\\Tests\\Section12\\"+output.filename().wstring()+L"-"+std::to_wstring(dpi)+L"-"+std::to_wstring(theme)};
        HKEY handle{};DWORD disposition{};check(RegCreateKeyExW(HKEY_CURRENT_USER,key.value.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&handle,&disposition)==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY,"fresh private theme key");
        const DWORD value=theme;check(RegSetValueExW(handle,L"Theme",0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value))==ERROR_SUCCESS,"private theme");RegCloseKey(handle);
        s.appearance.~DesktopAppearance();new(&s.appearance)win32::DesktopAppearance(key.value);
        const auto tag=std::to_string(dpi)+"-"+(theme?"dark":"classic");
        s.window=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Owned Section12 native controls",WS_POPUP|WS_CLIPCHILDREN,-30000,-30000,MulDiv(1050,dpi,96),MulDiv(850,dpi,96),nullptr,nullptr,s.instance,nullptr);
        check(s.window&&SetWindowSubclass(s.window,synthetic_root,99,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s),"create native Config");requested_fonts(s);
        chat_query_fixture=chat_snapshot;chat_change_fixture=chat_change;door_catalog_fixture=snapshot;door_change_fixture=change;
        show_page(s,27);refresh_chat(s);remember_page(s);layout(s,MulDiv(1050,dpi,96),MulDiv(850,dpi,96));
        s.appearance.apply_controls(s.window);ShowWindow(s.window,SW_SHOWNOACTIVATE);chat_pump();
        check(SendMessageW(s.chat.list,LB_GETCOUNT,0,0)==1,"Lobby named list");chat_edit(s,true);
        check(door_checked(s.chat.enabled)&&door_text(s.chat.level)=="10"&&SendMessageW(s.chat.mode,CB_GETCURSEL,0,0)==0,"new room enabled minimum10");
        SetFocus(s.chat.name);MSG tab{};tab.hwnd=s.chat.name;tab.message=WM_KEYDOWN;tab.wParam=VK_TAB;check(IsDialogMessageW(s.window,&tab)&&GetFocus()==s.chat.enabled,"keyboard Tab moves name to Enabled");
        chat_pump();render_png(s,output/("rooms-new-defaults-"+tag+".png"));
        door_set(s.chat.name,"Games");check(save_chat_room(s),"save named room");check(chat_model.configuration().rooms.size()==2,"new room persists");
        refresh_chat(s);remember_page(s);select_list(s.chat.list,2);chat_edit(s,false);check(door_text(s.chat.name)=="Games"&&door_text(s.chat.level)=="10","reopen saved room");
        app::ChatParticipant occupant;occupant.identity.session.bytes[0]=1;occupant.identity.account=1;occupant.identity.node=1;occupant.name="Synthetic Caller";occupant.level=10;occupant.safe=true;
        check(chat_model.attach(occupant)&&chat_model.join(occupant.identity,2)==app::ChatResult::ok,"controlled room occupied");door_set(s.chat.name,"Busy edit retained");
        check(!save_chat_room(s)&&door_text(s.chat.name)=="Busy edit retained"&&chat_model.configuration().rooms[1].name=="Games","occupied rejects and retains draft");
        chat_pump();render_png(s,output/("rooms-busy-"+tag+".png"));chat_model.detach(occupant.identity);check(save_chat_room(s),"empty retry saved");
        refresh_chat(s);remember_page(s);select_list(s.chat.list,1);chat_edit(s,false);check(!IsWindowEnabled(s.chat.enabled),"entry cannot disable");
        chat_pump();render_png(s,output/("rooms-entry-"+tag+".png"));remember_page(s);s.chat.editing=false;
        show_page(s,28);refresh_chat(s);remember_page(s);chat_command_edit(s,true);check(SendMessageW(s.chat.command_door,CB_GETCOUNT,0,0)==2,"registered Doors by name");
        SendMessageW(s.chat.command_door,CB_SETCURSEL,0,0);door_set(s.chat.command_key,"K");door_set(s.chat.command_name,"Play Space Game");
        check(save_chat_command(s),"save shared Door placement");check(chat_model.configuration().commands.size()==1&&chat_model.configuration().commands[0].door==17,"one registered definition referenced");
        SendMessageW(s.chat.commands,LB_SETCURSEL,0,0);chat_command_edit(s,false);door_set(s.chat.command_key,"C");check(!save_chat_command(s)&&door_text(s.chat.command_key)=="C","built-in conflict retains edit");door_set(s.chat.command_key,"K");check(save_chat_command(s),"save corrected key");
        check(door_text(s.status)=="Door commands saved. Available commands appear when callers next refresh that menu.","Door command save describes normal menu refresh");
        chat_pump();render_png(s,output/("commands-"+tag+".png"));show_page(s,18);refresh_chat(s);remember_page(s);check(door_checked(s.chat.stats),"statistics default on");
        check(chat_configuration_command(s,IDC_CHAT_STATS_SAVE,BN_CLICKED),"statistics Save routed");
        check(door_text(s.status)=="Statistics setting saved. It applies to future logons.","statistics save names its own task");
        chat_pump();render_png(s,output/("statistics-"+tag+".png"));
        show_page(s,8);s.security_catalog.code=app::LocalAccountResultCode::ok;s.security_catalog.levels={{10,"Regular",{},app::SecurityLevelState::active}};
        SendMessageW(s.level_list,LB_RESETCONTENT,0,0);SendMessageW(s.level_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"10 Regular"));SendMessageW(s.level_list,LB_SETCURSEL,0,0);select_level(s);
        check(text(s.level_chat_minutes)==L"0"&&text(s.level_chat_day_minutes)==L"0","private Chat defaults unlimited");
        s.security_catalog.levels[0].private_chat_minutes=3;s.security_catalog.levels[0].private_chat_minutes_per_day=20;select_level(s);
        check(text(s.level_chat_minutes)==L"3"&&text(s.level_chat_day_minutes)==L"20","private Chat limits read selected level");
        SetFocus(s.level_day_minutes);MSG limit_tab{};limit_tab.hwnd=s.level_day_minutes;limit_tab.message=WM_KEYDOWN;limit_tab.wParam=VK_TAB;
        check(IsDialogMessageW(s.window,&limit_tab)&&GetFocus()==s.level_chat_minutes,"keyboard reaches private Chat limits");
        RECT chat_day{},defaults{},messages{};GetWindowRect(s.level_chat_day_minutes,&chat_day);GetWindowRect(s.level_new_user,&defaults);GetWindowRect(s.level_message_lines,&messages);
        check(chat_day.bottom<defaults.top&&defaults.bottom<messages.top,"new controls do not overlap defaults or message limits");
        chat_pump();render_png(s,output/("private-limits-"+tag+".png"));DestroyWindow(s.window);
    }
    std::cout<<"PASS "<<checks<<" native Chat UI checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
