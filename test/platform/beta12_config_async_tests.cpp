// Real hidden Config HWND/controls/WM_TIMER dispatch with synthetic results.
// The final status-only worker reads an explicitly absent fixture. No updater,
// vendor connection, installed board, service, saved setting or dialog is used.
#define SFX_ALPHA_TEST_AUTOMATION 1
#define wWinMain retained_beta12_config_entry
#include "fixture_paths.hpp"
#include "../../src/apps/config_ui/main.cpp"
#undef wWinMain
#include <iostream>

namespace {
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
LRESULT CALLBACK hidden_config_proc(HWND window,UINT message,WPARAM a,LPARAM b){
    if(message==WM_CREATE)return 0; // Build controls without first-run/installation I/O.
    return window_proc(window,message,a,b);
}
std::shared_ptr<State::ClamJob> completed(State& state,win32::ClamAvStatus status){
    auto job=std::make_shared<State::ClamJob>();job->status=std::move(status);job->done=true;
    state.clam_job=job;state.clam_status_read=State::ClamStatusRead::checking;render_clam(state);
    return job;
}
void tick(State& state,UINT_PTR timer){SendMessageW(state.window,WM_TIMER,timer,0);}
void settled(State& state){
    check(!state.clam_job&&!clam_busy(state),"completed worker collected before button rendering");
    check(IsWindowEnabled(state.clam_refresh)&&!IsWindowEnabled(state.clam_cancel),"Refresh restored and Cancel disabled after collection");
    check(IsWindowEnabled(state.file_editor.scan)&&IsWindowEnabled(state.clam_save),"existing ready policy controls restored after collection");
}
}

