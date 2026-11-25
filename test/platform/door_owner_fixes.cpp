// Native complete workflow qualification. Synthetic model only; no BBS/network.
#define main retained_command_early_main
#define render retained_early_render
#define select_list retained_fixture_list
#define category retained_fixture_category
#define open_entry retained_fixture_open
#include "door_command_line_early.cpp"
#undef select_list
#undef category
#undef open_entry
#undef render
#undef main
#include <new>
#include "native_png_capture.hpp"
#include "door_owner_native_checks.inc"
namespace {
struct OwnedThemeKey {
    std::wstring path;bool created{};
    ~OwnedThemeKey(){if(created)RegDeleteKeyW(HKEY_CURRENT_USER,path.c_str());}
};
LRESULT CALLBACK final_notifications(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
    if(m==WM_NCDESTROY)RemoveWindowSubclass(h,final_notifications,id);
    if(m==WM_NOTIFY){auto& s=*reinterpret_cast<State*>(data);const auto* header=reinterpret_cast<NMHDR*>(l);
        if(const auto result=door_navigation_notify(s,header))return *result;
        if(header&&header->hwndFrom==s.navigation&&!s.navigating){
            if(header->code==TVN_SELCHANGINGW)return resolve_dirty(s)?FALSE:TRUE;
            if(header->code==TVN_SELCHANGEDW){door_navigation_open(s,reinterpret_cast<NMTREEVIEWW*>(l)->itemNew.hItem);return 0;}
        }
        if(const auto result=door_configuration_notify(s,header))return *result;}
    if(m==WM_COMMAND&&door_configuration_command(*reinterpret_cast<State*>(data),LOWORD(w),HIWORD(w)))return 0;
    return DefSubclassProc(h,m,w,l);
}
void choose_combo(HWND h,unsigned value){for(LRESULT n=0;n<SendMessageW(h,CB_GETCOUNT,0,0);++n)if(SendMessageW(h,CB_GETITEMDATA,n,0)==static_cast<LRESULT>(value)){SendMessageW(h,CB_SETCURSEL,n,0);return;}throw std::runtime_error("named menu choice absent");}
void pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
void visible_focus(State& s,HWND h){SetFocus(h);pump();RECT r{},v{};GetWindowRect(h,&r);MapWindowPoints(HWND_DESKTOP,s.pages[26],reinterpret_cast<POINT*>(&r),2);GetClientRect(s.pages[26],&v);check(IsWindowVisible(h)&&IsWindowEnabled(h)&&GetFocus()==h&&r.top>=0&&r.bottom<=v.bottom,"keyboard focus scrolled into page viewport");}
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh final evidence directory");const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"retain earlier final evidence");std::filesystem::create_directories(output);parser_checks();unsigned captures{};
    for(const auto dpi:{96,144,192})for(unsigned theme=0;theme<2;++theme){
        State s;s.dpi=dpi;s.instance=GetModuleHandleW(nullptr);s.root=L"C:\\sfxtst\\synthetic-final-ui-not-installed";
        // Dedicated test preference only; never change the owner's theme key.
        const auto key=std::wstring(L"Software\\Spitfire-X BBS\\Tests\\DoorUiFinal\\")+output.parent_path().filename().wstring()+L"-"+std::to_wstring(dpi)+L"-"+std::to_wstring(theme);
        OwnedThemeKey owned_key{key};
        HKEY handle{};DWORD disposition{};const auto key_result=RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&handle,&disposition);
        if(key_result!=ERROR_SUCCESS)throw std::runtime_error("private theme fixture Win32 error "+std::to_string(key_result));owned_key.created=disposition==REG_CREATED_NEW_KEY;check(owned_key.created,"fresh private theme fixture");
        const DWORD preference=theme;check(RegSetValueExW(handle,L"Theme",0,REG_DWORD,reinterpret_cast<const BYTE*>(&preference),sizeof(preference))==ERROR_SUCCESS,"private theme value");RegCloseKey(handle);
        s.appearance.~DesktopAppearance();new(&s.appearance)win32::DesktopAppearance(key);check(static_cast<unsigned>(s.appearance.theme())==theme,"isolated native theme loaded");
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"native classes");const int width=MulDiv(1050,dpi,96),height=MulDiv(760,dpi,96);
        s.window=CreateWindowExW(0,L"STATIC",L"Synthetic complete Door workflow",WS_POPUP,-30000,-30000,width,height,nullptr,nullptr,s.instance,nullptr);
        check(s.window&&SetWindowSubclass(s.window,synthetic_root,91,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s),"native final controls");requested_fonts(s);
        check(SetWindowSubclass(s.window,final_notifications,92,reinterpret_cast<DWORD_PTR>(&s))!=FALSE,"actual control notifications");
        door_catalog_fixture=snapshot;door_change_fixture=change;seed();show_page(s,26);refresh_door_configuration_page(s);remember_page(s);s.appearance.apply_controls(s.window);ShowWindow(s.window,SW_SHOWNOACTIVATE);UpdateWindow(s.window);auto& d=s.doors;
        const auto capture=[&](const char* name,bool bottom=false,bool all=false,int offset=0){if(!all&&(dpi!=96||theme!=1))return;pump();layout(s,width,height);SendMessageW(s.pages[26],WM_VSCROLL,bottom?SB_BOTTOM:SB_TOP,0);if(offset){SCROLLINFO info{sizeof(info),SIF_POS};info.nPos=MulDiv(offset,dpi,96);SetScrollInfo(s.pages[26],SB_VERT,&info,TRUE);layout(s,width,height);}render_png(s,output/(std::to_string(dpi)+"-"+(theme?"dark-":"classic-")+name+".png"));++captures;};
        check(text(s.status).find(L"loaded")!=std::wstring::npos,"initial list reports loaded state");
        {
            const auto root=s.route_items[26];wchar_t title[64]{};TVITEMW item{};item.hItem=root;item.mask=TVIF_TEXT;item.pszText=title;item.cchTextMax=64;
            check(TreeView_GetItem(s.navigation,&item)&&std::wstring(title)==L"Door Menu","left route is named Door Menu");
            HTREEITEM category{};for(const auto& node:d.navigation_items)if(node.entry.kind==app::DoorEntryKind::submenu&&node.entry.target==2)category=node.handle;
            check(category&&TreeView_SelectItem(s.navigation,category)&&d.view==DoorPage::entries&&d.menu==2,"left category click opens right contents");
            owner_toggle(s.navigation,category);HTREEITEM game{},nested{};
            for(const auto& node:d.navigation_items)if(TreeView_GetParent(s.navigation,node.handle)==category){if(node.entry.kind==app::DoorEntryKind::door)game=node.handle;else nested=node.handle;}
            check(game&&nested,"left plus expands both Doors and nested categories");
            check(TreeView_SelectItem(s.navigation,game)&&d.view==DoorPage::entries&&d.door==17,"left Door click selects the same browser actions as the right list");press(s,IDC_DOOR_EDIT);check(d.editing()&&d.door==17,"explicit Edit Door opens shared settings");capture("28-followup-left-door",false,true);
            d.pending=app::DoorConfigurationChange{};check(!TreeView_SelectItem(s.navigation,root)&&d.editing()&&TreeView_GetSelection(s.navigation)==game,"pending save blocks left navigation");d.pending.reset();
            check(TreeView_SelectItem(s.navigation,nested)&&d.menu==9&&d.view==DoorPage::entries,"nested left category is directly reachable");capture("29-followup-left-nested",false,true);
            check(TreeView_SelectItem(s.navigation,category)&&d.menu==2,"left ancestor remains usable several levels deep");
            check(text(GetDlgItem(s.pages[26],IDC_DOOR_BACK))==L"Back","existing return action is labelled Back");press(s,IDC_DOOR_BACK);check(d.view==DoorPage::categories,"renamed Back keeps working root overview action");
            owner_toggle(s.navigation,category);check(!TreeView_GetChild(s.navigation,category),"left minus releases hidden branch metadata");owner_toggle(s.navigation,category);check(TreeView_GetChild(s.navigation,category)!=nullptr,"left plus repopulates after a native collapse");
            TreeView_SelectItem(s.navigation,root);check(d.view==DoorPage::categories,"Door Menu left root always restores overview");capture("30-followup-left-overview",false,true);
        }
        capture("01-categories",false,true);select_list(d.categories,2);SendMessageW(d.categories,WM_KEYDOWN,VK_RETURN,0);check(d.view==DoorPage::entries&&d.menu==2,"Enter opens named category");capture("02-door-list");
        select_list(d.entries,42);SendMessageW(d.entries,WM_KEYDOWN,VK_RETURN,0);check(d.editing()&&d.door==17,"Enter opens named Door");
        check(GetDlgItem(s.pages[26],IDC_DOOR_REMOTE)==nullptr,"no alternate RLogin setup button");const auto original=app::find_door(catalog,17)->rlogin;
        door_set(d.name,"Edited Space Game");check(page_dirty(s)&&text(s.status).find(L"not saved")!=std::wstring::npos,"real edit notification clears previous saved status");capture("03-edit-unsaved",false,true);
        {HWND next=d.enabled;unsigned steps{};do{next=GetNextDlgTabItem(s.pages[26],next,FALSE);check(next&&IsWindowVisible(next)&&IsWindowEnabled(next),"keyboard visits visible enabled controls");}while(next!=d.command_line&&++steps<12);check(next==d.command_line,"tabbed editor keyboard reaches normal command");}check(!IsWindowEnabled(d.pepper),"plain mode cannot edit hashing pepper");press(s,IDC_DOOR_ADVANCED);visible_focus(s,IsWindowEnabled(d.auth)?d.auth:d.secret_clear);check(GetScrollPos(s.pages[26],SB_VERT)>0,"keyboard reaches advanced fields");
        check(save_door_configuration_page(s)&&same_target(original,app::find_door(catalog,17)->rlogin),"native rename preserves existing options");capture("04-saved-list");
        press(s,IDC_DOOR_NEW);check(door_checked(d.enabled)&&text(d.level)==L"10"&&text(d.execution_level)==L"10"&&!door_checked(d.sysop)&&!door_checked(d.execution_sysop),"Add Door defaults enabled minimum10 access/execution without Sysop Only");door_set(d.name,"New Placed Door");door_set(d.command_line,"rlogin example.invalid:5130 -q -t xtrn=NEW");const auto added=d.door;const auto add_number=d.entry;capture("20-add-door");
        check(save_door_configuration_page(s)&&app::find_door(catalog,added)->enabled&&catalog.doors.size()==3,"normal Add Door saves enabled definition and placement");capture("21-added-door");
        select_list(d.entries,add_number);press(s,IDC_DOOR_ENTRY_REMOVE);press(s,IDC_DOOR_CONFIRM);press(s,IDC_DOOR_ALL);select_list(d.definitions,added);press(s,IDC_DOOR_REMOVE);press(s,IDC_DOOR_CONFIRM);
        press(s,IDC_DOOR_NEW);check(!d.edit_placement,"Add from configured Doors starts unplaced");door_set(d.name,"New Unplaced Door");door_set(d.command_line,"test.exe \"%D\\DOORDROP.INI\"");door_set(d.startup,"C:\\Games");const auto unplaced=d.door;const auto entry_count=catalog.entries.size();
        SendMessageW(d.program_type,CB_SETCURSEL,static_cast<unsigned>(app::DoorProgramType::native),0);
        door_configuration_command(s,IDC_DOOR_PROGRAM_TYPE,CBN_SELCHANGE);
        choose_combo(d.drop_file,14);
        door_configuration_command(s,IDC_DOOR_DROP_FILE,CBN_SELCHANGE);
        check(IsWindowVisible(d.drop_encoding)&&SendMessageW(d.drop_encoding,CB_GETCURSEL,0,0)==0,"DOORDROP.INI shows CP437 default");
        SendMessageW(d.drop_encoding,CB_SETCURSEL,1,0);
        SendMessageW(d.drop_location,CB_SETCURSEL,1,0);door_check(d.multiple,true);
        const auto before_ini_save=changes;
        check(!save_door_configuration_page(s)&&changes==before_ini_save,"shared Start-up directory validation retains unsaved selections");
        SendMessageW(d.drop_location,CB_SETCURSEL,2,0);
        check(save_door_configuration_page(s)&&catalog.entries.size()==entry_count&&app::find_door(catalog,unplaced),"corrected DOORDROP.INI settings save without creating a placement");
        select_list(d.definitions,unplaced);press(s,IDC_DOOR_EDIT);
        check(door_drop_value(d)==14&&SendMessageW(d.drop_encoding,CB_GETCURSEL,0,0)==1,"reopen keeps saved drop file and UTF-8");
        capture("31-doordrop-ini",false,false,360);
        press(s,IDC_DOOR_CANCEL);select_list(d.definitions,unplaced);press(s,IDC_DOOR_REMOVE);press(s,IDC_DOOR_CONFIRM);category(s,2);
        open_entry(s,42);door_set(d.command_line,"rlogin example.invalid -m expression");const auto no_write=changes;check(!save_door_configuration_page(s)&&changes==no_write&&GetFocus()==d.command_line,"invalid command no write and focus");capture("05-validation");
        discard_page(s);remember_page(s);door_set(d.name,"Edit after validation");check(!GetWindowTextLengthW(d.error)&&text(s.status).find(L"not saved")!=std::wstring::npos,"normal edit removes stale error status");discard_page(s);remember_page(s);
        door_set(d.command_line,"rlogin example.invalid -h -q -t xtrn=GAME");door_configuration_command(s,IDC_DOOR_COMMAND_LINE,EN_KILLFOCUS);door_set(d.salt,"SyntheticSalt");door_set(d.pepper,"SyntheticPepperOnly");
        check(SendMessageW(d.pepper,EM_GETPASSWORDCHAR,0,0)!=0&&save_door_configuration_page(s),"masked caller-hash pepper save");const auto hashed=app::find_door(catalog,17)->rlogin;check(hashed.prompt_credential&&!hashed.protected_pepper.empty()&&hashed.protected_credential.empty(),"caller hash keeps protected pepper, no stored caller password");
        open_entry(s,42);if(!d.advanced)press(s,IDC_DOOR_ADVANCED);visible_focus(s,d.pepper);capture("06-advanced-settings",true,true);door_set(d.name,"Protected options retained");check(save_door_configuration_page(s)&&same_target(hashed,app::find_door(catalog,17)->rlogin),"blank native pepper preserves exact blob");
        open_entry(s,42);door_check(d.pepper_clear,true);check(save_door_configuration_page(s)&&app::find_door(catalog,17)->rlogin.protected_pepper.empty(),"explicit pepper removal");
        open_entry(s,42);door_set(d.command_line,"rlogin example.invalid -q -t xtrn=GAME");SendMessageW(d.auth,CB_SETCURSEL,2,0);door_check(d.prompt,false);door_access_controls(s);door_set(d.secret,"SyntheticRemoteOnly");check(save_door_configuration_page(s),"predefined protected remote password");const auto protected_target=app::find_door(catalog,17)->rlogin;
        open_entry(s,42);door_set(d.name,"Keep protected password");check(save_door_configuration_page(s)&&same_target(protected_target,app::find_door(catalog,17)->rlogin),"blank password preserves all fields");
        // A fully acknowledged first save followed by refused placement must
        // retry an edit as an edit, not as a colliding new entry.
        open_entry(s,42);door_set(d.name,"Partial save retry");door_set(d.entry_title,"Retry entry");reject_at=changes+2;check(!save_door_configuration_page(s)&&!d.pending&&!d.place_new,"partial existing-entry refusal retains edit identity");capture("07-partial-save");check(save_door_configuration_page(s),"existing placement retry succeeds");
        open_entry(s,42);door_set(d.name,"Unknown outcome");lose_at=changes+2;check(!save_door_configuration_page(s)&&d.pending&&!IsWindowEnabled(d.command_line),"lost second reply freezes controls");const auto committed=changes;capture("08-save-recovery",false,true);press(s,IDC_DOOR_RECONCILE);check(!d.pending&&changes==committed,"same receipt replay does not duplicate second save");
        open_entry(s,42);door_set(d.name,"Read recovery");fail_read=true;check(!save_door_configuration_page(s)&&d.pending&&text(s.status).find(L"accepted")!=std::wstring::npos,"accepted save/read failure is distinguished");const auto accepted=changes;press(s,IDC_DOOR_RECONCILE);check(!d.pending&&changes==accepted+1,"read recovery replays before pending placement");
        open_entry(s,42);door_set(d.name,"My stale edit");catalog.doors.front().name="Concurrent owner edit";++catalog.revision;const auto before_conflict=changes;
        check(!save_door_configuration_page(s)&&d.conflict&&!d.pending&&changes==before_conflict&&text(d.name)==L"My stale edit","conflict retains draft without overwriting concurrent edit");check(!save_door_configuration_page(s)&&changes==before_conflict,"conflict requires explicit reload");capture("09-conflict");press(s,IDC_DOOR_RELOAD_DRAFT);check(!d.conflict&&text(d.name)==L"Concurrent owner edit"&&!page_dirty(s),"explicit reload discards stale draft and loads authoritative settings");press(s,IDC_DOOR_CANCEL);
        // Number edits apply to the selected placement only.
        select_list(d.entries,42);press(s,IDC_DOOR_PLACEMENT_EDIT);door_set_number(d.placement_number,37);door_set(d.placement_title,"Play Here");capture("10-menu-entry");check(save_door_configuration_page(s)&&std::none_of(catalog.entries.begin(),catalog.entries.end(),[](const auto& e){return e.menu==2&&e.number==42;}),"number move preserves target and other gaps");
        select_list(d.entries,37);press(s,IDC_DOOR_SHOW_ANOTHER);choose_combo(d.destination,8);door_configuration_command(s,IDC_DOOR_DESTINATION,CBN_SELCHANGE);door_set_number(d.destination_number,6);check(save_door_configuration_page(s)&&catalog.doors.size()==2,"named destination shares Door");
        select_list(d.entries,37);press(s,IDC_DOOR_ENTRY_REMOVE);check(d.view==DoorPage::confirm&&GetFocus()==d.confirm_cancel,"remove entry requires confirmation with Cancel focus");capture("11-remove-entry",false,true);const auto kept=changes;press(s,IDC_DOOR_CANCEL);check(changes==kept,"confirmation Cancel has no write");select_list(d.entries,37);press(s,IDC_DOOR_ENTRY_REMOVE);press(s,IDC_DOOR_CONFIRM);check(app::find_door(catalog,17)&&std::any_of(catalog.entries.begin(),catalog.entries.end(),[](const auto& e){return e.menu==8&&e.number==6&&e.target==17;}),"remove only selected placement");
        press(s,IDC_DOOR_ALL);select_list(d.definitions,17);press(s,IDC_DOOR_REMOVE);check(d.view==DoorPage::definitions&&GetWindowTextLengthW(d.error)>0,"placed Door cannot be deleted");
        select_list(d.definitions,17);press(s,IDC_DOOR_DUPLICATE);door_set(d.duplicate_name,"Separate Door Copy");const auto copy_id=*app::next_unused_door_id(catalog);capture("12-duplicate-door");check(save_door_configuration_page(s),"create independent Door copy");const auto* copied=app::find_door(catalog,copy_id);check(copied&&!copied->enabled&&same_target(copied->rlogin,app::find_door(catalog,17)->rlogin)&&std::none_of(catalog.entries.begin(),catalog.entries.end(),[&](const auto& e){return e.kind==app::DoorEntryKind::door&&e.target==copy_id;}),"copy disabled, unplaced and protected options retained");
        capture("13-all-configured",false,true);select_list(d.definitions,copy_id);press(s,IDC_DOOR_EDIT);check(!d.edit_placement&&!IsWindowVisible(d.entry_number),"unplaced editing hides unrelated menu fields");door_set(d.name,"Independent Name");check(save_door_configuration_page(s)&&app::find_door(catalog,17)->name!="Independent Name","copy edits do not change source");select_list(d.definitions,copy_id);press(s,IDC_DOOR_REMOVE);capture("14-delete-door");press(s,IDC_DOOR_CONFIRM);check(!app::find_door(catalog,copy_id),"confirmed unused definition deletion");
        // Category creation uses named placement; duplicates share their Doors.
        press(s,IDC_DOOR_BACK);press(s,IDC_DOOR_MENU_NEW);door_set(d.category_name,"Adventure Games");choose_combo(d.category_parent,0);door_set_number(d.category_number,12);const auto new_menu=d.category_id;capture("15-add-category",false,true);check(save_door_configuration_page(s)&&app::find_door_menu(catalog,new_menu)&&std::any_of(catalog.entries.begin(),catalog.entries.end(),[&](const auto& e){return e.menu==0&&e.number==12&&e.target==new_menu;}),"new category saved and placed");
        select_list(d.categories,new_menu);press(s,IDC_DOOR_CATEGORY_EDIT);check(!IsWindowVisible(d.category_parent),"editing category does not silently replace a placement");door_set(d.category_name,"Adventure Collection");check(save_door_configuration_page(s)&&app::find_door_menu(catalog,new_menu)->name=="Adventure Collection","category rename uses stable identity");
        select_list(d.categories,2);press(s,IDC_DOOR_MENU_DUPLICATE);door_set(d.duplicate_name,"Strategy Copy");const auto copy_menu=*app::next_unused_door_menu_id(catalog);capture("16-duplicate-category");check(save_door_configuration_page(s)&&app::find_door_menu(catalog,copy_menu)&&catalog.doors.size()==2,"category copy adds no Door definitions");
        select_list(d.categories,copy_menu);press(s,IDC_DOOR_CATEGORY_PLACE);choose_combo(d.destination,copy_menu);door_set_number(d.destination_number,8);const auto before_cycle=changes;check(!save_door_configuration_page(s)&&changes==before_cycle,"self-cycle rejected before write");discard_page(s);remember_page(s);press(s,IDC_DOOR_CANCEL);
        select_list(d.categories,0);press(s,IDC_DOOR_MENU_REMOVE);check(d.view==DoorPage::categories&&GetWindowTextLengthW(d.error)>0,"root delete refused");
        select_list(d.categories,new_menu);press(s,IDC_DOOR_MENU_REMOVE);check(d.view==DoorPage::confirm,"placed category offers direct confirmed deletion");press(s,IDC_DOOR_CANCEL);category(s,0);select_list(d.entries,12);press(s,IDC_DOOR_ENTRY_REMOVE);press(s,IDC_DOOR_CONFIRM);press(s,IDC_DOOR_BACK);select_list(d.categories,new_menu);press(s,IDC_DOOR_MENU_REMOVE);capture("17-delete-category");press(s,IDC_DOOR_CONFIRM);check(!app::find_door_menu(catalog,new_menu),"empty unplaced category can be deleted");
        press(s,IDC_DOOR_MENU_NEW);door_set(d.category_name,"Partially saved category");reject_at=changes+2;check(!save_door_configuration_page(s)&&!d.category_new&&d.category_place_new,"partial category save keeps pending placement");check(save_door_configuration_page(s),"category partial save retry completes");
        category(s,8);press(s,IDC_DOOR_ADD_EXISTING);select_list(d.pick,43);door_configuration_command(s,IDC_DOOR_PICK,LBN_SELCHANGE);door_set_number(d.pick_number,6);const auto occupied=changes;check(!save_door_configuration_page(s)&&changes==occupied,"occupied menu number refused without writes");door_set_number(d.pick_number,11);check(save_door_configuration_page(s)&&catalog.doors.size()==2,"named existing Door adds shared placement");
        check(app::validate_door_catalog(catalog).valid(),"final catalog remains valid");open_entry(s,6);door_set(d.command_line,"rlogin example.invalid -p -P -q -t xtrn=GAME");door_configuration_command(s,IDC_DOOR_COMMAND_LINE,EN_KILLFOCUS);capture("18-password-prompt",false,false,450);check(text(s.status).find(L"not saved")!=std::wstring::npos&&!GetWindowTextLengthW(d.error),"standalone p capture has clean unsaved status");check(save_door_configuration_page(s),"p and P final native save");
        open_entry(s,6);SetWindowPos(s.window,nullptr,-30000,-30000,MulDiv(780,dpi,96),MulDiv(610,dpi,96),SWP_NOACTIVATE|SWP_NOZORDER);layout(s,MulDiv(780,dpi,96),MulDiv(610,dpi,96));SendMessageW(s.pages[26],WM_VSCROLL,SB_TOP,0);render_png(s,output/(std::to_string(dpi)+"-"+(theme?"dark-":"classic-")+"19-compact.png"));++captures;
        SetWindowPos(s.window,nullptr,-30000,-30000,width,height,SWP_NOACTIVATE|SWP_NOZORDER);layout(s,width,height);owner_ui_checks(s,capture);visible_focus(s,IsWindowEnabled(d.auth)?d.auth:d.secret_clear);check(!d.pending,"no unresolved operation at completion");DestroyWindow(s.window);DeleteObject(s.heading_font);DeleteObject(s.ui_font);check(RegDeleteKeyW(HKEY_CURRENT_USER,key.c_str())==ERROR_SUCCESS,"release only owned private theme fixture");owned_key.created=false;
    }
    door_catalog_fixture=nullptr;door_change_fixture=nullptr;std::cout<<"PASS "<<checks<<" final native/parser assertions; "<<captures<<" native renders across 96/144/192 DPI and both themes. Synthetic model; no live or physical owner acceptance.\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
