// Render real Config controls with synthetic display values, no admin actions.
#define SFX_ALPHA_TEST_AUTOMATION 1
#define wWinMain unused_config_entry
#include "../../src/apps/config_ui/main.cpp"
#undef wWinMain
#include <iostream>

namespace {
int dialog_choice=103;
bool dialog_labels=false;
int dirty_dialogs=0,refusal_dialogs=0;
int fixture_admin_calls=0;
app::LocalClassicInformationSnapshot classic_fixture;
app::ClassicInformationChange last_classic_change;
bool classic_conflict=false;
app::MessageConfigurationSnapshot message_fixture;
app::MessageConfigurationChange last_message_change;
unsigned message_admin_calls{};
bool message_uncertain{};
bool delete_yes{};
bool catalog_yes=true;
bool presentation_yes=false;
unsigned message_next_queries{};
HWND message_fixture_status{};
HWND folder_picker_owner{};
bool folder_picker_accept=false;
unsigned folder_picker_activations{};
std::wstring folder_picker_selection;
LRESULT CALLBACK folder_picker_hook(int code,WPARAM window,LPARAM data) {
    if(code==HCBT_ACTIVATE) {
        const auto target=reinterpret_cast<HWND>(window);
        std::array<wchar_t,64> name{};GetClassNameW(target,name.data(),static_cast<int>(name.size()));
        if(GetWindow(target,GW_OWNER)==folder_picker_owner && std::wstring(name.data())==L"#32770") {
            ++folder_picker_activations;
            if(folder_picker_accept) SendMessageW(target,BFFM_SETSELECTIONW,TRUE,reinterpret_cast<LPARAM>(folder_picker_selection.c_str()));
            PostMessageW(target,WM_COMMAND,folder_picker_accept?IDOK:IDCANCEL,0);
        }
    }
    return CallNextHookEx(nullptr,code,window,data);
}
LRESULT CALLBACK dialog_hook(int code,WPARAM window,LPARAM data) {
    if(code==HCBT_ACTIVATE) {
        const auto target=reinterpret_cast<HWND>(window);
        std::cout<<"dialog activation: "<<wide_to_utf8(text(target)).value_or("unavailable")<<std::endl;
        if(text(target)==kWindowTitle) {
            ++dirty_dialogs;
            dialog_labels=true;
            PostMessageW(target,TDM_CLICK_BUTTON,dialog_choice,0);
        }
        if(text(target)==L"User change not saved") {++refusal_dialogs;PostMessageW(GetDlgItem(target,IDOK),BM_CLICK,0,0);}
        if(text(target)==L"Save User" || text(target)==L"Confirm User Action") PostMessageW(GetDlgItem(target,IDYES),BM_CLICK,0,0);
        if(text(target)==L"Review Message Catalog")PostMessageW(GetDlgItem(target,catalog_yes?IDYES:IDNO),BM_CLICK,0,0);
        if(text(target)==L"Review Clone Options" || text(target)==L"Review Message Limit" || text(target)==L"Review System Sysop")PostMessageW(GetDlgItem(target,IDYES),BM_CLICK,0,0);
        if(text(target)==L"Review Private Message Presentation")PostMessageW(GetDlgItem(target,presentation_yes?IDYES:IDNO),BM_CLICK,0,0);
        if(text(target)==L"Permanently Delete Echo" || text(target)==L"Permanently Delete Group")PostMessageW(GetDlgItem(target,delete_yes?IDYES:IDNO),BM_CLICK,0,0);
    }
    return CallNextHookEx(nullptr,code,window,data);
}
bool capture(HWND window, const std::filesystem::path& output) {
    RECT rectangle{}; GetWindowRect(window,&rectangle);
    const int width=rectangle.right-rectangle.left, height=rectangle.bottom-rectangle.top;
    HDC screen=GetDC(nullptr), dc=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);
    const auto old=SelectObject(dc,bitmap);
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    const bool printed=PrintWindow(window,dc,2)!=FALSE;
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width)*height*4);
    SelectObject(dc,old);
    const bool read=GetDIBits(dc,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS)!=0;
    DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr,screen);
    if (!printed || !read) return false;
    BITMAPFILEHEADER header{}; header.bfType=0x4d42;
    header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size());
    std::ofstream file(output,std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header),sizeof(header));
    file.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(BITMAPINFOHEADER));
    file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());
    return file.good();
}
LRESULT CALLBACK review_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    // The production WM_CREATE also opens configuration/storage. This fixture
    // deliberately tests only real controls and painters, never that boundary.
    if (message==WM_CREATE) return 0;
    return window_proc(window,message,wparam,lparam);
}
}
int wmain(int argc, wchar_t** argv) {
    unsigned checks=0;
    State state; state.instance=GetModuleHandleW(nullptr);
    const auto original=state.appearance.theme();
    const auto require=[&](bool ok,const char* label) { ++checks; if(!ok) throw std::runtime_error(label); std::cout<<"check "<<checks<<": "<<label<<std::endl; };
    try {
        require(argc==3 || (argc==4 && std::wstring(argv[3])==L"--interactive-scroll"),"output and DPI required");
        const auto output=std::filesystem::absolute(argv[1]).lexically_normal();
        require(output.parent_path()==std::filesystem::path(SFX_UI_REVIEW_SCRATCH) &&
            output.filename().wstring().find(L"config-theme-")==0 && !std::filesystem::exists(output),"fresh exact evidence path");
        require(std::filesystem::create_directories(output),"create evidence");
        state.dpi=std::stoi(argv[2]); require(state.dpi==96 || state.dpi==144,"bounded DPI");
        state.root=output/L"synthetic-not-an-installation";
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
        WNDCLASSW cls{}; cls.hInstance=state.instance; cls.lpfnWndProc=review_proc;
        cls.lpszClassName=kWindowClass; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        require(RegisterClassW(&cls)!=0,"register actual Config class");
        HWND window=CreateWindowW(kWindowClass,L"SFXConfig native theme review — synthetic settings",
            WS_OVERLAPPEDWINDOW,25,25,MulDiv(980,state.dpi,96),MulDiv(750,state.dpi,96),
            nullptr,nullptr,state.instance,&state);
        require(window && create_ui(state),"actual Config pages created");
        SetMenu(window,create_menu(state));
        state.configuration.saved.bbs_name="Spitfire-X BBS";
        state.configuration.saved.bbs_time_zone="Pacific Standard Time";
        state.configuration.saved.security_policy_revision=1;
        state.configuration.saved.security_policy_hash=std::string(64,'a');
        state.configuration.active=state.configuration.saved;
        state.configuration.detail="Synthetic review; no configuration saved";
        populate_controls(state);
        state.scheduler.configuration_loaded = true;
        state.backup_settings = app::SchedulerConfiguration{};
        state.mode=State::ConnectionMode::offline;
        populate_controls(state);
        require(text(state.backup_destination)==(state.root/L"Backup"/L"Scheduled").native(),
            "default backup folder is visible instead of an empty field");
        require(IsWindowEnabled(state.backup_destination) && IsWindowEnabled(state.backup_browse) &&
            IsWindowEnabled(state.backup_retention), "stopped backup folder browse and retention are enabled");
        require(active_saved_line(state,L"setting",L"old",L"saved").find(L"Active:")==std::wstring::npos,
            "offline summary never labels a copied snapshot as active");
        state.mode=State::ConnectionMode::online;
        populate_controls(state);
        require(IsWindowEnabled(state.backup_destination) && IsWindowEnabled(state.backup_browse) &&
            !IsWindowEnabled(state.backup_retention), "live backup folder and Browse enabled; retention stays stopped-only");
        show_page(state,5); remember_page(state);
        SetWindowTextW(state.backup_destination,L"C:\\Private\\Selected");
        require(page_dirty(state), "backup folder selection participates in unsaved-change protection");
        discard_page(state);
        require(text(state.backup_destination)==(state.root/L"Backup"/L"Scheduled").native() && !page_dirty(state),
            "discard returns to saved effective default folder");
        require(GetParent(state.backup_browse)==state.pages[5] && text(state.backup_browse)==L"Browse...",
            "native Browse button belongs to scheduled jobs page");
        folder_picker_owner=window;folder_picker_selection=std::filesystem::current_path().native();
        const auto picker_hook=SetWindowsHookExW(WH_CBT,folder_picker_hook,nullptr,GetCurrentThreadId());
        require(picker_hook!=nullptr,"isolated native folder dialog test hook");
        const auto unchanged_folder=text(state.backup_destination);
        folder_picker_accept=false;browse_backup_destination(state);
        require(folder_picker_activations==1 && text(state.backup_destination)==unchanged_folder && !page_dirty(state),
            "native folder picker Cancel leaves saved field and dirty state unchanged");
        folder_picker_accept=true;browse_backup_destination(state);
        require(folder_picker_activations==2 && text(state.backup_destination)==folder_picker_selection && page_dirty(state),
            "native folder picker selection changes only unsaved field");
        UnhookWindowsHookEx(picker_hook);folder_picker_owner=nullptr;
        discard_page(state);remember_page(state);
        require(active_saved_line(state,L"setting",L"old",L"saved").find(L"Active: old; Saved: saved")!=std::wstring::npos,
            "live summary distinguishes actual active and saved values");
        state.mode=State::ConnectionMode::blocked;
        populate_controls(state);
        require(!IsWindowEnabled(state.backup_destination) && !IsWindowEnabled(state.backup_browse),
            "unverified connection cannot edit or browse backup destination");
        state.backup_settings.reset(); state.scheduler = {}; populate_controls(state);
        require(TreeView_GetCount(state.navigation)==31,"eight functional categories and twenty-three sidebar entries; three security tabs");
        require(sfx::config_ui::route(L"general.displays_menus").page==17 &&
            GetParent(state.menu_text)==state.pages[17],"Displays and Menus has a stable route and working controls");
        require(SendMessageW(state.menu_text,CB_GETCOUNT,0,0)==15 &&
            SendMessageW(state.menu_input,CB_GETCOUNT,0,0)==15 &&
            SendMessageW(state.menu_foreground,CB_GETCOUNT,0,0)==16 &&
            SendMessageW(state.menu_background,CB_GETCOUNT,0,0)==8,"supported named palette choices only");
        for(unsigned index=0;index<12;++index) for(unsigned format=0;format<2;++format) {
            SendMessageW(state.menu_select,CB_SETCURSEL,index,0);SendMessageW(state.menu_format,CB_SETCURSEL,format,0);
            refresh_menu_preview(state);
            std::string shown;for(const auto& cell:state.menu_screen.cells)shown+=static_cast<char>(cell.character);
            require(shown.find(app::menu_definition(static_cast<app::MenuId>(index)).title)!=std::string::npos,
                "safe generated preview contains selected menu without an installation or caller");
        }
        SendMessageW(state.menu_select,CB_SETCURSEL,0,0);SendMessageW(state.menu_format,CB_SETCURSEL,0,0);
        show_page(state,17);remember_page(state);
        SetWindowTextW(state.menu_level,L"999");
        require(!page_dirty(state),"preview level is not a saved configuration draft");
        SendMessageW(state.menu_text,CB_SETCURSEL,1,0);
        require(page_dirty(state) && candidate_for_page(state,17)->menus.text_color==2,"board color participates in ordinary unsaved-change protection");
        discard_page(state);require(!page_dirty(state),"discard restores saved palette");
        SetWindowTextW(state.menu_level,L"10");refresh_menu_preview(state);
        require(TabCtrl_GetItemCount(state.security_tabs)==3 && state.route_items[9]==state.route_items[11] && state.route_items[10]==state.route_items[11],"one BBS Security sidebar destination");
        require(sfx::config_ui::route(L"security.bbs_security").page==11,"new BBS Security route opens BotGate");
        for(auto invalid:{L"--edit-user ",L"--edit-user 0",L"--edit-user -1",L"--edit-user 1 trailing",L"--edit-user 9223372036854775808"})
            require(win32::edit_user_argument(invalid)==0,"malformed user deep link refuses");
        require(win32::edit_user_argument(L"--edit-user 4294967297")==4294967297LL && win32::edit_user_message_id(1,1)==4294967297LL,
            "user deep link preserves full account ID on Win32");
        for(const auto& route:sfx::config_ui::routes) {
            require(sfx::config_ui::route(route.id).page==route.page,"stable route round trip");
            require(state.route_items[route.page]!=nullptr,"route has native child");
        }
        require(sfx::config_ui::route(L"obsolete.route").page==0,"unknown saved route falls back");
        require(GetParent(state.allow_users)==state.pages[15],"one registration owner");
        require(GetParent(state.time_zone)==state.pages[13] && GetParent(state.caller_afk)==state.pages[14],"separate time and runtime owners");
        require(GetParent(state.strong_passwords)==state.pages[16],"password policy split");
        TreeView_SelectItem(state.navigation,state.route_items[13]);
        require(state.current_page==13 && text(state.heading)==L"General > Regional & Time",
            "native child selection updates page and breadcrumb");
        HKEY route_key{};
        require(RegOpenKeyExW(HKEY_CURRENT_USER,operator_registry_key(state.root).c_str(),0,KEY_QUERY_VALUE,&route_key)==ERROR_SUCCESS,
            "route preference uses isolated operator key");
        const auto saved_route=registry_string(route_key,L"ConfigurationRoute",REG_SZ,512);
        RegCloseKey(route_key);
        require(saved_route && *saved_route==L"general.regional_time" && sfx::config_ui::route(*saved_route).page==13,
            "last page stores stable ID and resolves independently of tree position");
        TreeView_SelectItem(state.navigation,TreeView_GetParent(state.navigation,state.route_items[13]));
        require(state.current_page==0 && text(state.heading)==L"General > Board Identity",
            "native category selection opens its first functional child");
        show_page(state,0);
        SetWindowTextW(state.bbs_name,L"Edited board");
        require(page_dirty(state),"identity draft detected");
        SetWindowTextW(state.caller_afk,L"bad");
        require(candidate_for_page(state,0).has_value(),"identity save ignores invalid hidden runtime draft");
        discard_page(state); require(!page_dirty(state),"discard restores page only");
        populate_controls(state);
        show_page(state,12);
        SetWindowTextW(state.logging_limits[0],L"20");
        require(page_dirty(state),"unnumbered logging edits detected");
        discard_page(state);
        show_page(state,7);
        SetWindowTextW(state.system_password,L"fixture-only");
        require(page_dirty(state),"secret draft detected without copying contents");
        for(const auto& value:state.baseline) if(value.control==state.system_password)
            require(value.kind==3 && value.value.empty(),"draft snapshot excludes secret");
        discard_page(state); require(text(state.system_password).empty(),"discard erases secret");
        show_page(state,0);
        SetWindowTextW(state.bbs_name,L"Unsaved board");
        const auto hook=SetWindowsHookExW(WH_CBT,dialog_hook,nullptr,GetCurrentThreadId());
        require(hook!=nullptr,"native dirty dialog hook");
        dialog_choice=103;
        require(!navigate_route(state,L"system.runtime_behavior") && state.current_page==0 && page_dirty(state),"Cancel Navigation retains page and edits");
        require(dialog_labels,"native dirty task dialog opened");
        SendMessageW(window,WM_CLOSE,0,0);
        require(IsWindow(window) && page_dirty(state),"close respects cancellation");
        SendMessageW(window,RegisterWindowMessageW(L"SFXConfig.ShowBotGate.v1"),0,0);
        require(state.current_page==0 && page_dirty(state),"existing-instance BotGate deep link respects cancellation");
        SendMessageW(window,RegisterWindowMessageW(win32::edit_user_message),2,0);
        require(state.current_page==0 && page_dirty(state),"existing-instance Edit User deep link respects cancellation");
        TreeView_SelectItem(state.navigation,state.route_items[13]);
        require(state.current_page==0 && page_dirty(state),"native tree selection respects dirty cancellation");
        dialog_choice=101;
        SetWindowTextW(state.bbs_name,L"");
        require(!navigate_route(state,L"system.runtime_behavior") && state.current_page==0 && page_dirty(state),"Save and Continue validation failure retains draft");
        dialog_choice=102;
        require(navigate_route(state,L"system.runtime_behavior") && !page_dirty(state),"Discard Changes navigates");
        show_page(state,3);
        wchar_t fixture_exe[32768]{}; GetModuleFileNameW(nullptr,fixture_exe,32768);
        SetWindowTextW(state.client_path,fixture_exe);
        SendMessageW(state.client_type,CB_SETCURSEL,1,0);
        SetWindowTextW(state.client_arguments,L"--host\r\n{HOST}\r\n--port\r\n{PORT}");
        dialog_choice=101;
        require(navigate_route(state,L"general.board_identity") && state.current_page==0,"Save and Continue commits valid page then navigates");
        require(read_client_profile(state.root).has_value(),"valid save uses isolated operator test profile");
        UnhookWindowsHookEx(hook);
        show_page(state,9);
        SetWindowTextW(state.source_limit,L"7");
        const auto security_hook=SetWindowsHookExW(WH_CBT,dialog_hook,nullptr,GetCurrentThreadId());
        dialog_choice=103;
        NMHDR security_notice{state.security_tabs,IDC_BBS_SECURITY_TABS,TCN_SELCHANGING};
        require(SendMessageW(window,WM_NOTIFY,IDC_BBS_SECURITY_TABS,reinterpret_cast<LPARAM>(&security_notice))!=0 && page_dirty(state),
            "security tab change is cancelled with unsaved Connections draft intact");
        dialog_choice=102;
        require(SendMessageW(window,WM_NOTIFY,IDC_BBS_SECURITY_TABS,reinterpret_cast<LPARAM>(&security_notice))==0 && !page_dirty(state),
            "explicit discard permits security tab change");
        TabCtrl_SetCurSel(state.security_tabs,2);security_notice.code=TCN_SELCHANGE;
        SendMessageW(window,WM_NOTIFY,IDC_BBS_SECURITY_TABS,reinterpret_cast<LPARAM>(&security_notice));
        require(state.current_page==10 && text(state.heading)==L"Security > BBS Security","native Failed Login Attempts tab opens original protection page");
        UnhookWindowsHookEx(security_hook);
        account_query_fixture=[](const app::LocalAccountQuery& query) {
            app::LocalAccountSnapshot result;result.code=app::LocalAccountResultCode::ok;
            result.policy={1,10,999,std::string(64,'a'),1,std::string(64,'a')};
            if(query.account_id==99) return result;
            app::AccountRecord record;record.account_id=query.account_id ? query.account_id : 1;
            record.display_name="Fixture Sysop";record.state=app::AccountState::active;record.security_level=std::uint16_t{999};record.revision=1;
            result.accounts.push_back(record);return result;
        };
        account_admin_fixture=[](app::LocalAccountRequest&) {++fixture_admin_calls;return app::LocalAccountResult{app::LocalAccountResultCode::last_sysop_protected};};
        show_page(state,17);
        RECT footer_bounds{},bars_bounds{},background_bounds{};
        GetWindowRect(state.menu_footer,&footer_bounds);GetWindowRect(state.menu_bars,&bars_bounds);
        GetWindowRect(state.menu_background,&background_bounds);
        require(footer_bounds.top>=background_bounds.bottom && bars_bounds.top>=footer_bounds.bottom,
            "Menu Footer and adjacent controls do not overlap at this DPI");
        require(SendMessageW(state.menu_footer,CB_GETCOUNT,0,0)==2 && !candidate_for_page(state,17)->menus.compact_footer,
            "Menu Footer has two choices and defaults to SF Original");
        SendMessageW(state.menu_footer,CB_SETCURSEL,1,0);refresh_menu_preview(state);
        require(candidate_for_page(state,17)->menus.compact_footer && page_dirty(state),"footer selection participates in Save/dirty protection");
        populate_controls(state);remember_page(state);
        require(open_user_record(state,4294967297LL) && state.account_selection.accounts[0].account_id==4294967297LL,
            "Edit User opens exact full-width ID without matching a handle");
        require(!open_user_record(state,99) && state.account_selection.accounts.empty(),"missing user does not open a different account");
        require(open_user_record(state,1),"open sole Sysop fixture");
        const auto action_hook=SetWindowsHookExW(WH_CBT,dialog_hook,nullptr,GetCurrentThreadId());
        const int dirty_start=dirty_dialogs,refusal_start=refusal_dialogs;
        SetWindowTextW(state.user_level,L"10");
        SendMessageW(state.account_apply,BM_CLICK,0,0);
        require(fixture_admin_calls==1 && refusal_dialogs==refusal_start+1 && dirty_dialogs==dirty_start && page_dirty(state),
            "real Save button shows one refusal with draft retained and no dirty loop");
        dialog_choice=101;
        require(!navigate_route(state,L"general.board_identity") && state.current_page==6 && page_dirty(state) &&
            fixture_admin_calls==2 && dirty_dialogs==dirty_start+1 && refusal_dialogs==refusal_start+2,
            "Save and Continue refuses once and remains on user draft");
        discard_page(state);remember_page(state);
        SendMessageW(state.user_delete,BM_CLICK,0,0);
        SendMessageW(state.user_toggle,BM_CLICK,0,0);
        require(fixture_admin_calls==4 && refusal_dialogs==refusal_start+4 && dirty_dialogs==dirty_start+1,
            "real Delete and Deactivate each display the precise refusal once");
        UnhookWindowsHookEx(action_hook);account_query_fixture=nullptr;account_admin_fixture=nullptr;
        classic_fixture={app::LocalAccountResultCode::ok,{{{7,2,{"Fixture BBS","bbs.example:2323","ssh.example:2222"}}},false},{4,1700000000,"1984-05-12"}};
        classic_query_fixture=[](std::int64_t){return classic_fixture;};
        classic_change_fixture=[](const app::ClassicInformationChange& change) {
            last_classic_change=change;
            if(classic_conflict)return app::LocalAccountResult{app::LocalAccountResultCode::conflict};
            if(change.action==app::ClassicInformationAction::opening_date) {classic_fixture.opening.configured_date=change.date;++classic_fixture.opening.revision;}
            else if(change.action==app::ClassicInformationAction::edit_board) {classic_fixture.boards.entries[0].values=change.values;++classic_fixture.boards.entries[0].revision;}
            return app::LocalAccountResult{app::LocalAccountResultCode::ok,true,false,change.expected_revision+1};
        };
        activate_page(state,18);
        require(text(state.opening_date)==L"1984-05-12" && GetParent(state.opening_date)==state.pages[18],"historical opening date uses separate Sysop page");
        SetWindowTextW(state.opening_date,L"2026-02-30");save_opening_date(state);
        require(text(state.status).find(L"Refused")==0 && text(state.opening_date)==L"2026-02-30","invalid date retains draft without mutation");
        SetWindowTextW(state.opening_date,L"1990-01-02");classic_conflict=true;save_opening_date(state);
        require(text(state.opening_date)==L"1990-01-02" && last_classic_change.expected_revision==4 && !action_succeeded(state),"opening date conflict retains shown revision and draft");
        classic_conflict=false;save_opening_date(state);remember_page(state);
        require(action_succeeded(state) && text(state.opening_date)==L"1990-01-02","opening date save refreshes committed truth");
        require(std::none_of(sfx::config_ui::routes.begin(),sfx::config_ui::routes.end(),[](const auto& route){return route.id==L"general.other_boards";}),"Other BBS navigation and page removed");
        require(sfx::config_ui::route(L"general.other_boards").page==0 && state.pages.size()==25,"retired Other BBS route safely falls back; four functional file configuration pages added");
        classic_query_fixture=nullptr;classic_change_fixture=nullptr;
        message_fixture.revision=1;message_fixture.assignment_revision=1;message_fixture.system_sysop=7;message_fixture.private_presentation_revision=1;
        message_fixture.groups={{{1},"Local",0,1,1,true},{{2},"Second",1,1,1,true}};
        message_fixture.echoes={{{1},{1},1,"Sysop Notices",0,1,{1,1,999},true},{{2},{1},2,"Public Chat",1,1,{1,1,1},true}};
        message_fixture.line_limits={{10,200,1},{999,1000,1}};
        message_query_fixture=[](const app::LocalMessageConfigurationQuery& q){auto s=message_fixture;if(q.group){s.groups.clear();s.echoes.erase(std::remove_if(s.echoes.begin(),s.echoes.end(),[&](const app::LocalEcho& e){return !(e.group==*q.group);}),s.echoes.end());}else s.echoes.clear();return app::LocalMessageConfigurationSnapshot{app::LocalAccountResultCode::ok,s};};
        message_change_fixture=[](const app::MessageConfigurationChange& c){++message_admin_calls;last_message_change=c;if(message_uncertain)return app::LocalMessageConfigurationResult{};return app::LocalMessageConfigurationResult{app::LocalAccountResultCode::ok,{message_fixture.revision,1,1,true}};};
        activate_page(state,19);
        require(SendMessageW(state.message_groups,LB_GETCOUNT,0,0)==2 && SendMessageW(state.message_echoes,LB_GETCOUNT,0,0)==2,"native message catalog loads typed group/echo identities");
        require(text(state.message_name)==L"Local" && !IsWindowEnabled(state.message_reading),"group form never invents independent reading permission");
        require(!IsWindowEnabled(state.message_pipe_colors) && Button_GetCheck(state.message_pipe_colors)==BST_UNCHECKED,"group cannot invent an echo pipe policy");
        state.editing_echo=true;state.editing_message_echo=message_fixture.echoes[1];populate_message_form(state);remember_page(state);
        require(IsWindowEnabled(state.message_reading) && text(state.message_name)==L"Public Chat","echo form exposes three independent permissions");
        require(IsWindowEnabled(state.message_pipe_colors) && Button_GetCheck(state.message_pipe_colors)==BST_UNCHECKED,"echo policy defaults Off and is an actual checkbox");
        Button_SetCheck(state.message_pipe_colors,BST_CHECKED);
        SendMessageW(state.message_parent,CB_SETCURSEL,1,0);SetWindowTextW(state.message_name,L"Moved Chat");
        const auto message_hook=SetWindowsHookExW(WH_CBT,dialog_hook,nullptr,GetCurrentThreadId());require(message_hook!=nullptr,"message confirmations use actual native dialogs");
        save_message_catalog(state);
        require(last_message_change.presentation_mask==1 && last_message_change.echo.renegade_pipe_colors,"echo Save captures explicit presentation mask/value");
        require(last_message_change.echo.id.value==2 && last_message_change.echo.group.value==2 && last_message_change.echo.conference_number==2 && last_message_change.expected_configuration_revision==1,"native move preserves stable identity/number and exact expected revision");
        const auto before_private=message_admin_calls;
        toggle_private_pipe_colors(state);require(message_admin_calls==before_private,"private policy default No makes no mutation");
        presentation_yes=true;toggle_private_pipe_colors(state);presentation_yes=false;
        require(last_message_change.action==app::MessageConfigurationAction::private_presentation && last_message_change.expected_private_presentation_revision==1 && last_message_change.private_pipe_colors,"private policy explicit confirmation uses typed CAS");
        activate_page(state,19);state.editing_echo=true;state.editing_message_echo=message_fixture.echoes[1];populate_message_form(state);remember_page(state);
        for(const auto command:{IDC_MESSAGE_INSERT,IDC_MESSAGE_MOVE_UP,IDC_MESSAGE_MOVE_DOWN,IDC_MESSAGE_DELETE})require(IsWindowEnabled(GetDlgItem(state.pages[19],command)),"new echo administration buttons are actual enabled controls");
        const auto before_cancel_delete=message_admin_calls;operate_message_echo(state,app::MessageConfigurationAction::delete_echo);
        require(message_admin_calls==before_cancel_delete,"native destructive No is default and submits no operation");
        delete_yes=true;operate_message_echo(state,app::MessageConfigurationAction::delete_echo);
        require(last_message_change.action==app::MessageConfigurationAction::delete_echo && last_message_change.echo.id.value==2 && last_message_change.echo.revision==1,"native confirmed delete captures stable echo and revision");delete_yes=false;
        state.editing_echo=true;state.editing_message_echo=message_fixture.echoes[1];populate_message_form(state);remember_page(state);
        operate_message_echo(state,app::MessageConfigurationAction::move_echo_up);
        require(last_message_change.action==app::MessageConfigurationAction::move_echo_up && last_message_change.echo.id.value==2,"native Move Up submits typed stable operation");
        state.editing_echo=true;state.editing_message_echo=message_fixture.echoes[1];populate_message_form(state);remember_page(state);
        operate_message_echo(state,app::MessageConfigurationAction::move_echo_down);
        require(last_message_change.action==app::MessageConfigurationAction::move_echo_down,"native Move Down is separate typed operation");
        state.editing_echo=true;state.editing_message_echo=message_fixture.echoes[1];populate_message_form(state);remember_page(state);insert_message_echo(state);
        require(state.message_insert_anchor && state.message_insert_anchor->id.value==2 && !state.editing_message_echo && text(state.message_name).empty(),"Insert Before Selected retains immutable anchor and opens new draft");
        SetWindowTextW(state.message_name,L"Support");save_message_catalog(state);
        require(last_message_change.action==app::MessageConfigurationAction::insert_echo && last_message_change.insert_before.value==2 && last_message_change.insert_before_revision==1 && last_message_change.echo.id.value==0,"native insertion submits fresh echo with anchor revision");
        // Production queries refresh Runtime status after commit. Saved state
        // must follow the typed result, never that overwritten caption.
        message_fixture_status=state.status;
        message_query_fixture=[](const app::LocalMessageConfigurationQuery& q){
            SetWindowTextW(message_fixture_status,L"Runtime running — refreshed after Save");
            auto s=message_fixture;if(q.group){s.groups.clear();s.echoes.erase(std::remove_if(s.echoes.begin(),s.echoes.end(),[&](const app::LocalEcho& e){return !(e.group==*q.group);}),s.echoes.end());}else s.echoes.clear();
            return app::LocalMessageConfigurationSnapshot{app::LocalAccountResultCode::ok,s};};
        for(unsigned kind=0;kind<5;++kind){
            const bool editing_echo=kind==1 || kind>=3;
            state.editing_echo=editing_echo;state.editing_message_group=kind==0?std::optional<app::MessageGroup>(message_fixture.groups[0]):std::nullopt;state.editing_message_echo=kind==1?std::optional<app::LocalEcho>(message_fixture.echoes[1]):std::nullopt;
            state.message_insert_anchor=kind==4?std::optional<app::LocalEcho>(message_fixture.echoes[1]):std::nullopt;
            populate_message_form(state);remember_page(state);SetWindowTextW(state.message_name,L"Committed edit");
            require(page_dirty(state),"changed group/echo starts dirty");
            require(save_current_page(state) && !page_dirty(state) && text(state.status).find(L"Runtime running")==0,"successful save clears dirty baseline despite refresh status");
            const auto before=dirty_dialogs;require(resolve_dirty(state) && dirty_dialogs==before,"navigation after successful save needs no discard or second save");
        }
        SetWindowTextW(state.message_name,L"Uncommitted edit");catalog_yes=false;
        const auto before_cancel_save=message_admin_calls;require(!save_current_page(state) && page_dirty(state) && message_admin_calls==before_cancel_save,"cancelled Save retains draft and submits nothing");catalog_yes=true;
        SetWindowTextW(state.message_name,L"");require(!save_current_page(state) && page_dirty(state),"invalid Save does not clear dirty baseline");
        SetWindowTextW(state.message_name,L"Conflict draft");message_change_fixture=[](const app::MessageConfigurationChange& c){++message_admin_calls;last_message_change=c;return app::LocalMessageConfigurationResult{app::LocalAccountResultCode::conflict};};
        require(!save_current_page(state) && page_dirty(state),"refused Save retains unsaved baseline");
        message_change_fixture=[](const app::MessageConfigurationChange& c){++message_admin_calls;last_message_change=c;if(message_uncertain)return app::LocalMessageConfigurationResult{};return app::LocalMessageConfigurationResult{app::LocalAccountResultCode::ok,{message_fixture.revision,1,1,true}};};
        message_uncertain=true;require(!save_current_page(state) && state.pending_message_apply && page_dirty(state) && !resolve_dirty(state),"uncertain Save freezes navigation without marking saved");message_uncertain=false;reconcile_message_apply(state);
        message_query_fixture=[](const app::LocalMessageConfigurationQuery& q){auto s=message_fixture;if(q.group){s.groups.clear();s.echoes.erase(std::remove_if(s.echoes.begin(),s.echoes.end(),[&](const app::LocalEcho& e){return !(e.group==*q.group);}),s.echoes.end());}else s.echoes.clear();return app::LocalMessageConfigurationSnapshot{app::LocalAccountResultCode::ok,s};};
        activate_page(state,19);require(text(GetDlgItem(state.pages[19],IDC_MESSAGE_DELETE))==L"Delete Group","group selection labels Delete Group explicitly");
        const auto populated_calls=message_admin_calls;delete_message_group(state);require(message_admin_calls==populated_calls && text(state.status).find(L"Refused")==0,"populated group refuses before destructive dialog");
        state.message_group_index=1;state.editing_echo=false;state.editing_message_group=message_fixture.groups[1];state.editing_message_echo.reset();load_message_echoes(state);populate_message_form(state);remember_page(state);
        const auto before_group_no=message_admin_calls;delete_message_group(state);require(message_admin_calls==before_group_no,"empty group confirmation No submits nothing");
        delete_yes=true;SendMessageW(window,WM_COMMAND,IDC_MESSAGE_DELETE,0);
        require(last_message_change.action==app::MessageConfigurationAction::delete_group && last_message_change.group.id.value==2 && last_message_change.group.revision==1 && !page_dirty(state),"confirmed Delete Group captures stable identity and clears refreshed baseline");delete_yes=false;
        state.clone_source=message_fixture.echoes[0];state.clone_echo_cursor={1};SetWindowTextW(state.clone_source_text,L"Source: Sysop Notices");
        activate_page(state,20);SendMessageW(state.clone_list,LB_SETSEL,TRUE,1);add_clone_targets(state);
        require(state.clone_targets.size()==1 && Button_GetCheck(state.clone_access)==BST_UNCHECKED && Button_GetCheck(state.clone_reading)==BST_UNCHECKED && Button_GetCheck(state.clone_posting)==BST_UNCHECKED,"clone starts with explicit targets and all categories unchecked");
        const auto before_noop=message_admin_calls;apply_clone(state);require(message_admin_calls==before_noop,"unchecked clone is no-op without a mutation");
        Button_SetCheck(state.clone_posting,BST_CHECKED);const auto preview=clone_preview(state);
        require(preview && preview->permission_mask==4 && text(state.clone_preview).find(L"Post 1 -> 999")!=std::wstring::npos,"permission-only clone previews exact old/new differences");
        apply_clone(state);require(last_message_change.permission_mask==4 && last_message_change.clone_targets.size()==1 && last_message_change.clone_targets[0].echo.value==2,"native clone submits explicit source and target revisions");
        require(text(state.clone_preview).empty() && text(state.clone_source_text).find(L"No source")==0,"committed clone clears its stale caption and preview");
        show_page(state,8);SetWindowTextW(state.level_number,L"10");refresh_message_limit(state);remember_page(state);SetWindowTextW(state.level_message_lines,L"201");
        SetWindowTextW(state.level_label,L"Unrelated unsaved label");
        message_uncertain=true;save_message_limit(state);const auto pending=state.pending_message_apply;
        require(pending && !IsWindowEnabled(state.level_message_lines) && !resolve_dirty(state),"unresolved apply freezes edits/navigation without a second operation");
        const auto frozen_calls=message_admin_calls;save_message_limit(state);require(message_admin_calls==frozen_calls,"frozen message apply cannot manufacture a new operation");
        message_uncertain=false;reconcile_message_apply(state);
        require(!state.pending_message_apply && app::encode_message_configuration_change(last_message_change)==app::encode_message_configuration_change(*pending),"explicit reconcile preserves exact OperationId and canonical request");
        require(text(state.level_label)==L"Unrelated unsaved label" && page_dirty(state),"message-limit reconciliation preserves unrelated unsaved security edits");
        discard_page(state);remember_page(state);
        SetWindowTextW(state.level_number,L"500");refresh_message_limit(state);message_controls(state,true);
        require(!IsWindowEnabled(state.level_message_lines) && !IsWindowEnabled(state.level_message_save),"global availability cannot enable a missing active-level allowance");
        show_page(state,0);remember_page(state);SetWindowTextW(state.bbs_name,L"Unsaved board identity");
        SendMessageW(window,WM_COMMAND,IDC_SYSTEM_SYSOP_FIND,0);
        require(text(state.bbs_name)==L"Unsaved board identity" && page_dirty(state),"Sysop candidate search does not mark BBS draft saved");
        discard_page(state);remember_page(state);
        // Save refreshes the catalog and removes the old optional continuation.
        // The production Next handler must not dereference/remap that cursor.
        activate_page(state,19);state.editing_echo=false;state.editing_message_group=message_fixture.groups[0];populate_message_form(state);remember_page(state);
        state.message_catalog.catalog.next=app::MessageCatalogCursor{1,0,1,50};
        message_change_fixture=[](const app::MessageConfigurationChange& c){++message_admin_calls;last_message_change=c;++message_fixture.revision;return app::LocalMessageConfigurationResult{app::LocalAccountResultCode::ok,{message_fixture.revision,1,2,true}};};
        message_query_fixture=[](const app::LocalMessageConfigurationQuery& q){auto s=message_fixture;if(q.cursor.after_id)++message_next_queries;if(q.group){s.groups.clear();}else s.echoes.clear();return app::LocalMessageConfigurationSnapshot{app::LocalAccountResultCode::ok,s};};
        dialog_choice=101;SetWindowTextW(state.message_name,L"Saved before Next");
        SendMessageW(window,WM_COMMAND,IDC_MESSAGE_GROUP_NEXT,0);
        require(message_next_queries==0 && !state.message_catalog.catalog.next && text(state.status).find(L"Catalog changed")!=std::wstring::npos,"Next captures stable cursor before Save and refuses refreshed catalog; no optional cursor dereference");
        state.message_catalog.catalog.next=app::MessageCatalogCursor{message_fixture.revision,0,1,50};
        state.editing_message_group=message_fixture.groups[0];populate_message_form(state);remember_page(state);
        dialog_choice=102;SetWindowTextW(state.message_name,L"Discard before Next");
        SendMessageW(window,WM_COMMAND,IDC_MESSAGE_GROUP_NEXT,0);
        require(message_next_queries==1 && text(state.message_name)!=L"Discard before Next","Next after Discard advances only the captured unchanged-revision cursor");
        dialog_choice=103;
        show_page(state,0);remember_page(state);
        UnhookWindowsHookEx(message_hook);message_query_fixture=nullptr;message_change_fixture=nullptr;
        show_page(state,14);
        require(candidate_for_page(state,14) && candidate_for_page(state,14)->caller_afk_minutes == 5,
            "System page has default five-minute AFK setting");
        require(candidate_for_page(state,14)->login_afk_seconds==60,"separate login timeout defaults to sixty seconds");
        for(const auto* invalid:{L"0",L"29",L"601",L""}) {SetWindowTextW(state.login_afk,invalid);require(!candidate_for_page(state,14),"login timeout validation");}
        SetWindowTextW(state.login_afk,L"60");
        for (const auto* invalid : {L"0",L"1",L"1441",L"",L"-1"}) {
            SetWindowTextW(state.caller_afk,invalid);
            require(!candidate_for_page(state,14),"System page rejects invalid AFK limit");
        }
        SetWindowTextW(state.caller_afk,L"2");
        require(candidate_for_page(state,14) && candidate_for_page(state,14)->caller_afk_minutes == 2,
            "System page accepts bounded nondefault AFK limit");
        populate_controls(state);
        Button_SetCheck(state.allow_users,BST_UNCHECKED);
        SetWindowTextW(state.ssh_port,L"invalid");
        const auto registration=candidate_for_page(state,15);
        require(registration && !registration->allow_new_users && registration->ssh.port==state.configuration.saved.ssh.port,
            "registration save ignores listener edits and uses one saved Boolean");
        require(!candidate_for_page(state,2),"listener validation is still enforced on its own page");
        populate_controls(state);
        require(candidate_for_page(state,9).has_value(),"connection defaults form is valid");
        SetWindowTextW(state.source_limit,L"8"); SetWindowTextW(state.account_limit,L"2");
        SetWindowTextW(state.source_exceptions,L"2001:db8::1|9\r\n192.0.2.1|3\r\n");
        const auto limits = candidate_for_page(state,9);
        require(limits && limits->maximum_connections_per_source == 8 && limits->maximum_sessions_per_account == state.configuration.saved.maximum_sessions_per_account &&
            !IsWindowEnabled(state.account_limit) &&
            limits->source_connection_exceptions.size() == 2 && limits->source_connection_exceptions.front().address == "192.0.2.1",
            "connection form sorts exact-address exceptions and retains inactive legacy account limit; 999-only policy is not user-broadened");
        for (const auto* invalid : {L"example.test|4",L"192.0.2.0/24|4",L"192.0.2.1|4,192.0.2.2|4",
                L"192.0.2.1|4\r\n192.0.2.1|8",L"-",L"127.0.0.1|129"}) {
            SetWindowTextW(state.source_exceptions,invalid);
            require(!candidate_for_page(state,9),"connection form refuses ambiguous or unbounded exceptions");
        }
        populate_controls(state);
        SetWindowTextW(state.status,L"Native presentation review — no Runtime or installation writes");
        {
            state.security_catalog={app::LocalAccountResultCode::ok,{1,10,999,std::string(64,'a'),1,std::string(64,'a')},
                {{10,"New User",{},app::SecurityLevelState::active,60,180},{999,"Sysop",{},app::SecurityLevelState::active}},false};
            SendMessageW(state.level_list,LB_RESETCONTENT,0,0);
            SendMessageW(state.level_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"10 New User"));
            SendMessageW(state.level_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"999 Sysop"));
            SendMessageW(state.level_list,LB_SETCURSEL,0,0);select_level(state);
            require(text(state.level_call_minutes)==L"60" && text(state.level_day_minutes)==L"180" &&
                IsWindowEnabled(state.level_call_minutes) && IsWindowEnabled(state.level_day_minutes),"independent calling-time controls show real selected values");
            const auto old_page=state.current_page;state.current_page=8;remember_page(state);
            SetWindowTextW(state.level_day_minutes,L"240");
            require(page_dirty(state),"calling limit participates in unsaved-edit safeguards");
            discard_page(state);require(!page_dirty(state) && text(state.level_day_minutes)==L"180","discard restores exact selected limit");
            SendMessageW(state.level_list,LB_SETCURSEL,1,0);select_level(state);
            require(text(state.level_call_minutes)==L"0" && text(state.level_day_minutes)==L"0" &&
                !IsWindowEnabled(state.level_call_minutes) && !IsWindowEnabled(state.level_day_minutes),"999 has fixed unlimited controls without changing general Sysop threshold");
            state.current_page=old_page;state.security_catalog={};state.selected_level_index=-1;
            SendMessageW(state.level_list,LB_RESETCONTENT,0,0);remember_page(state);
        }
        RECT client{}; GetClientRect(window,&client); layout(state,client.right,client.bottom);
        LVITEMW row{}; row.mask=LVIF_TEXT; row.pszText=const_cast<wchar_t*>(L"Synthetic-backup.zip");
        ListView_InsertItem(state.backup_list,&row);
        ListView_SetItemText(state.backup_list,0,1,const_cast<wchar_t*>(L"Test row only"));
        ListView_SetItemText(state.backup_list,0,3,const_cast<wchar_t*>(L"Unverified"));
        ShowWindow(window,SW_SHOW);
        for(const auto theme : {win32::DesktopTheme::classic,win32::DesktopTheme::dark_retro}) {
            SendMessageW(window,WM_COMMAND,theme==win32::DesktopTheme::classic ?
                win32::DesktopAppearance::classic_command : win32::DesktopAppearance::dark_command,0);
            require(state.appearance.theme()==theme,"actual theme command applies");
            require(state.appearance.palette().panel==(theme==win32::DesktopTheme::classic ? RGB(249,249,247) : RGB(27,30,32)),
                "Config retains its accepted content background in both themes");
            require(state.appearance.palette().alternate==(theme==win32::DesktopTheme::classic ? RGB(240,240,240) : RGB(30,34,36)),
                "Config retains its accepted alternate background in both themes");
            require(state.appearance.palette().cpu==(theme==win32::DesktopTheme::classic ? RGB(222,234,250) : RGB(37,59,104)),
                "Control Panel CPU adjustment does not alter Config palette");
            const std::wstring name=theme==win32::DesktopTheme::classic ? L"classic" : L"dark";
            for(int index=0;index<static_cast<int>(state.pages.size());++index) {
                show_page(state,index); state.navigating=true; TreeView_SelectItem(state.navigation,state.route_items[index]); state.navigating=false;
                require(capture(window,output/(name+L"-page-"+std::to_wstring(index)+L".bmp")),"capture real page controls");
            }
            show_page(state,6);
            require(text(state.user_new)==L"New User" && text(state.account_apply)==L"Save" && text(state.user_reload)==L"Reload" &&
                text(state.user_delete)==L"Delete User","familiar ordered primary actions");
            require(TabCtrl_GetItemCount(state.user_tabs)==2,"only supported Personal and Security tabs");
            state.creating_user=true;populate_user_editor(state);
            require(!IsWindowEnabled(state.account_apply),"Save disabled for clean user draft");
            SetWindowTextW(state.user_fields[0],L"Draft User");
            require(IsWindowEnabled(state.account_apply) && page_dirty(state),"record edit enables Save");
            const auto user_hook=SetWindowsHookExW(WH_CBT,dialog_hook,nullptr,GetCurrentThreadId());
            require(user_hook!=nullptr,"user unsaved guard hook");
            dialog_choice=103;
            SendMessageW(window,WM_COMMAND,IDC_USER_RELOAD,0);
            require(page_dirty(state) && text(state.user_fields[0])==L"Draft User","Reload cancellation preserves user draft");
            SendMessageW(window,WM_COMMAND,IDC_ACCOUNT_NEXT,0);
            require(page_dirty(state) && text(state.user_fields[0])==L"Draft User","Next cancellation preserves user draft");
            require(!navigate_route(state,L"users.security_levels") && state.current_page==6 && page_dirty(state),
                "away navigation preserves unsaved user draft on cancel");
            SendMessageW(window,WM_CLOSE,0,0);
            require(IsWindow(window) && page_dirty(state),"closing protects unsaved user draft");
            const auto dirty_before=dirty_dialogs;
            {
                AccountActionGuard action(state);
                require(!resolve_dirty(state) && dirty_dialogs==dirty_before,"account action refuses reentrant navigation without another dirty prompt");
                report_account_result(state,{app::LocalAccountResultCode::last_sysop_protected});
            }
            require(refusal_dialogs>0 && dirty_dialogs==dirty_before && page_dirty(state) &&
                text(state.status).find(L"at least one active Sysop")!=std::wstring::npos,"last-Sysop refusal is visible and draft is retained without looping");
            UnhookWindowsHookEx(user_hook);
            show_page(state,5);
            RECT folder{}, browse{};
            GetWindowRect(state.backup_destination,&folder);GetWindowRect(state.backup_browse,&browse);
            require(folder.right<browse.left && folder.top>=browse.top && folder.bottom<=browse.bottom,
                "folder field and Browse button remain separate and aligned in each theme/DPI");
            const auto old_mode=state.mode;const auto old_settings=state.backup_settings;const auto old_scheduler=state.scheduler;
            state.mode=State::ConnectionMode::online;state.backup_settings=app::SchedulerConfiguration{};state.scheduler.configuration_loaded=true;
            populate_controls(state);
            require(IsWindowEnabled(state.backup_browse) && capture(window,output/(name+L"-backup-live.bmp")),
                "capture live enabled folder picker controls without product execution");
            state.mode=old_mode;state.backup_settings=old_settings;state.scheduler=old_scheduler;populate_controls(state);
            RECT hour{},minute{};GetWindowRect(state.schedule_hour,&hour);GetWindowRect(state.schedule_minute,&minute);
            for(HWND child=GetWindow(state.pages[5],GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) if(text(child)==L":") {
                RECT colon{};GetWindowRect(child,&colon);
                require(hour.right<colon.left && colon.right<minute.left,"time separator leaves a gap on both edit borders in each theme/DPI");
            }
            show_page(state,6);
            TabCtrl_SetCurSel(state.user_tabs,1);
            NMHDR tab_notice{state.user_tabs,IDC_USER_TABS,TCN_SELCHANGE};
            SendMessageW(window,WM_NOTIFY,0,reinterpret_cast<LPARAM>(&tab_notice));
            require(SendMessageW(state.account_value,EM_GETPASSWORDCHAR,0,0)!=0 &&
                IsWindowVisible(state.account_confirm),"Security tab exposes masked password confirmation");
            SetWindowTextW(state.account_value,L"fixture-only-never-a-credential");
            TabCtrl_SetCurSel(state.user_tabs,0);user_tab(state);
            require(!IsWindowVisible(state.account_confirm) && SendMessageW(state.account_value,EM_GETPASSWORDCHAR,0,0)!=0,
                "switching tabs preserves masked draft without exposing password");
            discard_page(state);user_save_enabled(state);
            require(text(state.account_value).empty() && !IsWindowEnabled(state.account_apply),"discard removes secret draft and disables Save");
            require(SendMessageW(state.system_password,EM_GETPASSWORDCHAR,0,0)!=0 &&
                SendMessageW(state.system_confirm,EM_GETPASSWORDCHAR,0,0)!=0,
                "permanent System Password fields always masked");
            show_page(state,7);
            SetWindowTextW(state.system_password,L"fixture-only-never-a-credential");
            show_page(state,0);
            require(text(state.system_password).empty(),"navigation clears secret edit controls");
            show_page(state,2); EnableWindow(state.servers_save,FALSE);
            require(capture(window,output/(name+L"-disabled.bmp")),"disabled button and checkboxes capture");
            EnableWindow(state.servers_save,TRUE);
        }
        SetWindowPos(window,nullptr,0,0,MulDiv(780,state.dpi,96),MulDiv(610,state.dpi,96),SWP_NOMOVE|SWP_NOZORDER);
        GetClientRect(window,&client); layout(state,client.right,client.bottom);
        for (int page_index=0;page_index<static_cast<int>(state.pages.size());++page_index) {
            show_page(state,page_index);
            state.navigating=true; TreeView_SelectItem(state.navigation,state.route_items[page_index]); state.navigating=false;
            require(capture(window,output/(L"minimum-security-page-"+std::to_wstring(page_index)+L".bmp")),
                "minimum-size security controls capture");
            RECT viewport{}; GetClientRect(state.pages[page_index],&viewport);
            SCROLLINFO scrolling{sizeof(scrolling),SIF_RANGE|SIF_POS}; GetScrollInfo(state.pages[page_index],SB_VERT,&scrolling);
            for (HWND child = GetWindow(state.pages[page_index],GW_CHILD); child; child = GetWindow(child,GW_HWNDNEXT)) {
                RECT bounds{}; GetWindowRect(child,&bounds);
                MapWindowPoints(nullptr,state.pages[page_index],reinterpret_cast<POINT*>(&bounds),2);
                const bool fits = bounds.left >= 0 && bounds.top+scrolling.nPos >= 0 && bounds.right <= viewport.right && bounds.bottom+scrolling.nPos <= std::max(viewport.bottom,static_cast<LONG>(scrolling.nMax+1));
                if (!fits) std::cerr << "page=" << page_index << " control=" << GetDlgCtrlID(child)
                    << " bounds=" << bounds.left << ',' << bounds.top << ',' << bounds.right << ',' << bounds.bottom
                    << " viewport=" << viewport.right << ',' << viewport.bottom << '\n';
                require(fits,"security controls fit the minimum Config viewport");
            }
        }
        for(const auto theme:{win32::DesktopTheme::classic,win32::DesktopTheme::dark_retro}) {
            SendMessageW(window,WM_COMMAND,theme==win32::DesktopTheme::classic ?
                win32::DesktopAppearance::classic_command : win32::DesktopAppearance::dark_command,0);
            show_page(state,11);
            EnableWindow(state.gate_update,TRUE);
            SetFocus(state.gate_update);
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            RECT focused{},viewport{}; GetWindowRect(state.gate_update,&focused);
            MapWindowPoints(nullptr,state.pages[11],reinterpret_cast<POINT*>(&focused),2);
            GetClientRect(state.pages[11],&viewport);
            require(GetFocus()==state.gate_update && focused.top>=0 && focused.bottom<=viewport.bottom,
                "keyboard focus scrolls bottom action into minimum viewport in both themes");
            require(GetNextDlgTabItem(state.pages[11],state.gate_update,TRUE)!=nullptr,
                "native reverse tab traversal has an enabled destination");
            for(const auto page_index:{0,8,19,20}){
                show_page(state,page_index);const auto action=GetDlgItem(state.pages[page_index],IDC_MESSAGE_RECONCILE);
                EnableWindow(action,TRUE);SetFocus(action);
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
                GetWindowRect(action,&focused);MapWindowPoints(nullptr,state.pages[page_index],reinterpret_cast<POINT*>(&focused),2);GetClientRect(state.pages[page_index],&viewport);
                require(GetFocus()==action && focused.top>=0 && focused.bottom<=viewport.bottom,"message administration bottom action scrolls into minimum viewport in both themes");
                require(GetNextDlgTabItem(state.pages[page_index],action,TRUE)!=nullptr,"message administration action retains native keyboard traversal");
            }
        }
        show_page(state,17);
        const auto menu_page=state.pages[17];
        RECT hit_bounds{};GetWindowRect(menu_page,&hit_bounds);
        require(SendMessageW(menu_page,WM_NCHITTEST,0,MAKELPARAM(hit_bounds.left+8,hit_bounds.top+8))==HTCLIENT,
            "page accepts mouse input instead of STATIC transparent hit testing");
        SCROLLBARINFO native_bar{sizeof(native_bar)};
        require(GetScrollBarInfo(menu_page,OBJID_VSCROLL,&native_bar) &&
            SendMessageW(menu_page,WM_NCHITTEST,0,MAKELPARAM(native_bar.rcScrollBar.left+2,native_bar.rcScrollBar.bottom-4))==HTVSCROLL,
            "visible scrollbar is a native mouse target, not hit-through");
        const auto relative=[&](HWND control){RECT r{};GetWindowRect(control,&r);MapWindowPoints(nullptr,menu_page,reinterpret_cast<POINT*>(&r),2);return r;};
        SendMessageW(menu_page,WM_VSCROLL,SB_TOP,0);
        const auto preview_top=relative(state.menu_preview);
        const auto select_top=relative(state.menu_select);
        SendMessageW(menu_page,WM_VSCROLL,SB_BOTTOM,0);
        const auto offset=GetScrollPos(menu_page,SB_VERT);
        require(offset>0,"Displays page has real overflow at minimum viewport");
        require(relative(state.menu_preview).top==preview_top.top-offset && relative(state.menu_select).top==select_top.top-offset,
            "scrollbar moves both partly clipped preview and normal controls");
        RECT menu_view{};GetClientRect(menu_page,&menu_view);
        require(relative(state.menu_preview).bottom<=menu_view.bottom,"scrollbar exposes entire bottom of preview");
        SendMessageW(menu_page,WM_VSCROLL,SB_TOP,0);
        require(relative(state.menu_preview).top==preview_top.top,"scrollbar restores design position without drift");
        SendMessageW(menu_page,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA)),0);
        require(relative(state.menu_preview).top==preview_top.top-GetScrollPos(menu_page,SB_VERT),"mouse wheel moves clipped preview too");
        if(argc==4) {
            SetWindowPos(window,nullptr,25,25,MulDiv(980,state.dpi,96),MulDiv(620,state.dpi,96),SWP_NOZORDER);
            RECT size{};GetClientRect(window,&size);layout(state,size.right,size.bottom);
            show_page(state,17);SendMessageW(menu_page,WM_VSCROLL,SB_TOP,0);
            ShowWindow(window,SW_SHOW);UpdateWindow(window);
            std::cout<<"INTERACTIVE_SCROLL_READY dpi="<<state.dpi<<std::endl;
            SetTimer(window,900,100,nullptr);
            int last=-1;MSG message{};
            while(IsWindow(window) && GetMessageW(&message,nullptr,0,0)>0) {
                TranslateMessage(&message);DispatchMessageW(&message);
                const auto pos=GetScrollPos(menu_page,SB_VERT);
                if(IsWindow(menu_page) && pos!=last) {
                    const auto bounds=relative(state.menu_preview);
                    SCROLLBARINFO bar{sizeof(bar)};GetScrollBarInfo(menu_page,OBJID_VSCROLL,&bar);
                    RECT outer{};GetWindowRect(window,&outer);
                    std::cout<<"SCROLL pos="<<pos<<" preview_top="<<bounds.top<<" preview_bottom="<<bounds.bottom
                        <<" bar_x="<<bar.rcScrollBar.left-outer.left<<" bar_top="<<bar.rcScrollBar.top-outer.top
                        <<" bar_bottom="<<bar.rcScrollBar.bottom-outer.top<<" thumb_top="<<bar.xyThumbTop
                        <<" thumb_bottom="<<bar.xyThumbBottom<<std::endl;last=pos;
                }
            }
        } else DestroyWindow(window);
        require(state.appearance.save(original),"restore prior theme");
        std::ofstream report(output/L"RESULT.txt"); report<<"PASS checks="<<checks<<" dpi="<<state.dpi<<" failed=0 skipped=0\nSynthetic settings; actual production controls and painters.\n";
        std::cout<<"CONFIG THEME PASS checks="<<checks<<" dpi="<<state.dpi<<'\n'; return 0;
    } catch(const std::exception& error) {
        (void)state.appearance.save(original); std::cerr<<"CONFIG THEME FAIL "<<error.what()<<'\n'; return 1;
    }
}
