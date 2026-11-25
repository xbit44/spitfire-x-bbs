// Only this UI-thread handoff publishes a worker-owned RuntimeClient. The
// visible window never polls or exchanges on a client being initialized.
std::uint16_t startup_configured_node_count(const std::filesystem::path& root)noexcept{
    try{
        const win32::WindowsStorageDaclPolicy policy(win32::current_process_user_sid_string());
        if(!policy.assess(root,win32::StorageDaclObjectKind::directory).status||
            !policy.assess(root/L"Config",win32::StorageDaclObjectKind::directory).status||
            !policy.assess(root/L"Config"/L"SFX.conf",win32::StorageDaclObjectKind::file).status)return 0;
        const auto read=win32::read_operational_configuration_file(root);
        if(read.state==win32::ConfigurationFileState::loaded&&read.configuration&&
            read.configuration->logical_nodes>=1&&read.configuration->logical_nodes<=app::kAlphaNodePoolCapacity)
            return read.configuration->logical_nodes;
    }catch(...){}
    return 0; // Unknown metadata is not a default node count or a trust failure.
}

void startup_controls(AppState& state,bool enabled){
    const auto context=std::make_pair(&state,enabled);
    EnumChildWindows(state.window,[](HWND child,LPARAM value)->BOOL{
        const auto& setting=*reinterpret_cast<const std::pair<AppState*,bool>*>(value);
        auto& s=*setting.first;
        const bool display=child==s.node_list||child==s.list_header||child==s.heading||child==s.subheading||child==s.status_bar;
        EnableWindow(child,setting.second||display);return TRUE;
    },reinterpret_cast<LPARAM>(&context));
    auto style=GetWindowLongPtrW(state.node_list,GWL_STYLE);
    SetWindowLongPtrW(state.node_list,GWL_STYLE,enabled?(style|WS_TABSTOP):(style&~WS_TABSTOP));
    const auto menu=GetMenu(state.window);
    for(int position=1;position<GetMenuItemCount(menu);++position)
        EnableMenuItem(menu,position,MF_BYPOSITION|(enabled?MF_ENABLED:MF_GRAYED));
    DrawMenuBar(state.window);
}
void begin_startup(AppState& state,StartupWork::Phase phase){
    if(!state.startup)state.startup=std::make_unique<StartupWork>();
    auto& work=*state.startup;work.phase=phase;work.done.store(false);work.success=false;
    if(phase!=StartupWork::Phase::cleanup)work.diagnostic.clear();
    const auto window=state.window;
    if(phase!=StartupWork::Phase::metrics){
        startup_controls(state,false);
        if (phase == StartupWork::Phase::runtime) {
            show_startup_activity(state, true);
        } else {
            SetWindowTextW(state.subheading, phase == StartupWork::Phase::cleanup
                ? L"SFX: stopping BBS..." : L"SFX: checking program files...");
        }
        state.status_text=L"STARTING — caller and mail readiness is not confirmed";
        SendMessageW(state.status_bar,SB_SETTEXTW,SBT_OWNERDRAW,reinterpret_cast<LPARAM>(&state));
    }
    try{work.thread=std::thread([&work,window]{
        try{
            const auto root=win32::alpha_installation_root();
            if(work.phase==StartupWork::Phase::cleanup)work.cleanup_complete=work.runtime.cancel_owned_start();
            else if(!work.cancelled.load()){
                if(work.phase==StartupWork::Phase::verification){
                    const auto count=startup_configured_node_count(root);
                    if(!work.cancelled.load())PostMessageW(window,kStartupNodesMessage,count,0);
                    win32::installation::verify_runtime_installation(root,win32::current_process_user_sid_string());
                    if(!work.cancelled.load()){
                        work.first_start=win32::installation::read_prepared_first_start(root,win32::current_process_user_sid_string());
                        work.service=win32::installation::read_service_mode(root).has_value();work.success=true;
                    }
                }else if(work.phase==StartupWork::Phase::runtime){
                    work.success=work.runtime.launch(work.diagnostic,[&work]{return work.cancelled.load();});
                    if(work.success&&!work.cancelled.load()){
                        work.status=work.runtime.status();
                        if(work.first_start&&!work.runtime.is_service_mode()&&work.status&&
                           (work.status->state==app::AlphaRuntimePresentationState::ready||work.status->state==app::AlphaRuntimePresentationState::setup_required))
                            win32::installation::acknowledge_prepared_first_start(root,win32::current_process_user_sid_string(),work.first_start->operation);
                    }
                }else{
                    work.metrics=std::make_unique<win32::WindowsControlPanelMetricSampler>(root);work.success=true;
                }
            }
        }catch(const std::exception& error){work.success=false;work.diagnostic=error.what();}
        catch(...){work.success=false;work.diagnostic="startup_initialization_failed";}
        if(work.phase==StartupWork::Phase::runtime&&(!work.success||work.cancelled.load()))work.cleanup_complete=work.runtime.cancel_owned_start();
        if(!work.cleanup_complete){work.success=false;work.diagnostic="runtime_owned_child_cleanup_incomplete";}
        work.done.store(true);
        PostMessageW(window,kStartupMessage,0,0);
    });}catch(const std::exception& error){
        work.success=false;work.diagnostic=error.what();work.done.store(true);
        PostMessageW(window,kStartupMessage,0,0);
    }
}
void complete_startup(AppState& state){
    if(!state.startup||!state.startup->done.load())return;
    auto& work=*state.startup;if(work.thread.joinable())work.thread.join();
    if (work.phase == StartupWork::Phase::runtime) show_startup_activity(state, false);
    if(!work.cleanup_complete){
        state.startup_close=false;
        SetWindowTextW(state.subheading,L"Runtime stop is incomplete — keep this Control Panel open and try Close again.");
        MessageBoxW(state.window,L"The newly started Desktop Runtime has not stopped. SFX is keeping this window open to retain ownership. Try Close again.",L"Spitfire-X BBS",MB_OK|MB_ICONERROR);return;
    }
    if(state.startup_close){
        if(work.phase==StartupWork::Phase::runtime){begin_startup(state,StartupWork::Phase::cleanup);return;}
        (void)close_runtime(state,false);return;
    }
    if(work.phase==StartupWork::Phase::metrics){
        if(work.success)state.metric_sampler=std::move(work.metrics);
        state.startup.reset();return;
    }
    if(work.phase==StartupWork::Phase::verification){
        if(!work.success&&!state.preparation_attempted){
            state.preparation_attempted=true;
            const auto prepared=win32::offer_portable_preparation(win32::installation::control_panel_program_index);
            if(prepared.cancelled){(void)close_runtime(state,false);return;}
            if(prepared.confirmed){
                if(!prepared.detail.empty())MessageBoxW(state.window,utf8_to_wide(prepared.detail).c_str(),L"Spitfire-X BBS",MB_OK|MB_ICONINFORMATION);
                begin_startup(state,StartupWork::Phase::verification);return;
            }
            if(!prepared.detail.empty())work.diagnostic=prepared.detail;
        }
        if(work.success){
            if(work.first_start&&!work.service){
                const TASKDIALOG_BUTTON buttons[]{{IDYES,L"Start BBS"},{IDCANCEL,L"Close"}};
                TASKDIALOGCONFIG dialog{sizeof(dialog)};dialog.hwndParent=state.window;dialog.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION;
                dialog.pszWindowTitle=L"Spitfire-X BBS";dialog.pszMainInstruction=L"SFX is prepared";
                dialog.pszContent=L"Your recovered board is stopped. Choose Start BBS when you are ready. Closing this window keeps it stopped.";
                dialog.cButtons=static_cast<UINT>(_countof(buttons));dialog.pButtons=buttons;dialog.nDefaultButton=IDCANCEL;
                int selected=IDCANCEL;
                if(FAILED(TaskDialogIndirect(&dialog,&selected,nullptr,nullptr))||selected!=IDYES){(void)close_runtime(state,false);return;}
            }
            begin_startup(state,StartupWork::Phase::runtime);return;
        }
    }else if(work.success){
        state.runtime.swap(work.runtime);
        if(work.status){state.status=std::move(*work.status);state.have_status=true;}
        if(!state.configuration_admin.start(state.window,win32::alpha_installation_root(),win32::current_process_user_sid_string())){
            state.runtime.swap(work.runtime);work.diagnostic="configuration_administration_channel_unavailable";
            begin_startup(state,StartupWork::Phase::cleanup);return;
        }else{
            state.startup_pending=false;startup_controls(state,true);update_status(state);
            PostMessageW(state.window,kOpenSetupMessage,0,0);
            begin_startup(state,StartupWork::Phase::metrics);return;
        }
    }
    state.status.state=app::AlphaRuntimePresentationState::faulted;
    state.status_text=L"STOPPED — startup did not complete";
    SetWindowTextW(state.subheading,L"Startup failed — close SFX and use BBS Health Check in SFXConfig or matching Setup Repair.");
    SendMessageW(state.status_bar,SB_SETTEXTW,SBT_OWNERDRAW,reinterpret_cast<LPARAM>(&state));
    const auto detail=std::wstring(work.runtime.is_service_mode()||state.runtime.is_service_mode()?
        L"Control Panel startup did not complete. The existing Windows service was not stopped.\n\n":
        L"SFX startup did not complete. No new Desktop Runtime remains running.\n\n")+utf8_to_wide(work.diagnostic);
    MessageBoxW(state.window,detail.c_str(),L"Spitfire-X BBS",MB_OK|MB_ICONERROR);
}
