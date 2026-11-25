// Focused real native controls/painters and drafts. Synthetic presentation and
// existing test-only catalog ports; not shipping IPC/installed acceptance.
#define SFX_ALPHA_TEST_AUTOMATION 1
#define wWinMain unused_config_entry
#include "../../src/apps/config_ui/main.cpp"
#undef wWinMain
#include <iostream>
namespace fs=std::filesystem;

namespace {
unsigned checks{},changes{},dialogs{};bool confirm{};
app::MessageConfigurationSnapshot fixture;
app::MessageConfigurationChange last_change;
fs::path installed_root,capture_output;
unsigned deletion_mode{},deletion_dialogs{};bool exact_dialogs=true,progress_cancel{},progress_clicked{};
std::vector<std::wstring> deletion_questions;
bool capture(HWND window,const fs::path& path);
BOOL CALLBACK dialog_text(HWND child,LPARAM value){auto& out=*reinterpret_cast<std::wstring*>(value);const auto line=text(child);if(!line.empty()){out+=line;out+=L"\n";}return TRUE;}
void CALLBACK finish_deletion_dialog(HWND target,UINT,UINT_PTR timer,DWORD){
    if(!IsWindowVisible(target))return;KillTimer(target,timer);
    const auto title=text(target);++deletion_dialogs;std::wstring question;EnumChildWindows(target,dialog_text,reinterpret_cast<LPARAM>(&question));deletion_questions.push_back(title+L"\n"+question);
    win32::RuntimeInstanceGuard attempted;exact_dialogs=exact_dialogs && !attempted.acquire(installed_root,install::operator_sid()).succeeded();
    exact_dialogs=exact_dialogs && capture(target,capture_output/(L"installed-deletion-"+std::to_wstring(deletion_dialogs)+L".bmp"));
    if(title==L"Delete Area and Messages — Default No"){
        const auto default_id=LOWORD(SendMessageW(target,DM_GETDEFID,0,0));
        exact_dialogs=exact_dialogs && (default_id==IDNO || GetDlgCtrlID(GetFocus())==IDNO) && question.find(L"Public Chat")!=std::wstring::npos && question.find(L"Permanent messages INCLUDED: 2")!=std::wstring::npos;
    }
    const auto answer=title==L"Optional Deletion Backup" && deletion_mode==1?IDCANCEL:IDNO;
    PostMessageW(GetDlgItem(target,answer),BM_CLICK,0,0);
}
void require(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);std::cout<<"PASS "<<label<<std::endl;}
LRESULT CALLBACK hook(int code,WPARAM window,LPARAM data){
    if(code==HCBT_ACTIVATE){const auto target=reinterpret_cast<HWND>(window);const auto title=text(target);
        if(title==L"Review Message Catalog" || title==L"Review Clone Options" || title==L"Review Unread-Mail Age"){
            ++dialogs;PostMessageW(GetDlgItem(target,confirm?IDYES:IDNO),BM_CLICK,0,0);
        }else if(deletion_mode && (title==L"Optional Deletion Backup" || title==L"Delete Area and Messages — Default No")){
            // HCBT_ACTIVATE precedes the first visible paint. Defer capture and
            // the non-destructive answer to a native UI event after visibility;
            // never present the retained black pre-paint bitmap as visual PASS.
            exact_dialogs=exact_dialogs && SetTimer(target,0x8c08,1,finish_deletion_dialog)!=0;
        }else if(progress_cancel && title==kWindowTitle){progress_clicked=true;PostMessageW(target,TDM_CLICK_BUTTON,IDCANCEL,0);}
        }return CallNextHookEx(nullptr,code,window,data);
}
LRESULT CALLBACK review_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam){if(message==WM_CREATE)return 0;return window_proc(window,message,wparam,lparam);}
bool capture(HWND window,const fs::path& path){
    RECT r{};GetWindowRect(window,&r);const auto width=r.right-r.left,height=r.bottom-r.top;const auto screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);const auto bitmap=CreateCompatibleBitmap(screen,width,height);const auto old=SelectObject(dc,bitmap);
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);const bool printed=PrintWindow(window,dc,2)!=FALSE;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width)*height*4);SelectObject(dc,old);const bool read=GetDIBits(dc,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS)!=0;
    DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);if(!printed || !read)return false;
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size());
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(&header),sizeof(header));out.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(BITMAPINFOHEADER));out.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());return out.good();
}
}
int wmain(int argc,wchar_t** argv){
    State state;state.instance=GetModuleHandleW(nullptr);const auto original=state.appearance.theme();HHOOK dialog{};
    try{
        require(argc==3 || argc==4,"new output path and bounded DPI required");const auto output=fs::absolute(argv[1]).lexically_normal();
        require(output.parent_path()==fs::path(SFX_UI_REVIEW_SCRATCH) && output.filename().native().rfind(L"section8c-config-",0)==0 && !fs::exists(output),"fresh focused native evidence path only");
        require(fs::create_directories(output),"new native evidence directory");state.dpi=std::stoi(argv[2]);require(state.dpi==96 || state.dpi==144,"bounded 96/144DPI");state.root=output/L"synthetic-not-installation";
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};InitCommonControlsEx(&common);WNDCLASSW cls{};cls.hInstance=state.instance;cls.lpfnWndProc=review_proc;cls.lpszClassName=kWindowClass;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        require(RegisterClassW(&cls)!=0,"native Config class registration");const auto window=CreateWindowW(kWindowClass,L"Section8C Config review — synthetic settings",WS_OVERLAPPEDWINDOW,20,20,MulDiv(980,state.dpi,96),MulDiv(750,state.dpi,96),nullptr,nullptr,state.instance,&state);
        require(window && create_ui(state),"actual Config controls and pages");SetMenu(window,create_menu(state));state.mode=State::ConnectionMode::offline;
        state.configuration.saved.bbs_name="Section8C presentation fixture";state.configuration.saved.bbs_time_zone="Pacific Standard Time";state.configuration.active=state.configuration.saved;state.scheduler.configuration_loaded=true;state.backup_settings=app::SchedulerConfiguration{};
        fixture.revision=1;fixture.groups={{{1},"Local",0,1,1,true}};fixture.echoes={{{1},{1},1,"Sysop Notices",0,1},{{2},{1},2,"Public Chat",1,1}};
        message_query_fixture=[](const app::LocalMessageConfigurationQuery& q){app::LocalMessageConfigurationSnapshot result;result.code=app::LocalAccountResultCode::ok;result.catalog=fixture;if(q.group)result.catalog.groups.clear();else result.catalog.echoes.clear();return result;};
        message_change_fixture=[](const app::MessageConfigurationChange& c){++changes;last_change=c;app::LocalMessageConfigurationResult result;result.code=app::LocalAccountResultCode::ok;
            ++fixture.revision;result.result.configuration_revision=fixture.revision;
            if(c.action==app::MessageConfigurationAction::echo){for(auto& e:fixture.echoes)if(e.id==c.echo.id){e=c.echo;++e.revision;}}
            if(c.action==app::MessageConfigurationAction::unread_mail_retention){fixture.unread_mail_age_days=c.unread_mail_age_days;++fixture.unread_retention_revision;}return result;};
        populate_controls(state);show_page(state,19);refresh_message_configuration(state);state.editing_echo=true;state.editing_message_echo=fixture.echoes[1];populate_message_form(state);remember_page(state);
        require(text(state.message_max_count)==L"1000" && text(state.message_max_age)==L"Unlimited","actual per-echo Count/Age defaults displayed");
        SetWindowTextW(state.message_max_count,L"2");require(page_dirty(state),"count changes ordinary draft state");discard_page(state);require(!page_dirty(state) && text(state.message_max_count)==L"1000","discard exact count without save/purge");
        dialog=SetWindowsHookExW(WH_CBT,hook,nullptr,GetCurrentThreadId());require(dialog!=nullptr,"local native confirmation hook");
        for(const auto* invalid:{L"0",L"-1",L"1.5",L"1000000000",L"9999999999",L""}){SetWindowTextW(state.message_max_count,invalid);const auto before=changes;require(!save_message_catalog(state) && changes==before,"invalid count refused with draft retained and no authority apply");}
        SetWindowTextW(state.message_max_count,L"999999999");SetWindowTextW(state.message_max_age,L"65535");confirm=false;const auto before=changes;
        require(!save_message_catalog(state) && changes==before,"final catalog No preserves finite-endpoint draft");confirm=true;
        require(save_message_catalog(state) && last_change.retention_mask==3 && last_change.echo.retention.maximum_count==std::optional<std::uint32_t>{999999999} && last_change.echo.retention.maximum_age_days==std::optional<std::uint32_t>{65535},"native validated endpoint save submits explicit independent policies, no content");
        show_page(state,20);require(Button_GetCheck(state.clone_max_count)==BST_UNCHECKED && Button_GetCheck(state.clone_max_age)==BST_UNCHECKED,"both retention clone boxes begin unchecked");state.clone_source=fixture.echoes[1];state.clone_targets={fixture.echoes[0]};
        Button_SetCheck(state.clone_max_count,BST_CHECKED);auto clone=clone_preview(state);require(clone && clone->retention_mask==1 && clone->permission_mask==0 && text(state.clone_preview).find(L"Age ")==std::wstring::npos,"count-only clone preview excludes age/permission/private content");
        Button_SetCheck(state.clone_max_count,BST_UNCHECKED);Button_SetCheck(state.clone_max_age,BST_CHECKED);clone=clone_preview(state);require(clone && clone->retention_mask==2 && text(state.clone_preview).find(L"Count ")==std::wstring::npos,"age-only clone independent preview");
        show_page(state,5);state.selected_job=1;populate_message_jobs(state); // synthetic unavailable port remains disabled, never a clean claim
        require(!(GetWindowLongPtrW(state.backup_destination,GWL_STYLE)&WS_VISIBLE) && !IsWindowEnabled(state.message_run_now),"message route hides backup fields and refuses unavailable/live manual actions");
        state.message_job_status={};state.message_job_status.available=true;state.message_job_status.configuration=app::default_message_scheduler_configuration(app::kMessageRetentionJobId);state.message_job_status.state=app::default_message_scheduler_state(app::kMessageRetentionJobId);state.message_job_status.shared_verification_required=true;state.message_job_status.holds={{2,1}};
        app::MessageMaintenanceRun last;last.id.bytes[0]=1;last.started_utc=1791716400;last.finished_utc=last.started_utc+3;last.outcome=app::MessageMaintenanceOutcome::partial;last.reason=app::MessageMaintenanceReason::storage_failure;last.removed_public=9;last.expired_unread=2;last.committed_batches=1;last.echoes={{{2},9,4}};state.message_job_status.latest=last;
        render_message_job(state);SetWindowTextW(state.schedule_hour,L"0");SetWindowTextW(state.schedule_minute,L"0");SetWindowTextW(state.unread_mail_age,L"365");
        require(text(state.schedule_detail).find(L"PARTIAL")!=std::wstring::npos && text(state.schedule_detail).find(L"Public removed: 9")!=std::wstring::npos && text(state.schedule_detail).find(L"Permanent overage 4")!=std::wstring::npos && text(state.schedule_detail).find(L"HOLD")!=std::wstring::npos,"partial committed counts/protected overage/current holds shown, no success substitution");
        remember_page(state);SetWindowTextW(state.unread_mail_age,L"Unlimited");require(page_dirty(state),"unread policy ordinary draft protection");discard_page(state);require(text(state.unread_mail_age)==L"365" && !page_dirty(state),"discard global unread age exact");
        SetWindowTextW(state.schedule_hour,L"2");remember_page(state);SetWindowTextW(state.schedule_hour,L"3");state.private_mail_policy=message_query_fixture({});SetWindowTextW(state.unread_mail_age,L"Unlimited");
        require(save_unread_mail_age(state) && last_change.action==app::MessageConfigurationAction::unread_mail_retention && !last_change.unread_mail_age_days && page_dirty(state) && text(state.schedule_hour)==L"3","private-age Save is narrow and never marks unrelated schedule draft saved");discard_page(state);
        state.editing_echo=true;state.editing_message_echo=fixture.echoes[1];populate_message_form(state);
        for(const auto theme:{win32::DesktopTheme::classic,win32::DesktopTheme::dark_retro}){
            require(state.appearance.save(theme),"temporary actual theme save");state.appearance.apply_controls(window);ShowWindow(window,SW_SHOW);const auto prefix=theme==win32::DesktopTheme::classic?L"classic":L"dark";
            for(const auto page_index:{5,19,20}){show_page(state,page_index);RECT client{};GetClientRect(window,&client);layout(state,client.right,client.bottom);require(capture(window,output/(std::wstring(prefix)+L"-page-"+std::to_wstring(page_index)+L".bmp")),"real touched native page render");}
            SetWindowPos(window,nullptr,0,0,MulDiv(780,state.dpi,96),MulDiv(610,state.dpi,96),SWP_NOMOVE|SWP_NOZORDER);show_page(state,19);RECT client{};GetClientRect(window,&client);layout(state,client.right,client.bottom);
            require(capture(window,output/(std::wstring(prefix)+L"-minimum.bmp")),"minimum-size scrollable touched page render");SetWindowPos(window,nullptr,0,0,MulDiv(980,state.dpi,96),MulDiv(750,state.dpi,96),SWP_NOMOVE|SWP_NOZORDER);
        }
        if(argc==4){
            installed_root=fs::absolute(argv[3]).lexically_normal();capture_output=output;
            require(installed_root.parent_path()==fs::path(L"C:\\") && installed_root.filename().native().rfind(L"sfxbuilds-section8c-qualified-r",0)==0,"closed new installed UI fixture, never live/old root");
            install::verify_runtime_installation(installed_root,install::operator_sid());state.root=installed_root;state.owner=install::operator_sid();state.receipt=install::read_receipt(state.root,state.owner);
            message_query_fixture={};message_change_fixture={};state.mode=State::ConnectionMode::offline;
            SetWindowTextW(window,L"Section8C genuine installed controls — non-destructive review");
            const auto real=win32::offline_message_configuration_snapshot(state.root,state.owner,{app::MessageGroupId{1},{}});require(real.code==app::LocalAccountResultCode::ok,"actual current offline catalog for native confirmation");
            const auto selected=std::find_if(real.catalog.echoes.begin(),real.catalog.echoes.end(),[](const auto& e){return e.id.value==2;});require(selected!=real.catalog.echoes.end(),"actual populated Public Chat target");
            const app::MessageDeletionTarget target{app::MessageDeletionKind::echo,selected->id.value,selected->revision};
            show_page(state,19);remember_page(state);deletion_mode=1;delete_public_area_ui(state,target);
            require(deletion_dialogs==1 && text(state.status).find(L"Cancelled")!=std::wstring::npos,"actual optional backup Cancel stops before final deletion");
            deletion_mode=2;delete_public_area_ui(state,target);deletion_mode=0;
            require(deletion_dialogs==3 && exact_dialogs && text(state.status).find(L"Cancelled")!=std::wstring::npos,"actual optional No plus separate default-No exact-name/count/Permanent dialog; guard retained at both");
            std::string diagnostic;auto unchanged=win32::OfflineMessageDeletion::begin(state.root,state.owner,target,diagnostic);
            require(unchanged && unchanged->preview().message_count==2 && unchanged->preview().permanent_count==2,"native No/Cancel preserve all selected configuration/content/Permanent");unchanged.reset();
            show_page(state,5);state.selected_job=2;populate_message_jobs(state);remember_page(state);
            require(state.message_job_status.available && IsWindowEnabled(state.schedule_weekday) && SendMessageW(state.schedule_weekday,CB_GETCOUNT,0,0)==7,"real offline weekly schedule exposes all seven editable days");
            for(const auto theme:{win32::DesktopTheme::classic,win32::DesktopTheme::dark_retro}){
                require(state.appearance.save(theme),"temporary weekly route theme");state.appearance.apply_controls(window);RECT client{};GetClientRect(window,&client);layout(state,client.right,client.bottom);
                require(capture(window,output/(theme==win32::DesktopTheme::classic?L"installed-integrity-classic.bmp":L"installed-integrity-dark.bmp")),"genuine weekly day/time/current outcome native render");}
            progress_cancel=true;std::atomic<bool> observed{false};
            require(maintenance_progress(state,L"8C cancellation control",[&](std::function<bool()> stop){const auto limit=GetTickCount64()+10000;while(!stop()){if(GetTickCount64()>=limit)throw std::runtime_error("native cancel not delivered");std::this_thread::yield();}observed.store(true);}),"native responsive progress waits for safe worker cancellation");progress_cancel=false;
            require(progress_clicked && observed.load() && !state.account_action_active,"native Cancel closes only after joined worker, no abandoned operation");
            std::ofstream questions(output/L"installed-confirmation-text.txt");for(const auto& q:deletion_questions)questions<<wide_to_utf8(q).value_or("invalid UTF8")<<"\n";
        }
        UnhookWindowsHookEx(dialog);dialog=nullptr;DestroyWindow(window);require(state.appearance.save(original),"original operator theme restored");
        std::ofstream receipt(output/L"RESULT.txt");receipt<<"PASS Section8C native controls checks="<<checks<<" dpi="<<state.dpi<<" confirmations="<<dialogs<<"\nSynthetic display/catalog ports; not installed/IPC or owner physical platform acceptance.\n";
        std::cout<<"PASS Section8C focused Config checks="<<checks<<std::endl;return 0;
    }catch(const std::exception& e){if(dialog)UnhookWindowsHookEx(dialog);(void)state.appearance.save(original);std::cerr<<"FAIL Section8C Config review: "<<e.what()<<std::endl;return 1;}
}
