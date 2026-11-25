// Real Config controls/handlers with isolated synthetic persistence ports.
#define SFX_FTN_UI_NO_MAIN 1
#include "ftn_ui_tests.cpp"
#include <new>
LRESULT CALLBACK rc8_door_root(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
    auto& s=*reinterpret_cast<State*>(data);
    if(m==WM_COMMAND&&door_configuration_command(s,LOWORD(w),HIWORD(w)))return 0;
    if(m==WM_NOTIFY)if(const auto r=door_configuration_notify(s,reinterpret_cast<NMHDR*>(l)))return *r;
    if(m==WM_NCDESTROY)RemoveWindowSubclass(h,rc8_door_root,id);
    return DefSubclassProc(h,m,w,l);
}
int main(int argc,char** argv)try{
    check(argc==2,"new rc8 evidence directory required");
    const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"preserve prior evidence");std::filesystem::create_directories(output);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"common controls");
    door_catalog_fixture=snapshot;door_change_fixture=change;ftn_query_fixture=ftn_ui_query;ftn_change_fixture=ftn_ui_change;message_query_fixture=ftn_message_query;file_query_fixture=ftn_file_query;
    std::ofstream inventory(output/"coverage.tsv");inventory<<"dpi\ttheme\tpage\tstate\n";
    for(int dpi:{96,144,192})for(unsigned theme=0;theme<2;++theme){
        State s;s.dpi=dpi;s.instance=GetModuleHandleW(nullptr);s.root=output/L"synthetic-not-installed";s.system_recovery_only=true;s.configuration.saved.bbs_name="RC8 Config fixture";
        const auto key=L"Software\\Spitfire-X BBS\\Tests\\RC8Config\\"+output.filename().wstring()+L"-"+std::to_wstring(dpi)+L"-"+std::to_wstring(theme);
        HKEY h{};DWORD created{};check(RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&h,&created)==ERROR_SUCCESS&&created==REG_CREATED_NEW_KEY,"new owned theme preference");
        check(RegSetValueExW(h,L"Theme",0,REG_DWORD,reinterpret_cast<const BYTE*>(&theme),sizeof(theme))==ERROR_SUCCESS,"theme preference");RegCloseKey(h);
        s.appearance.~DesktopAppearance();new(&s.appearance)win32::DesktopAppearance(key);
        const int width=MulDiv(1050,dpi,96),height=MulDiv(850,dpi,96);
        s.window=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"RC8 Config native review",WS_POPUP|WS_CLIPCHILDREN,-30000,-30000,width,height,nullptr,nullptr,s.instance,nullptr);
        check(s.window&&SetWindowSubclass(s.window,ftn_native_root,120,reinterpret_cast<DWORD_PTR>(&s))&&SetWindowSubclass(s.window,rc8_door_root,121,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s),"production Config controls");
        requested_fonts(s);s.appearance.apply_controls(s.window);ShowWindow(s.window,SW_SHOWNOACTIVATE);
        const auto capture=[&](int page,const char* name,bool bottom=false){show_page(s,page);layout(s,width,height);SendMessageW(s.pages[page],WM_VSCROLL,bottom?SB_BOTTOM:SB_TOP,0);ftn_pump();render_png(s,output/(std::to_string(dpi)+"-"+std::to_string(theme)+"-"+std::to_string(page)+"-"+name+".png"));inventory<<dpi<<'\t'<<theme<<'\t'<<page<<'\t'<<name<<'\n';};
        seed();refresh_door_configuration_page(s);show_page(s,26);auto& d=s.doors;d.door=17;d.edit_placement=false;d.return_view=DoorPage::definitions;load_door_fields(s);door_show_view(s,DoorPage::settings);remember_page(s);
        const auto saved_rlogin=catalog.doors.front().rlogin;
        door_set(d.command_line,"telnet 127.0.0.1");press(s,IDC_DOOR_ADVANCED);
        check(d.advanced&&IsWindowVisible(d.telnet_host)&&door_text(d.telnet_port)=="23","real helper button initializes default Telnet fields");
        door_set(d.telnet_port,"2323");door_set(d.telnet_terminal,"ansi-bbs");door_set(d.telnet_timeout,"12");
        check(d.telnet_helper_dirty&&page_dirty(s),"helper edits are tracked");
        press(s,IDC_DOOR_ADVANCED);press(s,IDC_DOOR_ADVANCED);
        check(door_text(d.telnet_port)=="2323"&&d.telnet_helper_dirty,"closing and reopening helper retains draft");capture(26,"telnet-helper",true);
        check(save_door_configuration_page(s),"Save Changes directly applies helper draft");
        check(catalog.doors.front().kind==app::DoorExecutionKind::telnet&&catalog.doors.front().telnet.port==2323&&catalog.doors.front().telnet.terminal_type=="ansi-bbs"&&same_target(catalog.doors.front().rlogin,saved_rlogin),"Telnet saved independently without altering retained RLogin fields");
        d.door=17;load_door_fields(s);door_show_view(s,DoorPage::settings);remember_page(s);d.advanced=false;press(s,IDC_DOOR_ADVANCED);
        door_set(d.telnet_timeout,"0");check(!save_door_configuration_page(s)&&d.telnet_helper_dirty&&door_text(d.telnet_timeout)=="0","invalid helper preserves correction draft");
        door_set(d.telnet_timeout,"11");reject_at=changes+1;check(!save_door_configuration_page(s)&&page_dirty(s)&&catalog.doors.front().telnet.connection_timeout_seconds==12,"failed publication preserves draft and prior value");
        check(save_door_configuration_page(s)&&catalog.doors.front().telnet.connection_timeout_seconds==11,"corrected helper retry persists");
        d.door=17;load_door_fields(s);door_show_view(s,DoorPage::settings);remember_page(s);d.advanced=false;press(s,IDC_DOOR_ADVANCED);door_set(d.telnet_port,"2424");press(s,IDC_DOOR_TELNET_APPLY);
        check(!d.telnet_helper_dirty&&door_text(d.command_line).find(":2424")!=std::string::npos,"Use Telnet Options event reaches production handler");
        door_set(d.command_line,"telnet localhost:2525");check(save_door_configuration_page(s)&&catalog.doors.front().telnet.port==2525,"manual command edit takes precedence over old helper values");
        check(GetParent(s.caller_afk)==s.pages[41]&&GetParent(s.login_afk)==s.pages[41]&&GetParent(s.run_automatic)==s.pages[14]&&!s.pages[39],"idle moved once; hosting retained; retired MSG editor absent");
        capture(41,"idle");capture(14,"hosting");
        for(unsigned tab=0;tab<4;++tab){show_page(s,26);d.selected_tab=tab;TabCtrl_SetCurSel(d.tabs,tab);door_show_view(s,DoorPage::settings);capture(26,("door-tab"+std::to_string(tab)).c_str());}
        if(dpi==96)for(int page=0;page<static_cast<int>(s.pages.size());++page)if(s.pages[page])capture(page,"route");
        s.clone_source=app::LocalEcho{};s.clone_source->name="Source conference";s.clone_source->permissions={50,60,70};
        auto target=*s.clone_source;target.name="Target conference";target.id={2};s.clone_targets={target};
        SetWindowTextW(s.clone_source_text,L"Source: Source conference (#1)");Button_SetCheck(s.clone_access,BST_CHECKED);Button_SetCheck(s.clone_posting,BST_INDETERMINATE);clone_preview(s);capture(20,"clone-selected");
        show_page(s,17);populate_controls(s);refresh_menu_preview(s);capture(17,"menu-preview",true);
        s.system_recovery_only=false;open_clam_log(s);s.system_recovery_only=true;const auto log=FindWindowW(L"SFXClamAVReadOnlyUpdateLog",nullptr);DWORD log_pid{};if(log)GetWindowThreadProcessId(log,&log_pid);
        check(log&&log_pid==GetCurrentProcessId()&&GetWindow(log,GW_OWNER)==s.window,"owned actual auxiliary log");ftn_pump();render_native_png(log,reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1),output/(std::to_string(dpi)+"-"+std::to_string(theme)+"-aux-log.png"));DestroyWindow(log);
        DestroyWindow(s.window);DeleteObject(s.ui_font);DeleteObject(s.heading_font);RegDeleteKeyW(HKEY_CURRENT_USER,key.c_str());
    }
    std::cout<<"PASS "<<checks<<" native rc8 Config checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
