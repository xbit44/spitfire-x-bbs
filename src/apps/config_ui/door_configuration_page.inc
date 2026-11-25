// Task-oriented native Door editor. Storage identities stay internal; all
// writes still use protected owner RPC/offline administration.
void refresh_door_configuration_page(State&);
void show_door_bulletins(State&);
void layout(State&,int,int);
void door_fill_tree(State&);
void door_navigation_refresh(State&);
void door_navigation_sync(State&);
void activate_page(State&,int);
void door_filter_definitions(State&);
std::optional<DoorEditorState::TreeItem> door_tree_item(State&,HTREEITEM=nullptr);
bool door_tree_command(State&,UINT);
void door_drag_begin(State&,HWND,const DoorEditorState::TreeItem&);
std::optional<LRESULT> door_drag_message(State&,HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK door_surface_keys(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
void door_update_delete_actions(State&);
std::string door_text(HWND);
bool door_rlogin_options(const DoorEditorState& d){
    const auto command=door_text(d.command_line);const auto begin=command.find_first_not_of(" \t\r\n");
    return begin!=std::string::npos&&command.compare(begin,6,"rlogin")==0&&(begin+6==command.size()||command[begin+6]==' '||command[begin+6]=='\t');
}
bool door_telnet_options(const DoorEditorState& d){
    const auto command=door_text(d.command_line);const auto begin=command.find_first_not_of(" \t\r\n");
    return begin!=std::string::npos&&command.compare(begin,6,"telnet")==0&&(begin+6==command.size()||command[begin+6]==' '||command[begin+6]=='\t');
}
bool door_remote_options(const DoorEditorState& d){return door_rlogin_options(d)||door_telnet_options(d);}
LRESULT CALLBACK door_tree_keys(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
#ifdef SFX_ALPHA_TEST_AUTOMATION
std::optional<app::DoorCatalog> (*door_catalog_fixture)()=nullptr;
app::LocalDoorResult (*door_change_fixture)(const app::DoorConfigurationChange&)=nullptr;
#endif
bool door_number(HWND control,unsigned& value,unsigned maximum){
    const auto s=text(control);if(s.empty())return false;value=0;
    for(const auto c:s){if(c<L'0'||c>L'9'||value>(maximum-static_cast<unsigned>(c-L'0'))/10)return false;value=value*10+static_cast<unsigned>(c-L'0');}
    return value<=maximum;
}
std::string door_text(HWND control){const auto value=text(control);return value.empty()?std::string{}:wide_to_utf8(value).value_or(std::string{});}
void door_set(HWND control,std::string_view value){SetWindowTextW(control,utf8_to_wide(value).c_str());}
void door_set_number(HWND control,unsigned value){SetWindowTextW(control,std::to_wstring(value).c_str());}
bool door_checked(HWND control){return Button_GetCheck(control)==BST_CHECKED;}
void door_check(HWND control,bool value){Button_SetCheck(control,value?BST_CHECKED:BST_UNCHECKED);}
// Keep native tab selection/keyboard/accessibility while painting the Dark
// Retro strip with the same palette as the surrounding editor.
LRESULT CALLBACK door_tab_paint(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR context){
    auto& s=*reinterpret_cast<State*>(context);
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,door_tab_paint,id);
    if(s.appearance.theme()==win32::DesktopTheme::dark_retro){
        if(message==WM_ERASEBKGND)return 1;
        if(message==WM_PAINT||message==WM_PRINTCLIENT){
            PAINTSTRUCT paint{};const auto dc=message==WM_PAINT?BeginPaint(window,&paint):reinterpret_cast<HDC>(w);
            RECT bounds{};GetClientRect(window,&bounds);FillRect(dc,&bounds,s.appearance.background_brush());
            const auto old=SelectObject(dc,s.ui_font);SetBkMode(dc,TRANSPARENT);const auto& p=s.appearance.palette();
            for(int n=0;n<TabCtrl_GetItemCount(window);++n){RECT rect{};TabCtrl_GetItemRect(window,n,&rect);const bool selected=n==TabCtrl_GetCurSel(window);
                auto brush=CreateSolidBrush(selected?p.selection:p.surface);FillRect(dc,&rect,brush);DeleteObject(brush);
                brush=CreateSolidBrush(selected?p.gold:p.border);FrameRect(dc,&rect,brush);DeleteObject(brush);
                wchar_t caption[64]{};TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=caption;item.cchTextMax=64;TabCtrl_GetItem(window,n,&item);
                SetTextColor(dc,selected?p.selected_text:p.text);DrawTextW(dc,caption,-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                if(selected&&GetFocus()==window){InflateRect(&rect,-3,-3);DrawFocusRect(dc,&rect);}
            }
            SelectObject(dc,old);if(message==WM_PAINT)EndPaint(window,&paint);return 0;
        }
    }
    return DefSubclassProc(window,message,w,l);
}
void door_io_choices(DoorEditorState& d,app::DoorIo selected){
    const bool native=SendMessageW(d.program_type,CB_GETCURSEL,0,0)==1;
    SendMessageW(d.io,CB_RESETCONTENT,0,0);
    const auto add=[&](const wchar_t* name,app::DoorIo value){const auto row=SendMessageW(d.io,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));SendMessageW(d.io,CB_SETITEMDATA,row,static_cast<unsigned>(value));if(value==selected)SendMessageW(d.io,CB_SETCURSEL,row,0);};
    add(native?L"Standard input/output":L"Standard",app::DoorIo::standard);
    if(native)add(L"Socket",app::DoorIo::socket);
    else{add(L"FOSSIL or UART",app::DoorIo::fossil_or_uart);add(L"UART Only",app::DoorIo::uart_only);add(L"FOSSIL Only",app::DoorIo::fossil_only);}
    if(SendMessageW(d.io,CB_GETCURSEL,0,0)==CB_ERR)SendMessageW(d.io,CB_SETCURSEL,native?0:1,0);
}
void door_drop_choices(DoorEditorState& d,app::DoorDropFile selected){
    SendMessageW(d.drop_file,CB_RESETCONTENT,0,0);
    // Persist the format ID, not its position in this shorter Sysop-facing list.
    const auto add=[&](const wchar_t* name,app::DoorDropFile value){const auto row=SendMessageW(d.drop_file,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));SendMessageW(d.drop_file,CB_SETITEMDATA,row,static_cast<unsigned>(value));if(value==selected)SendMessageW(d.drop_file,CB_SETCURSEL,row,0);};
    add(L"None",app::DoorDropFile::none);
    add(L"XTRN.DAT",app::DoorDropFile::xtrn_dat);
    add(L"CHAIN.TXT",app::DoorDropFile::chain_txt);
    add(L"DOOR.SYS (52 lines)",app::DoorDropFile::door_sys52);
    add(L"DORINFO#.DEF",app::DoorDropFile::dorinfo_node);
    add(L"SFDOORS.DAT",app::DoorDropFile::sfdoors_dat);
    add(L"DOOR.SYS (31 lines)",app::DoorDropFile::door_sys31);
    add(L"DOORFILE.SR",app::DoorDropFile::doorfile_sr);
    add(L"DORINFO1.DEF",app::DoorDropFile::dorinfo1_def);
    add(L"DOOR32.SYS",app::DoorDropFile::door32_sys);
    add(L"BBSDEV.DRP",app::DoorDropFile::bbsdev_drp);
    add(L"DOORDROP.INI",app::DoorDropFile::doordrop_ini);
}
LRESULT door_drop_value(const DoorEditorState& d) {
    const auto row=SendMessageW(d.drop_file,CB_GETCURSEL,0,0);
    return row==CB_ERR?CB_ERR:SendMessageW(d.drop_file,CB_GETITEMDATA,row,0);
}
void door_drop_encoding_visibility(const DoorEditorState& d) {
    const bool visible=d.editing() && d.selected_tab==0 && !door_remote_options(d) &&
        door_drop_value(d)==static_cast<unsigned>(app::DoorDropFile::doordrop_ini);
    ShowWindow(d.drop_encoding_label,visible?SW_SHOW:SW_HIDE);
    ShowWindow(d.drop_encoding,visible?SW_SHOW:SW_HIDE);
}
std::optional<app::DoorCatalog> load_door_catalog(State& s){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(door_catalog_fixture)return door_catalog_fixture();
#endif
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline&&!s.navigation_rendering){const auto r=win32::offline_door_configuration(s.root,s.owner);return r&&r.value?r.value:std::nullopt;}
    if(s.mode!=State::ConnectionMode::online&&!s.navigation_rendering)return {};
    app::LocalDoorQuery query;std::vector<std::uint8_t> bytes;std::uint32_t total{};
    for(unsigned page_index=0;page_index<840;++page_index){
        app::LocalControlFrame response;
        if(!online_exchange(s,request(s,app::LocalControlMessageType::door_configuration_query_request,app::encode_door_configuration_query(query)),app::LocalControlMessageType::door_configuration_query_response,response))return {};
        const auto page=app::decode_door_configuration_snapshot(response.payload);
        if(!page||page->code!=app::LocalAccountResultCode::ok||page->offset!=bytes.size()||
            (query.revision&&query.revision!=page->revision)||(total&&total!=page->total))return {};
        if(!total){total=page->total;bytes.reserve(total);}bytes.insert(bytes.end(),page->bytes.begin(),page->bytes.end());
        if(bytes.size()==total)return app::decode_door_catalog(bytes);
        query={page->revision,static_cast<std::uint32_t>(bytes.size())};
    }
    return {};
}
app::LocalDoorResult send_door_change(State& s,const app::DoorConfigurationChange& c){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(door_change_fixture)return door_change_fixture(c);
#endif
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline)return win32::offline_change_door_configuration(s.root,s.owner,c);
    if(s.mode!=State::ConnectionMode::online)return {};app::LocalControlFrame response;const auto payload=app::encode_door_configuration_change(c);
    if(payload.empty())return {app::LocalAccountResultCode::invalid_request};
    if(!online_exchange(s,request(s,app::LocalControlMessageType::door_configuration_change_request,payload),app::LocalControlMessageType::door_configuration_change_response,response))return {};
    return app::decode_door_configuration_result(response.payload).value_or(app::LocalDoorResult{});
}
std::wstring door_menu_name(const DoorEditorState& d,app::DoorMenuId id){
    const auto* m=d.catalog?app::find_door_menu(*d.catalog,id):nullptr;
    return id==0?L"Door Menu":m?utf8_to_wide(m->name):L"Category unavailable";
}
unsigned door_free_number(const app::DoorCatalog& c,app::DoorMenuId menu){
    for(unsigned n=1;n<=99;++n)if(std::none_of(c.entries.begin(),c.entries.end(),[&](const auto& e){return e.menu==menu&&e.number==n;}))return n;
    return 0;
}
const app::DoorMenuEntry* door_current_entry(const DoorEditorState& d){
    if(d.catalog)for(const auto& e:d.catalog->entries)if(e.menu==d.menu&&e.number==d.entry)return &e;
    return nullptr;
}
#include "door_actions.inc"
void door_access_controls(State& s){
    auto& d=s.doors;const bool numeric=!door_checked(d.sysop);
    EnableWindow(d.security_mode,numeric);EnableWindow(d.level,numeric);
    EnableWindow(d.execution_mode,!door_checked(d.execution_sysop));EnableWindow(d.execution_level,!door_checked(d.execution_sysop));
    for(unsigned k:{0U,3U}){const bool allowed=!door_checked(d.category_requirements[k+2]);EnableWindow(d.category_requirements[k],allowed);EnableWindow(d.category_requirements[k+1],allowed);}
    EnableWindow(d.server_name,SendMessageW(d.server_mode,CB_GETCURSEL,0,0)==1);
    bool command_password=false;
    if(d.command_line&&d.catalog){
        app::RloginTarget saved;if(const auto* item=app::find_door(*d.catalog,d.door))saved=item->rlogin;
        const auto parsed=sfx::config_ui::parse_door_command(door_text(d.command_line),saved);
        if(parsed&&parsed.password!=sfx::config_ui::DoorCommandPassword::unchanged){
            command_password=true;SendMessageW(d.auth,CB_SETCURSEL,parsed.password==sfx::config_ui::DoorCommandPassword::plain_prompt?0:1,0);door_check(d.prompt,true);
        }
    }
    EnableWindow(d.auth,!command_password);
    const auto mode=SendMessageW(d.auth,CB_GETCURSEL,0,0);
    EnableWindow(d.pepper,mode==1);EnableWindow(d.pepper_clear,mode==1);EnableWindow(d.salt,mode!=0);
    if(mode==1)door_check(d.prompt,true);
    EnableWindow(d.prompt,mode!=1&&!command_password);EnableWindow(d.secret,mode!=1&&!door_checked(d.prompt));EnableWindow(d.secret_clear,mode!=1);
    for(const auto control:d.local_controls)EnableWindow(control,!d.pending&&!door_remote_options(d));
    door_drop_encoding_visibility(d);
}
void door_show_view(State& s,DoorPage view){
    auto& d=s.doors;d.view=view;
    if(d.editing()&&(!door_remote_options(d)||d.selected_tab!=0))d.advanced=false;
    for(std::size_t n=0;n<d.controls.size();++n)for(const auto h:d.controls[n]){
        ShowWindow(h,n==static_cast<unsigned>(view)?SW_SHOW:SW_HIDE);
        wchar_t cls[32]{};GetClassNameW(h,cls,32);if(_wcsicmp(cls,L"STATIC"))EnableWindow(h,!d.pending);
    }
    const auto context=door_menu_name(d,d.menu);
    std::wstring title=L"Door Menu";
    std::wstring help=L"Select a category or Door to use its actions below. Drag to reorder; right-click for more actions.";
    if(view==DoorPage::entries){title=context+L" - Doors";help=L"Select a category or Door on either side. The buttons below apply to the selected item.";}
    if(d.editing()){
        title=d.new_door?L"Add Door - "+context:L"Edit Door - "+text(d.name);
        help=L"Edit this Door and its Command Line here. Settings are shared; its menu number and name apply here.";
    }
    if(view==DoorPage::existing){title=L"Add Existing Door - "+context;help=L"Choose a Door by name. It keeps the same settings on every menu where you add it.";}
    if(view==DoorPage::destination){title=L"Show on Another Menu";help=L"Choose the destination by name. The Door or category keeps its shared settings.";}
    if(view==DoorPage::definitions){title=L"All Configured Doors";help=L"Edit shared settings, make a separate copy, or delete an unused Door. Doors not on a menu are listed here too.";}
    if(view==DoorPage::category){title=d.category_new?L"Add Category":L"Edit Name / Security Levels";help=L"Edit the category's shared name, Access Requirements and Execution Requirements. Edit Menu Entry changes its number and displayed name on a menu.";}
    if(view==DoorPage::placement){title=L"Edit Menu Entry";help=L"Change the selected entry's number, displayed name or description on its parent menu. Its shared name and security levels stay the same.";}
    if(view==DoorPage::duplicate){title=d.duplicate_category?L"Duplicate Category":L"Duplicate Door";help=d.duplicate_category?L"Creates a separate category with the same entries. Its Doors and nested categories keep shared settings. The copy is not on a menu yet.":L"Creates a separate Door with copied settings and protected credentials. The copy starts disabled and is not on a menu.";}
    if(view==DoorPage::confirm){title=L"Confirm Removal";help=L"Review the item and what will be removed. Cancel keeps it unchanged.";}
    if(d.editing()&&!d.edit_placement)help=L"Edit this Door's shared settings. Use Add Existing Door in a category to show it on a menu.";
    for(unsigned tab=0;tab<d.tab_controls.size();++tab)for(const auto h:d.tab_controls[tab])ShowWindow(h,d.editing()&&d.selected_tab==tab?SW_SHOW:SW_HIDE);
    for(const auto h:d.placement_controls)ShowWindow(h,d.editing()&&d.selected_tab==3&&d.edit_placement?SW_SHOW:SW_HIDE);
    for(const auto h:d.category_placement_controls)ShowWindow(h,view==DoorPage::category&&d.category_place_new?SW_SHOW:SW_HIDE);
    for(const auto h:d.advanced_controls)ShowWindow(h,d.editing()&&d.advanced&&door_rlogin_options(d)?SW_SHOW:SW_HIDE);
    for(const auto h:d.telnet_controls)ShowWindow(h,d.editing()&&d.advanced&&door_telnet_options(d)?SW_SHOW:SW_HIDE);
    const bool connection=door_remote_options(d);
    if(d.advanced_button){ShowWindow(d.advanced_button,d.editing()&&d.selected_tab==0&&connection?SW_SHOW:SW_HIDE);SetWindowTextW(d.advanced_button,door_telnet_options(d)?(d.advanced?L"Hide Telnet options":L"Telnet options..."):(d.advanced?L"Hide RLogin options":L"RLogin options..."));}
    if(d.delete_current)EnableWindow(d.delete_current,!d.new_door&&!d.pending&&!d.conflict);
    if(d.back_settings)SetWindowTextW(d.back_settings,d.return_view==DoorPage::definitions?L"Back to Configured Doors":L"Back to Door List");
    if(view==DoorPage::destination||view==DoorPage::category||view==DoorPage::placement)for(const auto h:d.controls[static_cast<unsigned>(view)])if(GetDlgCtrlID(h)==IDC_DOOR_CANCEL)SetWindowTextW(h,(view==DoorPage::destination?d.back_view:d.return_view)==DoorPage::categories?L"Back to Categories":L"Back to Door List");
    SetWindowTextW(d.heading,title.c_str());SetWindowTextW(d.help,help.c_str());
    if(s.current_page==26)SetWindowTextW(s.heading,L"Doors");
    ShowWindow(d.error,GetWindowTextLengthW(d.error)?SW_SHOW:SW_HIDE);
    ShowWindow(d.recovery,d.pending?SW_SHOW:SW_HIDE);
    ShowWindow(d.reload_draft,d.conflict&&!d.pending?SW_SHOW:SW_HIDE);
    if(d.conflict)for(const auto& group:d.controls)for(const auto h:group){const auto id=GetDlgCtrlID(h);if(id==IDC_DOOR_SAVE||id==IDC_DOOR_CONFIRM||id==IDC_DOOR_PICK_USE||id==IDC_DOOR_DESTINATION_USE)EnableWindow(h,FALSE);}
    if(!d.pending)door_access_controls(s);
    else door_drop_encoding_visibility(d);
    door_update_delete_actions(s);
    if(!s.page_bounds[26].empty()){RECT r{};GetClientRect(s.window,&r);SetScrollPos(s.pages[26],SB_VERT,0,TRUE);layout(s,r.right,r.bottom);}
    door_navigation_sync(s);
}
bool door_problem(State& s,HWND field,const wchar_t* message){
    auto& d=s.doors;
    if(field&&d.editing())for(unsigned tab=0;tab<4;++tab)if(std::find(d.tab_controls[tab].begin(),d.tab_controls[tab].end(),field)!=d.tab_controls[tab].end()){d.selected_tab=tab;TabCtrl_SetCurSel(d.tabs,tab);}
    if(field){for(unsigned n=2;n<=3;++n)if(std::find(d.controls[n].begin(),d.controls[n].end(),field)!=d.controls[n].end())door_show_view(s,static_cast<DoorPage>(n));SetFocus(field);}
    SetWindowTextW(d.error,message);door_show_view(s,d.view);set_status(s,L"Changes are not saved. Correct the setting shown above; your edits are kept.");return false;
}
void door_fill_telnet_helper(DoorEditorState& d){
    const auto parsed=sfx::config_ui::parse_telnet_command(door_text(d.command_line));
    const auto target=parsed?parsed.target:app::TelnetTarget{};
    const auto previous=d.populating;d.populating=true;
    door_set(d.telnet_host,target.host);door_set_number(d.telnet_port,target.port);
    door_set(d.telnet_terminal,target.terminal_type);door_set_number(d.telnet_timeout,target.connection_timeout_seconds);
    d.populating=previous;
}
bool door_apply_telnet_helper(State& s){
    auto& d=s.doors;unsigned port{},timeout{};
    if(!door_number(d.telnet_port,port,65535)||!port){d.advanced=true;return door_problem(s,d.telnet_port,L"Use a Telnet port from 1 to 65535.");}
    if(!door_number(d.telnet_timeout,timeout,120)||!timeout){d.advanced=true;return door_problem(s,d.telnet_timeout,L"Use a connection timeout from 1 to 120 seconds.");}
    app::TelnetTarget target{door_text(d.telnet_host),static_cast<std::uint16_t>(port),door_text(d.telnet_terminal),static_cast<std::uint16_t>(timeout)};
    if(!app::valid_telnet_target(target)){d.advanced=true;return door_problem(s,d.telnet_host,L"Enter a hostname or IP address and a terminal type.");}
    const auto previous=d.populating;d.populating=true;
    door_set(d.command_line,sfx::config_ui::format_door_command(target));
    d.populating=previous;d.telnet_helper_dirty=false;return true;
}
void load_door_fields(State& s){
    auto& d=s.doors;if(!d.catalog)return;d.populating=true;d.telnet_helper_dirty=false;
    app::DoorDefinition item;item.id=d.door;if(const auto* found=app::find_door(*d.catalog,d.door))item=*found;
    else{item.enabled=true;item.security_level=10;item.execution.level=10;}
    d.new_door=app::find_door(*d.catalog,item.id)==nullptr;const auto& t=item.rlogin;
    door_set(d.name,item.name);door_set(d.code,app::effective_door_code(item));
    door_set(d.command_line,item.kind==app::DoorExecutionKind::telnet?sfx::config_ui::format_door_command(item.telnet):item.kind==app::DoorExecutionKind::rlogin?sfx::config_ui::format_door_command(t):item.program.command_line);door_check(d.enabled,item.enabled);door_set(d.host,t.host);door_set_number(d.port,t.port);
    door_set(d.startup,item.program.startup_directory);SendMessageW(d.program_type,CB_SETCURSEL,static_cast<unsigned>(item.program.type),0);door_io_choices(d,item.program.io);
    door_check(d.shell,item.program.command_shell);door_drop_choices(d,item.program.drop_file);SendMessageW(d.drop_location,CB_SETCURSEL,static_cast<unsigned>(item.program.drop_location),0);
    SendMessageW(d.drop_encoding,CB_SETCURSEL,item.program.doordrop_utf8?1:0,0);
    door_check(d.multiple,item.allow_multiple);door_set(d.cleanup,item.program.cleanup_command);SendMessageW(d.automatic_event,CB_SETCURSEL,static_cast<unsigned>(item.automatic_event),0);door_check(d.event_only,item.event_only);
    d.selected_tab=0;d.suggest_code=false;TabCtrl_SetCurSel(d.tabs,0);
    SendMessageW(d.server_mode,CB_SETCURSEL,static_cast<unsigned>(t.server_identity),0);door_set(d.server_name,t.server_name);door_set(d.client_name,t.client_name);door_set(d.terminal,t.terminal_type);
    SendMessageW(d.auth,CB_SETCURSEL,static_cast<unsigned>(t.authentication),0);door_set(d.secret,"");door_set(d.pepper,"");door_check(d.prompt,t.prompt_credential);door_check(d.secret_clear,false);door_check(d.pepper_clear,false);door_set(d.salt,t.salt);
    door_set_number(d.timeout,t.connection_timeout_seconds);door_check(d.quiet,t.quiet);door_check(d.banner,t.banner);door_check(d.pause,t.pause);
    SendMessageW(d.security_mode,CB_SETCURSEL,static_cast<unsigned>(item.security_mode),0);door_set_number(d.level,item.security_level);door_check(d.sysop,item.sysop_only);
    SendMessageW(d.execution_mode,CB_SETCURSEL,static_cast<unsigned>(item.execution.mode),0);door_set_number(d.execution_level,item.execution.level);door_check(d.execution_sysop,item.execution.sysop_only);
    const auto* entry=d.place_new?nullptr:door_current_entry(d);
    door_set_number(d.entry_number,entry?entry->number:door_free_number(*d.catalog,d.menu));
    door_set(d.entry_title,entry?entry->title:item.name);door_set(d.entry_description,entry?entry->description:"");
    const auto uses=std::count_if(d.catalog->entries.begin(),d.catalog->entries.end(),[&](const auto& e){return e.kind==app::DoorEntryKind::door&&e.target==d.door;});
    SetWindowTextW(d.detail,(std::wstring(L"Appears on ")+std::to_wstring(uses)+(uses==1?L" menu.":L" menus.")+(!app::door_has_command(item)?L" Unfinished slot - fill in Command Line later.":L"")+(!d.edit_placement?L" Open a category and use Add Existing Door to add a menu entry.":L"")).c_str());
    SetWindowTextW(d.credential_detail,(std::wstring(L"Saved remote password: ")+(t.protected_credential.empty()?L"none":L"protected")+L". Saved hash pepper: "+(t.protected_pepper.empty()?L"none":L"protected")+L".").c_str());
    SetWindowTextW(d.error,L"");d.populating=false;door_access_controls(s);
}
void door_fill_lists(State& s){
    auto& d=s.doors;d.populating=true;
    door_fill_tree(s);SendMessageW(d.entries,LB_RESETCONTENT,0,0);SendMessageW(d.definitions,LB_RESETCONTENT,0,0);
    door_navigation_refresh(s);
    if(d.catalog){
        auto entries=d.catalog->entries;std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.number<b.number;});
        for(const auto& e:entries)if(e.menu==d.menu){
            auto label=std::to_wstring(e.number)+L"    "+utf8_to_wide(e.title);
            if(e.kind==app::DoorEntryKind::submenu)label+=L"    [Category]";
            else if(const auto* item=app::find_door(*d.catalog,e.target))label+=!app::door_has_command(*item)?L"    [Unfinished]":item->enabled?L"":L"    [Disabled]";
            const auto row=SendMessageW(d.entries,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));SendMessageW(d.entries,LB_SETITEMDATA,row,e.number);if(e.number==d.entry)SendMessageW(d.entries,LB_SETCURSEL,row,0);
        }
        if(d.navigation_placement&&d.navigation_placement->target==d.menu&&!d.entry)SendMessageW(d.entries,LB_SETCURSEL,static_cast<WPARAM>(-1),0);
        else if(SendMessageW(d.entries,LB_GETCURSEL,0,0)==LB_ERR&&SendMessageW(d.entries,LB_GETCOUNT,0,0)>0)SendMessageW(d.entries,LB_SETCURSEL,0,0);
        door_filter_definitions(s);
    }
    d.populating=false;
}
void refresh_door_configuration_page(State& s){
    auto& d=s.doors;if(d.pending)return;const auto catalog=load_door_catalog(s);
    if(!catalog){set_status(s,L"Door settings could not be read. Your edits are kept. Try Reload List when the BBS is available.");return;}
    d.catalog=catalog;door_fill_lists(s);door_show_view(s,d.view);set_status(s,L"Saved Door settings loaded. Choose a category or Door to continue.");
}
void door_saved(State& s){
    auto& d=s.doors;const bool created=d.open_created;d.open_created=false;d.deleting_door=false;d.confirmation_steps.clear();d.save_steps.clear();d.save_step=0;d.place_new=false;d.new_door=false;d.category_new=false;d.category_place_new=false;d.conflict=false;d.confirmation.reset();
    d.clipboard.reset();
    door_set(d.secret,"");door_set(d.pepper,"");SetWindowTextW(d.error,L"");
    door_fill_lists(s);if(created){load_door_fields(s);d.suggest_code=true;}door_show_view(s,created?DoorPage::settings:d.return_view);remember_page(s);SetFocus(created?d.name:d.return_view==DoorPage::definitions?d.definitions:d.return_view==DoorPage::categories?d.categories:d.entries);
    set_status(s,created?L"Door slot added and saved. Fill it in now or later; unfinished slots are not listed to callers.":L"Changes saved. Completed Doors are available on the next menu refresh; active Doors keep their current settings.");
}
bool door_run_save(State& s){
    auto& d=s.doors;
    while(d.save_step<d.save_steps.size()){
        auto& c=d.save_steps[d.save_step];
        if(!d.pending){
            try{win32::WindowsSecureIdentifierGenerator generator;c.operation=generator.generate();}catch(...){return door_problem(s,nullptr,L"Cannot start the save. Your edits are kept; try Save Changes again.");}
            if(c.action!=app::DoorConfigurationAction::remove_menu)c.expected_revision=d.catalog->revision;
            if(!app::valid_door_change(c)||!app::changed_door_catalog(*d.catalog,c))return door_problem(s,nullptr,L"Cannot save these settings. Check the menu number and selected Door or category.");
            d.pending=c;
        }
        const auto r=send_door_change(s,*d.pending);
        if(r.code==app::LocalAccountResultCode::unavailable||r.code==app::LocalAccountResultCode::storage_failure){
            SetWindowTextW(d.error,L"Save not confirmed. Choose Check Save Result before making more changes.");
            set_status(s,L"Your edits and the exact save request are kept. Check Save Result safely checks that same save.");door_show_view(s,d.view);return false;
        }
        if(r.code!=app::LocalAccountResultCode::ok){
            if(d.deleting_door){const bool partial=d.save_step!=0;d.deleting_door=false;d.confirmation_steps.clear();d.pending.reset();d.save_steps.clear();d.save_step=0;
                if(const auto fresh=load_door_catalog(s))d.catalog=fresh;door_fill_lists(s);door_show_view(s,d.return_view);remember_page(s);
                return door_problem(s,nullptr,partial?L"Some menu entries were removed, but the Door was not deleted. Review the refreshed list before trying again.":L"The Door was not deleted. Review the refreshed list before trying again.");}
            const bool partial=d.save_step!=0;const bool tree_copy=partial&&d.view==DoorPage::categories;d.pending.reset();d.save_steps.clear();d.save_step=0;
            if(const auto fresh=load_door_catalog(s))d.catalog=fresh;
            if(partial){d.new_door=false;d.category_new=false;}
            d.conflict=r.code==app::LocalAccountResultCode::conflict;
            if(tree_copy){d.clipboard.reset();d.conflict=false;door_fill_lists(s);door_show_view(s,DoorPage::categories);remember_page(s);return door_problem(s,nullptr,L"Copy configuration saved, but its menu entry was not saved. Review the copy under Not on a menu before placing it. The original is unchanged.");}
            door_show_view(s,d.view);
            return door_problem(s,nullptr,d.conflict?L"Settings changed elsewhere. Your edits are kept. Reload Latest Settings discards these edits so you can review the current settings.":partial?L"Shared settings saved, but the menu entry was not saved. Your menu edits are kept; review and Save Changes again.":L"The save was refused. Your edits are kept; check access and the menu number.");
        }
        // Read the authoritative result before advancing. If reading fails,
        // replay the same confirmed request on Check Save Result, never a new one.
        const auto fresh=load_door_catalog(s);
        if(!fresh){SetWindowTextW(d.error,L"Save accepted, but the updated list could not be read. Choose Check Save Result.");set_status(s,L"The save was accepted; its updated settings are not confirmed here yet. Check Save Result.");door_show_view(s,d.view);return false;}
        d.catalog=fresh;
        if(d.deleting_door&&fresh->revision!=r.result.revision){
            // A fresh snapshot may include another editor's change after our
            // receipt. Never adopt that revision for the next captured removal:
            // its menu number could now belong to a different Door.
            d.deleting_door=false;d.confirmation_steps.clear();d.pending.reset();d.save_steps.clear();d.save_step=0;
            door_fill_lists(s);door_show_view(s,d.return_view);remember_page(s);
            return door_problem(s,nullptr,L"Menu settings changed elsewhere during deletion. Completed removals are kept. Review the refreshed list before trying again.");
        }
        if(c.action==app::DoorConfigurationAction::save_door){d.door=c.door.id;d.new_door=false;}
        if(c.action==app::DoorConfigurationAction::save_menu){d.category_id=c.menu.id;d.category_new=false;}
        if(c.action==app::DoorConfigurationAction::save_entry&&c.entry.menu==d.menu){d.entry=c.entry.number;
            if(!c.order.empty()){auto numbers=c.order;std::sort(numbers.begin(),numbers.end());const auto at=std::find(c.order.begin(),c.order.end(),c.entry.number);if(at!=c.order.end())d.entry=numbers[static_cast<std::size_t>(at-c.order.begin())];}}
        if(c.action==app::DoorConfigurationAction::save_entry&&d.navigation_placement&&c.previous_number==d.navigation_placement->number&&c.entry.menu==d.navigation_placement->menu&&c.entry.kind==d.navigation_placement->kind&&c.entry.target==d.navigation_placement->target)d.navigation_placement=c.entry;
        if(c.action==app::DoorConfigurationAction::remove_entry&&d.navigation_placement&&c.entry.menu==d.navigation_placement->menu&&c.entry.number==d.navigation_placement->number&&c.entry.kind==app::DoorEntryKind::submenu&&c.entry.target==d.menu){d.menu=c.entry.menu;d.entry=0;d.navigation_placement.reset();}
        if(c.action==app::DoorConfigurationAction::remove_menu){d.menu=0;d.entry=0;d.navigation_placement.reset();d.return_view=DoorPage::categories;}
        d.pending.reset();++d.save_step;
    }
    door_saved(s);return true;
}
bool door_start_save(State& s,std::vector<app::DoorConfigurationChange> steps){
    auto& d=s.doors;if(d.pending||d.conflict)return false;
    // Validate the whole proposed journey before the first write. Separate
    // writes remain separate receipts; no schema/IPC batch operation is added.
    auto projected=*d.catalog;
    try{win32::WindowsSecureIdentifierGenerator generator;
        for(auto& c:steps){c.operation=generator.generate();
            if(c.action==app::DoorConfigurationAction::remove_menu){if(c.expected_revision!=projected.revision)return door_problem(s,nullptr,L"Settings changed after the deletion warning. Cancel and review the category again; nothing was deleted.");}
            else c.expected_revision=projected.revision;
            const auto next=app::changed_door_catalog(projected,c);if(!next)return door_problem(s,nullptr,L"Cannot save: the number is in use, or this category would create a loop or too many nested menus.");projected=*next;}
    }catch(...){return door_problem(s,nullptr,L"Cannot start the save. Your edits are kept.");}
    d.save_steps=std::move(steps);d.save_step=0;return door_run_save(s);
}
bool door_entry_fields(State& s,HWND number,HWND title,app::DoorMenuEntry& e,std::uint8_t old=0){
    auto& d=s.doors;unsigned n{};
    if(!door_number(number,n,99)||!n)return door_problem(s,number,L"Menu number: enter a number from 1 to 99.");
    if(std::any_of(d.catalog->entries.begin(),d.catalog->entries.end(),[&](const auto& item){return item.menu==e.menu&&item.number==n&&item.number!=old;}))return door_problem(s,number,L"That menu number is already in use. Choose an unused number.");
    e.number=static_cast<std::uint8_t>(n);e.title=door_text(title);
    if(!app::valid_door_text(e.title,64))return door_problem(s,title,L"Name on menu: enter a name up to 64 bytes.");return true;
}
std::optional<std::uint16_t> door_selected(HWND list,bool combo=false){
    wchar_t cls[64]{};GetClassNameW(list,cls,64);
    if(!_wcsicmp(cls,WC_TREEVIEWW)){TVITEMW item{};item.mask=TVIF_PARAM;item.hItem=TreeView_GetSelection(list);if(!item.hItem||!TreeView_GetItem(list,&item)||item.lParam<0||item.lParam>999)return {};return static_cast<std::uint16_t>(item.lParam);}
    const auto row=SendMessageW(list,combo?CB_GETCURSEL:LB_GETCURSEL,0,0);if(row==LB_ERR)return {};
    return static_cast<std::uint16_t>(SendMessageW(list,combo?CB_GETITEMDATA:LB_GETITEMDATA,row,0));
}
void door_menu_choices(State& s,HWND combo,app::DoorMenuId selected=0){
    auto& d=s.doors;SendMessageW(combo,CB_RESETCONTENT,0,0);
    for(const auto& menu:d.catalog->menus){const auto name=door_menu_name(d,menu.id);const auto row=SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));SendMessageW(combo,CB_SETITEMDATA,row,menu.id);if(menu.id==selected)SendMessageW(combo,CB_SETCURSEL,row,0);}
}
void door_load_category(State& s){
    auto& d=s.doors;d.populating=true;const auto* item=app::find_door_menu(*d.catalog,d.category_id);
    door_set(d.category_name,item?item->name:"");const auto parent=d.category_new?d.menu:app::DoorMenuId{0};door_menu_choices(s,d.category_parent,parent);door_set_number(d.category_number,door_free_number(*d.catalog,parent));
    auto value=item?*item:app::DoorMenuDefinition{};
    // Creation defaults apply to every build; saved and legacy requirements stay intact.
    if(!item){value.access.level=10;value.execution.level=10;}
    for(unsigned i=0;i<2;++i){const auto& r=i?value.execution:value.access;const auto k=i*3;SendMessageW(d.category_requirements[k],CB_SETCURSEL,static_cast<unsigned>(r.mode),0);door_set_number(d.category_requirements[k+1],r.level);door_check(d.category_requirements[k+2],r.sysop_only);}
    SetWindowTextW(d.category_help,d.category_place_new?L"The new category will also appear on the selected menu.":L"Edit Menu Entry changes this category's number, displayed name or description on the menu where it appears. Edit its shared name and security levels here.");
    SetWindowTextW(d.error,L"");d.populating=false;
}
void door_begin_destination(State& s,const app::DoorMenuEntry& source,DoorPage back){
    auto& d=s.doors;d.destination_entry=source;d.back_view=back;d.return_view=back;d.populating=true;door_menu_choices(s,d.destination);door_set_number(d.destination_number,door_free_number(*d.catalog,0));d.populating=false;
    SetWindowTextW(d.error,L"");door_show_view(s,DoorPage::destination);remember_page(s);SetFocus(d.destination);set_status(s,L"Choose a destination and an unused menu number. Nothing has been added yet.");
}
void door_confirm(State& s,app::DoorConfigurationChange change,DoorPage back,const std::wstring& description,const wchar_t* action){
    auto& d=s.doors;d.confirmation_steps.clear();d.deleting_door=false;d.confirmation=std::move(change);d.back_view=back;d.return_view=back;SetWindowTextW(d.confirm_text,description.c_str());SetWindowTextW(d.confirm_button,action);SetWindowTextW(d.error,L"");
    door_show_view(s,DoorPage::confirm);remember_page(s);set_status(s,L"Nothing has been removed. Confirm the named action or choose Cancel.");SetFocus(d.confirm_cancel);
}
bool door_request_delete(State& s,app::DoorId id,DoorPage back){
    auto& d=s.doors;if(!d.catalog||!resolve_dirty(s))return false;
    const auto* item=app::find_door(*d.catalog,id);if(!item)return door_problem(s,nullptr,L"Choose a saved Door first.");
    std::vector<app::DoorConfigurationChange> steps;
    for(const auto& e:d.catalog->entries)if(e.kind==app::DoorEntryKind::door&&e.target==id){app::DoorConfigurationChange c;c.action=app::DoorConfigurationAction::remove_entry;c.entry=e;steps.push_back(c);}
    app::DoorConfigurationChange remove;remove.action=app::DoorConfigurationAction::remove_door;remove.door.id=id;
    const auto count=steps.size();steps.push_back(remove);
    door_confirm(s,remove,back,L"Delete \""+utf8_to_wide(item->name)+L"\" and its "+std::to_wstring(count)+L" menu entries?\r\n\r\nThis deletes its saved settings and protected remote credentials everywhere it appears. Other Doors remain. Active Doors keep their captured settings.",L"Delete Door");
    d.confirmation_steps=std::move(steps);return true;
}
bool door_request_delete_category(State& s,app::DoorMenuId id,DoorPage back){
    auto& d=s.doors;if(!d.catalog||!resolve_dirty(s))return false;
    const auto after=app::without_door_category(*d.catalog,id);
    if(!after)return door_problem(s,nullptr,L"Choose a saved category. Door Menu cannot be deleted.");
    app::DoorConfigurationChange c;c.action=app::DoorConfigurationAction::remove_menu;c.menu.id=id;c.expected_revision=d.catalog->revision;
    door_confirm(s,c,back,L"Delete \""+door_menu_name(d,id)+L"\", its "+std::to_wstring(d.catalog->menus.size()-after->menus.size()-1)+L" subcategories and "+std::to_wstring(d.catalog->doors.size()-after->doors.size())+L" configured Doors?\r\n\r\nTheir settings, saved credentials and appearances on ALL menus will be deleted. Nothing is kept under Not on a menu.\r\n\r\nGame files stay on disk. Active Doors keep their captured settings.",L"Delete Category and Contents");
    return true;
}
bool door_read_requirements(State& s,HWND mode,HWND level,HWND sysop,app::DoorRequirements& value){
    unsigned n{};if(!door_number(level,n,999)||!n)return door_problem(s,level,L"Requirements: enter a security level from 1 to 999.");
    value={static_cast<app::DoorSecurityMode>(SendMessageW(mode,CB_GETCURSEL,0,0)),static_cast<std::uint16_t>(n),door_checked(sysop)};
    return app::valid_door_requirements(value)||door_problem(s,mode,L"Choose how the security level is compared.");
}
bool save_door_configuration_page(State& s){
    auto& d=s.doors;if(!d.catalog||d.pending||d.conflict)return false;
    if(d.view==DoorPage::category){
        app::DoorConfigurationChange c;c.action=app::DoorConfigurationAction::save_menu;c.create_only=d.category_new;c.menu.id=d.category_id;c.menu.name=door_text(d.category_name);
        for(unsigned i=0;i<2;++i){const auto k=i*3;if(!door_read_requirements(s,d.category_requirements[k],d.category_requirements[k+1],d.category_requirements[k+2],i?c.menu.execution:c.menu.access))return false;}
        if(!app::valid_door_text(c.menu.name,64))return door_problem(s,d.category_name,L"Category name: enter a name up to 64 bytes.");
        if(!d.category_place_new)return door_start_save(s,{c});
        const auto parent=door_selected(d.category_parent,true);unsigned number{};
        if(!parent)return door_problem(s,d.category_parent,L"Choose the menu where callers will see this category.");
        if(!door_number(d.category_number,number,99)||!number)return door_problem(s,d.category_number,L"Menu number: enter a number from 1 to 99.");
        app::DoorConfigurationChange place;place.action=app::DoorConfigurationAction::save_entry;place.create_only=true;place.entry={*parent,static_cast<std::uint8_t>(number),app::DoorEntryKind::submenu,d.category_id,c.menu.name,{}};
        return door_start_save(s,{c,place});
    }
    if(d.view==DoorPage::placement){
        const auto old=d.placement_entry;if(!old)return door_problem(s,nullptr,L"This menu entry is no longer available. Reload the list.");
        app::DoorConfigurationChange c;c.action=app::DoorConfigurationAction::save_entry;c.entry=*old;c.previous_number=old->number;
        if(!door_entry_fields(s,d.placement_number,d.placement_title,c.entry,old->number))return false;c.entry.description=door_text(d.placement_description);
        if(!app::valid_door_text(c.entry.description,256,true))return door_problem(s,d.placement_description,L"Description: use at most 256 bytes.");
        return door_start_save(s,{c});
    }
    if(d.view==DoorPage::duplicate){
        const auto name=door_text(d.duplicate_name);if(!app::valid_door_text(name,64))return door_problem(s,d.duplicate_name,L"Copy name: enter a name up to 64 bytes.");
        app::DoorConfigurationChange c;
        if(d.duplicate_category){const auto next=app::next_unused_door_menu_id(*d.catalog);if(!next)return door_problem(s,nullptr,L"There are already 999 categories.");c.action=app::DoorConfigurationAction::duplicate_menu;c.source_menu=d.category_id;c.menu={*next,name};d.return_view=DoorPage::categories;}
        else{const auto* source=app::find_door(*d.catalog,d.door);const auto next=app::next_unused_door_id(*d.catalog);if(!source||!next)return door_problem(s,nullptr,L"The Door is unavailable, or there are already 999 Doors.");c.action=app::DoorConfigurationAction::save_door;c.create_only=true;c.door=*source;c.door.id=*next;c.door.name=name;c.door.code=app::suggest_door_code(*d.catalog,name,*next);c.door.enabled=false;d.return_view=DoorPage::definitions;}
        return door_start_save(s,{c});
    }
    if(d.view==DoorPage::existing){
        const auto row=SendMessageW(d.pick,LB_GETCURSEL,0,0);if(row==LB_ERR)return door_problem(s,d.pick,L"Choose a Door from the list.");
        app::DoorConfigurationChange c;c.action=app::DoorConfigurationAction::save_entry;c.create_only=true;c.entry.menu=d.menu;c.entry.target=static_cast<app::DoorId>(SendMessageW(d.pick,LB_GETITEMDATA,row,0));
        if(!door_entry_fields(s,d.pick_number,d.pick_title,c.entry))return false;return door_start_save(s,{c});
    }
    if(d.view==DoorPage::destination){
        const auto row=SendMessageW(d.destination,CB_GETCURSEL,0,0);
        if(row==CB_ERR)return door_problem(s,d.destination,L"Choose a destination menu.");
        app::DoorConfigurationChange c;c.action=app::DoorConfigurationAction::save_entry;c.create_only=true;c.entry=d.destination_entry;c.entry.menu=static_cast<app::DoorMenuId>(SendMessageW(d.destination,CB_GETITEMDATA,row,0));
        unsigned n{};if(!door_number(d.destination_number,n,99)||!n)return door_problem(s,d.destination_number,L"Menu number: enter a number from 1 to 99.");c.entry.number=static_cast<std::uint8_t>(n);return door_start_save(s,{c});
    }
    if(!d.editing())return false;
    app::DoorConfigurationChange c;c.action=app::DoorConfigurationAction::save_door;c.create_only=d.new_door;
    if(const auto* saved=app::find_door(*d.catalog,d.door))c.door=*saved;
    auto& item=c.door;item.id=d.door;item.name=door_text(d.name);item.enabled=door_checked(d.enabled);item.sysop_only=door_checked(d.sysop);unsigned n{};
    if(item.name.empty()){unsigned number{};door_number(d.entry_number,number,99);item.name="Door "+std::to_string(d.edit_placement&&number?number:d.door);}
    if(!app::valid_door_text(item.name,64))return door_problem(s,d.name,L"Door name: enter a name up to 64 bytes.");
    item.code=door_text(d.code);if(item.code.empty())item.code=app::suggest_door_code(*d.catalog,item.name,item.id);
    if(!app::valid_door_code(item.code))return door_problem(s,d.code,L"Door code: use 1 to 16 letters, numbers, underscores or hyphens.");
    item.code=app::effective_door_code(item);
    for(const auto& other:d.catalog->doors)if(other.id!=item.id&&app::effective_door_code(other)==item.code)return door_problem(s,d.code,L"That Door code is already in use. Choose a unique code.");
    item.program.startup_directory=door_text(d.startup);item.program.cleanup_command=door_text(d.cleanup);
    item.program.type=static_cast<app::DoorProgramType>(SendMessageW(d.program_type,CB_GETCURSEL,0,0));
    item.program.io=static_cast<app::DoorIo>(SendMessageW(d.io,CB_GETITEMDATA,SendMessageW(d.io,CB_GETCURSEL,0,0),0));
    item.program.command_shell=door_checked(d.shell);
    const auto drop_value=door_drop_value(d);
    if(drop_value<0||drop_value>static_cast<unsigned>(app::DoorDropFile::doordrop_ini))return door_problem(s,d.drop_file,L"Choose a Drop File Type.");
    item.program.drop_file=static_cast<app::DoorDropFile>(drop_value);
    item.program.doordrop_utf8=SendMessageW(d.drop_encoding,CB_GETCURSEL,0,0)==1;
    item.program.drop_location=static_cast<app::DoorDropLocation>(SendMessageW(d.drop_location,CB_GETCURSEL,0,0));
    item.allow_multiple=door_checked(d.multiple);item.automatic_event=static_cast<app::DoorAutomaticEvent>(SendMessageW(d.automatic_event,CB_GETCURSEL,0,0));item.event_only=door_checked(d.event_only);
    if(item.program.drop_file==app::DoorDropFile::doordrop_ini && item.allow_multiple &&
        item.program.drop_location==app::DoorDropLocation::startup)
        return door_problem(s,d.drop_location,L"Choose Node or Temporary Directory for DOORDROP.INI when multiple callers can use this Door.");
    item.security_mode=static_cast<app::DoorSecurityMode>(SendMessageW(d.security_mode,CB_GETCURSEL,0,0));
    if(!door_number(d.level,n,999)||!n)return door_problem(s,d.level,L"Security level: enter a level from 1 to 999.");item.security_level=static_cast<std::uint16_t>(n);
    if(!door_read_requirements(s,d.execution_mode,d.execution_level,d.execution_sysop,item.execution))return false;
    if(door_telnet_options(d)&&d.telnet_helper_dirty&&!door_apply_telnet_helper(s))return false;
    const auto command=door_text(d.command_line);
    if(door_telnet_options(d)){
        const auto parsed=sfx::config_ui::parse_telnet_command(command);
        if(!parsed)return door_problem(s,d.command_line,utf8_to_wide(parsed.error).c_str());
        item.kind=app::DoorExecutionKind::telnet;item.telnet=parsed.target;
    }else if(!door_remote_options(d)){
        item.kind=app::DoorExecutionKind::local;item.program.command_line=command.find_first_not_of(" \t\r\n")==std::string::npos?std::string{}:command;
        if(!app::valid_door_text(item.program.command_line,4096,true))return door_problem(s,d.command_line,L"Command Line: use one line, up to 4096 bytes.");
    }else{
    item.kind=app::DoorExecutionKind::rlogin;
    auto parsed=sfx::config_ui::parse_door_command(command,item.rlogin);
    if(!parsed)return door_problem(s,d.command_line,utf8_to_wide(parsed.error).c_str());
    item.rlogin=parsed.target;auto& t=item.rlogin;
    t.salt=door_text(d.salt);
    t.authentication=static_cast<app::RloginAuthentication>(SendMessageW(d.auth,CB_GETCURSEL,0,0));t.prompt_credential=door_checked(d.prompt);
    if(parsed.password!=sfx::config_ui::DoorCommandPassword::unchanged){
        t.authentication=parsed.password==sfx::config_ui::DoorCommandPassword::plain_prompt?app::RloginAuthentication::traditional:app::RloginAuthentication::synchronet_caller_hash;
        t.prompt_credential=true;
    }
    if(door_checked(d.secret_clear)||t.prompt_credential)t.protected_credential.clear();if(door_checked(d.pepper_clear)||t.authentication!=app::RloginAuthentication::synchronet_caller_hash)t.protected_pepper.clear();
    const auto seal=[&](HWND h,std::vector<std::uint8_t>& destination){auto wide=text(h);if(wide.empty())return true;auto value=wide_to_utf8(wide);app::erase_rlogin_secret(wide.data(),wide.size()*sizeof(wchar_t));if(!value)return false;const auto protected_value=win32::protect_rlogin_secret(s.root,*value);app::erase_rlogin_secret(value->data(),value->size());if(!protected_value)return false;destination=*protected_value;return true;};
    if((!t.prompt_credential&&!seal(d.secret,t.protected_credential))||(t.authentication==app::RloginAuthentication::synchronet_caller_hash&&!seal(d.pepper,t.protected_pepper)))return door_problem(s,d.secret,L"SFX could not protect the password with this board's key. Your edits are kept; check the board's Config files. Nothing was saved.");
    }
    if(!app::valid_door_definition(item))return door_problem(s,d.command_line,L"Check the program, I/O and password settings. This combination cannot be saved.");
    if(!d.edit_placement)return door_start_save(s,{c});
    app::DoorConfigurationChange placement;placement.action=app::DoorConfigurationAction::save_entry;placement.create_only=d.place_new;placement.entry.menu=d.menu;placement.entry.target=d.door;
    if(const auto* previous=app::find_door(*d.catalog,d.door))if(const auto* entry=door_current_entry(d))if(entry->title==previous->name&&door_text(d.entry_title)==entry->title)door_set(d.entry_title,item.name);
    if(d.place_new&&text(d.entry_title).empty())door_set(d.entry_title,item.name);
    if(!door_entry_fields(s,d.entry_number,d.entry_title,placement.entry,d.place_new?0:d.entry))return false;
    placement.previous_number=d.place_new?0:d.entry;placement.entry.description=door_text(d.entry_description);
    return door_start_save(s,{c,placement});
}
void door_open_selected(State& s){
    auto& d=s.doors;if(!d.catalog||!resolve_dirty(s))return;
    const auto item=door_action_item(s);if(!item)return;const auto e=item->entry;
    if(e.kind==app::DoorEntryKind::submenu){d.menu=e.target;d.entry=0;d.navigation_placement=e;door_fill_lists(s);door_show_view(s,DoorPage::entries);SetFocus(d.entries);}
    else{const auto back=d.view;d.menu=e.menu;d.entry=e.number;d.door=e.target;d.place_new=false;d.edit_placement=!item->unplaced;d.return_view=back;load_door_fields(s);door_show_view(s,DoorPage::settings);SetFocus(d.name);}remember_page(s);
    set_status(s,L"Saved settings loaded. Changes become available on the next Door menu refresh; active Doors keep their current settings.");
}
void door_open_category(State& s){
    door_open_selected(s);
}
bool door_configuration_command(State& s,UINT id,UINT notification){
    if(id<IDC_DOOR_VIEW||id>=IDC_DOOR_LAST)return false;auto& d=s.doors;if(d.populating)return true;
    if(id==IDC_DOOR_RECONCILE){if(d.pending)door_run_save(s);return true;}
    if(d.pending){if(notification==BN_CLICKED||notification==LBN_DBLCLK)set_status(s,L"Save not confirmed. Choose Check Save Result before another action.");return true;}
    if(id==IDC_DOOR_FIND){if(notification==EN_CHANGE)door_filter_definitions(s);return true;}
    if(id==IDC_DOOR_FIND_CLEAR){SetWindowTextW(d.find,L"");SetFocus(d.find);return true;}
    if(id==IDC_DOOR_ADVANCED){if(notification==BN_CLICKED){d.advanced=!d.advanced;
        if(d.advanced&&door_telnet_options(d)&&!d.telnet_helper_dirty)door_fill_telnet_helper(d);
        door_show_view(s,d.view);}return true;}
    if(id==IDC_DOOR_TELNET_APPLY){if(notification==BN_CLICKED&&door_apply_telnet_helper(s))set_status(s,L"Telnet options placed in Command Line. Save Changes to keep them.");return true;}
    if(id>=IDC_DOOR_TELNET_HOST&&id<=IDC_DOOR_TELNET_TIMEOUT&&notification==EN_CHANGE){d.telnet_helper_dirty=true;return true;}
    if(id==IDC_DOOR_DELETE_CURRENT){door_request_delete(s,d.door,d.return_view);return true;}
    if(id==IDC_DOOR_DELETE_SELECTED){
        const auto item=door_action_item(s);if(item){if(item->entry.kind==app::DoorEntryKind::door)door_request_delete(s,item->entry.target,d.view);else door_request_delete_category(s,item->entry.target,d.view);}
        return true;
    }
    if(id==IDC_DOOR_ENTRIES&&notification==LBN_SELCHANGE){if(const auto selected=door_selected(d.entries))d.entry=static_cast<std::uint8_t>(*selected);door_update_delete_actions(s);door_navigation_sync(s);return true;}
    if(id>=IDC_DOOR_TREE_CUT&&id<=IDC_DOOR_TREE_ADD_DOOR)return door_tree_command(s,id);
    const bool form=d.editing()||d.view==DoorPage::category||d.view==DoorPage::placement||d.view==DoorPage::duplicate||d.view==DoorPage::existing||d.view==DoorPage::destination;
    const bool edited=notification==EN_CHANGE||notification==CBN_SELCHANGE||(notification==BN_CLICKED&&(id==IDC_DOOR_ENABLED||id==IDC_DOOR_SYSOP||id==IDC_DOOR_EXEC_SYSOP||id==IDC_DOOR_CAT_ACCESS_SYSOP||id==IDC_DOOR_CAT_EXEC_SYSOP||id==IDC_DOOR_PROMPT||id==IDC_DOOR_SECRET_CLEAR||id==IDC_DOOR_PEPPER_CLEAR||id==IDC_DOOR_BANNER));
    if(form&&edited&&!d.conflict&&s.current_page==26){
        const bool notice=GetWindowTextLengthW(d.error)!=0;SetWindowTextW(d.error,L"");ShowWindow(d.error,SW_HIDE);
        if(notice){RECT r{};GetClientRect(s.window,&r);layout(s,r.right,r.bottom);}
        const auto unsaved=d.view==DoorPage::duplicate?L"Copy not created. Choose Create Copy or Cancel.":d.view==DoorPage::existing?L"Entry not added. Choose Add to This Menu, or go back.":d.view==DoorPage::destination?L"Entry not added. Choose Add to Selected Menu, or go back.":L"Changes are not saved. Choose Save Changes to apply them, or go back to review navigation choices.";
        set_status(s,page_dirty(s)?unsaved:L"No unsaved changes on this page.");
    }
    if(id==IDC_DOOR_BULLETINS){show_door_bulletins(s);return true;}
    if(id==IDC_DOOR_RELOAD_DRAFT){
        const auto fresh=load_door_catalog(s);if(!fresh){set_status(s,L"Current settings could not be read. Your edits are still kept.");return true;}
        d.catalog=fresh;d.conflict=false;d.confirmation.reset();SetWindowTextW(d.error,L"");
        if(d.editing()&&app::find_door(*d.catalog,d.door)){load_door_fields(s);door_show_view(s,DoorPage::settings);}
        else if(d.view==DoorPage::category&&app::find_door_menu(*d.catalog,d.category_id)){d.category_new=false;door_load_category(s);door_show_view(s,DoorPage::category);}
        else{door_fill_lists(s);door_show_view(s,d.return_view);}
        remember_page(s);set_status(s,L"Latest saved settings loaded. Previous unsaved edits were discarded; review before making changes.");return true;
    }
    if(id==IDC_DOOR_ALL){if(resolve_dirty(s)){d.conflict=false;d.return_view=DoorPage::definitions;door_fill_lists(s);door_show_view(s,DoorPage::definitions);remember_page(s);SetFocus(d.definitions);set_status(s,L"All saved Door configurations, including Doors not on a menu.");}return true;}
    if(id==IDC_DOOR_ALL_LIST&&notification==LBN_DBLCLK){
        if(!d.catalog||!resolve_dirty(s))return true;const auto selected=door_selected(d.definitions);if(!selected)return true;d.door=*selected;d.edit_placement=false;d.return_view=DoorPage::definitions;d.place_new=false;load_door_fields(s);door_show_view(s,DoorPage::settings);remember_page(s);SetFocus(d.name);set_status(s,L"Shared Door settings loaded. No menu entry is being edited.");return true;
    }
    if(id==IDC_DOOR_MENU_NEW||id==IDC_DOOR_CATEGORY_EDIT){
        if(!d.catalog||!resolve_dirty(s))return true;const bool create=id==IDC_DOOR_MENU_NEW;const auto item=door_action_item(s);
        if(!create&&item&&item->entry.kind==app::DoorEntryKind::door){door_open_selected(s);return true;}
        const auto selected=create?app::next_unused_door_menu_id(*d.catalog):item&&item->entry.kind==app::DoorEntryKind::submenu?std::optional<app::DoorMenuId>{item->entry.target}:std::nullopt;if(!selected){door_problem(s,nullptr,create?L"There are already 999 categories.":L"Choose a category first.");return true;}
        const auto back=d.view;if(create)door_action_add_context(s);else if(item&&item->entry.menu==d.menu&&item->entry.number)d.entry=item->entry.number;
        d.category_id=*selected;d.category_new=create;d.category_place_new=create;d.return_view=back;door_load_category(s);door_show_view(s,DoorPage::category);remember_page(s);SetFocus(d.category_name);set_status(s,create?L"New category - not saved. Choose its name and where callers will see it.":L"Saved category settings loaded.");return true;
    }
    if(id==IDC_DOOR_CATEGORY_PLACE){
        if(!d.catalog||!resolve_dirty(s))return true;const auto selected=door_selected(d.categories);if(!selected||!*selected){door_problem(s,nullptr,L"Choose a category below Door Menu. The main menu cannot be placed inside another menu.");return true;}
        const auto* category=app::find_door_menu(*d.catalog,*selected);if(category)door_begin_destination(s,{0,1,app::DoorEntryKind::submenu,*selected,category->name,{}},DoorPage::categories);return true;
    }
    if(id==IDC_DOOR_PLACEMENT_EDIT){
        if(!d.catalog||!resolve_dirty(s))return true;const auto item=door_action_item(s);if(!item||!item->entry.number)return true;d.placement_entry=item->entry;
        const auto& entry=*d.placement_entry;d.populating=true;door_set_number(d.placement_number,entry.number);door_set(d.placement_title,entry.title);door_set(d.placement_description,entry.description);d.populating=false;d.return_view=d.view;SetWindowTextW(d.error,L"");door_show_view(s,DoorPage::placement);remember_page(s);SetFocus(d.placement_title);set_status(s,(L"Editing \""+utf8_to_wide(entry.title)+L"\" on "+door_menu_name(d,entry.menu)+L". Its shared name and security levels stay the same.").c_str());return true;
    }
    if(id==IDC_DOOR_DUPLICATE||id==IDC_DOOR_MENU_DUPLICATE){
        if(!d.catalog||!resolve_dirty(s))return true;d.duplicate_category=id==IDC_DOOR_MENU_DUPLICATE;const auto selected=door_selected(d.duplicate_category?d.categories:d.definitions);if(!selected){door_problem(s,nullptr,L"Select the item to copy first.");return true;}
        std::string name;if(d.duplicate_category){d.category_id=*selected;name=app::find_door_menu(*d.catalog,*selected)->name;d.back_view=DoorPage::categories;}else{d.door=*selected;name=app::find_door(*d.catalog,*selected)->name;d.back_view=DoorPage::definitions;}
        d.populating=true;door_set(d.duplicate_name,name.size()<=56?name+" - Copy":name);d.populating=false;SetWindowTextW(d.error,L"");door_show_view(s,DoorPage::duplicate);remember_page(s);SetFocus(d.duplicate_name);set_status(s,L"Copy not created yet. Choose its name, then Create Copy.");return true;
    }
    if(id==IDC_DOOR_ENTRY_REMOVE||id==IDC_DOOR_REMOVE||id==IDC_DOOR_MENU_REMOVE){
        if(!d.catalog||!resolve_dirty(s))return true;app::DoorConfigurationChange c;
        if(id==IDC_DOOR_ENTRY_REMOVE){const auto item=door_action_item(s);if(!item||!item->entry.number){set_status(s,L"Choose a Door or category entry to remove.");return true;}c.action=app::DoorConfigurationAction::remove_entry;c.entry=item->entry;
            door_confirm(s,c,d.view,L"Remove \""+utf8_to_wide(c.entry.title)+L"\" from "+door_menu_name(d,c.entry.menu)+L"?\r\n\r\nOnly this numbered menu entry is removed. The Door or category, its contents and every other placement remain configured.",L"Remove This Entry");}
        else if(id==IDC_DOOR_REMOVE){const auto selected=door_selected(d.definitions);if(!selected)return true;const auto* item=app::find_door(*d.catalog,*selected);if(!item)return true;
            if(std::any_of(d.catalog->entries.begin(),d.catalog->entries.end(),[&](const auto& e){return e.kind==app::DoorEntryKind::door&&e.target==*selected;})){door_problem(s,nullptr,L"This Door is still on a menu. Remove its menu entries first, or disable the Door in its settings.");return true;}
            c.action=app::DoorConfigurationAction::remove_door;c.door.id=*selected;door_confirm(s,c,DoorPage::definitions,L"Delete \""+utf8_to_wide(item->name)+L"\"?\r\n\r\nIts saved Door settings and protected remote credentials will be deleted. This Door is not on any menu. Already active Doors keep their captured settings.",L"Delete Door Configuration");}
        else{const auto item=door_action_item(s);if(item&&item->entry.kind==app::DoorEntryKind::submenu)door_request_delete_category(s,item->entry.target,d.view);}return true;
    }
    if(id==IDC_DOOR_CONFIRM){if(d.view==DoorPage::confirm&&d.confirmation&&!d.conflict){d.deleting_door=!d.confirmation_steps.empty();door_start_save(s,d.deleting_door?d.confirmation_steps:std::vector<app::DoorConfigurationChange>{*d.confirmation});}return true;}
    if(id==IDC_DOOR_AUTH||id==IDC_DOOR_SYSOP||id==IDC_DOOR_EXEC_SYSOP||id==IDC_DOOR_CAT_ACCESS_SYSOP||id==IDC_DOOR_CAT_EXEC_SYSOP||id==IDC_DOOR_SERVER_MODE||id==IDC_DOOR_PROMPT){door_access_controls(s);return true;}
    if(id==IDC_DOOR_CODE&&notification==EN_CHANGE){d.suggest_code=false;return true;}
    if(id==IDC_DOOR_NAME&&notification==EN_KILLFOCUS&&d.suggest_code&&d.catalog){const auto previous=d.populating;d.populating=true;door_set(d.code,app::suggest_door_code(*d.catalog,door_text(d.name),d.door));d.populating=previous;return true;}
    if(id==IDC_DOOR_COMMAND_LINE&&notification==EN_CHANGE){d.telnet_helper_dirty=false;SetWindowTextW(d.advanced_button,door_telnet_options(d)?L"Telnet Options":L"RLogin Options");ShowWindow(d.advanced_button,d.editing()&&door_remote_options(d)?SW_SHOW:SW_HIDE);return true;}
    if(id==IDC_DOOR_COMMAND_LINE&&notification==EN_KILLFOCUS){door_access_controls(s);if(d.advanced&&!door_remote_options(d)){d.advanced=false;door_show_view(s,d.view);}return true;}
    if(id==IDC_DOOR_PROGRAM_TYPE&&notification==CBN_SELCHANGE){door_io_choices(d,app::DoorIo::fossil_or_uart);return true;}
    if(id==IDC_DOOR_DROP_FILE&&notification==CBN_SELCHANGE){door_drop_encoding_visibility(d);return true;}
    if(id==IDC_DOOR_OPEN){door_open_category(s);return true;}
    if(id==IDC_DOOR_TREE&&notification==LBN_DBLCLK){door_open_category(s);return true;}
    if(id==IDC_DOOR_EDIT){if(d.view==DoorPage::definitions)return door_configuration_command(s,IDC_DOOR_ALL_LIST,LBN_DBLCLK);return door_configuration_command(s,IDC_DOOR_CATEGORY_EDIT,BN_CLICKED);}
    if(id==IDC_DOOR_ENTRIES&&notification==LBN_DBLCLK){door_open_selected(s);return true;}
    if(id==IDC_DOOR_SAVE||id==IDC_DOOR_PICK_USE||id==IDC_DOOR_DESTINATION_USE){save_door_configuration_page(s);return true;}
    if(id==IDC_DOOR_BACK||id==IDC_DOOR_CANCEL){const auto old=d.view;const auto back=d.editing()||old==DoorPage::category||old==DoorPage::placement?d.return_view:old==DoorPage::entries||old==DoorPage::definitions?DoorPage::categories:old==DoorPage::destination||old==DoorPage::duplicate||old==DoorPage::confirm?d.back_view:DoorPage::entries;
        if(resolve_dirty(s)){d.open_created=false;d.conflict=false;d.confirmation.reset();d.confirmation_steps.clear();door_set(d.secret,"");door_set(d.pepper,"");SetWindowTextW(d.error,L"");door_fill_lists(s);door_show_view(s,back);remember_page(s);SetFocus(back==DoorPage::categories?d.categories:back==DoorPage::definitions?d.definitions:d.entries);set_status(s,L"Saved list shown. No changes were made by this navigation.");}return true;}
    if(id==IDC_DOOR_RELOAD){if(resolve_dirty(s)){door_show_view(s,DoorPage::categories);refresh_door_configuration_page(s);remember_page(s);}return true;}
    if(id==IDC_DOOR_NEW){
        if(!d.catalog||!resolve_dirty(s))return true;const auto next=app::next_unused_door_id(*d.catalog);
        if(d.view==DoorPage::categories||d.view==DoorPage::entries)door_action_add_context(s);
        if(!next){door_problem(s,nullptr,L"There are already 999 configured Doors.");return true;}
        d.edit_placement=d.view!=DoorPage::definitions;d.return_view=d.edit_placement?DoorPage::entries:DoorPage::definitions;
        if(d.edit_placement&&!door_free_number(*d.catalog,d.menu)){door_problem(s,nullptr,L"This menu already has 99 entries. Choose another category.");return true;}
        d.door=*next;d.entry=0;d.place_new=d.edit_placement;load_door_fields(s);
        unsigned number=d.edit_placement?door_free_number(*d.catalog,d.menu):*next;
        if(d.edit_placement){unsigned last{};for(const auto& e:d.catalog->entries)if(e.menu==d.menu)last=std::max(last,static_cast<unsigned>(e.number));if(last<99)number=last+1;door_set_number(d.entry_number,number);}
        const auto name="Door "+std::to_string(number);door_set(d.name,name);door_set(d.entry_title,name);
        d.open_created=true;door_show_view(s,DoorPage::settings);save_door_configuration_page(s);return true;
    }
    if(id==IDC_DOOR_ADD_EXISTING){
        if(!d.catalog||!resolve_dirty(s))return true;door_action_add_context(s);d.return_view=DoorPage::entries;SendMessageW(d.pick,LB_RESETCONTENT,0,0);auto doors=d.catalog->doors;
        std::stable_sort(doors.begin(),doors.end(),[](const auto& a,const auto& b){return a.name<b.name;});
        for(const auto& item:doors){const auto label=utf8_to_wide(item.name)+(item.enabled?L"":L" [Disabled]");const auto row=SendMessageW(d.pick,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));SendMessageW(d.pick,LB_SETITEMDATA,row,item.id);}
        door_set_number(d.pick_number,door_free_number(*d.catalog,d.menu));door_set(d.pick_title,"");door_show_view(s,DoorPage::existing);remember_page(s);SetFocus(d.pick);set_status(s,L"Choose an existing Door, then Add to This Menu. No changes have been saved here.");return true;
    }
    if(id==IDC_DOOR_PICK&&notification==LBN_SELCHANGE){const auto row=SendMessageW(d.pick,LB_GETCURSEL,0,0);if(row!=LB_ERR){const auto* item=app::find_door(*d.catalog,static_cast<app::DoorId>(SendMessageW(d.pick,LB_GETITEMDATA,row,0)));if(item)door_set(d.pick_title,item->name);}return true;}
    if(id==IDC_DOOR_SHOW_ANOTHER){
        if(!d.catalog||!resolve_dirty(s))return true;const auto item=door_action_item(s);if(item&&item->entry.number)door_begin_destination(s,item->entry,d.view);return true;
    }
    if(id==IDC_DOOR_DESTINATION&&notification==CBN_SELCHANGE){const auto row=SendMessageW(d.destination,CB_GETCURSEL,0,0);if(row!=CB_ERR)door_set_number(d.destination_number,door_free_number(*d.catalog,static_cast<app::DoorMenuId>(SendMessageW(d.destination,CB_GETITEMDATA,row,0))));return true;}
    if(id==IDC_DOOR_CATEGORY_PARENT&&notification==CBN_SELCHANGE){if(const auto selected=door_selected(d.category_parent,true))door_set_number(d.category_number,door_free_number(*d.catalog,*selected));return true;}
    return true;
}
#include "door_tree.inc"
#include "door_navigation.inc"
#include "door_drag.inc"
LRESULT CALLBACK door_list_keys(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,door_list_keys,id);
    if(message==WM_GETDLGCODE&&w==VK_RETURN)return DefSubclassProc(window,message,w,l)|DLGC_WANTMESSAGE;
    if(message==WM_KEYDOWN&&w==VK_RETURN){auto& s=*reinterpret_cast<State*>(data);door_configuration_command(s,static_cast<UINT>(GetDlgCtrlID(window)),LBN_DBLCLK);return 0;}
    return DefSubclassProc(window,message,w,l);
}
void create_door_configuration_page(State& s){
    auto& d=s.doors;const auto p=s.pages[26]=page(s);
    const auto add=[&](DoorPage view,const wchar_t* cls,const wchar_t* caption,DWORD style,int x,int y,int width,int height,int id,DWORD extended=0){auto h=control(s,p,cls,caption,style,x,y-64,width,height,id,extended);d.controls[static_cast<unsigned>(view)].push_back(h);return h;};
    const auto label_d=[&](DoorPage v,const wchar_t* caption,int x,int y,int width,int height=20){return add(v,L"STATIC",caption,SS_LEFT|SS_NOPREFIX,x,y,width,height,0);};
    const auto field=[&](DoorPage v,HWND h){if(v==DoorPage::settings||v==DoorPage::remote||v==DoorPage::held)d.settings_fields.push_back(h);else if(v==DoorPage::existing)d.existing_fields.push_back(h);else if(v==DoorPage::destination)d.destination_fields.push_back(h);else if(v==DoorPage::category)d.category_fields.push_back(h);else if(v==DoorPage::placement)d.placement_fields.push_back(h);else if(v==DoorPage::duplicate)d.duplicate_fields.push_back(h);return h;};
    const auto edit=[&](DoorPage v,const wchar_t* caption,int x,int y,int width,int id,unsigned maximum,bool number=false,bool secret=false){label_d(v,caption,x,y,width);auto h=add(v,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|(number?ES_NUMBER:0)|(secret?ES_PASSWORD:0),x,y+20,width,25,id,WS_EX_CLIENTEDGE);SendMessageW(h,EM_SETLIMITTEXT,maximum,0);return field(v,h);};
    const auto button=[&](DoorPage v,const wchar_t* caption,int x,int y,int width,int id){return add(v,L"BUTTON",caption,WS_TABSTOP|BS_PUSHBUTTON,x,y,width,32,id);};
    const auto check=[&](DoorPage v,const wchar_t* caption,int x,int y,int width,int id){return field(v,add(v,L"BUTTON",caption,WS_TABSTOP|BS_AUTOCHECKBOX,x,y,width,24,id));};
    const auto combo=[&](DoorPage v,const wchar_t* caption,int x,int y,int width,int id,std::initializer_list<const wchar_t*> choices){label_d(v,caption,x,y,width);auto h=add(v,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,x,y+20,width,200,id);for(const auto* item:choices)SendMessageW(h,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(item));return field(v,h);};
    const auto list=[&](DoorPage v,int y,int height,int id){auto h=add(v,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|WS_VSCROLL,12,y,494,height,id,WS_EX_CLIENTEDGE);SetWindowSubclass(h,door_list_keys,81,reinterpret_cast<DWORD_PTR>(&s));return h;};
    d.heading=control(s,p,L"STATIC",L"",SS_NOPREFIX|SS_ENDELLIPSIS,12,8,494,24);SendMessageW(d.heading,WM_SETFONT,reinterpret_cast<WPARAM>(s.heading_font),FALSE);
    d.help=control(s,p,L"STATIC",L"",SS_NOPREFIX,12,40,494,36);
    d.error=control(s,p,L"STATIC",L"",SS_NOPREFIX,12,78,494,36);
    d.recovery=control(s,p,L"BUTTON",L"Check Save Result",WS_TABSTOP|BS_PUSHBUTTON,12,116,230,32,IDC_DOOR_RECONCILE);
    d.reload_draft=control(s,p,L"BUTTON",L"Reload Latest Settings (discard edits)",WS_TABSTOP|BS_PUSHBUTTON,12,116,360,32,IDC_DOOR_RELOAD_DRAFT);
    constexpr auto home=DoorPage::categories,rows=DoorPage::entries,basic=DoorPage::settings,pick=DoorPage::existing,dest=DoorPage::destination,held=DoorPage::held;
    d.categories=add(home,WC_TREEVIEWW,L"",WS_TABSTOP|WS_BORDER|TVS_HASBUTTONS|TVS_HASLINES|TVS_LINESATROOT|TVS_SHOWSELALWAYS,12,156,494,252,IDC_DOOR_TREE,WS_EX_CLIENTEDGE);
    TreeView_SetExtendedStyle(d.categories,TVS_EX_DOUBLEBUFFER,TVS_EX_DOUBLEBUFFER);
    SetWindowSubclass(d.categories,door_tree_keys,83,reinterpret_cast<DWORD_PTR>(&s));
    d.action_detail[0]=label_d(home,L"",12,416,494,36);
    button(home,L"Open Category",12,464,236,IDC_DOOR_OPEN);button(home,L"Reload List",266,464,240,IDC_DOOR_RELOAD);
    button(home,L"Add Category",12,508,236,IDC_DOOR_MENU_NEW);button(home,L"Edit Name / Security Levels",266,508,240,IDC_DOOR_CATEGORY_EDIT);
    button(home,L"Add Door",12,552,236,IDC_DOOR_TREE_ADD_DOOR);button(home,L"Duplicate Category",266,552,240,IDC_DOOR_MENU_DUPLICATE);
    d.delete_selected=button(home,L"Delete Category",12,596,236,IDC_DOOR_DELETE_SELECTED);button(home,L"All Configured Doors",266,596,240,IDC_DOOR_ALL);
    button(home,L"Edit Menu Entry",12,640,236,IDC_DOOR_PLACEMENT_EDIT);button(home,L"Show on Another Menu",266,640,240,IDC_DOOR_SHOW_ANOTHER);
    button(home,L"Remove from This Menu",12,684,236,IDC_DOOR_ENTRY_REMOVE);
    button(home,L"Door Bulletins",266,684,240,IDC_DOOR_BULLETINS);
    label_d(home,L"Caller order is shown. Ctrl+C Copy, Ctrl+X Cut, Ctrl+V Paste, Insert Add. Right-click for item actions.",12,732,494,36);
    d.entries=list(rows,156,232,IDC_DOOR_ENTRIES);
    SetWindowSubclass(d.entries,door_surface_keys,87,reinterpret_cast<DWORD_PTR>(&s));
    d.action_detail[1]=label_d(rows,L"",12,396,494,36);
    button(rows,L"Edit Name / Security Levels",12,444,236,IDC_DOOR_EDIT);button(rows,L"Open Category",266,444,240,IDC_DOOR_OPEN);
    button(rows,L"Add Door",12,488,150,IDC_DOOR_NEW);button(rows,L"Add Category",174,488,150,IDC_DOOR_MENU_NEW);button(rows,L"Add Existing Door",336,488,170,IDC_DOOR_ADD_EXISTING);
    button(rows,L"Back",12,532,150,IDC_DOOR_BACK);button(rows,L"Show on Another Menu",174,532,166,IDC_DOOR_SHOW_ANOTHER);button(rows,L"Delete Door",352,532,154,IDC_DOOR_DELETE_SELECTED);
    button(rows,L"Edit Menu Entry",12,576,236,IDC_DOOR_PLACEMENT_EDIT);button(rows,L"Remove from This Menu",266,576,240,IDC_DOOR_ENTRY_REMOVE);
    label_d(rows,L"Numbers are the choices callers see. Existing numbers stay the same.",12,624,494);
    d.name=edit(basic,L"Door name",12,156,320,IDC_DOOR_NAME,64);
    d.code=edit(basic,L"Door code (unique)",344,156,162,IDC_DOOR_CODE,16);
    d.enabled=check(basic,L"Enabled - callers may use this Door",12,210,494,IDC_DOOR_ENABLED);
    d.tabs=add(basic,WC_TABCONTROLW,L"",WS_TABSTOP|TCS_SINGLELINE,12,250,494,30,IDC_DOOR_TABS);
    SetWindowSubclass(d.tabs,door_tab_paint,1,reinterpret_cast<DWORD_PTR>(&s));
    for(const auto* caption:{L"Program",L"Access && Use",L"Automation",L"Menu Entry"}){TCITEMW tab{};tab.mask=TCIF_TEXT;tab.pszText=const_cast<wchar_t*>(caption);TabCtrl_InsertItem(d.tabs,TabCtrl_GetItemCount(d.tabs),&tab);}
    const auto tab_begin=[&](){return d.controls[static_cast<unsigned>(basic)].size();};
    const auto tab_end=[&](unsigned tab,std::size_t from){const auto& controls=d.controls[static_cast<unsigned>(basic)];d.tab_controls[tab].assign(controls.begin()+from,controls.end());};
    auto begin=tab_begin();
    d.startup=edit(basic,L"Start-up Directory",12,292,494,IDC_DOOR_STARTUP,1024);
    d.command_line=edit(basic,L"Command Line",12,348,494,IDC_DOOR_COMMAND_LINE,4096);
    d.advanced_button=add(basic,L"BUTTON",L"RLogin options...",WS_TABSTOP|BS_PUSHBUTTON,340,344,166,24,IDC_DOOR_ADVANCED);
    d.program_type=combo(basic,L"Program Type",12,404,236,IDC_DOOR_PROGRAM_TYPE,{L"DOS program",L"Native program"});
    d.io=combo(basic,L"I/O Method",266,404,240,IDC_DOOR_IO,{});
    d.shell=check(basic,L"Run using command shell",12,460,494,IDC_DOOR_SHELL);
    d.drop_file=combo(basic,L"Drop File Type",12,500,236,IDC_DOOR_DROP_FILE,{});door_drop_choices(d,app::DoorDropFile::none);
    const auto encoding_label=tab_begin();
    d.drop_encoding=combo(basic,L"DOORDROP.INI text encoding",266,500,240,IDC_DOOR_DROP_ENCODING,
        {L"CP437 (default)",L"UTF-8 (Door must support it)"});
    d.drop_encoding_label=d.controls[static_cast<unsigned>(basic)][encoding_label];
    SendMessageW(d.drop_encoding,CB_SETCURSEL,0,0);
    d.drop_location=combo(basic,L"Place Drop File In",12,556,494,IDC_DOOR_DROP_LOCATION,{L"Node Directory",L"Start-up Directory",L"Temporary Directory"});
    label_d(basic,L"Use a program command, rlogin host or telnet host[:port]. Blank keeps an unfinished slot.",12,612,494,36);
    tab_end(0,begin);
    d.local_controls={d.startup,d.program_type,d.io,d.shell,d.drop_file,d.drop_location,d.drop_encoding};
    begin=tab_begin();
    d.security_mode=combo(basic,L"Access Requirements",12,292,236,IDC_DOOR_SECURITY_MODE,{L"At least this security level",L"At most this security level",L"Exactly this security level"});d.level=edit(basic,L"Security level (1-999)",266,292,240,IDC_DOOR_LEVEL,3,true);
    d.sysop=check(basic,L"Sysop Only - uses the BBS Sysop security level",12,346,494,IDC_DOOR_SYSOP);
    d.execution_mode=combo(basic,L"Execution Requirements",12,390,236,IDC_DOOR_EXEC_MODE,{L"At least this security level",L"At most this security level",L"Exactly this security level"});d.execution_level=edit(basic,L"Security level (1-999)",266,390,240,IDC_DOOR_EXEC_LEVEL,3,true);
    d.execution_sysop=check(basic,L"Sysop Only - execution",12,444,494,IDC_DOOR_EXEC_SYSOP);
    d.multiple=check(basic,L"Allow multiple callers at once",12,496,494,IDC_DOOR_MULTIPLE);
    label_d(basic,L"When unchecked, this Door is busy until its current caller and cleanup finish, on every menu where it appears.",12,532,494,54);
    tab_end(1,begin);
    begin=tab_begin();
    d.cleanup=edit(basic,L"Cleanup Command Line",12,292,494,IDC_DOOR_CLEANUP,4096);
    d.automatic_event=combo(basic,L"Run automatically on",12,356,494,IDC_DOOR_AUTO_EVENT,{L"None",L"Logon",L"Logoff"});
    d.event_only=check(basic,L"Event only (hide from menus)",12,418,494,IDC_DOOR_EVENT_ONLY);
    label_d(basic,L"Logon runs before the main menu. Logoff runs before a normal disconnect. Lost connections do not start another Door.",12,462,494,64);
    tab_end(2,begin);
    begin=tab_begin();
    d.entry_number=edit(basic,L"Number on this menu (1-99)",12,292,180,IDC_DOOR_ENTRY_NUMBER,2,true);d.entry_title=edit(basic,L"Name on this menu",206,292,300,IDC_DOOR_ENTRY_TITLE,64);
    d.entry_description=edit(basic,L"Optional description",12,352,494,IDC_DOOR_ENTRY_DESCRIPTION,256);
    d.placement_controls.assign(d.controls[static_cast<unsigned>(basic)].begin()+begin,d.controls[static_cast<unsigned>(basic)].end());
    d.detail=label_d(basic,L"",12,414,494,54);
    tab_end(3,begin);
    const auto advanced_begin=tab_begin();
    d.auth=combo(basic,L"Password method",12,700,494,IDC_DOOR_AUTH,{L"Plain remote password",L"Caller password (hashed)",L"Predefined password (hashed)"});
    d.secret=edit(basic,L"New remote password (blank keeps saved password)",12,756,494,IDC_DOOR_SECRET,127,false,true);
    d.prompt=check(basic,L"Use the caller's login password automatically",12,810,494,IDC_DOOR_PROMPT);
    d.secret_clear=check(basic,L"Remove the saved remote password",12,838,494,IDC_DOOR_SECRET_CLEAR);
    label_d(basic,L"-p uses the caller's login password; -h hashes it before sending. Command options take precedence.",12,872,494,36);
    d.salt=edit(basic,L"Hash salt (blank uses this BBS's QWK ID)",12,918,494,IDC_DOOR_SALT,128);
    d.pepper=edit(basic,L"New hash pepper (blank keeps saved pepper)",12,974,494,IDC_DOOR_PEPPER,128,false,true);
    d.pepper_clear=check(basic,L"Remove the saved hash pepper",12,1028,494,IDC_DOOR_PEPPER_CLEAR);
    label_d(basic,L"Pepper applies only to caller-password hashing (-h). Passwords and pepper stay out of Command Line.",12,1060,494,36);
    d.credential_detail=label_d(basic,L"",12,1104,494,36);
    d.advanced_controls.assign(d.controls[static_cast<unsigned>(basic)].begin()+advanced_begin,d.controls[static_cast<unsigned>(basic)].end());
    const auto telnet_begin=tab_begin();
    d.telnet_host=edit(basic,L"Telnet host",12,700,360,IDC_DOOR_TELNET_HOST,253);
    d.telnet_port=edit(basic,L"Port (default 23)",384,700,122,IDC_DOOR_TELNET_PORT,5,true);
    d.telnet_terminal=edit(basic,L"Terminal type",12,756,494,IDC_DOOR_TELNET_TERMINAL,127);
    d.telnet_timeout=edit(basic,L"Connect timeout (seconds, 1-120; default 10)",12,812,494,IDC_DOOR_TELNET_TIMEOUT,3,true);
    button(basic,L"Use Telnet Options",12,884,240,IDC_DOOR_TELNET_APPLY);
    label_d(basic,L"Remote Telnet is unencrypted. No SFX password is sent automatically. Ctrl-X is unavailable in BINARY mode.",12,938,494,70);
    d.telnet_controls.assign(d.controls[static_cast<unsigned>(basic)].begin()+telnet_begin,d.controls[static_cast<unsigned>(basic)].end());
    const auto after_advanced_begin=tab_begin();
    button(basic,L"Save Changes",12,1192,150,IDC_DOOR_SAVE);d.delete_current=button(basic,L"Delete Door",174,1192,150,IDC_DOOR_DELETE_CURRENT);d.back_settings=button(basic,L"Back to Door List",336,1192,170,IDC_DOOR_CANCEL);
    d.after_advanced.assign(d.controls[static_cast<unsigned>(basic)].begin()+after_advanced_begin,d.controls[static_cast<unsigned>(basic)].end());
    // Retained structured fields have no competing visible host/options editor.
    d.host=edit(held,L"BBS address",12,204,360,IDC_DOOR_HOST,253);d.port=edit(held,L"Port",384,204,122,IDC_DOOR_PORT,5,true);
    d.server_mode=combo(held,L"Log in as",12,260,236,IDC_DOOR_SERVER_MODE,{L"Caller's alias",L"Remote account"});d.server_name=edit(held,L"Remote account name",266,260,240,IDC_DOOR_SERVER_NAME,127);
    d.terminal=edit(held,L"Terminal",12,316,494,IDC_DOOR_TERMINAL,127);
    // Command-owned values have no competing visible editor. The canonical
    // Command Line owns these options; remaining settings are above on one page.
    d.client_name=edit(held,L"Client name",12,160,494,IDC_DOOR_CLIENT_NAME,127);
    d.banner=check(held,L"Retained connection banner",12,530,494,IDC_DOOR_BANNER);
    d.timeout=edit(held,L"Connection timeout",12,364,494,IDC_DOOR_TIMEOUT,3,true);d.quiet=check(held,L"Quiet connection",12,420,494,IDC_DOOR_QUIET);d.pause=check(held,L"Pause before connecting",12,476,494,IDC_DOOR_PAUSE);
    d.pick=list(pick,156,220,IDC_DOOR_PICK);d.pick_number=edit(pick,L"Number on this menu (1-99)",12,392,180,IDC_DOOR_COPY_NUMBER,2,true);d.pick_title=edit(pick,L"Name on this menu",206,392,300,IDC_DOOR_ENTRY_TITLE,64);
    button(pick,L"Add to This Menu",12,456,236,IDC_DOOR_PICK_USE);button(pick,L"Back to Door List",266,456,240,IDC_DOOR_CANCEL);
    d.destination=combo(dest,L"Destination menu",12,160,494,IDC_DOOR_DESTINATION,{});d.destination_number=edit(dest,L"Number on destination menu (1-99)",12,224,494,IDC_DOOR_COPY_NUMBER,2,true);
    button(dest,L"Add to Selected Menu",12,296,236,IDC_DOOR_DESTINATION_USE);button(dest,L"Back to Door List",266,296,240,IDC_DOOR_CANCEL);
    constexpr auto all=DoorPage::definitions,cat=DoorPage::category,place=DoorPage::placement,copy=DoorPage::duplicate,confirm=DoorPage::confirm;
    d.find=edit(all,L"Find a Door (Ctrl+F)",12,156,366,IDC_DOOR_FIND,64);d.find_clear=button(all,L"Clear",390,176,116,IDC_DOOR_FIND_CLEAR);
    d.definitions=list(all,220,232,IDC_DOOR_ALL_LIST);
    SetWindowSubclass(d.find,door_tree_keys,83,reinterpret_cast<DWORD_PTR>(&s));SetWindowSubclass(d.definitions,door_tree_keys,83,reinterpret_cast<DWORD_PTR>(&s));
    button(all,L"Edit Door",12,468,150,IDC_DOOR_EDIT);button(all,L"Add Door",174,468,150,IDC_DOOR_NEW);button(all,L"Duplicate Door",336,468,170,IDC_DOOR_DUPLICATE);
    button(all,L"Delete Door Configuration",12,512,236,IDC_DOOR_REMOVE);button(all,L"Back to Categories",266,512,240,IDC_DOOR_BACK);
    label_d(all,L"To place a saved Door, open a category and choose Add Existing Door.",12,560,494,36);
    d.category_name=edit(cat,L"Category name",12,156,494,IDC_DOOR_MENU_NAME,64);
    const auto category_begin=d.controls[static_cast<unsigned>(cat)].size();
    d.category_parent=combo(cat,L"Show this category on",12,220,494,IDC_DOOR_CATEGORY_PARENT,{});
    d.category_number=edit(cat,L"Number on that menu (1-99)",12,280,494,IDC_DOOR_CATEGORY_NUMBER,2,true);
    d.category_placement_controls.assign(d.controls[static_cast<unsigned>(cat)].begin()+category_begin,d.controls[static_cast<unsigned>(cat)].end());
    d.category_requirements[0]=combo(cat,L"Access Requirements",12,340,236,IDC_DOOR_CAT_ACCESS_MODE,{L"At least this security level",L"At most this security level",L"Exactly this security level"});d.category_requirements[1]=edit(cat,L"Security level (1-999)",266,340,240,IDC_DOOR_CAT_ACCESS_LEVEL,3,true);d.category_requirements[2]=check(cat,L"Sysop Only - access",12,394,494,IDC_DOOR_CAT_ACCESS_SYSOP);
    d.category_requirements[3]=combo(cat,L"Execution Requirements",12,434,236,IDC_DOOR_CAT_EXEC_MODE,{L"At least this security level",L"At most this security level",L"Exactly this security level"});d.category_requirements[4]=edit(cat,L"Security level (1-999)",266,434,240,IDC_DOOR_CAT_EXEC_LEVEL,3,true);d.category_requirements[5]=check(cat,L"Sysop Only - execution",12,488,494,IDC_DOOR_CAT_EXEC_SYSOP);
    d.category_help=label_d(cat,L"",12,532,494,54);
    button(cat,L"Save Changes",12,606,236,IDC_DOOR_SAVE);button(cat,L"Back to Categories",266,606,240,IDC_DOOR_CANCEL);
    d.placement_number=edit(place,L"Number on this menu (1-99)",12,156,180,IDC_DOOR_ENTRY_NUMBER,2,true);d.placement_title=edit(place,L"Name on this menu",206,156,300,IDC_DOOR_ENTRY_TITLE,64);
    d.placement_description=edit(place,L"Description",12,216,494,IDC_DOOR_ENTRY_DESCRIPTION,256);
    button(place,L"Save Changes",12,286,236,IDC_DOOR_SAVE);button(place,L"Back to Door List",266,286,240,IDC_DOOR_CANCEL);
    d.duplicate_name=edit(copy,L"Name for the copy",12,156,494,IDC_DOOR_DUP_NAME,64);
    button(copy,L"Create Copy",12,232,236,IDC_DOOR_SAVE);button(copy,L"Cancel",266,232,240,IDC_DOOR_CANCEL);
    d.confirm_text=label_d(confirm,L"",12,156,494,150);d.confirm_button=button(confirm,L"Confirm Removal",12,326,236,IDC_DOOR_CONFIRM);d.confirm_cancel=button(confirm,L"Cancel",266,326,240,IDC_DOOR_CANCEL);
    door_show_view(s,home);
}
