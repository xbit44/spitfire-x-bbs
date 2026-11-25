// Early UI checkpoint: actual native SFXConfig controls, synthetic protected
// model port; no installed accounts, settings, network or remote credentials.
#define SFX_ALPHA_TEST_AUTOMATION 1
#define wWinMain retained_workflow_entry
#include "../../src/apps/config_ui/main.cpp"
#undef wWinMain
#include <iostream>
#include <map>
namespace {
unsigned checks{},changes{};app::DoorCatalog catalog;unsigned lose_at{},reject_at{},queries{};bool fail_read{};
std::map<std::array<std::uint8_t,16>,app::DoorChangeOutcome> receipts;
void check(bool c,const char* why){++checks;if(!c)throw std::runtime_error(why);}
std::optional<app::DoorCatalog> snapshot(){++queries;if(fail_read){fail_read=false;return {};}return catalog;}
app::LocalDoorResult change(const app::DoorConfigurationChange& c){
    check(app::decode_door_configuration_change(app::encode_door_configuration_change(c)).has_value(),"strict existing IPC encoding");
    if(const auto found=receipts.find(c.operation.bytes);found!=receipts.end()){auto r=found->second;r.replayed=true;return {app::LocalAccountResultCode::ok,r};}
    if(reject_at&&changes+1==reject_at){reject_at=0;return {app::LocalAccountResultCode::policy_rejected};}
    const auto next=app::changed_door_catalog(catalog,c);if(!next)return {app::LocalAccountResultCode::conflict};catalog=*next;++catalog.revision;++changes;
    app::DoorChangeOutcome result{catalog.revision,false};receipts.emplace(c.operation.bytes,result);if(lose_at==changes){lose_at=0;return {};}return {app::LocalAccountResultCode::ok,result};
}
void select_list(HWND h,LPARAM data){for(LRESULT row=0;row<SendMessageW(h,LB_GETCOUNT,0,0);++row)if(SendMessageW(h,LB_GETITEMDATA,row,0)==data){SendMessageW(h,LB_SETCURSEL,row,0);return;}throw std::runtime_error("named list item missing");}
void press(State& s,UINT id){door_configuration_command(s,id,BN_CLICKED);}
void category(State& s,unsigned id){door_show_view(s,DoorPage::categories);remember_page(s);select_list(s.doors.categories,id);press(s,IDC_DOOR_OPEN);}
void open_entry(State& s,unsigned n){select_list(s.doors.entries,n);press(s,IDC_DOOR_EDIT);}
void seed(){catalog={};receipts.clear();changes=queries=0;lose_at=reject_at=0;catalog.menus.push_back({2,"Strategy Games"});catalog.menus.push_back({8,"Remote BBSes"});catalog.menus.push_back({9,"Classic Games"});
    app::DoorDefinition first;first.id=17;first.name="Example Space Game";first.enabled=true;first.rlogin.host="games.example.invalid";first.rlogin.terminal_type="xtrn=SPACE";first.rlogin.salt="KEEP-SALT";first.rlogin.client_name="KEEP-CLIENT";first.rlogin.connection_timeout_seconds=27;catalog.doors.push_back(first);
    auto second=first;second.id=43;second.name="Example Remote Menu";second.rlogin.host="bbs.example.invalid";second.rlogin.terminal_type="xtrn_sec=GAMES";catalog.doors.push_back(second);
    catalog.entries={{0,7,app::DoorEntryKind::submenu,2,"Strategy Games",""},{0,21,app::DoorEntryKind::submenu,8,"Remote BBSes",""},{0,31,app::DoorEntryKind::door,43,"Visit Example BBS",""},{2,9,app::DoorEntryKind::submenu,9,"Classic Games",""},{2,42,app::DoorEntryKind::door,17,"Space Game","Keep description"}};
}
}
namespace {
BOOL CALLBACK font_child(HWND window,LPARAM font){SendMessageW(window,WM_SETFONT,static_cast<WPARAM>(font),FALSE);return TRUE;}
LRESULT CALLBACK synthetic_root(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
    auto& s=*reinterpret_cast<State*>(data);LRESULT result{};
    if(s.appearance.control_message(h,msg,w,l,result))return result;
    if(msg==WM_ERASEBKGND){RECT r{};GetClientRect(h,&r);FillRect(reinterpret_cast<HDC>(w),&r,s.appearance.background_brush());return 1;}
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,synthetic_root,id);
    return DefSubclassProc(h,msg,w,l);
}
void requested_fonts(State& s){
    const auto dc=GetDC(s.window);const auto host_dpi=std::max(96,GetDeviceCaps(dc,LOGPIXELSX));ReleaseDC(s.window,dc);
    LOGFONTW body{},heading{};check(GetObjectW(s.ui_font,sizeof(body),&body)&&GetObjectW(s.heading_font,sizeof(heading),&heading),"host system-font metrics");
    body.lfHeight=MulDiv(body.lfHeight,s.dpi,host_dpi);heading.lfHeight=MulDiv(heading.lfHeight,s.dpi,host_dpi);
    const auto old_body=s.ui_font,old_heading=s.heading_font;s.ui_font=CreateFontIndirectW(&body);s.heading_font=CreateFontIndirectW(&heading);check(s.ui_font&&s.heading_font,"requested-DPI system-font equivalents");
    EnumChildWindows(s.window,font_child,reinterpret_cast<LPARAM>(s.ui_font));SendMessageW(s.heading,WM_SETFONT,reinterpret_cast<WPARAM>(s.heading_font),FALSE);SendMessageW(s.doors.heading,WM_SETFONT,reinterpret_cast<WPARAM>(s.heading_font),FALSE);DeleteObject(old_body);DeleteObject(old_heading);
}
void print_children(HWND parent,HDC dc,POINT origin){
    for(auto child=GetWindow(parent,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        if(!(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE))continue;
        RECT rect{};GetWindowRect(child,&rect);MapWindowPoints(HWND_DESKTOP,parent,reinterpret_cast<POINT*>(&rect),2);
        POINT at{origin.x+rect.left,origin.y+rect.top};const auto saved=SaveDC(dc);
        SetViewportOrgEx(dc,at.x,at.y,nullptr);IntersectClipRect(dc,0,0,rect.right-rect.left,rect.bottom-rect.top);
        SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);
        print_children(child,dc,at);RestoreDC(dc,saved);
    }
}
void render(State& s,const std::filesystem::path& file){
    RECT rect{};GetClientRect(s.window,&rect);const int width=rect.right,height=rect.bottom;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* bits{};const auto reference=GetDC(s.window),dc=CreateCompatibleDC(reference);
    const auto bitmap=CreateDIBSection(reference,&info,DIB_RGB_COLORS,&bits,nullptr,0);check(bitmap&&dc,"fresh offscreen DIB");const auto old=SelectObject(dc,bitmap);
    FillRect(dc,&rect,s.appearance.background_brush());print_children(s.window,dc,{0,0});GdiFlush();
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+static_cast<DWORD>(width*height*4);
    {std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(&header),sizeof(header));out.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));out.write(static_cast<const char*>(bits),static_cast<std::streamsize>(width)*height*4);check(out.good(),"write owned native render");}
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(s.window,reference);
}
}int main(int argc,char** argv){try{
    check(argc==2,"fresh output directory");const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"preserve prior evidence");std::filesystem::create_directories(output);
    for(const auto dpi:{96,144,192}){
        State s;s.dpi=dpi;s.instance=GetModuleHandleW(nullptr);s.root=L"C:\\sfxtst\\synthetic-door-ui-no-install";
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"native classes");
        const int width=MulDiv(1050,dpi,96),height=MulDiv(760,dpi,96);
        s.window=CreateWindowExW(0,L"STATIC",L"Synthetic native Door workflow",WS_POPUP,-30000,-30000,width,height,nullptr,nullptr,s.instance,nullptr);check(s.window&&SetWindowSubclass(s.window,synthetic_root,91,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s),"native UI construction");requested_fonts(s);
        door_catalog_fixture=snapshot;door_change_fixture=change;seed();show_page(s,26);refresh_door_configuration_page(s);remember_page(s);s.appearance.apply_controls(s.window);ShowWindow(s.window,SW_SHOWNOACTIVATE);UpdateWindow(s.window);
        auto& d=s.doors;const auto capture=[&](const char* name){layout(s,width,height);render(s,output/(std::to_string(dpi)+"-"+name+".bmp"));};
        set_status(s,L"Early UI review - synthetic example settings; no installed data.");capture("01-categories");
        check(SendMessageW(d.categories,LB_GETCOUNT,0,0)==4&&d.view==DoorPage::categories,"named category list");
        category(s,2);check(d.menu==2&&d.view==DoorPage::entries&&SendMessageW(d.entries,LB_GETCOUNT,0,0)==2,"category opens Door list");capture("02-door-list");
        open_entry(s,42);check(d.door==17&&!d.new_door&&d.view==DoorPage::settings,"named Door opens shared settings");capture("03-door-settings");
        check(!page_dirty(s),"loaded settings clean");door_set(d.name,"Edited Space Game");press(s,IDC_DOOR_REMOTE);check(page_dirty(s)&&text(d.name)==L"Edited Space Game","cross-page draft retained and tracked");capture("04-rlogin");
        check(save_door_configuration_page(s)&&d.view==DoorPage::entries,"Save Changes returns to list");
        const auto* saved=app::find_door(catalog,17);check(saved&&saved->name=="Edited Space Game"&&saved->rlogin.salt=="KEEP-SALT"&&saved->rlogin.client_name=="KEEP-CLIENT"&&saved->rlogin.connection_timeout_seconds==27,"hidden advanced values preserved");
        check(catalog.entries.back().number==42&&catalog.entries.back().description=="Keep description","number and description retained");
        press(s,IDC_DOOR_NEW);check(d.new_door&&d.place_new&&d.view==DoorPage::settings,"Add Door opens new settings");door_set(d.name,"New Example Door");door_check(d.enabled,true);capture("11-add-door");press(s,IDC_DOOR_REMOTE);door_set(d.host,"new.example.invalid");
        check(save_door_configuration_page(s)&&catalog.doors.size()==3&&catalog.entries.size()==6,"Add Door saves settings and placement");const auto new_id=d.door;const auto new_number=d.entry;check(new_id!=17&&new_number==1,"automatic identity and unused menu number");capture("05-added");
        press(s,IDC_DOOR_ADD_EXISTING);select_list(d.pick,43);door_configuration_command(s,IDC_DOOR_PICK,LBN_SELCHANGE);capture("06-existing-picker");
        check(page_dirty(s),"named choice participates in draft");door_set_number(d.pick_number,42);const auto before=changes;check(!save_door_configuration_page(s)&&changes==before,"occupied menu number refused before write");door_set_number(d.pick_number,4);check(save_door_configuration_page(s)&&catalog.doors.size()==3,"named existing Door shares settings");
        select_list(d.entries,42);press(s,IDC_DOOR_SHOW_ANOTHER);capture("07-destination-picker");check(SendMessageW(d.destination,CB_GETCOUNT,0,0)==4,"destination menus named");door_set_number(d.destination_number,12);check(page_dirty(s)&&save_door_configuration_page(s),"shared placement save with stable destination number");
        check(std::count_if(catalog.entries.begin(),catalog.entries.end(),[](const auto& e){return e.kind==app::DoorEntryKind::door&&e.target==17;})==2,"shared Door used twice");
        open_entry(s,new_number);door_set(d.name,"Lost reply first step");lose_at=changes+1;const auto old_count=changes;check(!save_door_configuration_page(s)&&d.pending.has_value(),"first step lost reply preserves pending request");check(!IsWindowEnabled(d.name)&&!IsWindowEnabled(d.host)&&IsWindowEnabled(d.recovery),"pending save freezes edits; check remains available");const auto exact=d.pending->operation;capture("08-save-recovery");press(s,IDC_DOOR_BACK);check(d.pending&&d.pending->operation==exact&&d.editing(),"pending save blocks navigation");press(s,IDC_DOOR_RECONCILE);check(!d.pending&&changes==old_count+2&&d.view==DoorPage::entries,"same receipt then next step, no duplicate Door");
        open_entry(s,new_number);door_set(d.name,"Lost reply placement");lose_at=changes+2;const auto second=changes;check(!save_door_configuration_page(s)&&d.pending&&d.pending->action==app::DoorConfigurationAction::save_entry,"second step uncertainty retained");press(s,IDC_DOOR_RECONCILE);check(!d.pending&&changes==second+2,"placement replay has no duplicate");
        open_entry(s,new_number);door_set_number(d.port,0);const auto unchanged=changes;check(!save_door_configuration_page(s)&&changes==unchanged&&d.view==DoorPage::remote&&GetFocus()==d.port,"field-directed invalid port with draft retained");capture("09-error");discard_page(s);door_access_controls(s);check(!page_dirty(s),"discard restores both settings pages");press(s,IDC_DOOR_CANCEL);
        category(s,0);check(std::any_of(catalog.entries.begin(),catalog.entries.end(),[](const auto& e){return e.menu==0&&e.kind==app::DoorEntryKind::door;}),"Doors directly on main menu retained");open_entry(s,7);check(d.menu==2&&d.view==DoorPage::entries,"nested category opens");open_entry(s,9);check(d.menu==9&&d.view==DoorPage::entries,"second nested category opens");
        category(s,2);open_entry(s,42);door_set(d.name,"Discard across tabs");press(s,IDC_DOOR_REMOTE);door_set(d.host,"discard.example.invalid");discard_page(s);check(!page_dirty(s)&&text(d.host)==L"games.example.invalid"&&text(d.name)==L"Edited Space Game","discard keeps both tab baselines");
        // A real password is never used. The dummy is protected by the normal
        // adapter, checked in memory, and excluded from every rendered image.
        door_set(d.secret,"SyntheticOnlyCredential");check(SendMessageW(d.secret,EM_GETPASSWORDCHAR,0,0)!=0&&save_door_configuration_page(s),"native masked password and protected save");const auto secret=app::find_door(catalog,17)->rlogin.protected_credential;check(!secret.empty()&&!GetWindowTextLengthW(d.secret),"saved opaque blob; input cleared");
        const auto clear=win32::unprotect_rlogin_secret(secret);check(clear&&clear->view()=="SyntheticOnlyCredential","protected value exact");
        open_entry(s,42);door_set(d.name,"Preserve protected value");check(save_door_configuration_page(s)&&app::find_door(catalog,17)->rlogin.protected_credential==secret,"blank password preserves existing credential");
        category(s,2);open_entry(s,42);door_check(d.sysop,true);door_access_controls(s);check(!IsWindowEnabled(d.level)&&!IsWindowEnabled(d.security_mode),"Sysop Only numeric controls disabled");discard_page(s);door_access_controls(s);
        // Narrow screen evidence uses the same native controls and scroll path.
        SetWindowPos(s.window,nullptr,-30000,-30000,MulDiv(780,dpi,96),MulDiv(610,dpi,96),SWP_NOACTIVATE|SWP_NOZORDER);layout(s,MulDiv(780,dpi,96),MulDiv(610,dpi,96));render(s,output/(std::to_string(dpi)+"-10-compact.bmp"));
        DestroyWindow(s.window);DeleteObject(s.heading_font);DeleteObject(s.ui_font);
    }
    door_catalog_fixture=nullptr;door_change_fixture=nullptr;std::cout<<"PASS "<<checks<<" early native workflow assertions; 33 native renders at 96/144/192 DPI. Remaining layout and full qualification held for DR4.\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
