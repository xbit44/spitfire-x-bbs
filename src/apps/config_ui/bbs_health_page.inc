enum {IDC_BBS_HEALTH_RUN=5701};
void create_bbs_health_page(State& state){
    const auto parent=state.pages[40]=page(state);
    label(state,parent,L"Check this board while the BBS is stopped. Findings include a next step; this check does not repair or delete anything.",12,10,510,58);
    state.health_run=control(state,parent,L"BUTTON",L"Run BBS Health Check",WS_TABSTOP|BS_PUSHBUTTON,12,78,230,30,IDC_BBS_HEALTH_RUN);
    label(state,parent,L"A dated text report is saved in this board's Logs folder when access is available.",12,122,510,42);
    state.health_result=control(state,parent,L"EDIT",L"Stop the BBS, then choose Run BBS Health Check.",WS_TABSTOP|ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,12,174,510,290,0,WS_EX_CLIENTEDGE);
    SendMessageW(state.health_result,EM_SETLIMITTEXT,2U*1024U*1024U,0);
}
void start_bbs_health(State& state){
    if(state.current_page==40&&state.navigation_loading){set_status(state,L"Finishing the previous settings read; Health Check will be ready shortly.");return;}
    if(state.health_job&&!state.health_job->done){state.health_job->cancel=true;EnableWindow(state.health_run,FALSE);set_status(state,L"Stopping the check; findings collected so far will be preserved.");return;}
    state.health_job=std::make_unique<State::HealthJob>();auto* job=state.health_job.get();const auto root=state.root;const auto owner=state.owner;
    SetWindowTextW(state.health_result,L"Checking this board... No repairs or cleanup will be performed.");SetWindowTextW(state.health_run,L"Cancel Check");set_status(state,L"Checking BBS health...");
    job->thread=std::thread([job,root,owner]{try{job->result=win32::run_bbs_health_check(root,owner,[job]{return job->cancel.load();});}catch(...){job->result.findings.push_back({win32::BbsHealthState::could_not_check,L"BBS Health Check",L"The check could not finish. No success or saved report is claimed.",L"Review folder access and required files, then retry."});}job->done=true;});
    SetTimer(state.window,config_timer::bbs_health,100,nullptr);
}
void poll_bbs_health(State& state){
    if(!state.health_job||!state.health_job->done)return;
    KillTimer(state.window,config_timer::bbs_health);const auto& report=state.health_job->result;auto output=report.text();
    if(report.saved_report)output+=L"Saved report: "+report.saved_report->native()+L"\r\n";
    else output+=L"No complete saved report is claimed.\r\n";
    SetWindowTextW(state.health_result,output.c_str());SetWindowTextW(state.health_run,L"Run BBS Health Check");EnableWindow(state.health_run,TRUE);
    set_status(state,report.stopped?L"Health Check finished. Review the findings and next steps.":L"Health Check requires a stopped BBS. No board changes were made.");if(state.current_page==40)remember_page(state);
}
