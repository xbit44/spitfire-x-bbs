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
}namespace {
bool same_target(const app::RloginTarget& a,const app::RloginTarget& b){app::DoorDefinition x,y;x.id=y.id=1;x.name=y.name="same";x.rlogin=a;y.rlogin=b;return app::encode_door_definition(x)==app::encode_door_definition(y);}
void parser_checks(){
    using namespace sfx::config_ui;app::RloginTarget base;
    for(const auto* terminal:{"xtrn=GW777","xtrn_sec=xtrn_sec"}){
        const auto result=parse_door_command(std::string("rlogin x-bit.org:5130 -q -t ")+terminal,base);
        check(result&&result.target.host=="x-bit.org"&&result.target.port==5130&&result.target.quiet&&result.target.terminal_type==terminal,"owner command examples map existing native fields");
    }
    auto p=parse_door_command("rlogin example.invalid -p",base);check(p&&p.password==DoorCommandPassword::plain_prompt&&p.target.pause,"lowercase p means alias/password, not pause");
    auto bigp=parse_door_command("rlogin example.invalid -P",base);check(bigp&&bigp.password==DoorCommandPassword::unchanged&&!bigp.target.pause,"uppercase P disables pause only");
    auto h=parse_door_command("rlogin example.invalid -h",base);check(h&&h.password==DoorCommandPassword::caller_hash_prompt,"h requests existing caller hash prompt");
    for(const auto* bad:{"?rlogin host","rlogin.exe host","rlogin.js host","cmd /c rlogin host","rlogin host -m TG_RLOGINSWAP","rlogin host -m 1","rlogin host -v","rlogin host -C","rlogin host -z","rlogin host -qP","rlogin host -q -q","rlogin host -p -h","rlogin host -p -s somebody","rlogin host -h -c somebody","rlogin host -H DummyOnly","rlogin host -HDummyOnly","rlogin host -hDummyPepper","rlogin host -t","rlogin host -t -q","rlogin host -T 0","rlogin host -T 121","rlogin host -T 999999999999999999999","rlogin host:0","rlogin host:65536","rlogin host:","rlogin host -t \"open","rlogin host ; whoami","rlogin host\n-q","rlogin http://host","rlogin user@host","rlogin host -t a -t b","rlogin host -c a -c b"}){
        const auto result=parse_door_command(bad,base);check(!result&&!result.error.empty(),"unsupported/unsafe/ambiguous command rejected");check(result.error.find("Dummy")==std::string::npos,"error does not echo supplied secret");
    }
    check(!parse_door_command(std::string(4097,'x'),base),"bounded command input");
    const auto quote=parse_door_command("rlogin [::1]:5130 -s \"Remote Account\" -c '-q' -t \"xtrn=NAME WITH SPACE\" -T120",base);
    check(quote&&quote.target.host=="::1"&&quote.target.server_name=="Remote Account"&&quote.target.client_name=="-q"&&quote.target.connection_timeout_seconds==120,"quoted fields, bracket IPv6 and attached values");
    const std::vector<std::string> names={"ordinary","With Space","quote\"and\\slash","-q","Unicode-\xc3\xa9"};
    unsigned roundtrips{};
    for(unsigned mode=0;mode<3;++mode)for(unsigned identity=0;identity<2;++identity)for(unsigned flags=0;flags<8;++flags)for(const auto& name:names){
        app::RloginTarget t;t.host=flags&1?"2001:db8::1":"preserve.example.invalid";t.port=flags&2?65535:513;t.quiet=(flags&1)!=0;t.pause=(flags&2)!=0;t.banner=(flags&4)!=0;t.client_name=name;t.server_identity=static_cast<app::RloginIdentity>(identity);t.server_name=name;t.terminal_type="xtrn_sec="+name;t.connection_timeout_seconds=120;t.salt="PRESERVE-SALT";t.authentication=static_cast<app::RloginAuthentication>(mode);t.prompt_credential=mode==1;t.protected_credential=mode==1?std::vector<std::uint8_t>{}:std::vector<std::uint8_t>{0x10,0x22,0x33};if(mode==1)t.protected_pepper={0x31,0x32};
        check(app::valid_rlogin_target(t),"valid existing target fixture");const auto command=format_door_command(t);const auto parsed=parse_door_command(command,t);check(parsed&&same_target(parsed.target,t)&&parsed.password==DoorCommandPassword::unchanged,"every structured option/opaque credential survives format/parse");
        check(command.find("PRESERVE-SALT")==std::string::npos&&command.find("-H")==std::string::npos&&command.find("-h")==std::string::npos,"salt/password/pepper not formatted into command");++roundtrips;
    }
    check(roundtrips==240,"240 complete structured target round trips");
    for(const auto* host:{"quote\"host","apostrophe'host","-saved-host","host:legacy"}){
        auto t=base;t.host=host;check(app::valid_rlogin_target(t),"existing model accepts address preservation fixture");
        const auto result=parse_door_command(format_door_command(t),t);
        check(result&&same_target(result.target,t),"legacy address punctuation survives without DNS reinterpretation");
    }
}
}
int main(int argc,char** argv){try{
    parser_checks();check(argc==2,"fresh output directory");const auto output=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(output),"preserve old evidence");std::filesystem::create_directories(output);
    for(const auto dpi:{96,144,192}){
        State s;s.dpi=dpi;s.instance=GetModuleHandleW(nullptr);s.root=L"C:\\sfxtst\\synthetic-command-ui-not-installed";
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"native classes");const int width=MulDiv(1050,dpi,96),height=MulDiv(760,dpi,96);
        s.window=CreateWindowExW(0,L"STATIC",L"Synthetic unified Door Command Line",WS_POPUP,-30000,-30000,width,height,nullptr,nullptr,s.instance,nullptr);check(s.window&&SetWindowSubclass(s.window,synthetic_root,91,reinterpret_cast<DWORD_PTR>(&s))&&create_ui(s),"native unified form");requested_fonts(s);
        door_catalog_fixture=snapshot;door_change_fixture=change;seed();
        auto& retained=catalog.doors.front().rlogin;retained.authentication=app::RloginAuthentication::synchronet_caller_hash;retained.prompt_credential=true;retained.server_identity=app::RloginIdentity::configured;retained.server_name="Retained Remote Account";retained.port=5131;retained.quiet=false;retained.banner=true;retained.pause=true;
        const auto pepper=win32::protect_rlogin_secret("SyntheticPepperOnly");check(pepper.has_value(),"protect synthetic existing pepper");retained.protected_pepper=*pepper;check(app::valid_rlogin_target(retained),"native existing hash target with protected pepper");
        show_page(s,26);refresh_door_configuration_page(s);remember_page(s);s.appearance.apply_controls(s.window);ShowWindow(s.window,SW_SHOWNOACTIVATE);UpdateWindow(s.window);
        auto& d=s.doors;const auto capture=[&](const char* name,bool bottom=false){layout(s,width,height);SendMessageW(s.pages[26],WM_VSCROLL,bottom?SB_BOTTOM:SB_TOP,0);render(s,output/(std::to_string(dpi)+"-"+name+".bmp"));};
        category(s,2);open_entry(s,42);check(GetDlgItem(s.pages[26],IDC_DOOR_REMOTE)==nullptr,"no separate Remote BBS button/path");const auto exact=app::find_door(catalog,17)->rlogin;
        check(door_text(d.command_line)==sfx::config_ui::format_door_command(exact),"existing target is formatted on ordinary Edit Door");door_set(d.name,"Rename only");check(save_door_configuration_page(s)&&same_target(exact,app::find_door(catalog,17)->rlogin),"native rename-only save preserves every target field");
        press(s,IDC_DOOR_NEW);door_set(d.name,"Example Global War");door_set(d.command_line,"rlogin x-bit.org:5130 -q -t xtrn=GW777");door_check(d.enabled,true);capture("01-add-command");
        check(page_dirty(s)&&save_door_configuration_page(s),"ordinary Add Door uses Command Line");const auto added=d.door;const auto number=d.entry;check(app::find_door(catalog,added)->rlogin.host=="x-bit.org"&&app::find_door(catalog,added)->rlogin.terminal_type=="xtrn=GW777","example mapped to existing native target only");
        open_entry(s,number);capture("02-edit-command");capture("03-password-settings",true);
        door_set(d.command_line,"rlogin x-bit.org:5130 -q -t xtrn_sec=xtrn_sec");door_set(d.name,"Example Remote Menu");check(save_door_configuration_page(s),"remote menu selector through same screen");open_entry(s,number);capture("04-menu-command");
        const auto before=changes;door_set(d.command_line,"rlogin x-bit.org:5130 -m TG_RLOGINSWAP");check(!save_door_configuration_page(s)&&changes==before&&door_text(d.command_line).find("-m")!=std::string::npos&&GetFocus()==d.command_line,"unsupported option has field error, no write, draft retained");capture("05-command-error");discard_page(s);check(!page_dirty(s),"discard restores command field");
        door_set(d.command_line,"rlogin x-bit.org:5130 -p -P -q -t xtrn=GW777");door_configuration_command(s,IDC_DOOR_COMMAND_LINE,EN_KILLFOCUS);check(door_checked(d.prompt)&&!IsWindowEnabled(d.prompt)&&SendMessageW(d.auth,CB_GETCURSEL,0,0)==0,"p visibly selects existing per-launch plain password prompt");capture("06-p-password-prompt",true);
        check(save_door_configuration_page(s),"p uses existing credential policy");const auto& ptarget=app::find_door(catalog,added)->rlogin;check(ptarget.prompt_credential&&!ptarget.pause&&ptarget.protected_credential.empty()&&ptarget.authentication==app::RloginAuthentication::traditional,"p and P saved separately, no reversible password");
        open_entry(s,number);door_set(d.command_line,"rlogin x-bit.org:5130 -h -q -t xtrn=GW777");door_configuration_command(s,IDC_DOOR_COMMAND_LINE,EN_KILLFOCUS);check(save_door_configuration_page(s)&&app::find_door(catalog,added)->rlogin.authentication==app::RloginAuthentication::synchronet_caller_hash,"h native existing mode");
        open_entry(s,number);door_set(d.command_line,"rlogin x-bit.org:5130 -q -t xtrn=GW777");door_configuration_command(s,IDC_DOOR_COMMAND_LINE,EN_KILLFOCUS);SendMessageW(d.auth,CB_SETCURSEL,2,0);door_check(d.prompt,false);door_access_controls(s);door_set(d.secret,"SyntheticRemotePasswordOnly");check(save_door_configuration_page(s),"masked predefined password save without H text");
        const auto protected_target=app::find_door(catalog,added)->rlogin;check(!protected_target.protected_credential.empty()&&GetWindowTextLengthW(d.secret)==0,"protected password stored, input cleared");
        open_entry(s,number);check(door_text(d.command_line).find("SyntheticRemotePasswordOnly")==std::string::npos,"password absent from displayed command");door_set(d.name,"Preserved password Door");check(save_door_configuration_page(s)&&same_target(protected_target,app::find_door(catalog,added)->rlogin),"existing protected password and all options preserved by native save");
        open_entry(s,number);door_set(d.name,"Check this save");lose_at=changes+1;const auto count=changes;check(!save_door_configuration_page(s)&&d.pending&&!IsWindowEnabled(d.command_line),"unknown save freezes command draft");capture("07-save-recovery");press(s,IDC_DOOR_RECONCILE);check(!d.pending&&changes==count+2,"exact receipt recovery still resumes placement");
        press(s,IDC_DOOR_ADD_EXISTING);select_list(d.pick,added);door_configuration_command(s,IDC_DOOR_PICK,LBN_SELCHANGE);door_set_number(d.pick_number,6);check(save_door_configuration_page(s)&&catalog.doors.size()==3,"named existing Door shares unified target");
        category(s,2);open_entry(s,42);SetWindowPos(s.window,nullptr,-30000,-30000,MulDiv(780,dpi,96),MulDiv(610,dpi,96),SWP_NOACTIVATE|SWP_NOZORDER);layout(s,MulDiv(780,dpi,96),MulDiv(610,dpi,96));SendMessageW(s.pages[26],WM_VSCROLL,SB_TOP,0);render(s,output/(std::to_string(dpi)+"-08-compact.bmp"));
        DestroyWindow(s.window);DeleteObject(s.heading_font);DeleteObject(s.ui_font);
    }
    door_catalog_fixture=nullptr;door_change_fixture=nullptr;std::cout<<"PASS "<<checks<<" parser/native assertions; 240 full target round trips and 24 native renders at 96/144/192 DPI. Revised early review only; no network or shipping build.\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" after "<<checks<<" checks\n";return 1;}}
