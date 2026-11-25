// Board Identity's separate one-owner QWK policy; no caller/body SQL here.
app::LocalQwkPolicyResult query_qwk_policy(State& s){
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline&&!s.navigation_rendering)return win32::offline_qwk_policy_snapshot(s.root,s.owner);
    if(s.mode!=State::ConnectionMode::online&&!s.navigation_rendering)return {};app::LocalControlFrame response;
    if(!online_exchange(s,request(s,app::LocalControlMessageType::qwk_policy_query_request),app::LocalControlMessageType::qwk_policy_query_response,response))return {};
    return app::decode_qwk_policy_result(response.payload).value_or(app::LocalQwkPolicyResult{});
}
app::LocalQwkPolicyResult change_qwk_policy(State& s,const app::QwkPolicyChange& c){
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline)return win32::offline_change_qwk_policy(s.root,s.owner,c);
    if(s.mode!=State::ConnectionMode::online)return {};const auto bytes=app::encode_qwk_policy_change(c);if(bytes.empty())return {app::LocalAccountResultCode::invalid_request};app::LocalControlFrame response;
    if(!online_exchange(s,request(s,app::LocalControlMessageType::qwk_policy_change_request,bytes),app::LocalControlMessageType::qwk_policy_change_response,response))return {};
    return app::decode_qwk_policy_result(response.payload).value_or(app::LocalQwkPolicyResult{});
}
void qwk_policy_controls(State& s){
    const bool ready=s.qwk_policy.code==app::LocalAccountResultCode::ok&&!s.pending_qwk_policy;
    EnableWindow(s.qwk_id,ready);EnableWindow(s.qwk_maximum,ready);EnableWindow(s.qwk_save,ready||s.pending_qwk_policy.has_value());SetWindowTextW(s.qwk_save,s.pending_qwk_policy?L"Reconcile QWK":L"Save QWK");
}
void refresh_qwk_policy(State& s){
    if(!s.pending_qwk_policy){s.qwk_policy=query_qwk_policy(s);if(s.qwk_policy.code==app::LocalAccountResultCode::ok){SetWindowTextW(s.qwk_id,utf8_to_wide(s.qwk_policy.policy.board_id).c_str());SetWindowTextW(s.qwk_maximum,std::to_wstring(s.qwk_policy.policy.maximum_messages).c_str());}}
    qwk_policy_controls(s);
}
bool save_qwk_policy(State& s){
    app::QwkPolicyChange c;
    if(s.pending_qwk_policy)c=*s.pending_qwk_policy;
    else{
        const auto board=wide_to_utf8(text(s.qwk_id));const auto count=message_number(s.qwk_maximum,app::kQwkMaximumMessageCount);
        if(!board||!count||!*count||s.qwk_policy.code!=app::LocalAccountResultCode::ok){set_status(s,L"Refused - enter a safe QWK ID and a finite 1-65,535 message count. Draft retained.");return false;}
        c.board_id=*board;c.maximum_messages=static_cast<std::uint32_t>(*count);c.expected_revision=s.qwk_policy.policy.revision;
        try{win32::WindowsSecureIdentifierGenerator generator;c.operation=generator.generate();}catch(...){set_status(s,L"Unavailable - secure operation identity could not be created. Draft retained.");return false;}
        if(!app::valid_qwk_policy_change(c)){set_status(s,L"Refused - QWK ID is at most 8 safe DOS characters, starts with a letter and cannot be a reserved filename. No Unlimited count. Draft retained.");return false;}
        const bool changed=app::canonical_qwk_board_id(c.board_id)!=s.qwk_policy.policy.board_id;
        if(changed && MessageBoxW(s.window,L"Change this QWK ID? Older QWK/REP packets for the previous ID will no longer match. Stored messages and permanent conference mappings are retained.",
            L"Review QWK ID Change",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)!=IDYES)return false;
    }
    const auto result=change_qwk_policy(s,c);
    if(result.code==app::LocalAccountResultCode::unavailable||result.code==app::LocalAccountResultCode::storage_failure){s.pending_qwk_policy=c;qwk_policy_controls(s);set_status(s,L"QWK outcome unresolved - edits frozen. Reconcile the same operation before changing it.");return false;}
    s.pending_qwk_policy.reset();if(result.code!=app::LocalAccountResultCode::ok){qwk_policy_controls(s);set_status(s,L"QWK policy refused or revision changed. Draft retained; reload before retrying.");return false;}
    s.qwk_policy=result;SetWindowTextW(s.qwk_id,utf8_to_wide(result.policy.board_id).c_str());SetWindowTextW(s.qwk_maximum,std::to_wstring(result.policy.maximum_messages).c_str());qwk_policy_controls(s);
    // Clear ONLY the two committed QWK fields, not unrelated unsaved Board/
    // Sysop controls on this shared page.
    for(auto& before:s.baseline)if(before.control==s.qwk_id||before.control==s.qwk_maximum)before.value=text(before.control);
    set_status(s,result.policy.replayed?L"QWK saved - matching operation reconciled; no duplicate change.":L"QWK policy saved. Future packets use the current ID and finite download count.");return true;
}
bool qwk_policy_dirty(State& s){
    for(const auto& before:s.baseline)if((before.control==s.qwk_id||before.control==s.qwk_maximum)&&before.value!=text(before.control))return true;return false;
}
void create_qwk_policy_controls(State& s){
    const auto p=s.pages[0];label(s,p,L"QWK ID:",220,102,100,22);
    s.qwk_id=control(s,p,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,338,99,168,25,IDC_QWK_ID,WS_EX_CLIENTEDGE);SendMessageW(s.qwk_id,EM_SETLIMITTEXT,8,0);
    label(s,p,L"Messages per QWK download (1-65,535):",12,143,245,22);
    s.qwk_maximum=control(s,p,L"EDIT",L"",WS_TABSTOP|ES_NUMBER|ES_AUTOHSCROLL,266,139,106,25,IDC_QWK_MAXIMUM,WS_EX_CLIENTEDGE);SendMessageW(s.qwk_maximum,EM_SETLIMITTEXT,5,0);
    s.qwk_save=control(s,p,L"BUTTON",L"Save QWK",WS_TABSTOP|BS_PUSHBUTTON,386,138,120,30,IDC_QWK_SAVE);
    label(s,p,L"QWK ID: at most 8 safe DOS characters. Changing it makes older packets incompatible. Default download count: 10,000; no Unlimited.",12,174,494,36);
    SetWindowPos(s.system_detail,nullptr,12,216,500,40,SWP_NOZORDER|SWP_NOACTIVATE);
}
