#ifdef SFX_ALPHA_TEST_AUTOMATION
std::optional<app::FtnHeldControlResponse> (*ftn_held_fixture)(const app::FtnHeldControlRequest&)=nullptr;
#endif
std::optional<app::FtnHeldControlResponse> ftn_held_exchange(State& s,const app::FtnHeldControlRequest& q){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(ftn_held_fixture)return ftn_held_fixture(q);
#endif
    if(s.system_recovery_only||!load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline&&!s.navigation_rendering)return win32::offline_ftn_held(s.root,s.owner,q);
    if(s.mode!=State::ConnectionMode::online&&!s.navigation_rendering)return {};const auto bytes=app::encode_ftn_held_request(q);if(bytes.empty())return {};app::LocalControlFrame response;
    if(!online_exchange(s,request(s,app::LocalControlMessageType::ftn_held_request,bytes),app::LocalControlMessageType::ftn_held_response,response))return {};return app::decode_ftn_held_response(response.payload);
}
const app::FtnHeldReviewMessage* ftn_held_message(State& s){const auto& h=s.ftn.held;const auto row=SendMessageW(h.messages,CB_GETCURSEL,0,0);return h.detail&&row>=0&&static_cast<std::size_t>(row)<h.detail->messages.size()?&h.detail->messages[static_cast<std::size_t>(row)]:nullptr;}
std::wstring ftn_held_reason_name(const std::string& reason){
    if(reason.empty())return L"All reasons";
    if(reason=="ftn_recipient_unmatched")return L"No matching recipient";
    if(reason=="ftn_recipient_ambiguous")return L"More than one recipient";
    if(reason=="ftn_recipient_stale")return L"Recipient choice changed";
    if(reason=="ftn_echo_receive_not_authorized")return L"Echo mapping or permission";
    if(reason=="ftn_packet_password_wrong")return L"Packet password wrong";
    if(reason=="ftn_packet_invalid")return L"Invalid packet";
    if(reason=="ftn_message_import_held")return L"Message import held";
    if(reason=="ftn_message_identity_or_controls_ambiguous")return L"Conflicting message identifiers or controls";
    if(reason=="ftn_message_id_content_collision")return L"Message identifier reused for different content";
    if(reason=="ftn_message_charset_unsupported")return L"Unsupported message character set";
    if(reason=="ftn_message_body_unsupported")return L"Message exceeds supported display limits";
    if(reason=="ftn_tic_password_wrong")return L"TIC password wrong";
    if(reason=="ftn_tic_not_authorized"||reason=="ftn_tic_mapping_missing"||reason=="ftn_tic_area_not_authorized")return L"File echo mapping or permission";
    if(reason=="ftn_tic_continuation_review_required")return L"Interrupted file needs review";
    if(reason=="ftn_tic_replaces_required")return L"Replacement not authorized";
    if(reason=="ftn_tic_forward_manifest_missing")return L"Original forwarding plan missing";
    return utf8_to_wide(reason);
}
void ftn_held_reason_add(State& s,const std::string& reason){auto& h=s.ftn.held;if(std::find(h.reason_codes.begin(),h.reason_codes.end(),reason)!=h.reason_codes.end())return;const auto label=ftn_held_reason_name(reason);h.reason_codes.push_back(reason);SendMessageW(h.reason,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}
std::wstring ftn_held_row_reason(const app::FtnHeldReviewRow& row){if(row.retry_pending)return L"Reviewed retry pending";if(row.state==app::FtnIngressState::processed)return L"Processed";if(row.tic)return ftn_held_reason_name(row.reason);if(row.state==app::FtnIngressState::processing)return L"Processing";return row.reason.empty()?L"Held original":ftn_held_reason_name(row.reason);}
void ftn_held_buttons(State& s){
    const auto& h=s.ftn.held;const auto* m=ftn_held_message(s);const bool idle=!h.pending;const bool detail=h.detail&&h.detail->rows.size()==1;const bool mutable_hold=detail&&!h.detail->rows.front().tic&&h.detail->rows.front().state==app::FtnIngressState::held&&!h.detail->rows.front().retry_pending&&h.detail->mode!=app::FtnMailerMode::disabled&&!h.detail->serial_recovery_required;
    EnableWindow(GetDlgItem(s.pages[36],IDC_FTN_HELD_RESOLVE),idle&&mutable_hold&&m&&m->local_private&&!m->committed&&SendMessageW(h.accounts,CB_GETCURSEL,0,0)>=0);
    const bool mutable_tic=detail&&h.detail->tic&&h.detail->rows.front().tic&&h.detail->rows.front().state!=app::FtnIngressState::processed&&!h.detail->rows.front().retry_pending&&h.detail->mode!=app::FtnMailerMode::disabled&&!h.detail->serial_recovery_required;
    EnableWindow(GetDlgItem(s.pages[36],IDC_FTN_HELD_REPROCESS),idle&&(mutable_tic||(mutable_hold&&app::ftn_message_wire_name(h.detail->rows.front().file))));
    EnableWindow(h.messages,idle&&detail&&!h.detail->tic);
    EnableWindow(GetDlgItem(s.pages[36],IDC_FTN_HELD_CHECK),!idle);
    EnableWindow(h.rows,idle);
    EnableWindow(GetDlgItem(s.pages[36],IDC_FTN_HELD_NEXT),idle&&h.list&&h.list->more);
    EnableWindow(GetDlgItem(s.pages[36],IDC_FTN_HELD_MESSAGE_NEXT),idle&&detail&&h.detail->more);
    EnableWindow(GetDlgItem(s.pages[36],IDC_FTN_HELD_ACCOUNT_NEXT),idle&&h.recipients&&h.recipients->more);
    for(const auto id:{IDC_FTN_HELD_ACCOUNT_SEARCH,IDC_FTN_HELD_ACCOUNT_FIND,IDC_FTN_HELD_ACCOUNTS})EnableWindow(GetDlgItem(s.pages[36],id),idle&&mutable_hold&&m&&m->local_private&&!m->committed);
}
std::wstring ftn_held_help(const std::string& reason){
    if(reason=="ftn_recipient_unmatched"||reason=="ftn_recipient_ambiguous"||reason=="ftn_recipient_stale")return L"Review the original To and choose the intended active local recipient below. Save that choice, then request Reprocess Original.";
    if(reason=="ftn_echo_receive_not_authorized")return L"Review this network's echo mapping and the hub connection. Normal setup links your hub automatically. Connection > Advanced > Echo Access Exceptions contains any saved restrictions. Save a correction, then reload Details and request Reprocess Original.";
    if(reason=="ftn_message_identity_or_controls_ambiguous"||reason=="ftn_message_id_content_collision")return L"This message has conflicting identifiers or reuses an identifier for different content. Other valid messages can be imported. The complete original stays intact; ask the sending Sysop to review this message. SFX will not invent a replacement identifier.";
    if(reason=="ftn_packet_password_wrong")return L"Check the sending connection's packet password with its Sysop. After correcting the saved connection, reload Details and request Reprocess Original.";
    return L"Review the exact reason and the saved network, connection or mapping. Reprocessing checks the same original again under current rules. A changed or damaged original cannot be repaired by choosing a recipient.";
}
std::wstring ftn_held_tic_stage(const app::FtnHeldTicDetail& tic){
    if(!tic.publication.valid())return L"No publication started";
    constexpr const wchar_t* stages[]={L"Reserved",L"Copy in progress",L"New file staged",L"Recovery in progress",L"Previous file retained in recovery",L"New file placed; catalog pending",L"Catalog committed"};
    return tic.phase<7?stages[tic.phase]:L"Unavailable";
}
void ftn_held_describe(State& s){
    auto& h=s.ftn.held;std::wstring text=L"Select an original and choose Details.";
    if(h.detail&&h.detail->rows.size()==1){const auto& r=h.detail->rows.front();text=L"Original: "+utf8_to_wide(r.file)+L"\r\nReceived: "+(r.received_utc>0?bbs_local_time(s,r.received_utc):L"Unknown")+L"\r\nNetwork / connection: "+utf8_to_wide(r.network.empty()?"Unconfigured sender":r.network)+L" / "+utf8_to_wide(r.link)+L"\r\nCurrent reason: "+utf8_to_wide(r.reason.empty()?"None":r.reason);
        if(h.detail->retry)text+=L"\r\nLast reviewed retry: "+std::wstring(!h.detail->retry->finished?L"Queued / processing":h.detail->retry->processed?L"Processed":L"Not processed")+(h.detail->retry->reason.empty()?L"":L" ("+utf8_to_wide(h.detail->retry->reason)+L")");
        if(h.detail->tic){const auto& t=*h.detail->tic;const auto known=[](const std::string& value){return utf8_to_wide(value.empty()?"Not recorded":value);};text+=L"\r\nTIC From (claimed): "+known(t.claimed_from)+L"\r\nFile echo: "+known(t.echo)+L"\r\nPayload: "+known(t.payload)+L"\r\nPublication: "+ftn_held_tic_stage(t);
            if(t.publication.valid())text+=L"\r\nOriginal forwarding plan: "+std::wstring(t.forward_plan_present?L"Retained":L"Missing - retry cannot reconstruct it");
            if(t.retry)text+=L"\r\nLast reviewed TIC retry: "+std::wstring(!t.retry->finished?L"Queued / processing":t.retry->processed?L"Processed":L"Not processed")+(t.retry->reason.empty()?L"":L" ("+utf8_to_wide(t.retry->reason)+L")");
            text+=L"\r\n\r\nReview the saved connection, TIC password, file echo mapping and Receive permission. Reload Details after a correction, then choose Reprocess Original. The same original and any partial publication are checked again. Missing plans or changed files cannot be repaired by retrying.";
        }else if(const auto* m=ftn_held_message(s)){text+=L"\r\nFrom: "+utf8_to_wide(m->from)+L" ("+utf8_to_wide(m->origin)+L")\r\nOriginal To: "+utf8_to_wide(m->to)+L" ("+utf8_to_wide(m->destination)+L")\r\nIntended echo: "+utf8_to_wide(m->echo.empty()?"Private NetMail":m->echo)+L"\r\nFirst rejection: "+utf8_to_wide(m->reason);
            if(m->resolution)text+=L"\r\nReviewed local recipient: account "+std::to_wstring(m->resolution->request.recipient)+L", revision "+std::to_wstring(m->resolution->request.recipient_revision)+L". Original To is unchanged.";
            if(m->committed)text+=L"\r\nThis constituent is already committed and cannot be reassigned.";
        }else if(h.detail->messages.empty())text+=L"\r\nNo readable message details were recorded. No sender or recipient is inferred.";
        if(!h.detail->tic)text+=L"\r\n\r\n"+ftn_held_help(r.reason);if(h.detail->serial_recovery_required)text+=L"\r\nMessage ID history requires recovery. New review actions are held.";if(h.detail->mode==app::FtnMailerMode::disabled)text+=L"\r\nThe mailer is disabled. New review actions are held.";
    }SetWindowTextW(h.text,text.c_str());ftn_held_buttons(s);
}
void ftn_held_load_detail(State& s,const app::Identifier128& ingress,std::uint32_t offset=0){
    auto& h=s.ftn.held;if(h.pending){set_status(s,L"Choose Check Action Result before another Held Mail action.");return;}app::FtnHeldReviewQuery q;q.ingress=ingress;q.message_offset=offset;const auto r=ftn_held_exchange(s,q);
    h.detail.reset();h.recipients.reset();SendMessageW(h.messages,CB_RESETCONTENT,0,0);SendMessageW(h.accounts,CB_RESETCONTENT,0,0);
    if(!r||r->code!=app::FtnConfigurationCode::ok||!std::holds_alternative<app::FtnHeldReview>(r->value)){ftn_held_describe(s);set_status(s,L"Held Mail details are unavailable. No action was taken.");return;}
    h.detail=std::get<app::FtnHeldReview>(r->value);
    if(h.list&&h.detail->rows.size()==1){const auto fresh=h.detail->rows.front();for(std::size_t i=0;i<h.list->rows.size();++i)if(h.list->rows[i].ingress.bytes==fresh.ingress.bytes){h.list->rows[i]=fresh;auto received=fresh.received_utc>0?bbs_local_time(s,fresh.received_utc):L"Unknown";auto network=utf8_to_wide(fresh.network.empty()?"Unconfigured":fresh.network);auto from=utf8_to_wide(fresh.from.empty()?fresh.link:fresh.from);ListView_SetItemText(h.rows,static_cast<int>(i),0,received.data());ListView_SetItemText(h.rows,static_cast<int>(i),1,network.data());ListView_SetItemText(h.rows,static_cast<int>(i),2,from.data());auto why=ftn_held_row_reason(fresh);ListView_SetItemText(h.rows,static_cast<int>(i),3,why.data());ftn_held_reason_add(s,fresh.reason);break;}}
    for(const auto& m:h.detail->messages){const auto title=L"Message "+std::to_wstring(m.message_index+1)+L": "+utf8_to_wide(m.to)+(m.committed?L" (committed)":L"");SendMessageW(h.messages,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title.c_str()));}
    // Explicit message selection prevents an old row from becoming a new target.
    SendMessageW(h.messages,CB_SETCURSEL,static_cast<WPARAM>(-1),0);ftn_held_describe(s);set_status(s,h.detail->tic?L"TIC details loaded. Review this exact original before choosing Reprocess Original; no change has been made.":L"Original details loaded. Select a message to review its recipient; no change has been made.");
}
void ftn_held_refresh(State& s,bool next=false){
    auto& h=s.ftn.held;if(h.pending){ftn_held_buttons(s);set_status(s,L"An action is unconfirmed. Choose Check Action Result; its exact request is retained.");return;}
    if(next){if(!h.list||!h.list->more||h.list->rows.empty())return;h.query.after=h.list->rows.back().ingress;}
    else{h.query={};const auto network=SendMessageW(h.network,CB_GETCURSEL,0,0);if(network>=0&&static_cast<std::size_t>(network)<h.network_codes.size())h.query.network=h.network_codes[static_cast<std::size_t>(network)];const auto reason=SendMessageW(h.reason,CB_GETCURSEL,0,0);if(reason>=0&&static_cast<std::size_t>(reason)<h.reason_codes.size())h.query.reason=h.reason_codes[static_cast<std::size_t>(reason)];h.query.find=door_text(h.find);}
    const auto r=ftn_held_exchange(s,h.query);h.list.reset();h.detail.reset();h.recipients.reset();ListView_DeleteAllItems(h.rows);SendMessageW(h.messages,CB_RESETCONTENT,0,0);SendMessageW(h.accounts,CB_RESETCONTENT,0,0);
    if(!r||r->code!=app::FtnConfigurationCode::ok||!std::holds_alternative<app::FtnHeldReview>(r->value)){ftn_held_describe(s);set_status(s,L"Held Mail is unavailable. An empty queue is not claimed.");return;}
    h.list=std::get<app::FtnHeldReview>(r->value);for(std::size_t i=0;i<h.list->rows.size();++i){const auto& row=h.list->rows[i];ftn_held_reason_add(s,row.reason);auto time=row.received_utc>0?bbs_local_time(s,row.received_utc):L"Unknown";LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=time.data();ListView_InsertItem(h.rows,&item);int col=1;for(auto value:{utf8_to_wide(row.network.empty()?"Unconfigured":row.network),utf8_to_wide(row.from.empty()?row.link:row.from),ftn_held_row_reason(row)})ListView_SetItemText(h.rows,static_cast<int>(i),col++,value.data());}
    ftn_held_describe(s);set_status(s,h.list->rows.empty()?L"No matching held originals or pending reviewed retries.":L"Held originals loaded. Select one and choose Details. Network, exact reason and Find filters apply when you choose Find / Reload.");
}
void ftn_held_open(State& s){
    auto& h=s.ftn.held;if(!h.pending){const auto current=SendMessageW(h.network,CB_GETCURSEL,0,0);const auto code=current>=0&&static_cast<std::size_t>(current)<h.network_codes.size()?h.network_codes[static_cast<std::size_t>(current)]:std::string{};const auto c=query_ftn(s);if(c||h.network_codes.empty()){h.network_codes.clear();h.network_codes.push_back({});SendMessageW(h.network,CB_RESETCONTENT,0,0);SendMessageW(h.network,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"All networks"));int selected{};if(c)for(const auto& n:c->networks){h.network_codes.push_back(n.code);if(n.code==code)selected=static_cast<int>(h.network_codes.size()-1);const auto title=utf8_to_wide(n.name);SendMessageW(h.network,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title.c_str()));}SendMessageW(h.network,CB_SETCURSEL,selected,0);}}ftn_held_refresh(s);
}
void ftn_held_selection_changed(State& s){auto& h=s.ftn.held;if(h.pending)return;h.detail.reset();h.recipients.reset();SendMessageW(h.messages,CB_RESETCONTENT,0,0);SendMessageW(h.accounts,CB_RESETCONTENT,0,0);ftn_held_describe(s);}
void ftn_held_accounts(State& s,bool next=false){
    auto& h=s.ftn.held;const auto* m=ftn_held_message(s);if(h.pending||!m||!m->local_private||m->committed)return;
    if(next){if(!h.recipients||!h.recipients->more||h.recipients->rows.empty())return;h.account_query.after=h.recipients->rows.back().id;}else h.account_query={0,door_text(h.account_find)};
    const auto r=ftn_held_exchange(s,h.account_query);h.recipients.reset();SendMessageW(h.accounts,CB_RESETCONTENT,0,0);if(r&&r->code==app::FtnConfigurationCode::ok&&std::holds_alternative<app::FtnHeldRecipients>(r->value)){h.recipients=std::get<app::FtnHeldRecipients>(r->value);for(const auto& account:h.recipients->rows){const auto title=utf8_to_wide(account.name)+L" (account "+std::to_wstring(account.id)+L")";SendMessageW(h.accounts,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title.c_str()));}set_status(s,h.recipients->rows.empty()?L"No matching active local recipients.":L"Select the intended active local recipient. No choice is selected or saved automatically.");}else set_status(s,L"Active recipient choices are unavailable. No choice was saved.");SendMessageW(h.accounts,CB_SETCURSEL,static_cast<WPARAM>(-1),0);ftn_held_buttons(s);
}
void ftn_held_submit(State& s,const app::FtnHeldControlRequest& q){
    auto& h=s.ftn.held;const auto selected=h.detail&&h.detail->rows.size()==1?h.detail->rows.front().ingress:app::Identifier128{};h.pending=q;ftn_held_buttons(s);const auto r=ftn_held_exchange(s,q);
    if(!r||r->code==app::FtnConfigurationCode::unavailable){set_status(s,L"Action result is unconfirmed. Choose Check Action Result to reconcile this exact request; do not repeat it as a new action.");return;}
    const bool expected=q.index()==2?std::holds_alternative<app::FtnRecipientResolution>(r->value):q.index()==4?std::holds_alternative<app::FtnTicRetryRecord>(r->value):std::holds_alternative<app::FtnReprocessRecord>(r->value);
    if(r->code==app::FtnConfigurationCode::ok&&!expected){set_status(s,L"Action result could not be verified. The exact request is retained for Check Action Result.");return;}
    h.pending.reset();if(selected.valid())ftn_held_load_detail(s,selected);ftn_held_buttons(s);
    if(r->code!=app::FtnConfigurationCode::ok){set_status(s,r->code==app::FtnConfigurationCode::busy?L"The writer is busy. Reload Details and review before trying again.":L"The original, settings or selected account changed. Reload Details and review again; no stale choice was applied.");return;}
    if(q.index()==2)set_status(s,L"Recipient choice saved. The original To is unchanged. Choose Reprocess Original for a separate current-policy check.");
    else{const auto finished=q.index()==4?std::get<app::FtnTicRetryRecord>(r->value).finished:std::get<app::FtnReprocessRecord>(r->value).finished;const auto processed=q.index()==4?std::get<app::FtnTicRetryRecord>(r->value).processed:std::get<app::FtnReprocessRecord>(r->value).processed;set_status(s,finished?(processed?L"This exact reviewed retry completed. The original was processed.":L"This exact reviewed retry did not process the original. Review its current reason."):s.mode==State::ConnectionMode::offline?L"Reviewed retry queued for the next enabled BBS start. It has not processed the original.":L"Reviewed retry queued for the existing mail processor. Reload Details to see its result.");}
}
void ftn_held_resolve(State& s){
    auto& h=s.ftn.held;const auto* m=ftn_held_message(s);const auto row=SendMessageW(h.accounts,CB_GETCURSEL,0,0);if(h.pending||!h.detail||h.detail->rows.size()!=1||!m||!m->local_private||m->committed||!h.recipients||row<0||static_cast<std::size_t>(row)>=h.recipients->rows.size())return;
    const auto account=h.recipients->rows[static_cast<std::size_t>(row)];app::FtnRecipientResolutionRequest q;q.operation.bytes=ftn_token();q.held_message=m->operation;q.previous_resolution=m->resolution?m->resolution->request.operation:app::Identifier128{};q.configuration_revision=h.detail->configuration_revision;q.recipient=account.id;q.recipient_revision=account.revision;
    const auto body=L"Original From: "+utf8_to_wide(m->from)+L"\nOriginal To: "+utf8_to_wide(m->to)+L"\n\nChoose "+utf8_to_wide(account.name)+L" (account "+std::to_wstring(account.id)+L") as this private message's local recipient?\n\nThe original To and packet bytes remain unchanged. Reprocessing is a separate action.";
    if(MessageBoxW(s.window,body.c_str(),L"Review Local Recipient",MB_OKCANCEL|MB_DEFBUTTON2|MB_ICONWARNING)!=IDOK){set_status(s,L"Recipient review cancelled. No choice was saved.");return;}ftn_held_submit(s,q);
}
void ftn_held_reprocess(State& s){
    auto& h=s.ftn.held;if(h.pending||!h.detail||h.detail->rows.size()!=1)return;const auto& original=h.detail->rows.front();
    if(original.tic){if(!h.detail->tic||original.state==app::FtnIngressState::processed||original.retry_pending||h.detail->serial_recovery_required||h.detail->mode==app::FtnMailerMode::disabled)return;const auto& detail=*h.detail->tic;app::FtnTicRetryRequest q;q.operation.bytes=ftn_token();q.ingress=original.ingress;q.previous_request=detail.retry?detail.retry->request.operation:app::Identifier128{};q.configuration_revision=h.detail->configuration_revision;q.review=detail.review;
        const auto body=L"Retry this exact retained TIC and its original publication?\n\nTIC: "+utf8_to_wide(original.file)+L"\nEcho: "+utf8_to_wide(detail.echo.empty()?"Not recorded":detail.echo)+L"\nPayload: "+utf8_to_wide(detail.payload.empty()?"Not recorded":detail.payload)+L"\nStage: "+ftn_held_tic_stage(detail)+L"\nReason: "+utf8_to_wide(original.reason)+L"\n\nCurrent credentials and permissions, original files, recovery and forwarding plan will be checked again. Changed or missing objects stay held. This queues one reviewed attempt; it does not promise completion.";
        if(MessageBoxW(s.window,body.c_str(),L"Review Reprocess TIC",MB_OKCANCEL|MB_DEFBUTTON2|MB_ICONWARNING)!=IDOK){set_status(s,L"TIC review cancelled. No retry was queued.");return;}ftn_held_submit(s,q);return;
    }if(original.state!=app::FtnIngressState::held||original.retry_pending||!app::ftn_message_wire_name(original.file))return;
    app::FtnReprocessRequest q;q.operation.bytes=ftn_token();q.ingress=original.ingress;q.previous_request=h.detail->retry?h.detail->retry->request.operation:app::Identifier128{};q.configuration_revision=h.detail->configuration_revision;
    const auto body=L"Reprocess this exact retained original?\n\nFile: "+utf8_to_wide(original.file)+L"\nCurrent reason: "+utf8_to_wide(original.reason)+L"\n\nCurrent credentials, admission, mappings and recipient decisions will be checked again. Already committed messages are not delivered twice. This does not resend mail or replace the original.";
    if(MessageBoxW(s.window,body.c_str(),L"Review Reprocess Original",MB_OKCANCEL|MB_DEFBUTTON2|MB_ICONWARNING)!=IDOK){set_status(s,L"Reprocess cancelled. No retry was queued.");return;}ftn_held_submit(s,q);
}
bool ftn_held_command(State& s,int id,int notification){
    if(id<IDC_FTN_HELD_NETWORK||id>IDC_FTN_HELD_CHECK)return false;auto& h=s.ftn.held;
    if(h.pending&&id!=IDC_FTN_HELD_CHECK){set_status(s,L"Choose Check Action Result before another Held Mail action.");return true;}
    switch(id){case IDC_FTN_HELD_REFRESH:ftn_held_refresh(s);break;case IDC_FTN_HELD_NEXT:ftn_held_refresh(s,true);break;
    case IDC_FTN_HELD_DETAILS:{const auto row=ListView_GetNextItem(h.rows,-1,LVNI_SELECTED);if(h.list&&row>=0&&static_cast<std::size_t>(row)<h.list->rows.size())ftn_held_load_detail(s,h.list->rows[static_cast<std::size_t>(row)].ingress);else set_status(s,L"Select a held original first.");break;}
    case IDC_FTN_HELD_MESSAGES:if(notification==CBN_SELCHANGE){h.recipients.reset();SendMessageW(h.accounts,CB_RESETCONTENT,0,0);ftn_held_describe(s);}break;
    case IDC_FTN_HELD_MESSAGE_NEXT:if(h.detail&&h.detail->more&&!h.detail->messages.empty())ftn_held_load_detail(s,h.detail->rows.front().ingress,static_cast<std::uint32_t>(h.detail->messages.back().message_index)+1);break;
    case IDC_FTN_HELD_ACCOUNT_SEARCH:ftn_held_accounts(s);break;case IDC_FTN_HELD_ACCOUNT_NEXT:ftn_held_accounts(s,true);break;case IDC_FTN_HELD_ACCOUNTS:ftn_held_buttons(s);break;
    case IDC_FTN_HELD_RESOLVE:ftn_held_resolve(s);break;case IDC_FTN_HELD_REPROCESS:ftn_held_reprocess(s);break;case IDC_FTN_HELD_CHECK:if(h.pending){const auto q=*h.pending;ftn_held_submit(s,q);}break;default:break;
    }return true;
}
void create_ftn_held(State& s){
    auto& h=s.ftn.held;FtnControls ui{s,s.pages[36]=page(s)};ui.label(L"Held Mail",12,8);h.network=ui.combo(L"Network",12,48,160,IDC_FTN_HELD_NETWORK);h.reason=ui.combo(L"Reason",186,48,174,IDC_FTN_HELD_REASON);h.find=ui.edit(L"Find",374,48,148,IDC_FTN_HELD_FIND,140);
    for(const auto* reason:{"","ftn_recipient_unmatched","ftn_recipient_ambiguous","ftn_recipient_stale","ftn_echo_receive_not_authorized","ftn_packet_password_wrong","ftn_packet_invalid","ftn_message_import_held"})ftn_held_reason_add(s,reason);SendMessageW(h.reason,CB_SETCURSEL,0,0);
    ui.button(L"Find / Reload",12,108,160,IDC_FTN_HELD_REFRESH);ui.button(L"Next Originals",186,108,174,IDC_FTN_HELD_NEXT);ui.button(L"Details",374,108,148,IDC_FTN_HELD_DETAILS);
    h.rows=control(s,ui.parent,WC_LISTVIEWW,L"",WS_TABSTOP|WS_BORDER|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,12,150,510,146,IDC_FTN_HELD_ROWS);ListView_SetExtendedListViewStyle(h.rows,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);int column{};for(const auto& entry:std::array<std::pair<const wchar_t*,int>,4>{{{L"Received",170},{L"Network",100},{L"From",120},{L"Reason",220}}}){LVCOLUMNW c{};c.mask=LVCF_TEXT|LVCF_WIDTH;c.pszText=const_cast<wchar_t*>(entry.first);c.cx=MulDiv(entry.second,s.dpi,96);ListView_InsertColumn(h.rows,column++,&c);}
    h.messages=ui.combo(L"Message in selected original",12,308,348,IDC_FTN_HELD_MESSAGES);ui.button(L"More Messages",374,330,148,IDC_FTN_HELD_MESSAGE_NEXT);
    h.text=control(s,ui.parent,L"EDIT",L"Select an original and choose Details.",WS_TABSTOP|WS_BORDER|ES_READONLY|ES_MULTILINE|WS_VSCROLL,12,372,510,184,IDC_FTN_HELD_TEXT);
    ui.button(L"Open FTN History Recovery",12,786,320,IDC_FTN_HISTORY_OPEN);
    h.account_find=ui.edit(L"Find active local recipient",12,570,348,IDC_FTN_HELD_ACCOUNT_FIND,140);ui.button(L"Find Recipients",374,592,148,IDC_FTN_HELD_ACCOUNT_SEARCH);
    h.accounts=ui.combo(L"Choose intended local recipient",12,630,348,IDC_FTN_HELD_ACCOUNTS);ui.button(L"More Recipients",374,652,148,IDC_FTN_HELD_ACCOUNT_NEXT);
    ui.button(L"Review Recipient...",12,698,240,IDC_FTN_HELD_RESOLVE);ui.button(L"Reprocess Original...",270,698,252,IDC_FTN_HELD_REPROCESS);ui.button(L"Check Action Result",12,742,240,IDC_FTN_HELD_CHECK);ui.button(L"Back to Networks",270,742,252,IDC_FTN_BACK);ftn_held_buttons(s);
}
