// Native owner findings08-14, synthetic protected model and exact owned keys.
#define main retained_round2_early_main
#define render retained_round2_early_render
#include "door_command_line_early.cpp"
#undef render
#undef main
#include "door_native_render.hpp"
#include <new>
namespace {
void r2pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}GdiFlush();}
// Native Classic edit focus borders animate. Let that finite transition finish
// before both captures; do not invalidate or repair the actual-window sample.
void r2settle(){const auto until=GetTickCount64()+250;do{r2pump();MsgWaitForMultipleObjectsEx(0,nullptr,20,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}while(GetTickCount64()<until);r2pump();}
LRESULT CALLBACK r2root(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
    auto& s=*reinterpret_cast<State*>(data);if(const auto v=navigation_splitter_message(s,m,w,l))return *v;
    if(m==WM_NCDESTROY)RemoveWindowSubclass(h,r2root,id);
    if(m==WM_NOTIFY){const auto* n=reinterpret_cast<NMHDR*>(l);if(const auto v=door_navigation_notify(s,n))return *v;if(const auto v=door_configuration_notify(s,n))return *v;}
    if(m==WM_COMMAND&&door_configuration_command(s,LOWORD(w),HIWORD(w)))return 0;
    return DefSubclassProc(h,m,w,l);
}
struct R2Key {std::wstring path;~R2Key(){if(!path.empty())RegDeleteKeyW(HKEY_CURRENT_USER,path.c_str());}};
DoorEditorState::TreeItem r2node(State& s,HWND tree,app::DoorEntryKind kind,unsigned target){
    auto& items=tree==s.navigation?s.doors.navigation_items:s.doors.tree_items;
    for(std::size_t i=0;i<items.size();++i){const auto item=items[i];if(!item.group&&item.entry.kind==kind&&item.entry.target==target)return item;if(tree==s.navigation)door_navigation_expand(s,item.handle);else door_tree_expand(s,item.handle);}
    throw std::runtime_error("round2 native node missing");
}
void r2category(State& s,unsigned menu){s.doors.menu=static_cast<app::DoorMenuId>(menu);door_fill_lists(s);door_show_view(s,DoorPage::entries);remember_page(s);}
void r2open(State& s,unsigned number){select_list(s.doors.entries,number);press(s,IDC_DOOR_EDIT);}
std::vector<std::uint32_t> r2pixels(HWND window,const std::filesystem::path& path){
    RECT r{};GetClientRect(window,&r);const auto ref=GetDC(window);DoorPrintSurface image(ref,r.right,r.bottom);
    check(BitBlt(image.dc,0,0,r.right,r.bottom,ref,0,0,SRCCOPY)!=FALSE,"actual owned window capture");ReleaseDC(window,ref);GdiFlush();
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=r.right;info.biHeight=-r.bottom;info.biPlanes=1;info.biBitCount=32;
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info);header.bfSize=header.bfOffBits+static_cast<DWORD>(r.right*r.bottom*4);
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(&header),sizeof(header));out.write(reinterpret_cast<const char*>(&info),sizeof(info));out.write(static_cast<const char*>(image.bits),static_cast<std::streamsize>(r.right)*r.bottom*4);check(out.good(),"actual pixel file");
    const auto* p=static_cast<const std::uint32_t*>(image.bits);return {p,p+static_cast<std::size_t>(r.right)*r.bottom};
}
HWND r2dialog_owner{};bool r2dialog_clicked{};
void CALLBACK r2dialog_tick(HWND,UINT,UINT_PTR,DWORD){
    EnumThreadWindows(GetCurrentThreadId(),[](HWND h,LPARAM)->BOOL{wchar_t cls[40]{};GetClassNameW(h,cls,40);if(GetWindow(h,GW_OWNER)==r2dialog_owner&&!wcscmp(cls,L"#32770")){PostMessageW(h,TDM_CLICK_BUTTON,101,0);r2dialog_clicked=true;}return TRUE;},0);
}
void r2begin(State& s,HWND tree,const DoorEditorState::TreeItem& item){
    NMTREEVIEWW n{};n.hdr={tree,static_cast<UINT_PTR>(GetDlgCtrlID(tree)),TVN_BEGINDRAGW};n.itemNew.hItem=item.handle;
    if(tree==s.navigation)door_navigation_notify(s,&n.hdr);else door_configuration_notify(s,&n.hdr);
}
app::LocalDoorResult r2concurrent_delete(const app::DoorConfigurationChange& c){
    const auto result=change(c);
    if(result.code==app::LocalAccountResultCode::ok&&c.action==app::DoorConfigurationAction::remove_entry)
        for(auto& entry:catalog.entries)if(entry.menu==8&&entry.number==6&&entry.target==17){entry.target=43;entry.title="Another editor's Door";++catalog.revision;break;}
    return result;
}
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh round2 output");const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"preserve prior round2 views");std::filesystem::create_directories(output);
    unsigned renders{},pixel_cases{};std::size_t damaged{};
    for(const int dpi:{96,144,192})for(unsigned theme=0;theme<2;++theme){
        State s;s.dpi=dpi;s.instance=GetModuleHandleW(nullptr);s.root=L"C:\\sfxtst\\round2-synthetic-not-installed";
        const auto tag=std::to_string(dpi)+"-"+std::to_string(theme);const auto key=L"Software\\Spitfire-X BBS\\Tests\\DoorRound2\\"+output.parent_path().filename().wstring()+L"-"+std::to_wstring(dpi)+L"-"+std::to_wstring(theme);
        HKEY h{};DWORD disposition{};check(RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&h,&disposition)==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY,"fresh exact owned preference key");R2Key owned{key};const DWORD value=theme;check(RegSetValueExW(h,L"Theme",0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value))==ERROR_SUCCESS,"test theme");RegCloseKey(h);
        navigation_registry_fixture=key;s.appearance.~DesktopAppearance();new(&s.appearance)win32::DesktopAppearance(key);
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"native controls");
        const int width=MulDiv(1050,dpi,96),height=MulDiv(760,dpi,96);
        s.window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,L"STATIC",L"SFX owned round2 diagnostic",WS_POPUP|WS_CLIPCHILDREN,dpi==96?20:-30000,dpi==96?20:-30000,width,height,nullptr,nullptr,s.instance,nullptr);
        check(s.window&&SetWindowSubclass(s.window,synthetic_root,91,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s)&&SetWindowSubclass(s.window,r2root,94,reinterpret_cast<DWORD_PTR>(&s)),"native round2 UI");requested_fonts(s);
        door_catalog_fixture=snapshot;door_change_fixture=change;seed();catalog.doors.resize(1);catalog.entries={{0,7,app::DoorEntryKind::submenu,2,"Strategy Games",{}},{2,1,app::DoorEntryKind::door,17,"Example Space Game",{}}};
        show_page(s,26);refresh_door_configuration_page(s);r2category(s,2);s.appearance.apply_controls(s.window);ShowWindow(s.window,SW_SHOWNOACTIVATE);layout(s,width,height);r2pump();auto& d=s.doors;
        const auto capture=[&](const char* name){r2pump();render(s,output/(tag+"-"+name+".bmp"));++renders;};
        RECT nav{};GetWindowRect(s.navigation,&nav);MapWindowPoints(HWND_DESKTOP,s.window,reinterpret_cast<POINT*>(&nav),2);
        check(navigation_splitter_message(s,WM_LBUTTONDOWN,0,MAKELPARAM(nav.right+1,nav.top+10)).has_value()&&s.resizing_navigation,"divider captures mouse");
        navigation_splitter_message(s,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(MulDiv(332,dpi,96),50));navigation_splitter_message(s,WM_LBUTTONUP,0,0);check(s.navigation_width==320&&!s.resizing_navigation,"divider stores logical width");
        State reopened;load_navigation_width(reopened);check(reopened.navigation_width==320,"new editor state reloads exact operator width");
        navigation_splitter_message(s,WM_LBUTTONDOWN,0,MAKELPARAM(MulDiv(333,dpi,96),50));navigation_splitter_message(s,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(MulDiv(480,dpi,96),50));navigation_splitter_message(s,WM_KEYDOWN,VK_ESCAPE,0);check(s.navigation_width==320&&!s.resizing_navigation,"Escape restores prior divider width");capture("01-remembered-width");
        s.navigation_width=204;layout(s,width,height);
        std::vector<app::DoorId> slots;
        for(unsigned number=2;number<=4;++number){press(s,IDC_DOOR_NEW);slots.push_back(d.door);const auto* slot=app::find_door(catalog,d.door);
            check(slot&&slot->rlogin.host.empty()&&slot->enabled&&slot->security_level==10&&slot->execution.level==10,"Add immediately saves enabled/min10 unfinished slot");
            check(d.editing()&&!d.new_door&&!d.place_new&&d.entry==number&&!page_dirty(s),"new slot is placed and clean for later editing");
            check(std::any_of(d.navigation_items.begin(),d.navigation_items.end(),[&](const auto& n){return n.entry.kind==app::DoorEntryKind::door&&n.entry.target==slot->id&&n.entry.number==number;}),"left hierarchy includes new slot immediately");
            check(!IsWindowVisible(d.advanced_button)&&!app::door_accessible(*slot,{999,999}),"empty slot hides RLogin controls and cannot launch");
            if(number==2)capture("02-empty-slot");press(s,IDC_DOOR_CANCEL);check(d.view==DoorPage::entries&&!page_dirty(s),"clean saved slot returns without save loop");
        }
        lose_at=changes+2;press(s,IDC_DOOR_NEW);const auto recovered_slot=d.door;check(d.pending&&d.open_created,"lost new-slot placement reply remains recoverable");press(s,IDC_DOOR_RECONCILE);check(!d.pending&&d.editing()&&!page_dirty(s)&&app::find_door(catalog,recovered_slot),"new-slot recovery replays the same receipt and opens one saved slot");press(s,IDC_DOOR_CANCEL);
        reject_at=changes+2;press(s,IDC_DOOR_NEW);const auto partial_slot=d.door;check(!d.pending&&!d.new_door&&d.place_new&&app::find_door(catalog,partial_slot),"refused new-slot placement retains its saved unplaced definition");check(save_door_configuration_page(s)&&d.editing()&&!d.place_new&&app::find_door(catalog,partial_slot),"new-slot placement retry edits existing identity without duplicate create");press(s,IDC_DOOR_CANCEL);
        r2open(s,2);door_set(d.name,"Named later");door_set(d.command_line,"");check(save_door_configuration_page(s),"save a named empty slot later");
        check(app::find_door(catalog,slots[0])->name=="Named later"&&door_tree_entries(catalog,2)[1].title=="Named later","default placement label follows its renamed slot");
        r2open(s,2);door_set(d.name,"Saved via navigation");r2dialog_owner=s.window;r2dialog_clicked=false;check(SetTimer(s.window,89,40,r2dialog_tick)!=0,"owned dialog timer");const bool continued=resolve_dirty(s);KillTimer(s.window,89);
        check(continued&&r2dialog_clicked&&!page_dirty(s)&&app::find_door(catalog,slots[0])->name=="Saved via navigation","real Save and Continue saves name-only slot and exits loop");
        r2open(s,2);door_set(d.command_line,"not a supported command");const auto old_changes=changes;check(!save_door_configuration_page(s)&&changes==old_changes,"unsupported nonempty command remains refused");capture("03-validation");door_set(d.command_line,"");check(save_door_configuration_page(s),"blank command deliberately saves unfinished slot");
        r2open(s,2);door_set(d.command_line,"rlogin example.invalid:513 -p -q");check(IsWindowVisible(d.advanced_button),"RLogin options appear beside an RLogin command");press(s,IDC_DOOR_ADVANCED);check(d.advanced&&IsWindowVisible(d.secret)&&(GetWindowLongPtrW(d.secret,GWL_STYLE)&ES_PASSWORD),"optional masked settings remain accessible");capture("04-contextual-options");check(save_door_configuration_page(s)&&app::door_accessible(*app::find_door(catalog,slots[0]),{10,999}),"finish existing slot without changing identity");
        r2open(s,4);catalog.entries.push_back({8,6,app::DoorEntryKind::door,slots[2],"Shared slot",{}});++catalog.revision;refresh_door_configuration_page(s);remember_page(s);const auto before=app::encode_door_catalog(catalog);
        press(s,IDC_DOOR_DELETE_CURRENT);check(d.view==DoorPage::confirm&&text(d.confirm_text).find(L"2 menu entries")!=std::wstring::npos&&GetFocus()==d.confirm_cancel,"direct Delete names all shared uses and focuses Cancel");capture("05-delete-confirm");press(s,IDC_DOOR_CANCEL);check(app::encode_door_catalog(catalog)==before,"cancelled shared deletion changes no bytes");
        r2open(s,4);press(s,IDC_DOOR_DELETE_CURRENT);lose_at=changes+1;press(s,IDC_DOOR_CONFIRM);check(d.pending&&d.deleting_door,"lost delete reply retains exact staged request");press(s,IDC_DOOR_RECONCILE);check(!d.pending&&!app::find_door(catalog,slots[2])&&app::find_door(catalog,17)&&std::none_of(catalog.entries.begin(),catalog.entries.end(),[&](const auto& e){return e.kind==app::DoorEntryKind::door&&e.target==slots[2];}),"replayed delete removes target/shared placements once and keeps other Doors");
        r2open(s,3);press(s,IDC_DOOR_DELETE_CURRENT);reject_at=changes+2;press(s,IDC_DOOR_CONFIRM);check(app::find_door(catalog,slots[1])&&text(d.error).find(L"Some menu entries")!=std::wstring::npos&&!d.pending,"partial delete preserves definition and reports incomplete action");
        door_show_view(s,DoorPage::categories);remember_page(s);const auto first=r2node(s,d.categories,app::DoorEntryKind::door,17);TreeView_SelectItem(d.categories,first.handle);check(text(d.delete_selected)==L"Delete Door","overview visible delete follows selected item type");press(s,IDC_DOOR_DELETE_SELECTED);check(d.view==DoorPage::confirm,"overview Delete Door opens real confirmation");press(s,IDC_DOOR_CANCEL);
        seed();catalog.entries.push_back({8,6,app::DoorEntryKind::door,17,"Shared Space Game",{}});++catalog.revision;refresh_door_configuration_page(s);remember_page(s);r2category(s,2);r2open(s,42);press(s,IDC_DOOR_DELETE_CURRENT);
        door_change_fixture=r2concurrent_delete;press(s,IDC_DOOR_CONFIRM);door_change_fixture=change;
        check(app::find_door(catalog,17)&&std::any_of(catalog.entries.begin(),catalog.entries.end(),[](const auto& e){return e.menu==8&&e.number==6&&e.target==43;})&&!d.pending&&text(d.error).find(L"changed elsewhere")!=std::wstring::npos,"concurrent delete stops before removing another editor's replacement entry");
        seed();d.navigation_revision=-1;d.menu=0;d.view=DoorPage::categories;refresh_door_configuration_page(s);remember_page(s);layout(s,width,height);
        auto source=r2node(s,s.navigation,app::DoorEntryKind::submenu,2),target=r2node(s,s.navigation,app::DoorEntryKind::submenu,8);TreeView_EnsureVisible(s.navigation,target.handle);
        r2begin(s,s.navigation,source);check(d.dragging&&d.drag_preview&&text(d.drag_preview).find(L"Strategy Games")!=std::wstring::npos,"left tree drag has readable item preview");RECT row{};TreeView_GetItemRect(s.navigation,target.handle,&row,TRUE);
        const auto moves_before=changes,reads_before=queries;
        for(unsigned n=0;n<100;++n){if(dpi==96)SendMessageW(s.navigation,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(row.left+5,row.bottom-1));else door_drag_target_at(s,s.navigation,{row.left+5,row.bottom-1});}
        check(d.drag_drop&&d.drag_feedback_updates<=2&&changes==moves_before&&queries==reads_before,"100 drag moves update one marker, never read/write configuration");KillTimer(s.navigation,86);if(dpi==96)capture("06-left-drag-preview");SendMessageW(s.navigation,WM_LBUTTONUP,0,0);
        auto ordered=door_tree_entries(catalog,0);check(ordered[0].target==8&&ordered[1].target==2&&!d.dragging&&!d.drag_preview,"left tree drops through protected stable reorder");
        door_show_view(s,DoorPage::categories);remember_page(s);source=r2node(s,d.categories,app::DoorEntryKind::door,43);target=r2node(s,d.categories,app::DoorEntryKind::submenu,8);TreeView_Expand(d.categories,TreeView_GetParent(d.categories,target.handle),TVE_EXPAND);r2begin(s,d.categories,source);TreeView_GetItemRect(d.categories,target.handle,&row,TRUE);door_drag_target_at(s,d.categories,{row.left+5,(row.top+row.bottom)/2});SendMessageW(d.categories,WM_LBUTTONUP,0,0);
        check(door_tree_entries(catalog,8).size()==1&&door_tree_entries(catalog,8)[0].target==43,"overview drag into a category retains shared Door identity");
        const auto original=door_tree_entries(catalog,2);const auto game=*std::find_if(original.begin(),original.end(),[](const auto& e){return e.kind==app::DoorEntryKind::door;});d.clipboard=DoorEditorState::TreeItem{nullptr,game};d.clipboard_cut=true;d.clipboard_revision=catalog.revision;
        door_tree_paste(s,{nullptr,{0,7,app::DoorEntryKind::submenu,8,"Remote BBSes",{}}},0,DoorPage::entries);r2category(s,8);
        RECT a{},b{};SendMessageW(d.entries,LB_GETITEMRECT,1,reinterpret_cast<LPARAM>(&a));SendMessageW(d.entries,LB_GETITEMRECT,0,reinterpret_cast<LPARAM>(&b));
        if(dpi==96){DWORD_PTR context{};std::cerr<<"LIST BEGIN subclass="<<GetWindowSubclass(d.entries,door_surface_keys,87,&context)<<" row="<<a.left<<','<<a.top<<','<<a.right<<','<<a.bottom<<" target="<<b.top<<','<<b.bottom<<" visible="<<IsWindowVisible(d.entries)<<'\n';SendMessageW(d.entries,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(a.left+8,(a.top+a.bottom)/2));std::cerr<<"LIST DOWN index="<<d.drag_list_index<<" capture="<<(GetCapture()==d.entries)<<'\n';SendMessageW(d.entries,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(b.left+8,b.top+1));}
        else {door_drag_begin(s,d.entries,*door_surface_item(s,d.entries,nullptr,1));door_drag_target_at(s,d.entries,{b.left+8,b.top+1});}
        if(!d.dragging||!d.drag_drop)std::cerr<<"LIST DRAG state="<<d.dragging<<" target="<<bool(d.drag_drop)<<" index="<<d.drag_list_index<<" count="<<SendMessageW(d.entries,LB_GETCOUNT,0,0)<<" view="<<static_cast<unsigned>(d.view)<<" capture="<<(GetCapture()==d.entries)<<" dirty="<<page_dirty(s)<<'\n';
        check(d.dragging&&d.drag_drop,"category list supports thresholded mouse drag");SendMessageW(d.entries,WM_LBUTTONUP,0,0);check(door_tree_entries(catalog,8)[0].target==17&&!d.dragging&&!d.drag_preview,"category list drag persists requested order");capture("07-category-reorder");
        const auto cancelled=app::encode_door_catalog(catalog);door_drag_begin(s,d.entries,*door_surface_item(s,d.entries,nullptr,0));SendMessageW(d.entries,WM_KEYDOWN,VK_ESCAPE,0);check(app::encode_door_catalog(catalog)==cancelled&&!d.dragging&&!d.drag_preview,"Escape destroys drag feedback without mutation");
        if(dpi==96){
            r2category(s,8);r2open(s,1);
            for(const int w:{840,1050}){SetWindowPos(s.window,nullptr,20,20,w,600,SWP_NOZORDER|SWP_NOACTIVATE);layout(s,w,600);RedrawWindow(s.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);r2pump();
                for(unsigned step=0;step<12;++step){if(step%4==0)door_problem(s,d.command_line,L"Use a supported command, or leave it blank for an unfinished slot.");else if(step%4==1){d.advanced=!d.advanced;door_show_view(s,d.view);}else if(step%4==2){SetWindowTextW(d.error,L"");door_show_view(s,d.view);}else SendMessageW(s.pages[26],WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA)),0);
                    r2settle();const auto name=tag+"-"+std::to_string(w)+"-"+std::to_string(step);const auto actual=r2pixels(s.pages[26],output/(name+"-actual.bmp"));RedrawWindow(s.pages[26],nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);r2settle();const auto repaired=r2pixels(s.pages[26],output/(name+"-redrawn.bmp"));std::size_t stale{};for(std::size_t n=0;n<actual.size();++n)if((actual[n]&0xffffffU)!=(repaired[n]&0xffffffU))++stale;damaged+=stale;++pixel_cases;std::cout<<"round2_layout="<<name<<" stale_pixels="<<stale<<'\n';}
            }
        }
        door_drag_end(s);DestroyWindow(s.window);DeleteObject(s.heading_font);DeleteObject(s.ui_font);navigation_registry_fixture.clear();door_catalog_fixture=nullptr;door_change_fixture=nullptr;
    }
    check(damaged==0,"all actual validation/options/navigation layout pixels equal full repair");
    std::cout<<"PASS round2 native checks="<<checks<<" renders="<<renders<<" actual_layout_comparisons="<<pixel_cases<<" stale_pixels="<<damaged<<"; native synthetic/owned windows, physical owner visual testing pending\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL round2 after "<<checks<<": "<<e.what()<<'\n';return 1;}}