int main(){State state;state.instance=GetModuleHandleW(nullptr);try{
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_WIN95_CLASSES};check(InitCommonControlsEx(&common)!=FALSE,"common native controls");
    WNDCLASSW cls{};cls.hInstance=state.instance;cls.lpfnWndProc=hidden_config_proc;cls.lpszClassName=kWindowClass;
    check(RegisterClassW(&cls)!=0,"owned hidden Config window class");
    state.window=CreateWindowW(kWindowClass,L"Beta12 async Config checks",WS_OVERLAPPEDWINDOW,0,0,980,750,nullptr,nullptr,state.instance,&state);
    check(state.window&&create_ui(state),"actual Config controls created without first-run actions");
    check(!IsWindowVisible(state.window)&&win32::clamav_available(),"supported Windows hidden fixture only");
    show_page(state,17);
    const auto folder=GetDlgItem(state.pages[17],IDC_MENU_FOLDER), doors=GetDlgItem(state.pages[17],IDC_CHAT_COMMANDS);
    RECT save_rect{},folder_rect{},door_rect{},switch_rect{},style_rect{};
    check(GetWindowRect(state.menu_save,&save_rect)&&GetWindowRect(folder,&folder_rect)&&GetWindowRect(doors,&door_rect),"native display action rectangles");
    check(save_rect.top==folder_rect.top&&folder_rect.top==door_rect.top&&save_rect.right<folder_rect.left&&folder_rect.right<door_rect.left,
        "Door Commands immediately follows Open Display Folder on Save row without overlap");
    check(GetWindowRect(state.private_pipe_colors,&switch_rect)&&GetWindowRect(state.menu_pipe_style,&style_rect)&&switch_rect.top==style_rect.top&&switch_rect.right<style_rect.left&&
        GetParent(state.private_pipe_colors)==state.pages[17]&&!GetDlgItem(state.pages[0],IDC_PRIVATE_PIPE_COLORS),"private check and style share a distinct nonoverlapping row on Displays & Menus");
    message_query_fixture=[](const app::LocalMessageConfigurationQuery&){
        app::LocalMessageConfigurationSnapshot result;
        result.code=app::LocalAccountResultCode::ok;
        result.catalog.revision=1;result.catalog.private_presentation_revision=1;
        result.catalog.private_pipe_colors=false;
        return result;
    };
    refresh_private_pipe_colors(state);
    check(IsWindowEnabled(state.private_pipe_colors)&&Button_GetCheck(state.private_pipe_colors)==BST_UNCHECKED,"saved private switch loads independently of style");
    SendMessageW(state.menu_pipe_style,CB_SETCURSEL,0,0);remember_page(state);
    SendMessageW(state.menu_pipe_style,CB_SETCURSEL,1,0);
    message_change_fixture=[](const app::MessageConfigurationChange& change){
        check(change.action==app::MessageConfigurationAction::private_presentation&&change.private_pipe_colors,
            "private switch asks to commit the displayed change");
        return app::LocalMessageConfigurationResult{app::LocalAccountResultCode::ok,{2,1,2,true}};
    };
    toggle_private_pipe_colors(state);
    check(Button_GetCheck(state.private_pipe_colors)==BST_CHECKED&&changed_control(state,state.menu_pipe_style)&&
        text(state.status).find(L"Saved")==0,"private switch saves without a dialog and preserves the unsaved style");
    refresh_private_pipe_colors(state);
    message_change_fixture=[](const app::MessageConfigurationChange&){return app::LocalMessageConfigurationResult{};};
    app::MessageConfigurationChange pipe;
    pipe.action=app::MessageConfigurationAction::private_presentation;pipe.expected_configuration_revision=1;
    pipe.expected_private_presentation_revision=1;pipe.private_pipe_colors=true;
    check(!apply_message_change(state,pipe)&&state.pending_message_apply&&!IsWindowEnabled(state.private_pipe_colors)&&
        Button_GetCheck(state.private_pipe_colors)==BST_UNCHECKED&&(GetWindowLongPtrW(state.private_pipe_reconcile,GWL_STYLE)&WS_VISIBLE),
        "unresolved private change keeps saved checkbox, freezes that action and exposes Reconcile on its page");
    message_change_fixture=[](const app::MessageConfigurationChange&){
        return app::LocalMessageConfigurationResult{app::LocalAccountResultCode::ok,{2,1,2,true}};
    };
    reconcile_message_apply(state);
    check(!state.pending_message_apply&&Button_GetCheck(state.private_pipe_colors)==BST_CHECKED&&changed_control(state,state.menu_pipe_style)&&
        !(GetWindowLongPtrW(state.private_pipe_reconcile,GWL_STYLE)&WS_VISIBLE),"confirmed private reconciliation preserves unsaved style choice");
    SendMessageW(state.menu_text,CB_SETCURSEL,6,0);SendMessageW(state.menu_input,CB_SETCURSEL,10,0);
    SendMessageW(state.menu_foreground,CB_SETCURSEL,0,0);SendMessageW(state.menu_background,CB_SETCURSEL,7,0);
    state.configuration.saved.bbs_time_zone="UTC";
    state.configuration.saved.security_policy_revision=1;
    state.configuration.saved.security_policy_hash=std::string(64,'a');
    const auto menu_candidate=candidate_for_page(state,17);
    check(menu_candidate&&menu_candidate->menus.legacy_message_pipe_colors,"existing Save maps Legacy SFX selector to saved operational setting");
    message_query_fixture=nullptr;message_change_fixture=nullptr;
    app::OperationalConfiguration previous,next;
    previous.maximum_connections_per_source=4;next.maximum_connections_per_source=2;
    check(!widens_connection_limits(previous,next),"ordinary source limit reduction requires no second review");
    next.source_connection_exceptions={{"192.0.2.1",5}};
    check(widens_connection_limits(previous,next),"a wider exception still requires review when the default is lowered");
    previous.source_connection_exceptions={{"192.0.2.1",1}};next.source_connection_exceptions.clear();
    check(widens_connection_limits(previous,next),"removing a restrictive exception can widen access");
    app::LocalAccountRequest password;
    password.password="Good fixture password";password.confirmation="Different fixture password";
    check(!password_fields_valid(state,password,{}),"mismatched password cannot reach the disconnection confirmation");
    password.confirmation=password.password;
    check(password_fields_valid(state,password,{}),"matching Standard password is accepted by the field check");
    app::erase_local_account_secrets(password);
    state.mode=State::ConnectionMode::offline;state.file_editor.ready=true;show_page(state,25);
    render_clam(state);
    check(text(state.clam_detail).find(L"not been read")!=std::wstring::npos&&text(state.clam_detail).find(L"Not installed")==std::wstring::npos,"unread status never fabricates missing engine");
    check(!IsWindowEnabled(state.clam_install)&&!IsWindowEnabled(state.clam_update),"unread status cannot authorize install or update");
    win32::ClamAvStatus absent;absent.diagnostic="clamav_not_installed";
    auto job=completed(state,absent);
    check(text(state.clam_detail)==L"Checking ClamAV status...","initial checking state is explicit");
    check(clam_busy(state)&&!IsWindowEnabled(state.clam_install)&&!IsWindowEnabled(state.clam_refresh),"pending completion stays owned and actions disabled");
    check(SetTimer(state.window,config_timer::clam_job,200,nullptr)!=0&&SetTimer(state.window,config_timer::ftn_setup,100,nullptr)!=0,"distinct ClamAV and FTN setup timers registered");
    tick(state,config_timer::ftn_setup);
    check(state.clam_job==job&&KillTimer(state.window,config_timer::clam_job)!=FALSE,"idle FTN setup tick cannot consume worker or kill ClamAV timer");
    tick(state,config_timer::clam_job);settled(state);
    check(state.clam_status_read==State::ClamStatusRead::ready&&text(state.clam_detail).find(L"Not installed")!=std::wstring::npos&&IsWindowEnabled(state.clam_install)&&!IsWindowEnabled(state.clam_update),"completed authoritative absence permits only offline install");

    win32::ClamAvStatus verified;verified.installed=true;verified.verified=true;verified.version="1.4.6";
    verified.definitions_present=true;verified.definitions_verified=true;
    job=completed(state,verified);job->thread=std::thread([]{});
    auto setup_job=std::make_shared<FtnEditorState::Setup::Load>();setup_job->done=true;state.ftn.setup.load=setup_job;
    tick(state,config_timer::clam_job);settled(state);
    check(!job->thread.joinable()&&state.ftn.setup.load==setup_job,"ClamAV completion joins its thread without consuming FTN completion");
    check(IsWindowEnabled(state.clam_update)&&!IsWindowEnabled(state.clam_install)&&IsWindowEnabled(state.clam_log),"verified engine restores update/log actions");
    tick(state,config_timer::ftn_setup);
    check(!state.ftn.setup.load,"FTN setup still independently collects its own result");
    auto retired=std::make_shared<FtnEditorState::Setup::Load>();retired->done=true;state.ftn.setup.retired.push_back(retired);
    job=completed(state,verified);tick(state,config_timer::ftn_setup);
    check(state.ftn.setup.retired.empty()&&state.clam_job==job,"retired FTN loader cleanup cannot steal ClamAV completion");
    tick(state,config_timer::clam_job);settled(state);

    auto imported=std::make_shared<FtnEditorState::ImportJob>();imported->done=true;imported->result.code=win32::FtnEchoListDownloadCode::cancelled;state.ftn.import_job=imported;
    check(SetTimer(state.window,config_timer::ftn_import,100,nullptr)!=0&&SetTimer(state.window,config_timer::message_maintenance,100,nullptr)!=0,"distinct FTN import and message-maintenance timers registered");
    state.selected_job=0;tick(state,config_timer::message_maintenance);
    check(state.ftn.import_job==imported&&KillTimer(state.window,config_timer::ftn_import)!=FALSE,"message-maintenance idle tick cannot consume import or kill its timer");
    tick(state,config_timer::ftn_import);
    check(!state.ftn.import_job&&IsWindowEnabled(GetDlgItem(state.pages[35],IDC_FTN_IMPORT_LOAD)),"FTN import independently collects cancelled result and restores controls");
    state.selected_job=1;state.mode=State::ConnectionMode::online;tick(state,config_timer::message_maintenance);
    check(text(state.status).find(L"Outcome unconfirmed")!=std::wstring::npos&&!state.message_job_status.available,"message-maintenance unavailable-channel branch is reachable, no invented success");
    state.selected_job=0;state.mode=State::ConnectionMode::offline;

    win32::ClamAvStatus unknown;unknown.diagnostic="clamav_status_unavailable";
    completed(state,unknown);tick(state,config_timer::clam_job);settled(state);
    check(state.clam_status_read==State::ClamStatusRead::failed&&text(state.clam_detail).find(L"Not installed")==std::wstring::npos&&!IsWindowEnabled(state.clam_install)&&!IsWindowEnabled(state.clam_update),"failed inspection stays unknown and cannot enable an engine action");
    completed(state,verified)->failed=true;tick(state,config_timer::clam_job);settled(state);
    check(text(state.status).find(L"background operation failed")!=std::wstring::npos&&!IsWindowEnabled(state.clam_update),"worker failure releases controls without stale verified authority");
    auto unverified=verified;unverified.verified=false;unverified.definitions_verified=false;
    completed(state,unverified);tick(state,config_timer::clam_job);settled(state);
    check(text(state.clam_detail).find(L"could not be verified")!=std::wstring::npos&&!IsWindowEnabled(state.clam_update)&&!IsWindowEnabled(state.clam_install),"unverified installation never authorizes updater or replacement install");
    job=completed(state,verified);win32::ClamAvUpdateResult current;current.succeeded=true;current.change=win32::ClamAvDefinitionChange::current;job->result=current;
    SendMessageW(state.window,WM_COMMAND,MAKEWPARAM(IDC_CLAM_CANCEL,BN_CLICKED),0);settled(state);
    check(text(state.status).find(L"Already current")!=std::wstring::npos,"late Cancel collects already-finished outcome instead of becoming a no-op");
    job=completed(state,verified);win32::ClamAvUpdateResult cancelled;cancelled.cancelled=true;cancelled.diagnostic="clamav_update_cancelled";job->result=cancelled;job->inspection_cancelled=true;
    tick(state,config_timer::clam_job);settled(state);
    check(text(state.status).find(L"Cancelled safely")!=std::wstring::npos&&state.clam_status_read==State::ClamStatusRead::cancelled&&!IsWindowEnabled(state.clam_update),"cancelled updater reports safe completion only with reaped child and cleanup result");
    for(const bool child_unconfirmed:{false,true}){
        job=completed(state,verified);auto retained=cancelled;retained.process_reaped=!child_unconfirmed;retained.cleanup_complete=child_unconfirmed;retained.diagnostic=child_unconfirmed?"clamav_update_child_exit_unconfirmed":"clamav_update_scratch_retained";job->result=retained;job->inspection_cancelled=true;
        tick(state,config_timer::clam_job);settled(state);
        check(text(state.status).find(L"Cancelled safely")==std::wstring::npos&&text(state.status).find(L"Not completed")!=std::wstring::npos,"unconfirmed child or retained scratch never claims safe cancellation");
    }

    auto saw_cancel=std::make_shared<std::atomic<bool>>(false),release=std::make_shared<std::atomic<bool>>(false);
    job=std::make_shared<State::ClamJob>();state.clam_job=job;state.clam_status_read=State::ClamStatusRead::checking;
    job->thread=std::thread([job,saw_cancel,release]{while(!job->cancel)Sleep(1);*saw_cancel=true;const auto end=GetTickCount64()+5000;while(!*release&&GetTickCount64()<end)Sleep(1);job->inspection_cancelled=true;job->done=true;});
    render_clam(state);SendMessageW(state.window,WM_COMMAND,MAKEWPARAM(IDC_CLAM_CANCEL,BN_CLICKED),0);
    check(job->cancel&&clam_busy(state)&&!IsWindowEnabled(state.clam_cancel)&&!IsWindowEnabled(state.clam_refresh),"Cancel requests owned worker shutdown and holds actions until cleanup completion");
    SendMessageW(state.window,WM_CLOSE,0,0);
    check(IsWindow(state.window)&&state.close_after_clam&&clam_busy(state),"normal close waits for actual worker completion");
    *release=true;const auto cancel_deadline=GetTickCount64()+5000;while(!job->done&&GetTickCount64()<cancel_deadline)Sleep(1);
    check(job->done&&*saw_cancel,"cooperative cancellation reached safe worker boundary");tick(state,config_timer::clam_job);settled(state);
    MSG pending{};check(PeekMessageW(&pending,state.window,WM_CLOSE,WM_CLOSE,PM_REMOVE)!=FALSE,"completion posts deferred close only after joining worker");state.close_after_clam=false;
    check(!job->thread.joinable()&&text(state.status).find(L"status check cancelled")!=std::wstring::npos,"cancelled status read has explicit outcome after join");

    state.root=(sfx::test::fixture_checkout() / L"build/beta12-config-absent-engine-status-r1");state.owner=install::operator_sid();
    check(!std::filesystem::exists(state.root),"explicit absent status fixture remains uncreated");
    start_clam_job(state,false);
    check(state.clam_job&&text(state.clam_detail)==L"Checking ClamAV status...","actual asynchronous status worker starts with truthful checking display");
    const auto deadline=GetTickCount64()+10000;
    while(state.clam_job&&GetTickCount64()<deadline){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessageW(&message);}}Sleep(5);}
    settled(state);check(state.clam_status_read==State::ClamStatusRead::ready&&state.clam_status.diagnostic=="clamav_not_installed"&&IsWindowEnabled(state.clam_install),"actual worker completion is collected through the native timer message pump");
    check(!std::filesystem::exists(state.root),"status-only worker did not create a board or maintenance files");
    DestroyWindow(state.window);state.window=nullptr;
    std::cout<<"PASS beta12 Config async "<<checks<<" native HWND/control/worker checks; synthetic update results and absent-engine status only, no installed/vendor/physical/pixel claim\n";return 0;
}catch(const std::exception& error){
    if(state.clam_job){state.clam_job->cancel=true;if(state.clam_job->thread.joinable())state.clam_job->thread.join();state.clam_job.reset();}
    if(state.window)DestroyWindow(state.window);
    std::cerr<<"FAIL beta12 Config async after "<<checks<<": "<<error.what()<<'\n';return 1;
}}
