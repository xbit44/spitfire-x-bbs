// Included after message configuration actions; all authority remains in the
// authenticated Runtime channel or genuine held offline transaction context.
app::MessageMaintenanceKind selected_message_job(const State& s){return s.selected_job==2?app::MessageMaintenanceKind::integrity:app::MessageMaintenanceKind::retention;}
app::MessageMaintenanceStatus query_message_job(State& s){
    const auto kind=selected_message_job(s);
    if(s.mode==State::ConnectionMode::offline)return win32::offline_message_maintenance_status(s.root,s.owner,kind);
    if(s.mode!=State::ConnectionMode::online)return {};
    app::LocalControlFrame response;const auto outgoing=request(s,app::LocalControlMessageType::message_maintenance_query_request,app::encode_message_maintenance_kind(kind));
    if(!online_exchange(s,outgoing,app::LocalControlMessageType::message_maintenance_query_response,response))return {};
    const auto result=app::decode_message_maintenance_status(response.payload);return result && result->kind==kind?*result:app::MessageMaintenanceStatus{};
}
const wchar_t* maintenance_outcome(app::MessageMaintenanceOutcome outcome){
    switch(outcome){case app::MessageMaintenanceOutcome::running:return L"RUNNING";case app::MessageMaintenanceOutcome::success:return L"COMPLETED";
        case app::MessageMaintenanceOutcome::partial:return L"PARTIAL — earlier batches committed";case app::MessageMaintenanceOutcome::failed:return L"FAILED";
        case app::MessageMaintenanceOutcome::skipped:return L"SKIPPED";case app::MessageMaintenanceOutcome::interrupted:return L"INTERRUPTED — not replayed";}return L"Unknown";
}
const wchar_t* maintenance_reason(app::MessageMaintenanceReason reason){
    switch(reason){case app::MessageMaintenanceReason::none:return L"None";case app::MessageMaintenanceReason::corruption_hold:return L"Corruption hold";
        case app::MessageMaintenanceReason::busy:return L"Storage busy";case app::MessageMaintenanceReason::policy_changed:return L"Captured policy changed";
        case app::MessageMaintenanceReason::cancelled:return L"Cancelled";case app::MessageMaintenanceReason::deadline:return L"Check/work deadline";
        case app::MessageMaintenanceReason::corrupt:return L"Integrity findings";case app::MessageMaintenanceReason::storage_failure:return L"Storage/publication failure";
        case app::MessageMaintenanceReason::stale:return L"Stale result";case app::MessageMaintenanceReason::incomplete:return L"Incomplete verification";
        case app::MessageMaintenanceReason::runtime_restart:return L"Runtime restart";}return L"Unknown";
}
void render_message_job(State& s){
    const auto& status=s.message_job_status;
    if(!status.available){SetWindowTextW(s.schedule_detail,L"Message maintenance unavailable. Reload the matching installation; no empty history or clean check is claimed.");return;}
    const auto& c=status.configuration;const auto& run=status.state;
    const wchar_t* days[]={L"Sunday",L"Monday",L"Tuesday",L"Wednesday",L"Thursday",L"Friday",L"Saturday"};
    std::wstring out=s.selected_job==2?L"Built-in job: Weekly message integrity check":L"Built-in job: Daily message retention";
    out+=L"\r\nSchedule: "+std::wstring(c.enabled?L"On":L"Off")+L"; "+(c.local_weekday?std::wstring(days[*c.local_weekday])+L" ":L"Daily ")+
        std::to_wstring(c.local_hour)+L":"+(c.local_minute<10?L"0":L"")+std::to_wstring(c.local_minute)+L" BBS local";
    out+=L"\r\nNext eligible run: "+bbs_local_time(s,run.next_run_utc)+L"\r\nCurrent state: "+(run.active?std::wstring(L"RUNNING"):std::wstring(L"Idle"));
    out+=L"\r\nLast completion: "+bbs_local_time(s,run.last_run_utc)+L"\r\nLast result: "+scheduler_result_text(run.last_result)+L"\r\nDetail: "+utf8_to_wide(run.last_detail);
    out+=L"\r\nShared current-verification prerequisite: "+std::wstring(status.shared_verification_required?L"HOLD — fresh complete current integrity check required":L"No uncertainty hold");
    out+=L"\r\nCorruption holds: "+std::to_wstring(status.holds.size());
    for(const auto& h:status.holds)out+=L"\r\n  "+(h.scope==0?std::wstring(L"Shared store"):h.scope==-1?std::wstring(L"Private mail"):L"Echo ID "+std::to_wstring(h.scope))+L": "+std::to_wstring(h.count)+L" finding(s)";
    if(status.latest){const auto& last=*status.latest;out+=L"\r\n\r\nActual committed run: "+std::wstring(maintenance_outcome(last.outcome))+L"; reason: "+maintenance_reason(last.reason);
        out+=L"\r\nDuration: "+(last.finished_utc?std::to_wstring(last.finished_utc-last.started_utc)+L"s":std::wstring(L"Still running"))+L"; committed batches: "+std::to_wstring(last.committed_batches);
        out+=L"\r\nPublic removed: "+std::to_wstring(last.removed_public)+L"; unread recipient inbox expired: "+std::to_wstring(last.expired_unread);
        for(const auto& e:last.echoes)out+=L"\r\n  Echo ID "+std::to_wstring(e.echo.value)+L": removed "+std::to_wstring(e.removed)+L"; Permanent overage "+std::to_wstring(e.protected_overage);}
    out+=L"\r\n\r\nMissed runs skipped; no immediate retry, purge on Save, automatic repair or nightly compaction. Fresh complete current clean checking clears only corruption/verification holds; it never runs retention. Details use existing Logs/history.";
    SetWindowTextW(s.schedule_detail,out.c_str());
}
void populate_message_jobs(State& s,bool keep_unread_draft){
    const bool message=s.selected_job!=0,writable=s.mode!=State::ConnectionMode::blocked && !s.system_recovery_only;
    SendMessageW(s.schedule_job,CB_SETCURSEL,s.selected_job,0);
    for(const auto h:s.backup_job_controls)ShowWindow(h,message?SW_HIDE:SW_SHOW);
    for(const auto h:s.message_job_controls)ShowWindow(h,message?SW_SHOW:SW_HIDE);
    ShowWindow(s.schedule_weekday,s.selected_job==2?SW_SHOW:SW_HIDE);
    EnableWindow(s.schedule_job,writable);
    if(!message)return;
    s.message_job_status=query_message_job(s);
    const auto policy=query_message_configuration(s,{});if(!keep_unread_draft)s.private_mail_policy=policy;
    Button_SetCheck(s.schedule_enabled,s.message_job_status.available && s.message_job_status.configuration.enabled?BST_CHECKED:BST_UNCHECKED);
    SetWindowTextW(s.schedule_enabled,L"Enable selected message job");
    SetWindowTextW(s.schedule_hour,s.message_job_status.available?std::to_wstring(s.message_job_status.configuration.local_hour).c_str():L"");
    SetWindowTextW(s.schedule_minute,s.message_job_status.available?std::to_wstring(s.message_job_status.configuration.local_minute).c_str():L"");
    SendMessageW(s.schedule_weekday,CB_SETCURSEL,s.message_job_status.configuration.local_weekday.value_or(0),0);
    if(!keep_unread_draft)SetWindowTextW(s.unread_mail_age,policy.code==app::LocalAccountResultCode::ok?(policy.catalog.unread_mail_age_days?std::to_wstring(*policy.catalog.unread_mail_age_days).c_str():L"Unlimited"):L"");
    for(const auto h:{s.schedule_enabled,s.schedule_hour,s.schedule_minute,s.schedule_weekday,s.schedule_save})EnableWindow(h,writable && s.message_job_status.available);
    for(const auto h:{s.unread_mail_age,s.unread_mail_save})EnableWindow(h,writable && policy.code==app::LocalAccountResultCode::ok);
    EnableWindow(s.message_run_now,writable && s.mode==State::ConnectionMode::online && s.message_job_status.available && !s.message_job_status.state.active);
    SetWindowTextW(s.message_run_now,s.selected_job==2?L"Check Integrity Now":L"Run Retention Now");
    EnableWindow(s.message_compact,writable && s.mode==State::ConnectionMode::offline);
    render_message_job(s);
}
bool save_message_schedule(State& s){
    const auto hour=byte_value(s.schedule_hour,23),minute=byte_value(s.schedule_minute,59);
    if(!hour || !minute || !s.message_job_status.available){set_status(s,L"Refused — enter a valid BBS-local time and reload authoritative job state.");return false;}
    app::MessageMaintenanceScheduleRequest change;change.kind=selected_message_job(s);change.expected_revision=s.message_job_status.configuration.revision;
    change.enabled=Button_GetCheck(s.schedule_enabled)==BST_CHECKED;change.hour=*hour;change.minute=*minute;
    if(s.selected_job==2){const auto day=SendMessageW(s.schedule_weekday,CB_GETCURSEL,0,0);if(day<0 || day>6){set_status(s,L"Refused — choose Sunday through Saturday.");return false;}change.weekday=static_cast<std::uint8_t>(day);}
    app::AlphaControlResponse result{app::LocalControlResponseCode::configuration_failure,"message_schedule_channel_unconfirmed"};
    if(s.mode==State::ConnectionMode::offline)result=win32::offline_configure_message_schedule(s.root,s.owner,change);
    else if(s.mode==State::ConnectionMode::online){app::LocalControlFrame response;const auto outgoing=request(s,app::LocalControlMessageType::message_maintenance_schedule_request,app::encode_message_maintenance_schedule(change));
        if(!outgoing.payload.empty() && online_exchange(s,outgoing,app::LocalControlMessageType::message_maintenance_schedule_response,response)){const auto decoded=app::decode_control_response(response.payload);if(decoded)result=*decoded;}}
    if(result.code!=app::LocalControlResponseCode::ok){set_status(s,L"Refused — "+utf8_to_wide(result.diagnostic_code)+L". No purge or automatic retry.");return false;}
    populate_message_jobs(s,true);
    for(auto& baseline:s.baseline)if(baseline.control==s.schedule_enabled)baseline.value=std::to_wstring(Button_GetCheck(s.schedule_enabled));else if(baseline.control==s.schedule_hour || baseline.control==s.schedule_minute)baseline.value=text(baseline.control);else if(baseline.control==s.schedule_weekday)baseline.value=std::to_wstring(SendMessageW(s.schedule_weekday,CB_GETCURSEL,0,0));
    set_status(s,L"Saved — message schedule only. Next eligible run uses it; no cleanup on Save and no backup schedule change.");return true;
}
bool save_unread_mail_age(State& s){
    if(s.pending_message_apply || s.private_mail_policy.code!=app::LocalAccountResultCode::ok)return false;
    const auto encoded=wide_to_utf8(text(s.unread_mail_age));const auto limit=encoded?app::parse_message_retention_limit(*encoded,app::kMaximumMessageAgeDays):app::ParsedMessageRetentionLimit{};
    if(!limit.valid){set_status(s,L"Refused — unread-mail age must be 1–65,535 full days or Unlimited; draft retained.");return false;}
    const auto previous=s.private_mail_policy.catalog.unread_mail_age_days;
    const bool shorter=limit.value && (!previous || *limit.value<*previous);
    app::MessageConfigurationChange change;change.action=app::MessageConfigurationAction::unread_mail_retention;change.expected_configuration_revision=s.private_mail_policy.catalog.revision;
    change.expected_unread_retention_revision=s.private_mail_policy.catalog.unread_retention_revision;change.unread_mail_age_days=limit.value;
    if(!apply_message_change(s,change))return false;
    s.private_mail_policy=query_message_configuration(s,{});
    for(auto& baseline:s.baseline)if(baseline.control==s.unread_mail_age)baseline.value=text(s.unread_mail_age);
    set_status(s,shorter?L"Saved — older unread inbox copies can expire at a later retention run. Saving keeps current mail.":
        L"Saved — unread inbox age applies on later retention runs. Read mail and sender copies are retained.");return true;
}
void run_message_job_now(State& s){
    if(s.selected_job==0 || !resolve_dirty(s))return;
    if(s.mode!=State::ConnectionMode::online){set_status(s,L"Refused — start Runtime normally for bounded online Run Now. This does not start Runtime or force callers off.");return;}
    app::LocalControlFrame response;const auto outgoing=request(s,app::LocalControlMessageType::message_maintenance_run_request,app::encode_message_maintenance_kind(selected_message_job(s)));
    if(!online_exchange(s,outgoing,app::LocalControlMessageType::message_maintenance_run_response,response)){set_status(s,L"Outcome unconfirmed — inspect the same job's status; do not blindly click Run Now again.");return;}
    const auto result=app::decode_control_response(response.payload);
    if(!result || result->code!=app::LocalControlResponseCode::ok){set_status(s,L"Refused — "+utf8_to_wide(result?result->diagnostic_code:"invalid maintenance response"));return;}
    s.message_job_status=query_message_job(s);render_message_job(s);EnableWindow(s.message_run_now,FALSE);SetTimer(s.window,config_timer::message_maintenance,1500,nullptr);
    set_status(s,L"Queued — review actual completion below or in existing Logs. Committed counts are not predicted.");
}
void compact_messages_ui(State& s){
    if(!resolve_dirty(s))return;
    if(s.mode!=State::ConnectionMode::offline){set_status(s,L"Refused — stop Runtime normally before physical compaction. No callers are forced off.");return;}
    if(MessageBoxW(s.window,L"Compact the stopped shared database? A NEW successfully verified Backup Now is mandatory first, using the saved destination/retention. Failure refuses compaction. All logical data/IDs must remain; size need not shrink. This is not repair or secure erasure.",L"Manual Physical Compaction",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)!=IDYES)return;
    win32::OfflineCompactionResult result;
    if(!maintenance_progress(s,L"Verified backup and manual physical compaction",[&](std::function<bool()> stop){result=win32::offline_compact_messages(s.root,s.owner,std::move(stop));}))return;
    const auto detail=std::wstring(result.status.succeeded()?L"Completed":L"Failed or cancelled")+L" — physical commit: "+(result.physical_compaction_committed?L"Yes":L"No")+L"; all logical contents verified: "+(result.logical_content_verified?L"Yes":L"Not confirmed")+
        L"\r\nBytes before/after: "+std::to_wstring(result.bytes_before)+L" / "+std::to_wstring(result.bytes_after)+L"\r\nVerified prerequisite backup: "+(result.verified_backup.empty()?std::wstring(L"None"):result.verified_backup.native())+L"\r\n"+utf8_to_wide(result.status.operation)+L"\r\nNo automatic repair/restore/retry. Inspect retained recovery data if verification is unconfirmed.";
    SetWindowTextW(s.schedule_detail,detail.c_str());set_status(s,result.status.succeeded()?L"Completed — manual compaction preserved verified logical contents.":L"Failed or cancelled — see actual commit/verification and recovery backup below; no success claimed.");refresh_backup_inventory(s);
}
void delete_public_area_ui(State& s,const app::MessageDeletionTarget& target){
    if(s.mode!=State::ConnectionMode::offline){set_status(s,L"Refused — stop Runtime normally to delete an echo/group and messages. Connected callers and selected data are preserved.");return;}
    std::unique_ptr<win32::OfflineMessageDeletion> context;std::string diagnostic;
    if(!maintenance_progress(s,L"Verify and bind exact public-area deletion",[&](std::function<bool()> stop){context=win32::OfflineMessageDeletion::begin(s.root,s.owner,target,diagnostic,std::move(stop));}))return;
    if(!context){set_status(s,L"Refused — Runtime started, target changed, or exclusive preview was unavailable. Reload and explicitly select the target again; nothing substituted.");return;}
    const auto& p=context->preview();const auto noun=p.target.kind==app::MessageDeletionKind::group?L"group":L"echo";
    const auto backup_question=L"Would you like to back up before deleting this "+std::wstring(noun)+L"?\r\n\r\n"+utf8_to_wide(p.name)+L"\r\nYes creates and verifies a backup using the saved destination/retention. No skips it. Either answer still requires a separate final default-No deletion confirmation. Cancel stops without deletion.";
    const auto backup_choice=MessageBoxW(s.window,backup_question.c_str(),L"Optional Deletion Backup",MB_YESNOCANCEL|MB_DEFBUTTON3|MB_ICONQUESTION);
    if(backup_choice!=IDYES && backup_choice!=IDNO){set_status(s,L"Cancelled — exact area and all messages preserved.");return;}
    if(backup_choice==IDYES){win32::OfflineMaintenanceBackupResult backup;
        if(!maintenance_progress(s,L"Create and verify requested deletion backup",[&](std::function<bool()> stop){backup=context->backup(std::move(stop));}))return;
        refresh_backup_inventory(s);
        if(!backup.status){set_status(s,L"Failed — requested backup was not successfully verified. This deletion attempt stopped; no silent no-backup fallback. A new deliberate attempt may choose No.");return;}}
    const auto confirm=L"Permanently delete "+std::wstring(noun)+L" \""+utf8_to_wide(p.name)+L"\" and ALL included messages?\r\n\r\nEchoes: "+std::to_wstring(p.echo_count)+L"\r\nMessages: "+std::to_wstring(p.message_count)+L"\r\nPermanent messages INCLUDED: "+std::to_wstring(p.permanent_count)+L"\r\n\r\nOther groups/echoes/private mail/accounts/jobs and existing backups are preserved. This is not Disable, compaction or secure erasure. Recovery requires a supported backup restore.";
    if(MessageBoxW(s.window,confirm.c_str(),L"Delete Area and Messages — Default No",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)!=IDYES){set_status(s,L"Cancelled — area/configuration/messages preserved. Any successfully requested backup remains.");return;}
    app::StorageResult<app::MessageDeletionResult> committed;
    if(!maintenance_progress(s,L"Commit only the exact confirmed deletion",[&](std::function<bool()> stop){committed=context->commit(std::move(stop));}))return;
    if(!committed || !committed.value){
        if(MessageBoxW(s.window,L"Deletion did not report success. Reconcile the SAME exact operation once? This checks its existing receipt and exact target; it never selects another target. No automatic retry or generic repair.",L"Unconfirmed Deletion Outcome",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)==IDYES)
            if(!maintenance_progress(s,L"Reconcile the same confirmed deletion",[&](std::function<bool()> stop){committed=context->commit(std::move(stop));}))return;
    }
    // Release the held guard/database BEFORE an ordinary catalog refresh.
    context.reset();
    if(!committed || !committed.value){set_status(s,L"Failed/stale/cancelled — no successful deletion claimed. Reload the authoritative catalog and inspect retained backups; do not assume a whole operation committed or rolled back.");return;}
    const auto result=*committed.value;s.message_group_cursor={};refresh_message_configuration(s);remember_page(s);
    set_status(s,L"Completed — exact confirmed deletion: "+std::to_wstring(result.echo_count)+L" echo(es), "+std::to_wstring(result.message_count)+L" message(s), including "+std::to_wstring(result.permanent_count)+L" Permanent. Unrelated data preserved.");
}
