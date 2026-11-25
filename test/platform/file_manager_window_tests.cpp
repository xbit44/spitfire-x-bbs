// Real product controls with explicitly fake owner ports. Native file/SQL
// operations are qualified separately by file_manager_tests.
#include "../../src/apps/file_manager/window.cpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <fstream>
#include <mutex>
#include <thread>
namespace ui=sfx::file_manager;namespace app=sfx::application;namespace win=sfx::platform::win32;
namespace {
unsigned checks{};void check(bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);}
std::filesystem::path pictures;
void picture(HWND window,const std::string& name){
    if(pictures.empty())return;RECT client{};GetWindowRect(window,&client);client.right-=client.left;client.bottom-=client.top;client.left=client.top=0;if(client.right<=0||client.bottom<=0)return;
    const auto screen=GetDC(window),dc=CreateCompatibleDC(screen);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=client.right;info.bmiHeader.biHeight=-client.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;void* pixels{};const auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);const auto saved=SelectObject(dc,bitmap);SendMessageW(window,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND|PRF_NONCLIENT);
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);const auto bytes=static_cast<DWORD>(client.right*client.bottom*4);header.bfSize=header.bfOffBits+bytes;const auto path=pictures/(name+".bmp");if(!std::filesystem::exists(path)){std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(&header),sizeof(header));file.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));file.write(static_cast<const char*>(pixels),bytes);}
    SelectObject(dc,saved);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,screen);
}
struct Ports {std::mutex mutex;std::vector<app::ManagedFile> files;std::map<std::array<std::uint8_t,16>,app::FileManagementJobStatus> jobs;bool lose_next{};unsigned applied{},reads{},destroyed{};};
app::ManagedFile row(unsigned n,unsigned area=1){app::ManagedFile r;r.file.id={n};r.file.area={area};r.file.revision=1;r.file.publication_sequence=n;r.file.name="file-"+std::to_string(n)+".zip";r.file.descriptions={"Test description","Extended text"};r.file.public_uploader="Test Sysop";r.file.bytes=4;r.file.sha256[0]=1;r.payload={4,r.file.sha256,"fake-volume","fake-"+std::to_string(n)};return r;}
class Fake final:public win::IFileAdministrationClient {
    std::shared_ptr<Ports> p;
public:
    explicit Fake(std::shared_ptr<Ports> value):p(std::move(value)){}
    ~Fake(){++p->destroyed;}
    bool online()const noexcept override{return true;}
    std::optional<app::FileConfiguration> configuration()override{app::FileConfiguration c;app::FileGroup g;g.id={1};g.name="Main";c.groups.push_back(g);for(unsigned n=1;n<=2;++n){app::FileAreaConfiguration a;a.area.id={n};a.area.group={1};a.area.number=static_cast<std::uint16_t>(n);a.area.revision=1;a.area.name=n==1?"General":"Uploads";a.storage_path=n==1?"Files/General":"Files/Uploads";c.areas.push_back(a);}return c;}
    app::FileManagementReadSnapshot read(const app::FileManagementReadRequest& q)override{std::lock_guard<std::mutex> lock(p->mutex);++p->reads;app::FileManagementReadSnapshot out;out.code=app::LocalAccountResultCode::ok;out.kind=q.kind;
        if(q.kind==app::FileManagementQueryKind::detail){for(const auto& r:p->files)if(r.file.id==q.selection)out.records.push_back(r);return out;}
        if(q.kind!=app::FileManagementQueryKind::records)return out;
        for(const auto& r:p->files){if(r.file.id.value<=q.after.value||(q.filter.area.value&&!(q.filter.area==r.file.area))||(q.filter.state&&*q.filter.state!=r.file.state))continue;
            if(out.records.size()==app::kFileManagementRecordPage){out.next=out.records.back().file.id;break;}auto copy=r;copy.file.descriptions.long_text.clear();out.records.push_back(copy);}return out;
    }
    app::FileManagementJobStatus start(const app::FileManagementTask& t)override{std::lock_guard<std::mutex> lock(p->mutex);const auto prior=p->jobs.find(t.job.bytes);if(prior!=p->jobs.end())return prior->second;app::FileManagementJobStatus out;out.code=app::LocalAccountResultCode::ok;out.job=t.job;out.kind=t.kind;out.state=app::FileManagementJobState::completed;out.journal_confirmed=true;out.detail="Synthetic owner result";
        if(t.kind==app::FileManagementTaskKind::operation){for(auto& r:p->files)if(r.file.id==t.operation.file){if(r.file.revision!=t.operation.expected_revision){out.state=app::FileManagementJobState::refused;break;}++p->applied;++r.file.revision;out.result=r.file.id;if(t.operation.action==app::FileOperationAction::purge){r.file.state=app::FileCatalogState::removed;r.purged=true;}if(t.operation.action==app::FileOperationAction::rename)r.file.name=t.operation.filename;}}
        if(t.kind==app::FileManagementTaskKind::open_view){out.journal_confirmed=false;app::FileManagementViewPage view;view.cursor=t.job;view.file=t.view_file;view.revision=1;view.filename="synthetic.zip";view.archive=true;out.view=view;}
        if(t.kind==app::FileManagementTaskKind::close_view)out.journal_confirmed=false;
        if(t.kind==app::FileManagementTaskKind::scan_pending){out.detail="Scan finished. Added: 2.\n";out.report=app::FileManagementJobStatus::Report{0,2};}
        if(t.kind==app::FileManagementTaskKind::scan_report){out.journal_confirmed=false;out.detail=t.view_page?"Added: second.zip [Main / Uploads]\n":"Added: first.zip [Main / General]\n";out.report=app::FileManagementJobStatus::Report{t.view_page,2};}
        p->jobs[t.job.bytes]=out;if(p->lose_next){p->lose_next=false;return {};}return out;
    }
    app::FileManagementJobStatus status(app::Identifier128 id)override{std::lock_guard<std::mutex> lock(p->mutex);const auto found=p->jobs.find(id.bytes);return found==p->jobs.end()?app::FileManagementJobStatus{app::LocalAccountResultCode::policy_rejected}:found->second;}
    app::FileManagementJobStatus cancel(app::Identifier128 id)override{return status(id);}
};
void pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){if(m.message==WM_QUIT)continue;TranslateMessage(&m);DispatchMessageW(&m);}}
template<class F>void until(ui::State& state,F predicate){const auto deadline=GetTickCount64()+30000;while(GetTickCount64()<deadline){pump();state.tick();if(predicate())return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}throw std::runtime_error("native UI deadline");}
struct TestAppearance {
    std::wstring key=L"Software\\Spitfire-X BBS\\Tests\\FM-Polish-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    TestAppearance(){HKEY handle{};DWORD disposition{};check(RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&handle,&disposition)==ERROR_SUCCESS&&disposition==REG_CREATED_NEW_KEY,"fresh isolated appearance key");const DWORD dark=1;check(RegSetValueExW(handle,L"Theme",0,REG_DWORD,reinterpret_cast<const BYTE*>(&dark),sizeof(dark))==ERROR_SUCCESS,"isolated dark preference");RegCloseKey(handle);}
    void classic(){HKEY handle{};check(RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_SET_VALUE,&handle)==ERROR_SUCCESS,"owned test preference");const DWORD theme=0;check(RegSetValueExW(handle,L"Theme",0,REG_DWORD,reinterpret_cast<const BYTE*>(&theme),sizeof(theme))==ERROR_SUCCESS,"isolated Classic preference");RegCloseKey(handle);}
    ~TestAppearance(){RegDeleteKeyW(HKEY_CURRENT_USER,key.c_str());}
};
struct ModalDriver {
    DWORD thread_id=GetCurrentThreadId();std::atomic<bool> stop{};std::atomic<int> answer{IDCANCEL};std::atomic<unsigned> confirmations{},summaries{},details{};std::atomic<bool> saw_warning{},saw_count{},details_readonly{},details_list_enabled{},modal_draw{},scan_complete{};std::atomic<unsigned> scans{},forms{};std::atomic<bool> forms_fit{true};
    std::thread thread;
    ModalDriver():thread([this]{while(!stop){EnumThreadWindows(thread_id,[](HWND h,LPARAM data)->BOOL{auto& self=*reinterpret_cast<ModalDriver*>(data);wchar_t title[128]{};GetWindowTextW(h,title,128);const std::wstring name=title;
            if(name==L"Search Filenames"||name==L"Rename File"||name==L"Move File"){if(!GetDlgItem(h,IDOK)||!GetDlgItem(h,IDCANCEL))return TRUE;RECT client{};GetClientRect(h,&client);for(const auto id:{IDOK,IDCANCEL}){RECT button{};GetWindowRect(GetDlgItem(h,id),&button);MapWindowPoints(nullptr,h,reinterpret_cast<POINT*>(&button),2);if(button.left<0||button.top<0||button.right>client.right||button.bottom>client.bottom)self.forms_fit=false;}++self.forms;picture(h,name==L"Search Filenames"?"search-form":name==L"Rename File"?"rename-form":"move-form");SendMessageW(h,WM_COMMAND,IDCANCEL,0);return TRUE;}
            if(name!=L"Confirm File Removal"&&name!=L"File Removal Results"&&name!=L"File Details"&&name!=L"File Scan Results"&&name!=L"File Manager Help")return TRUE;ShowWindow(h,SW_HIDE);const auto edit=GetDlgItem(h,101);if(!edit||!GetDlgItem(h,IDOK)||(name==L"Confirm File Removal"&&!GetDlgItem(h,IDCANCEL)))return TRUE;
            picture(h,name==L"Confirm File Removal"?(self.answer==IDOK?"confirm-yes":"confirm-no"):name==L"File Details"?(self.details?"details-classic":"details-dark"):name==L"File Scan Results"?"scan-results":name==L"File Manager Help"?"help":"removal-results");
            if(name==L"File Manager Help"){SendMessageW(h,WM_COMMAND,IDOK,0);return TRUE;}
            const auto button=GetDlgItem(h,IDOK);const auto screen=GetDC(h);const auto dc=CreateCompatibleDC(screen);const auto bitmap=CreateCompatibleBitmap(screen,200,40);const auto saved=SelectObject(dc,bitmap);NMCUSTOMDRAW draw{};draw.hdr={button,IDOK,NM_CUSTOMDRAW};draw.hdc=dc;draw.rc={0,0,100,28};draw.dwDrawStage=CDDS_PREPAINT;SendMessageW(h,WM_NOTIFY,IDOK,reinterpret_cast<LPARAM>(&draw));self.modal_draw=GetWindowLongPtrW(h,DWLP_MSGRESULT)==CDRF_SKIPDEFAULT;SelectObject(dc,saved);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(h,screen);
            if(name==L"File Scan Results"){const auto length=GetWindowTextLengthW(edit);std::wstring text(static_cast<std::size_t>(length)+1,L'\0');GetWindowTextW(edit,text.data(),length+1);self.scan_complete=text.find(L"Added: 2")!=text.npos&&text.find(L"first.zip [Main / General]")!=text.npos&&text.find(L"second.zip [Main / Uploads]")!=text.npos;++self.scans;SendMessageW(h,WM_COMMAND,IDOK,0);}
            else if(name==L"Confirm File Removal"){const auto length=GetWindowTextLengthW(edit);std::wstring text(static_cast<std::size_t>(length)+1,L'\0');GetWindowTextW(edit,text.data(),length+1);self.saw_warning=text.find(L"Are you sure you want to remove the listed file(s)?")!=text.npos;self.saw_count=text.find(L"Files selected: 650")!=text.npos;++self.confirmations;SendMessageW(h,WM_COMMAND,self.answer,0);}
            else if(name==L"File Details"){self.details_list_enabled=IsWindowEnabled(GetDlgItem(GetWindow(h,GW_OWNER),ui::entries))!=FALSE;self.details_readonly=(GetWindowLongPtrW(edit,GWL_STYLE)&ES_READONLY)!=0;++self.details;SendMessageW(h,WM_COMMAND,IDOK,0);}
            else{++self.summaries;SendMessageW(h,WM_COMMAND,IDOK,0);}return TRUE;},reinterpret_cast<LPARAM>(this));std::this_thread::sleep_for(std::chrono::milliseconds(5));}}){}
    ~ModalDriver(){stop=true;thread.join();}
};
}
int main(int argc,char** argv)try{
    const int dpi=argc>=2?std::stoi(argv[1]):96;if(argc==3){pictures=std::filesystem::absolute(argv[2]);check(!std::filesystem::exists(pictures),"fresh generated visual evidence");std::filesystem::create_directory(pictures);}check(dpi==96||dpi==144,"supported test DPI");INITCOMMONCONTROLSEX init{sizeof(init),ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES};check(InitCommonControlsEx(&init)!=FALSE,"native controls");
    const auto instance=GetModuleHandleW(nullptr);auto ports=std::make_shared<Ports>();for(unsigned n=1;n<=650;++n)ports->files.push_back(row(n));ports->files.push_back(row(651,2));
    TestAppearance isolated_theme;
    ui::State state(instance,L"C:\\not-a-section9-ui-install",L"fake-owner",[ports](const auto&,const auto&,std::string&)->std::unique_ptr<win::IFileAdministrationClient>{return std::make_unique<Fake>(ports);},isolated_theme.key);state.persist_placement=false;
    WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=ui::State::proc;cls.hInstance=instance;cls.lpszClassName=ui::manager_class;check(RegisterClassExW(&cls)!=0,"register product window");auto window=CreateWindowExW(0,ui::manager_class,L"Hidden workflow model",WS_OVERLAPPEDWINDOW,10,10,1030,710,nullptr,nullptr,instance,&state);check(window!=nullptr,"create hidden product controls");
    until(state,[&]{return state.available();});state.dpi=dpi;state.layout();
    picture(window,"home");
    check(GetMenuItemCount(GetMenu(window))==2&&GetMenuItemCount(GetSubMenu(GetMenu(window),0))==1,"no FM View/Theme or extra recovery menu entries");
    check(state.screen==ui::Screen::home&&state.rows.empty(),"startup contains operation choices and no catalog");
    for(const auto id:{ui::browse,ui::search_files,ui::pending_files,ui::offline_files})check((GetWindowLongPtrW(state.control(id),GWL_STYLE)&WS_VISIBLE)!=0,"four startup choices visible");
    check(!(GetWindowLongPtrW(state.control(ui::entries),GWL_STYLE)&WS_VISIBLE),"file list hidden at startup");
    check(!(GetWindowLongPtrW(state.control(ui::entries),GWL_STYLE)&LVS_SINGLESEL),"native list admits Ctrl/Shift multiple selection");
    state.command(ui::browse);picture(window,"groups");check(state.screen==ui::Screen::groups&&state.groups.size()==1,"Browse opens groups");state.navigate();picture(window,"areas");check(state.screen==ui::Screen::areas&&state.areas.size()==2,"group opens its areas");state.navigate();until(state,[&]{return state.available()&&state.rows.size()==650;});
    picture(window,"files");
    {const auto header=ListView_GetHeader(state.control(ui::entries));RECT client{};GetClientRect(header,&client);const auto screen=GetDC(header),dc=CreateCompatibleDC(screen);const auto bitmap=CreateCompatibleBitmap(screen,client.right,client.bottom);const auto saved=SelectObject(dc,bitmap);SendMessageW(header,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);check(GetPixel(dc,client.right-3,client.bottom/2)==state.appearance.palette().surface,"actual native header trailing space uses shared palette, not white");SelectObject(dc,saved);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(header,screen);}
    check(state.screen==ui::Screen::files&&state.area.value==1&&ListView_GetItemCount(state.control(ui::entries))==650&&ports->reads>16,"all area files collected beyond native/client page boundaries");
    state.command(ui::contents);until(state,[&]{return state.available()&&state.viewport.has_value();});check(state.preview->window()!=nullptr,"actual modeless ZIP preview opens through typed port");picture(state.preview->window(),"zip-preview");ShowWindow(state.preview->window(),SW_HIDE);
    BYTE previous[256]{};GetKeyboardState(previous);BYTE keyboard[256]{};keyboard[VK_CONTROL]=0x80;SetKeyboardState(keyboard);SendMessageW(state.control(ui::entries),WM_KEYDOWN,'A',0);SetKeyboardState(previous);
    check(state.selected().size()==650&&state.rows.back().file.area.value==1,"Ctrl+A selects entire area and excludes other area's file");
    check(!IsWindowEnabled(state.control(ui::rename_file)),"Rename disabled for multiple files");
    SendMessageW(state.control(ui::entries),WM_KEYDOWN,VK_SPACE,0);check(state.selected().size()==649,"Space toggles focused selection");SendMessageW(state.control(ui::entries),WM_KEYDOWN,VK_DOWN,0);check(state.selected().size()==649,"arrow navigation preserves tagged selections");
    ListView_SetItemState(state.control(ui::entries),-1,LVIS_SELECTED,LVIS_SELECTED);
    ModalDriver dialogs;state.command(ui::remove_file);check(dialogs.confirmations==1&&ports->applied==0&&state.rows.size()==650,"one No confirmation preserves all selected files");
    dialogs.answer=IDOK;state.command(ui::remove_file);until(state,[&]{return state.available()&&state.removals.empty()&&state.rows.empty();});
    check(dialogs.confirmations==2&&dialogs.summaries==1&&dialogs.saw_warning&&dialogs.saw_count&&ports->applied==650&&!state.viewport,"one Yes confirmation deletes650 files with an open ZIP preview, one summary, no extra close-view job or purge");
    check(!ports->files.back().purged&&ports->files.back().file.area.value==2,"other area stays unchanged");
    state.command(ui::back);check(state.screen==ui::Screen::areas,"Back returns to areas");
    state.screen=ui::Screen::files;state.area={2};state.load();until(state,[&]{return state.available()&&state.rows.size()==1;});
    ports->lose_next=true;app::FileManagementTask rename;rename.job=ui::fresh_id();rename.operation.operation=ui::fresh_id();rename.operation.file={651};rename.operation.expected_revision=1;rename.operation.action=app::FileOperationAction::rename;rename.operation.destination={2};rename.operation.expected_destination_revision=1;rename.operation.filename="renamed.zip";
    check(state.start(rename,ui::Purpose::ordinary),"start exact selected rename");until(state,[&]{return !state.worker->busy()&&state.tracking.state()==ui::JobTracking::unknown;});check(!state.available()&&IsWindowEnabled(state.control(ui::retry_same)),"unknown job freezes edits and offers exact retry");picture(window,"busy-readonly-list");check(IsWindowEnabled(state.control(ui::entries))&&!IsWindowEnabled(state.control(ui::rename_file)),"busy snapshot keeps normal palette while actions remain disabled");const auto previous_focus=state.focused();SendMessageW(state.control(ui::entries),WM_KEYDOWN,VK_DOWN,0);check(state.focused()==previous_focus,"busy list input is blocked without disabling native painting");state.command(ui::retry_same);until(state,[&]{return state.available()&&state.rows.size()==1&&state.rows.front().file.name=="renamed.zip";});check(ports->applied==651,"same-job retry does not repeat mutation");
    state.command(ui::details);until(state,[&]{return state.available()&&dialogs.details==1;});check(dialogs.details_readonly&&dialogs.details_list_enabled,"Details is read-only and restores the underlying enabled list before modal paint");check(dialogs.modal_draw,"modal custom drawing uses DWLP_MSGRESULT for readable buttons");
    state.command(ui::pending_files);until(state,[&]{return state.available()&&dialogs.scans==1;});check(dialogs.scan_complete,"all bounded scan-result pages appear with summary and exact Group/Area");
    state.command(40001);state.command(ui::search_files);state.command(ui::rename_file);
    state.screen=ui::Screen::search;state.area={};state.show();picture(window,"search-results");state.command(ui::move_file);SetKeyboardState(keyboard);state.key('A');SetKeyboardState(previous);check(state.selected().size()==1,"Ctrl+A does not select across all File Areas in search mode");check(dialogs.forms==3&&dialogs.forms_fit,"Search/Rename/Move forms keep complete buttons inside client bounds without unnecessary scrollbars");
    isolated_theme.classic();SendMessageW(window,win::DesktopAppearance::changed_message(),0,0);check(state.appearance.theme()==win::DesktopTheme::classic,"FM follows shared theme invalidation without a separate setting");check(ListView_GetBkColor(state.control(ui::entries))==state.appearance.palette().panel,"Classic list colors refresh");state.command(win::DesktopAppearance::dark_command);check(state.appearance.theme()==win::DesktopTheme::classic,"removed theme commands cannot save an FM override");state.command(ui::legacy_files);state.command(ui::recovery_files);check(state.screen==ui::Screen::search,"removed recovery menu commands cannot start hidden cleanup");
    picture(window,"files-classic");
    ui::show_readonly(window,state.appearance,L"File Details",L"Classic read-only details");check(dialogs.details==2&&!dialogs.modal_draw,"Classic uses native button paint rather than dark custom drawing");
    state.close();until(state,[&]{return !state.window;});state.worker.reset();state.preview.reset();UnregisterClassW(ui::manager_class,instance);check(ports->destroyed==1,"owner client shuts down normally");
    std::cout<<"PASS File Manager workflow "<<checks<<" checks at "<<dpi<<" DPI; actual hidden controls/fake owner ports; modal Yes/No and650-row area selection\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL File Manager workflow after "<<checks<<": "<<e.what()<<'\n';return 1;}
