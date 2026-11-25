bool clam_busy(const State& s){return bool(s.clam_job);}
bool clam_maintenance_active(State& s){if(!s.clam_maintenance)return false;if(WaitForSingleObject(s.clam_maintenance.get(),0)==WAIT_OBJECT_0)s.clam_maintenance.reset();return bool(s.clam_maintenance);}
std::wstring clam_time(std::int64_t value){if(!value)return L"Unavailable";const auto t=static_cast<time_t>(value);tm date{};if(gmtime_s(&date,&t))return L"Unavailable";wchar_t text[64]{};if(!wcsftime(text,std::size(text),L"%Y-%m-%d %H:%M:%S UTC",&date))return L"Unavailable";return text;}
void clam_buttons(State& s){const bool busy=clam_busy(s),known=s.clam_status_read==State::ClamStatusRead::ready,maintenance=clam_maintenance_active(s),available=!s.system_recovery_only&&s.mode!=State::ConnectionMode::blocked;
    EnableWindow(s.file_editor.scan,available&&!busy&&!maintenance&&s.file_editor.ready&&!s.file_editor.pending);
    EnableWindow(s.clam_save,available&&!busy&&!maintenance&&s.file_editor.ready&&!s.file_editor.pending);
    EnableWindow(s.clam_refresh,!s.system_recovery_only&&!busy&&!maintenance&&!s.file_editor.pending);
    const bool upgrade=s.clam_status.diagnostic=="clamav_engine_upgrade_required";
    SetWindowTextW(s.clam_install,upgrade?L"Upgrade Portable ClamAV...":L"Install Portable ClamAV...");
    EnableWindow(s.clam_install,available&&known&&!busy&&!maintenance&&(!s.clam_status.installed||upgrade)&&s.mode==State::ConnectionMode::offline);
    EnableWindow(s.clam_update,available&&known&&!busy&&!maintenance&&s.clam_status.verified);
    EnableWindow(s.clam_cancel,busy&&!s.clam_job->done&&!s.clam_job->cancel);
    EnableWindow(s.clam_log,!s.system_recovery_only&&known&&s.clam_status.installed);
}
void render_clam(State& s){const auto& c=s.clam_status;std::wstring detail;
    if(s.clam_status_read!=State::ClamStatusRead::ready){
        switch(s.clam_status_read){
        case State::ClamStatusRead::checking:detail=s.clam_job&&s.clam_job->updating?L"Updating definitions. ClamAV status will be checked after the operation reaches its safe cleanup boundary.":L"Checking ClamAV status...";break;
        case State::ClamStatusRead::cancelled:detail=L"ClamAV status check cancelled. Current installation and definition status is unavailable. Refresh Status to check again.";break;
        case State::ClamStatusRead::failed:detail=L"ClamAV status unavailable. The inspection did not complete successfully. Refresh Status to check again; no missing engine or successful update is assumed.";break;
        default:detail=L"ClamAV status has not been read. Refresh Status to check the installation and definitions.";break;
        }
        SetWindowTextW(s.clam_detail,detail.c_str());clam_buttons(s);return;
    }
    if(!c.installed)detail=L"Portable ClamAV: Not installed.\r\nDefinitions: Not available.";
    else if(c.diagnostic=="clamav_engine_upgrade_required")detail=L"Portable ClamAV: Upgrade to 1.4.7 required.\r\nStop the BBS and Control Panel, then choose Upgrade Portable ClamAV. With Scan Uploads On, new uploads are held until a required scan can finish.";
    else if(!c.verified)detail=L"Portable ClamAV: Installation could not be verified.\r\nUse the matched SFX Setup to repair; no unverified engine will run.";
    else detail=L"Portable ClamAV: Verified engine "+utf8_to_wide(c.version)+L"\r\nDefinitions: "+(c.definitions_verified?std::wstring(L"Ready — matches the last successful vendor check."):c.definitions_present?std::wstring(L"Present; readiness not verified. Run Update definitions now."):std::wstring(L"Not ready or unavailable. Run Update definitions now."));
    detail+=L"\r\n\r\nLast successful definition check/update: "+clam_time(c.last_success)+L"\r\nLast attempt: "+clam_time(c.last_attempt);
    if(c.history_available)detail+=c.last_mode==win32::ClamAvUpdateMode::manual?L" (manual)":L" (automatic)";
    detail+=L"\r\nLast result/error: "+(c.last_result.empty()?std::wstring(L"Unavailable"):utf8_to_wide(c.last_result));
    if(c.last_result=="clamav_definitions_verified")detail+=L"\r\n"+utf8_to_wide(win32::clamav_change_text(c.change));
    detail+=L"\r\nLast definition change: "+clam_time(c.last_change);
    const std::array<const wchar_t*,3> database_names{{L"Main",L"Daily",L"Bytecode"}};
    for(std::size_t n=0;n<3;++n){detail+=L"\r\n"+std::wstring(database_names[n])+L": ";if(c.definitions_verified&&c.databases[n])detail+=L"version "+std::to_wstring(c.databases[n]->version)+L"; published "+clam_time(c.databases[n]->published);else detail+=L"Version/date unavailable (not verified).";}
    detail+=L"\r\n\r\nAutomatic updater owner: Runtime — daily UTC attempt, independent of Scan caller uploads.\r\n";
    detail+=s.mode==State::ConnectionMode::online?L"Runtime is running; maintenance pause, shared admission and vendor cooldown apply.":L"BBS stopped: automatic checks wait for Runtime. Manual update is available with a verified engine.";
    if(!c.diagnostic.empty()&&c.diagnostic!="clamav_not_installed")detail+=L"\r\nStatus diagnostic: "+utf8_to_wide(c.diagnostic);
    detail+=L"\r\n\r\nInstall later requires the BBS and Control Panel stopped. Config may stay open. Manual update works with Runtime running or stopped; it cannot override vendor cooldown.";
    SetWindowTextW(s.clam_detail,detail.c_str());clam_buttons(s);
}
bool deliver_clam_notice(const std::filesystem::path& root,const install::Receipt& receipt,const app::ManualClamAvNotice& notice){
    try{
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::clamav_manual_notice_request;
        request.request_id=static_cast<std::uint64_t>(notice.phase)+1;request.payload=app::encode_manual_clamav_notice(notice);if(request.payload.empty())return false;
        // One bounded retry of the SAME operation/phase, never a second event.
        for(unsigned attempt=0;attempt<2;++attempt)if(relay_exchange(root,receipt,request,app::LocalControlMessageType::clamav_manual_notice_response,response)){
            const auto result=app::decode_control_response(response.payload);return result&&result->code==app::LocalControlResponseCode::ok;
        }
    }catch(...){}return false;
}
bool offline_clam_attention(const std::filesystem::path& root,const std::wstring& owner,const app::LoggingConfiguration& limits,const std::string& zone,const app::AttentionRecord& notice){
    try{
        win32::WindowsBbsLogTimestamp clock;if(!clock.configure(zone))return false;
        const auto block=app::render_attention_journal(notice,[&](std::int64_t t){return clock.format(t);});if(!block)return false;
        win32::WindowsDurableJournalFiles files(root,owner);if(!files.configure(limits))return false;const auto kind=app::DurableJournalKind::attention;
        auto state=files.inspect(kind);if(!state||state->bytes>app::kDurableJournalMaximumFileBytes)return false;
        if(state->bytes&&files.read(kind,state->bytes-2,2)!=std::optional<std::string>("\r\n"))return false;
        const auto maximum=std::uint64_t(limits.attention.size_mib)*1024*1024;
        if(state->bytes>maximum||block->size()>maximum-state->bytes){if(!files.archive_and_reopen(kind))return false;state=files.inspect(kind);}
        return state&&state->bytes<=maximum&&block->size()<=maximum-state->bytes&&files.append(kind,*state,*block);
    }catch(...){return false;}
}
void start_clam_job(State& s,bool update){
    if(!win32::clamav_available())return;
    if(clam_busy(s)||clam_maintenance_active(s)||s.system_recovery_only)return;
    if(update&&!load_editor_state(s)){set_status(s,L"Update refused — current installation/Runtime ownership could not be verified.");return;}
    if(s.clam_job){if(s.clam_job->thread.joinable())s.clam_job->thread.join();s.clam_job.reset();}
    const bool offline=s.mode==State::ConnectionMode::offline;app::Identifier128 event_operation;
    if(update&&!offline)try{win32::WindowsSecureIdentifierGenerator generator;event_operation=generator.generate();}catch(...){set_status(s,L"Update not started — secure activity identity unavailable.");return;}
    auto job=std::make_shared<State::ClamJob>();job->updating=update;s.clam_job=job;s.clam_status_read=State::ClamStatusRead::checking;render_clam(s);const auto root=s.root;const auto owner=s.owner,id=s.receipt.id;const auto receipt=s.receipt;
    const auto logging=s.configuration.saved.logging;const auto zone=s.configuration.saved.bbs_time_zone;const auto offline_health=s.clam_offline_health;
    // State and HWNDs never cross into this worker. UI polls an owned result;
    // close requests cancel and wait for the safe updater/child boundary.
    if(!SetTimer(s.window,config_timer::clam_job,200,nullptr)){s.clam_job.reset();s.clam_status_read=State::ClamStatusRead::failed;render_clam(s);set_status(s,L"ClamAV status polling unavailable. No background operation was started.");return;}
    try{job->thread=std::thread([job=job.get(),root,owner,id,receipt,event_operation,update,offline,logging,zone,offline_health]{try{
        win32::OfflineInstallationGuard guard;
        if(update&&offline&&!guard.acquire(root,owner).succeeded()){win32::ClamAvUpdateResult refused;refused.diagnostic="clamav_update_installation_in_use";job->result=refused;job->status=win32::inspect_clamav_status(root,owner,id);job->done=true;return;}
        std::int64_t attempt_utc{};
        if(update){const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();attempt_utc=now;
            if(!offline)job->events_complete=deliver_clam_notice(root,receipt,{event_operation,app::ManualClamAvPhase::started,0,{}});
            job->result=win32::update_clamav_definitions(root,owner,id,now,[&]{return job->cancel.load();},[&](std::uint64_t elapsed){std::lock_guard<std::mutex> lock(job->mutex);job->progress=L"Checking definitions / waiting for shared admission... "+std::to_wstring(elapsed/1000)+L" seconds. No success is claimed yet.";},180000,win32::ClamAvUpdateMode::manual);}
        if(job->cancel.load())job->inspection_cancelled=true;
        else job->status=win32::inspect_clamav_status(root,owner,id,[&]{const bool cancel=job->cancel.load();if(cancel)job->inspection_cancelled=true;return cancel;});
        if(update&&offline&&job->result&&!job->result->cancelled&&(!job->result->succeeded||job->status.definitions_verified)){const auto& result=*job->result;
            const auto notice=offline_health->observe(false,result.succeeded,job->status.definitions_verified,result.diagnostic,attempt_utc);
            if(notice)job->events_complete=offline_clam_attention(root,owner,logging,zone,*notice);
        }
        if(update&&!offline&&job->result){const auto& result=*job->result;
            const auto phase=result.succeeded?app::ManualClamAvPhase::verified:result.cancelled?app::ManualClamAvPhase::cancelled:app::ManualClamAvPhase::failed;
            const bool recorded=deliver_clam_notice(root,receipt,{event_operation,phase,attempt_utc,result.diagnostic});job->events_complete=recorded&&job->events_complete;
        }}catch(...){job->failed=true;}job->done=true;
    });}catch(...){KillTimer(s.window,config_timer::clam_job);s.clam_job.reset();s.clam_status_read=State::ClamStatusRead::failed;render_clam(s);set_status(s,L"ClamAV background job unavailable. No operation was started.");return;}
    set_status(s,update?L"Updating definitions — Cancel is available. Existing definitions remain protected.":L"Checking ClamAV status...");clam_buttons(s);
}
void poll_clam_job(State& s){
    if(!s.clam_job)return;auto job=s.clam_job;
    if(!job->done){std::lock_guard<std::mutex> lock(job->mutex);if(!job->progress.empty())set_status(s,job->progress);return;}
    if(job->thread.joinable())job->thread.join();KillTimer(s.window,config_timer::clam_job);s.clam_status=job->status;
    s.clam_status_read=job->failed?State::ClamStatusRead::failed:job->inspection_cancelled?State::ClamStatusRead::cancelled:!job->status.installed&&job->status.diagnostic!="clamav_not_installed"?State::ClamStatusRead::failed:State::ClamStatusRead::ready;
    s.clam_job.reset();render_clam(s);
    if(job->failed)set_status(s,L"ClamAV background operation failed. Status is unavailable; no successful update or safe cleanup is assumed.");
    else if(job->result){const auto& r=*job->result;if(r.succeeded)set_status(s,utf8_to_wide(win32::clamav_change_text(r.change))+L". The scanning-policy choice was not changed.");else set_status(s,(r.cancelled&&r.process_reaped&&r.cleanup_complete?L"Cancelled safely — ":L"Not completed — ")+utf8_to_wide(r.diagnostic)+L". Existing definitions and any retained work remain protected; no scan pass is implied.");}
    else if(s.clam_status_read==State::ClamStatusRead::cancelled)set_status(s,L"ClamAV status check cancelled. Refresh Status to check again.");
    else if(s.clam_status_read==State::ClamStatusRead::failed)set_status(s,L"ClamAV status unavailable. Refresh Status to check again.");
    else set_status(s,L"Status refreshed. Missing history/readiness is shown as unavailable, not success.");
    if(job->result&&!job->events_complete)set_status(s,text(s.status)+L" Activity/attention logging unavailable; the result above remains authoritative.");
    if(s.close_after_clam)PostMessageW(s.window,WM_CLOSE,0,0);
}
void refresh_clam_page(State& s){refresh_file_configuration_page(s);start_clam_job(s,false);}
void install_clam(State& s){
    if(!win32::clamav_available())return;
    if(!resolve_dirty(s)||!load_editor_state(s)||s.mode!=State::ConnectionMode::offline){set_status(s,L"Stop the BBS and Control Panel before installing Portable ClamAV.");return;}
    try{const auto receipt=install::read_receipt(s.root,s.owner);install::verify_structure(receipt);install::require(receipt.program_count==install::current_program_count,"clamav_maintenance_update_sfx_first");const auto exe=s.root/L"Bin"/install::programs[install::maintenance_program_index];auto pin=install::open_object(exe,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);install::require(install::file_sha256(exe)==receipt.program_hashes[install::maintenance_program_index],"clamav_maintenance_image_changed");
        SHELLEXECUTEINFOW launch{};launch.cbSize=sizeof(launch);launch.hwnd=s.window;launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;launch.lpFile=exe.c_str();launch.lpVerb=L"open";launch.nShow=SW_SHOWNORMAL;install::require(ShellExecuteExW(&launch)&&launch.hProcess,"clamav_maintenance_launch_failed");s.clam_maintenance.reset(launch.hProcess);SetTimer(s.window,config_timer::clam_maintenance,1000,nullptr);set_status(s,L"Setup-owned ClamAV maintenance opened. Complete or close it, then Refresh Status.");clam_buttons(s);
    }catch(const std::exception& e){set_status(s,L"Maintenance unavailable — "+utf8_to_wide(e.what())+L". No installer was searched for or run from an untrusted location.");}
}
struct ClamLogWindow {std::filesystem::path root;std::wstring owner;HWND edit{},refresh{};bool owned{};};
void refresh_clam_log(ClamLogWindow& log){
    const auto view=win32::read_clamav_update_log(log.root,log.owner);
    auto caption=view.available?(view.truncated?std::wstring(L"Recent 64 KiB tail (older lines omitted).\r\n\r\n"):std::wstring{} )+utf8_to_wide(view.text):view.missing?std::wstring(L"No update log exists yet."):std::wstring(L"Update log unavailable, busy or unsafe to read. No update was started. Try Refresh.");
    SetWindowTextW(log.edit,caption.c_str());SendMessageW(log.edit,EM_SETSEL,caption.size(),caption.size());SendMessageW(log.edit,EM_SCROLLCARET,0,0);
}
LRESULT CALLBACK clam_log_proc(HWND window,UINT message,WPARAM a,LPARAM b){
    auto* log=reinterpret_cast<ClamLogWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){log=static_cast<ClamLogWindow*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(log));}
    if(!log)return DefWindowProcW(window,message,a,b);
    if(message==WM_CREATE){
        log->edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOHSCROLL,8,8,800,480,window,nullptr,nullptr,nullptr);
        log->refresh=CreateWindowW(L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,8,496,120,28,window,reinterpret_cast<HMENU>(1),nullptr,nullptr);
        SendMessageW(log->edit,EM_SETLIMITTEXT,131072,0);SendMessageW(log->edit,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)),TRUE);refresh_clam_log(*log);return 0;
    }
    if(message==WM_SIZE){const auto width=LOWORD(b),height=HIWORD(b);MoveWindow(log->edit,8,8,std::max<int>(120,width-16),std::max<int>(80,height-52),TRUE);MoveWindow(log->refresh,8,std::max<int>(90,height-36),120,28,TRUE);return 0;}
    if(message==WM_COMMAND&&LOWORD(a)==1){refresh_clam_log(*log);return 0;}
    if(message==WM_CLOSE){DestroyWindow(window);return 0;}
    if(message==WM_NCDESTROY){SetWindowLongPtrW(window,GWLP_USERDATA,0);if(log->owned)delete log;}
    return DefWindowProcW(window,message,a,b);
}
void open_clam_log(State& s){
    if(!win32::clamav_available()||s.system_recovery_only)return;
    const wchar_t* name=L"SFXClamAVReadOnlyUpdateLog";WNDCLASSW cls{};cls.hInstance=s.instance;cls.lpfnWndProc=clam_log_proc;cls.lpszClassName=name;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
    if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){set_status(s,L"Update log viewer unavailable.");return;}
    auto log=std::make_unique<ClamLogWindow>();log->root=s.root;log->owner=s.owner;
    const auto window=CreateWindowExW(0,name,L"ClamAV Update Log — read only",WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,CW_USEDEFAULT,900,600,s.window,nullptr,s.instance,log.get());
    if(window){log->owned=true;log.release();ShowWindow(window,SW_SHOW);}else set_status(s,L"Update log viewer unavailable.");
}
bool clam_command(State& s,UINT id,UINT notification){if(id<IDC_CLAM_DETAIL||id>IDC_CLAM_LOG)return false;if(notification!=BN_CLICKED)return true;
    if(!win32::clamav_available())return true;
    if(id==IDC_CLAM_LOG){open_clam_log(s);return true;}
    if(id==IDC_CLAM_CANCEL){if(s.clam_job&&s.clam_job->done)poll_clam_job(s);else if(clam_busy(s)){s.clam_job->cancel=true;clam_buttons(s);set_status(s,L"Cancellation requested. Waiting for the supervised child and safe cleanup...");}return true;}
    if(clam_busy(s))return true;
    if(id==IDC_CLAM_SAVE){save_file_configuration_page(s);clam_buttons(s);}
    if(id==IDC_CLAM_REFRESH&&resolve_dirty(s))refresh_clam_page(s);
    if(id==IDC_CLAM_INSTALL)install_clam(s);
    if(id==IDC_CLAM_UPDATE&&resolve_dirty(s))start_clam_job(s,true);
    return true;
}
void create_clam_page(State& s){auto p=s.pages[25]=page(s);label(s,p,L"Portable ClamAV",12,8,660,28);
    s.clam_detail=control(s,p,L"EDIT",L"Status has not been read.",ES_MULTILINE|ES_READONLY|WS_VSCROLL,12,44,660,280,IDC_CLAM_DETAIL,WS_EX_CLIENTEDGE);
    s.file_editor.scan=control(s,p,L"BUTTON",L"Scan caller uploads",WS_TABSTOP|BS_AUTOCHECKBOX,12,344,660,25,IDC_FILE_SCAN);
    label(s,p,L"Scanning Off does not stop daily definition updates. When scanning is On, an unavailable or incomplete scan holds new caller uploads privately. Sysop imports are not automatically scanned.",12,380,660,62);
    s.clam_save=control(s,p,L"BUTTON",L"Save Changes",WS_TABSTOP|BS_PUSHBUTTON,12,454,150,28,IDC_CLAM_SAVE);
    control(s,p,L"BUTTON",L"Reconcile",WS_TABSTOP|BS_PUSHBUTTON,174,454,150,28,IDC_FILE_RECONCILE);
    s.clam_refresh=control(s,p,L"BUTTON",L"Refresh Status",WS_TABSTOP|BS_PUSHBUTTON,336,454,150,28,IDC_CLAM_REFRESH);
    s.clam_install=control(s,p,L"BUTTON",L"Install Portable ClamAV...",WS_TABSTOP|BS_PUSHBUTTON,12,502,240,30,IDC_CLAM_INSTALL);
    s.clam_update=control(s,p,L"BUTTON",L"Update Definitions Now",WS_TABSTOP|BS_PUSHBUTTON,264,502,240,30,IDC_CLAM_UPDATE);
    s.clam_cancel=control(s,p,L"BUTTON",L"Cancel",WS_TABSTOP|BS_PUSHBUTTON,516,502,156,30,IDC_CLAM_CANCEL);
    s.clam_log=control(s,p,L"BUTTON",L"View Update Log",WS_TABSTOP|BS_PUSHBUTTON,12,548,240,30,IDC_CLAM_LOG);
}
