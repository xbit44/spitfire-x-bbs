// Hosting has its own verified SCM context. A settings snapshot failure must
// not disable the only controls that can stop Service Runtime and recover it.
void activate_page(State&,int);
void remember_page(State&);
void hosting_power(State&,bool);
void hosting_text(HWND h,const std::wstring& value){if(text(h)!=value)SetWindowTextW(h,value.c_str());}
bool hosting_dirty(State& s){
    return s.hosting&&(SendMessageW(s.run_mode,CB_GETCURSEL,0,0)!=(s.hosting->enabled?1:0)||
        (s.hosting->enabled&&(Button_GetCheck(s.run_automatic)==BST_CHECKED)!=(s.hosting->start_policy==SERVICE_AUTO_START)));
}
void hosting_baseline(State& s){
    for(auto& v:s.baseline){if(v.control==s.run_mode)v.value=std::to_wstring(SendMessageW(s.run_mode,CB_GETCURSEL,0,0));
        if(v.control==s.run_automatic)v.value=std::to_wstring(Button_GetCheck(s.run_automatic));}
}
bool runtime_idle_dirty(State& s){
    for(const auto& v:s.baseline)if((v.control==s.caller_afk||v.control==s.login_afk)&&v.value!=text(v.control))return true;
    return false;
}
void refresh_runtime_settings(State& s,bool values){
    const bool writable=s.mode!=State::ConnectionMode::blocked&&!s.system_recovery_only&&!s.navigation_loading;
    if(values){SetWindowTextW(s.caller_afk,std::to_wstring(s.configuration.saved.caller_afk_minutes).c_str());
        SetWindowTextW(s.login_afk,std::to_wstring(s.configuration.saved.login_afk_seconds).c_str());}
    for(const auto h:{s.caller_afk,s.login_afk,GetDlgItem(s.pages[41],IDC_RUNTIME_SAVE)})EnableWindow(h,writable);
    if(s.mode==State::ConnectionMode::blocked)hosting_text(s.runtime_detail,L"Open the Control Panel while Service runs, then choose Retry Idle Settings. Your edits remain.");
    else hosting_text(s.runtime_detail,active_saved_line(s,L"Caller AFK minutes",std::to_wstring(s.configuration.active.caller_afk_minutes),std::to_wstring(s.configuration.saved.caller_afk_minutes))+L"\r\n"+
        active_saved_line(s,L"Login idle seconds",std::to_wstring(s.configuration.active.login_afk_seconds),std::to_wstring(s.configuration.saved.login_afk_seconds)));
}
bool refresh_hosting(State& s,bool selection){
    const auto old=s.hosting_status;const auto previous=s.hosting_current;
    s.hosting_status.reset();s.hosting_current.reset();
    try{
        const auto m=selection?install::read_service_registration(s.root,true):install::read_service_selection(s.root,true);
        s.hosting_current=m;
        if(m)s.hosting_status=install::query_owned_service(*m);
        if(selection||(!s.hosting_loaded&&m)){
            s.hosting=m;s.hosting_loaded=true;
            SendMessageW(s.run_mode,CB_SETCURSEL,m&&m->enabled?1:0,0);
            Button_SetCheck(s.run_automatic,m&&m->enabled&&m->start_policy==SERVICE_AUTO_START?BST_CHECKED:BST_UNCHECKED);
        }else if(m&&s.hosting&&m->revision!=s.hosting->revision){
            if(hosting_dirty(s)||s.hosting_review_required)s.hosting_review_required=true;
            else {s.hosting=m;SendMessageW(s.run_mode,CB_SETCURSEL,m->enabled?1:0,0);Button_SetCheck(s.run_automatic,m->start_policy==SERVICE_AUTO_START?BST_CHECKED:BST_UNCHECKED);hosting_baseline(s);}
        }
        const bool service=m&&m->enabled;
        bool desktop_owned=false;
        if(m&&!service){
            const auto names=win32::installation_object_names(s.root);
            const auto existing=OpenMutexW(SYNCHRONIZE,FALSE,names.mutex_name.c_str());
            if(existing){desktop_owned=true;CloseHandle(existing);}
            else install::require(GetLastError()==ERROR_FILE_NOT_FOUND,"desktop_ownership_unavailable");
        }
        const bool stopped=s.hosting_status&&s.hosting_status->state==SERVICE_STOPPED;
        const bool running=s.hosting_status&&s.hosting_status->state==SERVICE_RUNNING;
        const bool ready=m&&!m->pending&&s.hosting_status&&!s.hosting_busy&&!s.hosting_job&&!s.navigation_loading;
        const DWORD desired=SendMessageW(s.run_mode,CB_GETCURSEL,0,0)==1?
            (Button_GetCheck(s.run_automatic)==BST_CHECKED?SERVICE_AUTO_START:SERVICE_DEMAND_START):SERVICE_DISABLED;
        const bool startup_differs=s.hosting_status&&desired!=s.hosting_status->start_type;
        const int action=ready&&service?(stopped&&s.hosting_status->start_type!=SERVICE_DISABLED?1:running?2:0):0;
        if(action!=s.hosting_action||bool(old)!=bool(s.hosting_status)||bool(previous)!=bool(m)||
            (old&&s.hosting_status&&(old->state!=s.hosting_status->state||old->process_id!=s.hosting_status->process_id))||
            (previous&&m&&previous->revision!=m->revision)){
            ++s.hosting_epoch;s.hosting_valid_after=static_cast<DWORD>(GetTickCount64());
        }
        s.hosting_action=action;
        hosting_text(s.run_current,m?(service?L"Windows Service (LocalSystem)":L"Desktop Mode"):L"Unavailable");
        hosting_text(s.run_status,s.hosting_job?(s.hosting_job->start?L"Starting...":L"Stopping..."):!s.hosting_status?L"Unavailable":running?L"Running":stopped?L"Stopped":s.hosting_status->state==SERVICE_START_PENDING?L"Starting...":L"Stopping...");
        hosting_text(s.run_power,s.hosting_job?(s.hosting_job->start?L"Starting BBS...":L"Stopping BBS..."):running?L"Stop BBS...":L"Start BBS");
        EnableWindow(s.run_power,action!=0&&!s.system_recovery_only);
        const bool edit=ready&&stopped&&!desktop_owned&&!s.system_recovery_only&&!s.hosting_review_required;
        EnableWindow(s.run_mode,edit);EnableWindow(s.run_automatic,edit&&SendMessageW(s.run_mode,CB_GETCURSEL,0,0)==1);
        EnableWindow(GetDlgItem(s.pages[14],IDC_RUN_MODE_APPLY),!s.system_recovery_only&&!s.hosting_busy&&!s.hosting_job&&!s.navigation_loading&&m&&stopped&&!desktop_owned&&!s.hosting_review_required&&(m->pending||hosting_dirty(s)||startup_differs));
        EnableWindow(GetDlgItem(s.pages[14],IDC_RUN_MODE_RELOAD),!s.hosting_busy&&!s.hosting_job&&!s.navigation_loading);
        hosting_text(s.run_note,service?L"Closing SFXConfig or the Control Panel leaves the service alone.":L"In Desktop Mode, use the Control Panel to run the BBS.");
        hosting_text(s.run_help,!m?L"Service registration unavailable. Use matching Setup Repair.":m->pending?L"Hosting change incomplete. Apply the recorded choice to recover.":
            s.hosting_review_required?L"Hosting changed externally. Reload Hosting Settings to review; draft retained.":desktop_owned?L"Close the Desktop Control Panel or wait for maintenance before changing hosting.":!stopped?L"Stop BBS before changing hosting settings.":startup_differs&&!hosting_dirty(s)?L"Windows startup differs from saved intent. The checkbox is pending; Apply the desired choice.":hosting_dirty(s)?L"Changes are not applied yet.":L"Applying settings leaves the BBS stopped.");
        if(m&&s.hosting_status)hosting_text(s.run_mode_detail,L"Current Windows startup: "+std::wstring(s.hosting_status->start_type==SERVICE_AUTO_START?L"Automatic":s.hosting_status->start_type==SERVICE_DISABLED?L"Disabled":L"Manual"));
        return m.has_value()&&s.hosting_status.has_value();
    }catch(const std::exception& e){
        ++s.hosting_epoch;s.hosting_action=0;hosting_text(s.run_status,L"Unavailable");
        hosting_text(s.run_help,L"Service state unavailable. Correct the reported condition and retry. "+utf8_to_wide(e.what()));
        for(const auto h:{s.run_power,s.run_mode,s.run_automatic,GetDlgItem(s.pages[14],IDC_RUN_MODE_APPLY)})EnableWindow(h,FALSE);
        return false;
    }
}
void reload_runtime_context(State& s){
    if((s.current_page!=14&&s.current_page!=41)||s.navigation_loading||(s.current_page==14&&(s.hosting_busy||s.hosting_job)))return;
    if(s.current_page==41&&runtime_idle_dirty(s)){
        s.runtime_idle_draft={{text(s.caller_afk),text(s.login_afk)}};
        s.runtime_idle_baseline={{text(s.caller_afk),text(s.login_afk)}};
        for(const auto& v:s.baseline){if(v.control==s.caller_afk)s.runtime_idle_baseline->first=v.value;if(v.control==s.login_afk)s.runtime_idle_baseline->second=v.value;}
    }
    activate_page(s,s.current_page);
}
void poll_hosting(State& s){
    const auto before=s.hosting_status;
    if(s.hosting_job&&s.hosting_job->done.load()){
        const auto job=s.hosting_job;job->worker.join();s.hosting_job.reset();
        refresh_hosting(s,false);
        s.hosting_notice=job->success?(job->start?L"BBS started. Windows Service is running.":L"BBS stopped. Hosting settings can now be changed."):
            L"Service action not confirmed. Actual status is shown; correct the condition and retry. "+utf8_to_wide(job->detail);
        if(job->success&&s.current_page==14)reload_runtime_context(s);
        else if(job->success&&!page_dirty(s))activate_page(s,s.current_page);
        else {set_status(s,s.hosting_notice);s.hosting_notice.clear();}
    }else {
        refresh_hosting(s,false);
        if(before&&s.hosting_status&&before->state!=s.hosting_status->state&&
            (s.hosting_status->state==SERVICE_RUNNING||s.hosting_status->state==SERVICE_STOPPED))reload_runtime_context(s);
    }
}
void hosting_power(State& s,bool start){
    if(s.hosting_busy||s.hosting_job||s.navigation_loading)return;
    refresh_hosting(s,false);
    if(s.hosting_action!=(start?1:2)||!s.hosting_current||!s.hosting_status)return;
    const auto m=*s.hosting_current;const auto before=*s.hosting_status;
    if(!start){
        s.hosting_busy=true;refresh_hosting(s,false);
        const auto answer=MessageBoxW(s.window,L"Stopping the BBS disconnects connected callers and stops Door, transfer and background work.\n\nStop BBS now?",L"Stop BBS?",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING);
        s.hosting_busy=false;refresh_hosting(s,false);
        if(answer!=IDYES)return;
    }
    // A modal decision never authorizes stopping a different service instance.
    refresh_hosting(s,false);
    if(!s.hosting_current||s.hosting_current->revision!=m.revision||!s.hosting_status||s.hosting_status->state!=before.state||s.hosting_status->process_id!=before.process_id){set_status(s,L"Service changed during review. Review its actual status and retry.");return;}
    auto job=std::make_shared<State::HostingAction>();job->start=start;s.hosting_job=job;
    try{
        job->worker=std::thread([result=job.get(),m,before,start]{
            try{result->success=start?install::start_owned_service(m,result->detail):install::stop_owned_service(m,before.process_id,result->detail);}
            catch(const std::exception& e){result->detail=e.what();}result->done.store(true);
        });
    }catch(const std::exception& e){s.hosting_job.reset();refresh_hosting(s,false);set_status(s,L"Service action could not begin. Retry is available. "+utf8_to_wide(e.what()));return;}
    refresh_hosting(s,false);set_status(s,start?L"Starting Windows Service...":L"Stopping Windows Service...");
}
bool save_hosting(State& s){
    if(s.system_recovery_only||s.hosting_busy||s.hosting_job||s.navigation_loading||!s.hosting)return false;
    refresh_hosting(s,false);
    if(s.hosting_review_required){set_status(s,L"Not Saved. Hosting changed or its outcome needs review. Draft retained; use Reload Hosting Settings.");return false;}
    if(!s.hosting_status||s.hosting_status->state!=SERVICE_STOPPED){set_status(s,L"Not Saved. Stop BBS before changing hosting settings. Draft retained.");return false;}
    const auto selected=SendMessageW(s.run_mode,CB_GETCURSEL,0,0);if(selected<0||selected>1)return false;
    const bool enabled=selected==1;const DWORD policy=!enabled?SERVICE_DISABLED:Button_GetCheck(s.run_automatic)==BST_CHECKED?SERVICE_AUTO_START:SERVICE_DEMAND_START;
    if(!s.hosting->pending&&!hosting_dirty(s)&&policy==s.hosting_status->start_type){set_status(s,L"Saved hosting settings unchanged. BBS was not started.");return true;}
    s.hosting_busy=true;refresh_hosting(s,false);
    const auto review=MessageBoxW(s.window,(std::wstring(L"Apply hosting settings?\n\n")+(enabled?L"Windows Service (LocalSystem). Doors inherit LocalSystem privileges.\n":L"Desktop Mode. Windows service startup will be Disabled.\n")+
        (enabled?(policy==SERVICE_AUTO_START?L"Start BBS when Windows starts: Yes.\n":L"Start BBS when Windows starts: No (Manual).\n"):L"")+
        L"BBS must be stopped. Close its Control Panel before applying. Windows may request administrator approval. Data and credentials are retained.").c_str(),L"Review BBS Hosting",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING);
    s.hosting_busy=false;
    if(review!=IDYES){refresh_hosting(s,false);set_status(s,L"Not Saved. Hosting review cancelled; draft retained.");return false;}
    s.hosting_busy=true;refresh_hosting(s,false);EnableWindow(s.window,FALSE);
    set_status(s,L"Applying hosting settings — waiting for Windows approval and confirmed outcome...");
    const auto result=win32::change_service_mode(*s.hosting,enabled,s.window,[]{MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){if(m.message==WM_QUIT){PostQuitMessage(static_cast<int>(m.wParam));break;}TranslateMessage(&m);DispatchMessageW(&m);}},policy);
    EnableWindow(s.window,TRUE);s.hosting_busy=false;SetForegroundWindow(s.window);
    if(result.confirmed){
        s.hosting_review_required=false;refresh_hosting(s,true);hosting_baseline(s);win32::configure_service_secret_scope(enabled);
        set_status(s,enabled?L"Saved — Windows Service mode is active. BBS is stopped. Choose Start BBS when ready.":
            L"Saved — Desktop Mode is active. BBS remains stopped.");
        return true;
    }
    s.hosting_review_required=result.uncertain;refresh_hosting(s,false);
    set_status(s,L"Not Saved / not confirmed. "+utf8_to_wide(result.detail)+L". Draft retained. Close stopped Control Panel and retry if requested; reload only to reconcile a changed or uncertain result.");return false;
}
