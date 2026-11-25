// Private Config UI; SQL and authority stay behind the existing host boundary.
#ifdef SFX_ALPHA_TEST_AUTOMATION
app::LocalClassicInformationSnapshot (*classic_query_fixture)(std::int64_t)=nullptr;
app::LocalAccountResult (*classic_change_fixture)(const app::ClassicInformationChange&)=nullptr;
#endif
app::LocalClassicInformationSnapshot query_classic_information(State& state,std::int64_t after) {
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(classic_query_fixture)return classic_query_fixture(after);
#endif
    if(state.system_recovery_only || !load_editor_state(state))return {};
    if(state.mode==State::ConnectionMode::offline&&!state.navigation_rendering)return win32::offline_classic_information_snapshot(state.root,state.owner,after);
    if(state.mode!=State::ConnectionMode::online&&!state.navigation_rendering)return {};
    app::LocalControlFrame response;
    if(!online_exchange(state,request(state,app::LocalControlMessageType::classic_information_query_request,
        app::encode_classic_information_query(after)),app::LocalControlMessageType::classic_information_query_response,response))return {};
    return app::decode_classic_information_snapshot(response.payload).value_or(app::LocalClassicInformationSnapshot{});
}
app::LocalAccountResult change_classic_information(State& state,const app::ClassicInformationChange& change) {
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(classic_change_fixture)return classic_change_fixture(change);
#endif
    if(state.system_recovery_only || !load_editor_state(state))return {};
    if(state.mode==State::ConnectionMode::offline)return win32::offline_change_classic_information(state.root,state.owner,change);
    if(state.mode!=State::ConnectionMode::online)return {};
    app::LocalControlFrame response;
    const auto payload=app::encode_classic_information_change(change);if(payload.empty())return {app::LocalAccountResultCode::invalid_request};
    if(!online_exchange(state,request(state,app::LocalControlMessageType::classic_information_change_request,payload),
        app::LocalControlMessageType::classic_information_change_response,response))return {};
    return app::decode_account_result(response.payload).value_or(app::LocalAccountResult{});
}
void refresh_classic_information(State& state) {
    state.classic_information=query_classic_information(state,0);
    const bool available=state.classic_information.code==app::LocalAccountResultCode::ok;
    for(const auto page:{18})for(HWND child=GetWindow(state.pages[page],GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
        if(GetDlgCtrlID(child))EnableWindow(child,available);
    std::string date;
    if(available) {
        const auto& opening=state.classic_information.opening;
        if(opening.configured_date)date=*opening.configured_date;
        else {
            win32::WindowsBbsLogTimestamp local;
            if(local.configure(state.configuration.saved.bbs_time_zone)) {
                const auto value=local.format(opening.initialization_utc);if(value && value->size()>=10)date=value->substr(0,10);
            }
        }
    }
    SetWindowTextW(state.opening_date,utf8_to_wide(date).c_str());
    SetWindowTextW(state.opening_detail,available
        ?L"Opening date is independent of Runtime uptime. Initial defaults use the retained installation/database initialization date in the BBS time zone; enter a historical date if appropriate. Saved dates survive updates and repairs."
        :L"Unavailable — no authoritative opening date snapshot. No change was made.");
}
void save_opening_date(State& state) {
    if(state.classic_information.code!=app::LocalAccountResultCode::ok)return;
    app::ClassicInformationChange change;change.action=app::ClassicInformationAction::opening_date;
    change.expected_revision=state.classic_information.opening.revision;change.date=wide_to_utf8(text(state.opening_date)).value_or("");
    if(!app::valid_classic_information_change(change)) {set_status(state,L"Refused — enter a real date in YYYY-MM-DD format. Your draft remains.");return;}
    const auto result=change_classic_information(state,change);
    set_status(state,account_result_text(result));if(result.committed)refresh_classic_information(state);
}
void create_classic_information_pages(State& state) {
    auto parent=state.pages[18]=page(state);
    label(state,parent,L"Public System Information",12,8,500,30);
    label(state,parent,L"This BBS began on (YYYY-MM-DD):",12,52,270,24);
    state.opening_date=control(state,parent,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,290,49,160,25,IDC_OPENING_DATE,WS_EX_CLIENTEDGE);
    SendMessageW(state.opening_date,EM_SETLIMITTEXT,10,0);
    control(state,parent,L"BUTTON",L"Save",WS_TABSTOP|BS_PUSHBUTTON,12,100,120,30,IDC_OPENING_SAVE);
    control(state,parent,L"BUTTON",L"Reload",WS_TABSTOP|BS_PUSHBUTTON,145,100,120,30,IDC_OPENING_RELOAD);
    state.opening_detail=control(state,parent,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,12,150,500,130,0,WS_EX_CLIENTEDGE);
}
