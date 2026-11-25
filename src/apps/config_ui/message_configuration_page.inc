// Native private configuration clients. Catalog/body SQL never belongs here.
#ifdef SFX_ALPHA_TEST_AUTOMATION
app::LocalMessageConfigurationSnapshot (*message_query_fixture)(const app::LocalMessageConfigurationQuery&)=nullptr;
app::LocalMessageConfigurationResult (*message_change_fixture)(const app::MessageConfigurationChange&)=nullptr;
#endif
app::LocalMessageConfigurationSnapshot query_message_configuration(State& s,const app::LocalMessageConfigurationQuery& q){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(message_query_fixture)return message_query_fixture(q);
#endif
    if(s.system_recovery_only || !load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline&&!s.navigation_rendering)return win32::offline_message_configuration_snapshot(s.root,s.owner,q);
    if(s.mode!=State::ConnectionMode::online&&!s.navigation_rendering)return {};app::LocalControlFrame response;
    if(!online_exchange(s,request(s,app::LocalControlMessageType::message_configuration_query_request,app::encode_message_configuration_query(q)),app::LocalControlMessageType::message_configuration_query_response,response))return {};
    return app::decode_message_configuration_snapshot(response.payload).value_or(app::LocalMessageConfigurationSnapshot{});
}
app::LocalMessageConfigurationResult change_message_configuration(State& s,const app::MessageConfigurationChange& c){
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(message_change_fixture)return message_change_fixture(c);
#endif
    if(s.system_recovery_only || !load_editor_state(s))return {};
    if(s.mode==State::ConnectionMode::offline)return win32::offline_change_message_configuration(s.root,s.owner,c);
    if(s.mode!=State::ConnectionMode::online)return {};app::LocalControlFrame response;
    const auto payload=app::encode_message_configuration_change(c);if(payload.empty())return {app::LocalAccountResultCode::invalid_request};
    if(!online_exchange(s,request(s,app::LocalControlMessageType::message_configuration_change_request,payload),app::LocalControlMessageType::message_configuration_change_response,response))return {};
    return app::decode_message_configuration_result(response.payload).value_or(app::LocalMessageConfigurationResult{});
}
std::optional<std::int64_t> message_number(HWND field,std::int64_t maximum){
    const auto input=text(field);if(input.empty() || input.size()>19)return {};std::int64_t number{};
    for(const auto ch:input){if(ch<L'0' || ch>L'9' || number>(maximum-(ch-L'0'))/10)return {};number=number*10+(ch-L'0');}return number;
}
void update_private_pipe_controls(State& state) {
    const bool pending = state.pending_message_apply &&
        state.pending_message_apply->action == app::MessageConfigurationAction::private_presentation;
    EnableWindow(state.private_pipe_colors, state.private_pipe_available && !state.pending_message_apply && !state.system_recovery_only);
    EnableWindow(state.private_pipe_reconcile, pending);
    if (state.private_pipe_reconcile) ShowWindow(state.private_pipe_reconcile, pending ? SW_SHOW : SW_HIDE);
}

void refresh_private_pipe_colors(State& state) {
    if (!state.pending_message_apply) {
        const auto current = query_message_configuration(state, {});
        state.private_pipe_available = current.code == app::LocalAccountResultCode::ok;
        if (state.private_pipe_available)
            Button_SetCheck(state.private_pipe_colors, current.catalog.private_pipe_colors ? BST_CHECKED : BST_UNCHECKED);
    }
    update_private_pipe_controls(state);
}

void message_controls(State& s,bool available){
    for(const auto page_index:{0,8,19,20})for(HWND child=GetWindow(s.pages[page_index],GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        const auto id=GetDlgCtrlID(child);
        if((id>=IDC_MESSAGE_GROUPS && id<=IDC_LEVEL_MESSAGE_SAVE && id!=IDC_MESSAGE_RECONCILE) || (id>=IDC_MESSAGE_MAX_COUNT && id<=IDC_CLONE_MAX_AGE))
            EnableWindow(child,available && !s.pending_message_apply);
    }
    const auto level=message_number(s.level_number,999);
    const bool finite_level=available && !s.pending_message_apply &&
        s.message_limits.code==app::LocalAccountResultCode::ok &&
        std::any_of(s.message_limits.catalog.line_limits.begin(),s.message_limits.catalog.line_limits.end(),
            [&](const app::MessageLineLimit& value){return level && value.security_level==*level;});
    EnableWindow(s.level_message_lines,finite_level);EnableWindow(s.level_message_save,finite_level);
    EnableWindow(s.message_pipe_colors,available && s.editing_echo && !s.pending_message_apply);
    update_private_pipe_controls(s);
    EnableWindow(s.message_max_count,available && s.editing_echo && !s.pending_message_apply);
    EnableWindow(s.message_max_age,available && s.editing_echo && !s.pending_message_apply);
}
bool apply_message_change(State& s,app::MessageConfigurationChange c,bool reconcile=false){
    if(s.pending_message_apply && !reconcile){set_status(s,L"Outcome unresolved — reconcile the existing operation before another message change.");return false;}
    if(!reconcile){
        try {win32::WindowsSecureIdentifierGenerator generator;c.operation=generator.generate();}catch(...){set_status(s,L"Unavailable — a secure operation identity could not be created. Draft retained.");return false;}
    }
    if(!app::valid_message_configuration_change(c)){set_status(s,L"Refused — invalid message settings. Draft retained.");return false;}
    const auto result=change_message_configuration(s,c);
    if(result.code==app::LocalAccountResultCode::unavailable || result.code==app::LocalAccountResultCode::storage_failure){
        s.pending_message_apply=c;message_controls(s,false);
        set_status(s,L"Outcome unresolved — edits are frozen. Use Reconcile; the same operation will be checked, not replaced.");return false;
    }
    s.pending_message_apply.reset();message_controls(s,true);
    if(result.code!=app::LocalAccountResultCode::ok){set_status(s,L"Refused — current revisions, permissions or assignment eligibility changed. No matching commit; draft retained. Reload and review.");return false;}
    set_status(s,result.result.replayed?L"Saved — matching committed operation reconciled. Do not repeat it.":L"Saved — message configuration committed and audited. Current caller operations use it immediately.");return true;
}
void select_message_limit(State& s){
    const auto level=message_number(s.level_number,999);const auto& snapshot=s.message_limits;
    const auto found=std::find_if(snapshot.catalog.line_limits.begin(),snapshot.catalog.line_limits.end(),[&](const app::MessageLineLimit& l){return level && l.security_level==*level;});
    const bool available=snapshot.code==app::LocalAccountResultCode::ok && found!=snapshot.catalog.line_limits.end() && !s.pending_message_apply;
    SetWindowTextW(s.level_message_lines,available?std::to_wstring(found->maximum_lines).c_str():L"");
    EnableWindow(s.level_message_lines,available);EnableWindow(s.level_message_save,available);
}
void refresh_message_limit(State& s){s.message_limits=query_message_configuration(s,{});select_message_limit(s);}
void save_message_limit(State& s){
    const auto level=message_number(s.level_number,999),lines=message_number(s.level_message_lines,65535);
    const auto found=std::find_if(s.message_limits.catalog.line_limits.begin(),s.message_limits.catalog.line_limits.end(),[&](const app::MessageLineLimit& l){return level && l.security_level==*level;});
    if(!lines || !*lines || s.message_limits.code!=app::LocalAccountResultCode::ok || found==s.message_limits.catalog.line_limits.end()){set_status(s,L"Refused — select an existing active level and enter 1–65,535 body lines. New levels start at 200.");return;}
    app::MessageConfigurationChange c;c.action=app::MessageConfigurationAction::line_limit;c.expected_configuration_revision=s.message_limits.catalog.revision;
    c.security_level=found->security_level;c.maximum_lines=static_cast<std::uint32_t>(*lines);c.expected_limit_revision=found->revision;
    if(apply_message_change(s,c)){
        const auto committed=text(s.status);
        refresh_message_limit(s);
        set_status(s,s.message_limits.code==app::LocalAccountResultCode::ok?committed:
            L"Saved — message limit committed. Current limits could not reload. Reopen this page before another change.");
    }
}
void refresh_system_sysop(State& s,bool next){
    if(s.pending_message_apply)return;const auto snapshot=query_message_configuration(s,{});s.message_limits=snapshot;
    std::wstring caption=L"System Sysop: explicit eligible assignment required.";
    if(snapshot.code==app::LocalAccountResultCode::ok && snapshot.catalog.system_sysop){const auto a=query_accounts(s,{*snapshot.catalog.system_sysop,{},{}});if(a.code==app::LocalAccountResultCode::ok && a.accounts.size()==1)caption=L"System Sysop: "+utf8_to_wide(a.accounts.front().display_name.value_or("Removed caller"))+L" (#"+std::to_wstring(*snapshot.catalog.system_sysop)+L")";}
    SetWindowTextW(s.sysop_detail,(caption+L"\r\nDesignation does not grant privileges or transfer old mail. Reassign before locking, removing or demoting this caller.").c_str());
    if(!next)s.sysop_offset=0;else if(s.sysop_more && s.sysop_offset<=UINT32_MAX-32)s.sysop_offset+=32;
    const auto candidates=query_accounts(s,{0,wide_to_utf8(text(s.sysop_filter)).value_or(""),s.sysop_offset});s.sysop_more=candidates.code==app::LocalAccountResultCode::ok && candidates.has_more;s.sysop_candidates.clear();SendMessageW(s.sysop_choice,CB_RESETCONTENT,0,0);
    if(candidates.code==app::LocalAccountResultCode::ok)for(const auto& a:candidates.accounts)if(a.state==app::AccountState::active && a.security_level && *a.security_level>=candidates.policy.sysop_threshold){
        s.sysop_candidates.push_back(a);const auto name=utf8_to_wide(a.display_name.value_or(""))+L" (#"+std::to_wstring(a.account_id)+L")";SendMessageW(s.sysop_choice,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
    }
    if(!s.sysop_candidates.empty())SendMessageW(s.sysop_choice,CB_SETCURSEL,0,0);
    message_controls(s,snapshot.code==app::LocalAccountResultCode::ok);EnableWindow(GetDlgItem(s.pages[0],IDC_SYSTEM_SYSOP_NEXT),s.sysop_more);
}
void assign_system_sysop(State& s){
    const auto index=SendMessageW(s.sysop_choice,CB_GETCURSEL,0,0);if(index<0 || static_cast<std::size_t>(index)>=s.sysop_candidates.size() || s.message_limits.code!=app::LocalAccountResultCode::ok)return;
    const auto& a=s.sysop_candidates[static_cast<std::size_t>(index)];app::MessageConfigurationChange c;c.action=app::MessageConfigurationAction::system_sysop;
    c.expected_configuration_revision=s.message_limits.catalog.revision;c.expected_assignment_revision=s.message_limits.catalog.assignment_revision;c.sysop=a.account_id;c.expected_account_revision=a.revision;
    const auto question=L"Assign System Sysop to "+utf8_to_wide(a.display_name.value_or(""))+L"?\r\nNo privileges change and old mail stays with its original recipient.";
    if(MessageBoxW(s.window,question.c_str(),L"Review System Sysop",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)!=IDYES)return;
    if(apply_message_change(s,c))refresh_system_sysop(s);
}
void populate_message_form(State& s){
    s.message_populating=true;std::string name;std::int64_t order{};std::uint16_t access=1,reading=1,posting=1;bool enabled=true;app::MessageGroupId parent;
    if(s.editing_echo && s.editing_message_echo){const auto& e=*s.editing_message_echo;name=e.name;order=e.order;access=e.permissions.access;reading=e.permissions.reading;posting=e.permissions.posting;enabled=e.enabled;parent=e.group;}
    else if(!s.editing_echo && s.editing_message_group){const auto& g=*s.editing_message_group;name=g.name;order=g.order;access=g.access;enabled=g.enabled;parent=g.id;}
    else if(s.message_group_index>=0 && static_cast<std::size_t>(s.message_group_index)<s.message_catalog.catalog.groups.size())parent=s.message_catalog.catalog.groups[static_cast<std::size_t>(s.message_group_index)].id;
    SetWindowTextW(s.message_name,utf8_to_wide(name).c_str());SetWindowTextW(s.message_order,std::to_wstring(order).c_str());SetWindowTextW(s.message_access,std::to_wstring(access).c_str());SetWindowTextW(s.message_reading,std::to_wstring(reading).c_str());SetWindowTextW(s.message_posting,std::to_wstring(posting).c_str());Button_SetCheck(s.message_enabled,enabled?BST_CHECKED:BST_UNCHECKED);
    Button_SetCheck(s.message_pipe_colors,s.editing_echo && s.editing_message_echo && s.editing_message_echo->renegade_pipe_colors?BST_CHECKED:BST_UNCHECKED);
    SendMessageW(s.message_parent,CB_RESETCONTENT,0,0);for(std::size_t i=0;i<s.message_catalog.catalog.groups.size();++i){const auto& g=s.message_catalog.catalog.groups[i];const auto caption=utf8_to_wide(g.name);SendMessageW(s.message_parent,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption.c_str()));if(g.id==parent)SendMessageW(s.message_parent,CB_SETCURSEL,i,0);}
    const bool available=s.message_catalog.code==app::LocalAccountResultCode::ok && !s.pending_message_apply;
    for(HWND field:{s.message_reading,s.message_posting,s.message_parent})EnableWindow(field,available && s.editing_echo);
    EnableWindow(s.message_pipe_colors,available && s.editing_echo);
    const auto policy=s.editing_message_echo?s.editing_message_echo->retention:app::MessageRetentionPolicy{};
    SetWindowTextW(s.message_max_count,s.editing_echo?(policy.maximum_count?std::to_wstring(*policy.maximum_count):L"Unlimited").c_str():L"");
    SetWindowTextW(s.message_max_age,s.editing_echo?(policy.maximum_age_days?std::to_wstring(*policy.maximum_age_days):L"Unlimited").c_str():L"");
    EnableWindow(s.message_max_count,available && s.editing_echo);EnableWindow(s.message_max_age,available && s.editing_echo);
    SetWindowTextW(GetDlgItem(s.pages[19],IDC_MESSAGE_DELETE),s.editing_echo?L"Delete Echo":L"Delete Group");
    EnableWindow(GetDlgItem(s.pages[19],IDC_MESSAGE_DELETE),available && (s.editing_echo?s.editing_message_echo.has_value():s.editing_message_group.has_value()));
    SetWindowTextW(s.message_detail,s.message_insert_anchor?L"New echo will be inserted before the selected stable echo when saved. Choose a name and review permissions.":s.editing_echo?L"Caller numbers follow readable group order. Internal echo IDs stay permanent. Disable retains messages; Delete permanently removes this echo and its messages.":L"Editing group. Delete Group requires Runtime stopped and separate final confirmation; it removes all contained echoes and messages, including Permanent messages. Disable is reversible.");s.message_populating=false;
}
void load_message_echoes(State& s){
    SendMessageW(s.message_echoes,LB_RESETCONTENT,0,0);s.message_echo_catalog={};s.message_echo_index=-1;
    if(s.message_group_index<0 || static_cast<std::size_t>(s.message_group_index)>=s.message_catalog.catalog.groups.size())return;
    const auto group=s.message_catalog.catalog.groups[static_cast<std::size_t>(s.message_group_index)].id;
    s.message_echo_cursor.configuration_revision=s.message_catalog.catalog.revision;s.message_echo_catalog=query_message_configuration(s,{group,s.message_echo_cursor});
    if(s.message_echo_catalog.code!=app::LocalAccountResultCode::ok){s.editing_message_echo.reset();EnableWindow(GetDlgItem(s.pages[19],IDC_MESSAGE_SAVE),FALSE);EnableWindow(GetDlgItem(s.pages[19],IDC_MESSAGE_ECHO_NEXT),FALSE);set_status(s,L"Unavailable — catalog changed or could not be read. Reload; no empty catalog is claimed.");return;}
    for(const auto& e:s.message_echo_catalog.catalog.echoes){const auto caption=std::to_wstring(e.conference_number)+L"  "+utf8_to_wide(e.name)+(e.enabled?L"":L" (disabled)");SendMessageW(s.message_echoes,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption.c_str()));}
    EnableWindow(GetDlgItem(s.pages[19],IDC_MESSAGE_ECHO_NEXT),s.message_echo_catalog.catalog.next.has_value());
}
void refresh_message_configuration(State& s){
    if(s.pending_message_apply)return;s.message_insert_anchor.reset();s.message_catalog=query_message_configuration(s,{{},s.message_group_cursor});s.message_group_index=-1;s.message_echo_index=-1;
    SendMessageW(s.message_groups,LB_RESETCONTENT,0,0);SendMessageW(s.message_echoes,LB_RESETCONTENT,0,0);SendMessageW(s.clone_group,CB_RESETCONTENT,0,0);
    const bool ok=s.message_catalog.code==app::LocalAccountResultCode::ok;message_controls(s,ok);
    if(!ok){set_status(s,L"Unavailable — authoritative message catalog could not load. Nothing changed.");return;}
    if(s.clone_source && s.clone_echo_cursor.configuration_revision && s.clone_echo_cursor.configuration_revision!=s.message_catalog.catalog.revision){s.clone_source.reset();s.clone_targets.clear();}
    if(!s.clone_source){SetWindowTextW(s.clone_source_text,L"No source; select an echo and Use as Clone Source.");SetWindowTextW(s.clone_preview,L"");}
    for(const auto& g:s.message_catalog.catalog.groups){const auto caption=utf8_to_wide(g.name)+(g.enabled?L"":L" (disabled)");SendMessageW(s.message_groups,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption.c_str()));SendMessageW(s.clone_group,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption.c_str()));}
    EnableWindow(GetDlgItem(s.pages[19],IDC_MESSAGE_GROUP_NEXT),s.message_catalog.catalog.next.has_value());
    if(!s.message_catalog.catalog.groups.empty()){s.message_group_index=0;SendMessageW(s.message_groups,LB_SETCURSEL,0,0);SendMessageW(s.clone_group,CB_SETCURSEL,0,0);s.editing_message_group=s.message_catalog.catalog.groups.front();}else s.editing_message_group.reset();
    s.editing_echo=false;s.editing_message_echo.reset();s.message_echo_cursor={};load_message_echoes(s);populate_message_form(s);
}
bool save_message_catalog(State& s){
    if(s.message_catalog.code!=app::LocalAccountResultCode::ok)return false;const auto order=message_number(s.message_order,INT64_MAX),access=message_number(s.message_access,999);
    const auto name=wide_to_utf8(text(s.message_name));if(!order || !access || !*access || !name){set_status(s,L"Refused — use a name, nonnegative order and security minima 1–999. Draft retained.");return false;}
    app::MessageConfigurationChange c;c.expected_configuration_revision=s.message_catalog.catalog.revision;
    if(s.editing_echo){const auto reading=message_number(s.message_reading,999),posting=message_number(s.message_posting,999);const auto parent=SendMessageW(s.message_parent,CB_GETCURSEL,0,0);
        if(!reading || !*reading || !posting || !*posting || parent<0 || static_cast<std::size_t>(parent)>=s.message_catalog.catalog.groups.size()){set_status(s,L"Refused — choose a loaded parent and minima 1–999.");return false;}
        c.action=app::MessageConfigurationAction::echo;if(s.editing_message_echo)c.echo=*s.editing_message_echo;c.echo.name=*name;c.echo.order=*order;c.echo.permissions={static_cast<std::uint16_t>(*access),static_cast<std::uint16_t>(*reading),static_cast<std::uint16_t>(*posting)};c.echo.group=s.message_catalog.catalog.groups[static_cast<std::size_t>(parent)].id;c.echo.enabled=Button_GetCheck(s.message_enabled)==BST_CHECKED;
        c.presentation_mask=1;c.echo.renegade_pipe_colors=Button_GetCheck(s.message_pipe_colors)==BST_CHECKED;
        const auto count_text=wide_to_utf8(text(s.message_max_count)),age_text=wide_to_utf8(text(s.message_max_age));
        const auto count=count_text?app::parse_message_retention_limit(*count_text,app::kMaximumRetainedMessageCount):app::ParsedMessageRetentionLimit{};
        const auto age=age_text?app::parse_message_retention_limit(*age_text,app::kMaximumMessageAgeDays):app::ParsedMessageRetentionLimit{};
        if(!count.valid || !age.valid){set_status(s,L"Refused — Count: 1–999,999,999 or Unlimited; Age: 1–65,535 full days or Unlimited. Draft retained; nothing purged.");return false;}
        c.retention_mask=3;c.echo.retention={count.value,age.value};
        if(!s.editing_message_echo && s.message_insert_anchor){c.action=app::MessageConfigurationAction::insert_echo;c.insert_before=s.message_insert_anchor->id;c.insert_before_revision=s.message_insert_anchor->revision;}
    }else{c.action=app::MessageConfigurationAction::group;if(s.editing_message_group)c.group=*s.editing_message_group;c.group.name=*name;c.group.order=*order;c.group.access=static_cast<std::uint16_t>(*access);c.group.enabled=Button_GetCheck(s.message_enabled)==BST_CHECKED;}
    auto preview=c;preview.operation.bytes[0]=1;if(!app::valid_message_configuration_change(preview)){set_status(s,L"Refused — names must be 1–64 printable characters, without leading/trailing spaces. Draft retained.");return false;}
    std::wstring effects;
    if(s.editing_echo && s.editing_message_echo){
        const auto& previous=*s.editing_message_echo;
        const auto& next=c.echo;
        if(!(previous.group==next.group))effects+=L"The echo will move to the selected group. Callers will use the new group's access rules.\n";
        if(previous.enabled!=next.enabled || previous.permissions.access!=next.permissions.access ||
            previous.permissions.reading!=next.permissions.reading || previous.permissions.posting!=next.permissions.posting)
            effects+=L"The echo's availability or caller permissions will change immediately.\n";
        if(previous.retention.maximum_count!=next.retention.maximum_count ||
            previous.retention.maximum_age_days!=next.retention.maximum_age_days)
            effects+=L"The new limits apply at a later authorized retention run. Saving retains current messages.\n";
    } else if(!s.editing_echo && s.editing_message_group){
        const auto& previous=*s.editing_message_group;
        if(previous.enabled!=c.group.enabled || previous.access!=c.group.access)
            effects+=L"The group's availability or caller access will change immediately for its echoes.\n";
    }
    if(!effects.empty() && MessageBoxW(s.window,(effects+L"\nSave these changes?").c_str(),
        L"Review Message Catalog",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)!=IDYES)return false;
    if(!apply_message_change(s,c))return false;
    const auto committed=text(s.status);
    s.message_group_cursor={};refresh_message_configuration(s);
    const bool reloaded=s.message_catalog.code==app::LocalAccountResultCode::ok &&
        (s.message_group_index<0 || s.message_echo_catalog.code==app::LocalAccountResultCode::ok);
    set_status(s,reloaded?committed:
        L"Saved — message configuration committed. The catalog could not reload. Choose Reload before another edit.");
    return true;
}
void insert_message_echo(State& s){
    if(!s.editing_echo || !s.editing_message_echo || s.pending_message_apply){set_status(s,L"Select the echo to insert before first.");return;}
    const auto anchor=*s.editing_message_echo;
    if(!resolve_dirty(s))return;
    if(!s.editing_message_echo || !(s.editing_message_echo->id==anchor.id) || s.editing_message_echo->revision!=anchor.revision){set_status(s,L"Selection changed. Select the insertion anchor again.");return;}
    s.editing_message_echo.reset();s.message_insert_anchor=anchor;s.editing_echo=true;populate_message_form(s);
    for(std::size_t i=0;i<s.message_catalog.catalog.groups.size();++i)if(s.message_catalog.catalog.groups[i].id==anchor.group)SendMessageW(s.message_parent,CB_SETCURSEL,i,0);
    SetWindowTextW(s.message_order,std::to_wstring(anchor.order).c_str());remember_page(s);SetFocus(s.message_name);
}
void operate_message_echo(State& s,app::MessageConfigurationAction action){
    if(!s.editing_echo || !s.editing_message_echo || s.pending_message_apply){set_status(s,L"Select an existing echo first.");return;}
    const auto selected=*s.editing_message_echo;
    if(!resolve_dirty(s))return;
    if(!s.editing_message_echo || !(s.editing_message_echo->id==selected.id) || s.editing_message_echo->revision!=selected.revision){set_status(s,L"Selection changed. Reload and explicitly select the echo again.");return;}
    if(action==app::MessageConfigurationAction::delete_echo){
        delete_public_area_ui(s,{app::MessageDeletionKind::echo,selected.id.value,selected.revision});return;
    }
    app::MessageConfigurationChange c;c.action=action;c.echo=selected;c.expected_configuration_revision=s.message_catalog.catalog.revision;
    if(apply_message_change(s,c)){s.message_group_cursor={};refresh_message_configuration(s);remember_page(s);}
}
void delete_message_group(State& s){
    if(s.editing_echo || !s.editing_message_group || s.pending_message_apply){set_status(s,L"Select an existing group first.");return;}
    const auto selected=*s.editing_message_group;
    if(!resolve_dirty(s))return;
    if(s.editing_echo || !s.editing_message_group || !(s.editing_message_group->id==selected.id) || s.editing_message_group->revision!=selected.revision){set_status(s,L"Selection changed. Reload and explicitly select the group again.");return;}
    delete_public_area_ui(s,{app::MessageDeletionKind::group,selected.id.value,selected.revision});
}
void load_clone_targets(State& s,bool next=false){
    const auto index=SendMessageW(s.clone_group,CB_GETCURSEL,0,0);if(index<0 || static_cast<std::size_t>(index)>=s.message_catalog.catalog.groups.size())return;
    if(!next)s.clone_echo_cursor={};s.clone_echo_cursor.configuration_revision=s.message_catalog.catalog.revision;
    const auto part=query_message_configuration(s,{s.message_catalog.catalog.groups[static_cast<std::size_t>(index)].id,s.clone_echo_cursor});s.clone_page.clear();SendMessageW(s.clone_list,LB_RESETCONTENT,0,0);
    if(part.code!=app::LocalAccountResultCode::ok){set_status(s,L"Unavailable — clone catalog changed. Reload and review a fresh preview.");return;}
    s.clone_page=part.catalog.echoes;s.clone_echo_cursor=part.catalog.next.value_or(app::MessageCatalogCursor{s.message_catalog.catalog.revision});EnableWindow(GetDlgItem(s.pages[20],IDC_MESSAGE_CLONE_NEXT),part.catalog.next.has_value());
    for(const auto& e:s.clone_page){const auto caption=std::to_wstring(e.conference_number)+L"  "+utf8_to_wide(e.name);SendMessageW(s.clone_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption.c_str()));}
}
void add_clone_targets(State& s){
    if(!s.clone_source){set_status(s,L"Choose a source on Groups & Local Echoes first.");return;}
    for(std::size_t i=0;i<s.clone_page.size();++i)if(SendMessageW(s.clone_list,LB_GETSEL,i,0)>0){const auto& e=s.clone_page[i];if(!(e.id==s.clone_source->id) && std::none_of(s.clone_targets.begin(),s.clone_targets.end(),[&](const app::LocalEcho& x){return x.id==e.id;}))s.clone_targets.push_back(e);}
    set_status(s,(L"Selected "+std::to_wstring(s.clone_targets.size())+L" explicit targets; browse other groups/pages to add more. Preview before Apply.").c_str());
}
std::optional<app::MessageConfigurationChange> clone_preview(State& s){
    if(!s.clone_source || s.message_catalog.code!=app::LocalAccountResultCode::ok)return {};app::MessageConfigurationChange c;c.action=app::MessageConfigurationAction::clone_permissions;c.expected_configuration_revision=s.message_catalog.catalog.revision;c.clone_source=s.clone_source->id;c.source_revision=s.clone_source->revision;
    c.permission_mask=static_cast<std::uint8_t>((Button_GetCheck(s.clone_access)==BST_CHECKED?1:0)|(Button_GetCheck(s.clone_reading)==BST_CHECKED?2:0)|(Button_GetCheck(s.clone_posting)==BST_CHECKED?4:0));
    c.retention_mask=static_cast<std::uint8_t>((Button_GetCheck(s.clone_max_count)==BST_CHECKED?1:0)|(Button_GetCheck(s.clone_max_age)==BST_CHECKED?2:0));
    const auto limit=[](const std::optional<std::uint32_t> value){return value?std::to_wstring(*value):L"Unlimited";};
    std::wstring preview=L"Source: "+utf8_to_wide(s.clone_source->name)+L"\r\nOnly checked fields copied. No content, Permanent flags, private policy, numbers or scan state changes. Limits apply at the next retention run; no purge on Apply.\r\n";
    for(const auto& e:s.clone_targets){c.clone_targets.push_back({e.id,e.revision});preview+=L"\r\n#"+std::to_wstring(e.conference_number)+L" "+utf8_to_wide(e.name);
        if(c.permission_mask&1)preview+=L"  Access "+std::to_wstring(e.permissions.access)+L" -> "+std::to_wstring(s.clone_source->permissions.access);
        if(c.permission_mask&2)preview+=L"  Read "+std::to_wstring(e.permissions.reading)+L" -> "+std::to_wstring(s.clone_source->permissions.reading);
        if(c.permission_mask&4)preview+=L"  Post "+std::to_wstring(e.permissions.posting)+L" -> "+std::to_wstring(s.clone_source->permissions.posting);
        if(c.retention_mask&1)preview+=L"  Count "+limit(e.retention.maximum_count)+L" -> "+limit(s.clone_source->retention.maximum_count);
        if(c.retention_mask&2)preview+=L"  Age "+limit(e.retention.maximum_age_days)+L" -> "+limit(s.clone_source->retention.maximum_age_days);
    }
    if((!c.permission_mask && !c.retention_mask) || c.clone_targets.empty())preview+=L"\r\nNo selected fields/targets: no change.";SetWindowTextW(s.clone_preview,preview.c_str());return c;
}
void apply_clone(State& s){
    const auto c=clone_preview(s);if(!c){set_status(s,L"Choose a source and explicit targets.");return;}
    if((!c->permission_mask && !c->retention_mask) || c->clone_targets.empty()){set_status(s,L"Completed — no selected fields/targets; no change.");return;}
    if(MessageBoxW(s.window,L"Apply displayed selected fields to every target atomically? Any stale/invalid entry refuses the entire batch. Retention applies at the next run; Apply itself deletes nothing.",L"Review Clone Options",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)!=IDYES)return;
    if(apply_message_change(s,*c)){s.clone_source.reset();s.clone_targets.clear();s.message_group_cursor={};refresh_message_configuration(s);}
}
void reconcile_message_apply(State& s){
    if(!s.pending_message_apply){set_status(s,L"No unresolved message operation.");return;}
    const auto c=*s.pending_message_apply;if(apply_message_change(s,c,true)){
        // Refresh only the committed operation's fields. Other security/BBS
        // edits remain dirty; reconciliation must never mark them saved.
        if(c.action==app::MessageConfigurationAction::line_limit){
            refresh_message_limit(s);
            if(s.current_page==8)for(auto& value:s.baseline)if(value.control==s.level_message_lines)value.value=text(s.level_message_lines);
        }else if(c.action==app::MessageConfigurationAction::system_sysop){refresh_system_sysop(s);}
        else if(c.action==app::MessageConfigurationAction::private_presentation){
            Button_SetCheck(s.private_pipe_colors,c.private_pipe_colors?BST_CHECKED:BST_UNCHECKED);
        }
        else if(c.action==app::MessageConfigurationAction::unread_mail_retention){
            s.private_mail_policy=query_message_configuration(s,{});
            if(s.private_mail_policy.code==app::LocalAccountResultCode::ok){
                const auto age=s.private_mail_policy.catalog.unread_mail_age_days;
                SetWindowTextW(s.unread_mail_age,age?std::to_wstring(*age).c_str():L"Unlimited");
                for(auto& value:s.baseline)if(value.control==s.unread_mail_age)value.value=text(s.unread_mail_age);
            }
        }
        else {s.message_group_cursor={};refresh_message_configuration(s);remember_page(s);}
    }
}
void toggle_private_pipe_colors(State& s){
    if(s.pending_message_apply)return;
    const bool requested=Button_GetCheck(s.private_pipe_colors)!=BST_CHECKED;
    const auto current=query_message_configuration(s,{});
    if(current.code!=app::LocalAccountResultCode::ok){
        s.private_pipe_available=false;
        update_private_pipe_controls(s);
        set_status(s,L"Unavailable — current private presentation policy could not load.");return;
    }
    Button_SetCheck(s.private_pipe_colors,current.catalog.private_pipe_colors?BST_CHECKED:BST_UNCHECKED);
    if(requested==current.catalog.private_pipe_colors){
        set_status(s,requested?L"Pipe colors are already enabled for private mail and NetMail.":
            L"Pipe codes already display as text in private mail and NetMail.");return;
    }
    app::MessageConfigurationChange c;c.action=app::MessageConfigurationAction::private_presentation;c.expected_configuration_revision=current.catalog.revision;
    c.expected_private_presentation_revision=current.catalog.private_presentation_revision;c.private_pipe_colors=requested;
    if(apply_message_change(s,c)){
        Button_SetCheck(s.private_pipe_colors,c.private_pipe_colors?BST_CHECKED:BST_UNCHECKED);
        set_status(s,c.private_pipe_colors?L"Saved — pipe colors enabled for private mail and NetMail.":
            L"Saved — pipe codes display as text in private mail and NetMail.");
    }
}
void create_message_configuration_pages(State& s){
    const auto button=[&](HWND parent,const wchar_t* caption,int x,int y,int width,int id){return control(s,parent,L"BUTTON",caption,WS_TABSTOP|BS_PUSHBUTTON,x,y,width,28,id);};
    const auto edit=[&](HWND parent,const wchar_t* caption,int x,int y,int width,int id,bool numeric){label(s,parent,caption,x,y,width,20);auto f=control(s,parent,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|(numeric?ES_NUMBER:0),x,y+20,width,24,id,WS_EX_CLIENTEDGE);SendMessageW(f,EM_SETLIMITTEXT,numeric?19:64,0);return f;};
    auto p=s.pages[19]=page(s);label(s,p,L"Groups",12,8,180,22);label(s,p,L"Local echoes (permanent internal codes)",212,8,294,22);
    s.message_groups=control(s,p,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|WS_VSCROLL,12,32,185,145,IDC_MESSAGE_GROUPS,WS_EX_CLIENTEDGE);
    s.message_echoes=control(s,p,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|WS_VSCROLL,212,32,294,145,IDC_MESSAGE_ECHOES,WS_EX_CLIENTEDGE);
    button(p,L"Next Groups",12,184,120,IDC_MESSAGE_GROUP_NEXT);button(p,L"Next Echoes",212,184,120,IDC_MESSAGE_ECHO_NEXT);button(p,L"Reload",370,184,136,IDC_MESSAGE_RELOAD);
    button(p,L"New Group",12,220,150,IDC_MESSAGE_NEW_GROUP);button(p,L"New Local Echo",174,220,160,IDC_MESSAGE_NEW_ECHO);
    s.message_name=edit(p,L"Name",12,260,320,IDC_MESSAGE_NAME,false);s.message_order=edit(p,L"Display order",350,260,156,IDC_MESSAGE_ORDER,true);
    s.message_access=edit(p,L"Access minimum",12,312,150,IDC_MESSAGE_ACCESS,true);s.message_reading=edit(p,L"Reading minimum",184,312,150,IDC_MESSAGE_READING,true);s.message_posting=edit(p,L"Posting minimum",356,312,150,IDC_MESSAGE_POSTING,true);
    label(s,p,L"Parent group (for echo moves)",12,364,310,20);s.message_parent=control(s,p,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,12,384,320,240,IDC_MESSAGE_PARENT);
    s.message_enabled=control(s,p,L"BUTTON",L"Enabled",WS_TABSTOP|BS_AUTOCHECKBOX,350,384,150,25,IDC_MESSAGE_ENABLED);
    s.message_pipe_colors=control(s,p,L"BUTTON",L"Renegade pipe colors (echo only)",WS_TABSTOP|BS_AUTOCHECKBOX,12,410,494,18,IDC_MESSAGE_PIPE_COLORS);
    button(p,L"Save",12,428,100,IDC_MESSAGE_SAVE);button(p,L"Reconcile",124,428,120,IDC_MESSAGE_RECONCILE);button(p,L"Use as Clone Source",260,428,246,IDC_MESSAGE_CLONE_SOURCE);
    button(p,L"Insert Before Selected",12,468,176,IDC_MESSAGE_INSERT);button(p,L"Move Up",198,468,96,IDC_MESSAGE_MOVE_UP);button(p,L"Move Down",304,468,100,IDC_MESSAGE_MOVE_DOWN);button(p,L"Delete",414,468,92,IDC_MESSAGE_DELETE);
    s.message_max_count=edit(p,L"Maximum message count (or Unlimited)",12,506,238,IDC_MESSAGE_MAX_COUNT,false);
    s.message_max_age=edit(p,L"Maximum age, full days (or Unlimited)",266,506,240,IDC_MESSAGE_MAX_AGE,false);
    SendMessageW(s.message_max_count,EM_SETLIMITTEXT,10,0);SendMessageW(s.message_max_age,EM_SETLIMITTEXT,10,0);
    s.message_detail=control(s,p,L"EDIT",L"",ES_READONLY|ES_MULTILINE|WS_VSCROLL,12,558,494,55,0,WS_EX_CLIENTEDGE);
    p=s.pages[20]=page(s);label(s,p,L"Selected-fields atomic Clone Options",12,8,494,24);
    s.clone_source_text=label(s,p,L"No source; select an echo and Use as Clone Source.",12,40,494,45);
    label(s,p,L"Target group",12,92,320,20);s.clone_group=control(s,p,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,12,114,320,240,IDC_MESSAGE_CLONE_GROUP);button(p,L"Next Echoes",350,112,156,IDC_MESSAGE_CLONE_NEXT);
    s.clone_list=control(s,p,L"LISTBOX",L"",WS_TABSTOP|LBS_EXTENDEDSEL|WS_VSCROLL,12,150,494,145,IDC_MESSAGE_CLONE_TARGETS,WS_EX_CLIENTEDGE);
    button(p,L"Add Selected Targets",12,306,230,IDC_MESSAGE_CLONE_ADD);button(p,L"Clear Targets",260,306,246,IDC_MESSAGE_CLONE_CLEAR);
    s.clone_access=control(s,p,L"BUTTON",L"Access",WS_TABSTOP|BS_AUTOCHECKBOX,12,348,150,25,IDC_MESSAGE_CLONE_ACCESS);
    s.clone_reading=control(s,p,L"BUTTON",L"Reading",WS_TABSTOP|BS_AUTOCHECKBOX,184,348,150,25,IDC_MESSAGE_CLONE_READING);
    s.clone_posting=control(s,p,L"BUTTON",L"Posting",WS_TABSTOP|BS_AUTOCHECKBOX,356,348,150,25,IDC_MESSAGE_CLONE_POSTING);
    s.clone_max_count=control(s,p,L"BUTTON",L"Maximum Message Count",WS_TABSTOP|BS_AUTOCHECKBOX,12,380,238,25,IDC_CLONE_MAX_COUNT);
    s.clone_max_age=control(s,p,L"BUTTON",L"Maximum Message Age",WS_TABSTOP|BS_AUTOCHECKBOX,266,380,240,25,IDC_CLONE_MAX_AGE);
    button(p,L"Preview",12,420,140,IDC_MESSAGE_CLONE_PREVIEW);button(p,L"Apply",174,420,140,IDC_MESSAGE_CLONE_APPLY);button(p,L"Reconcile",336,420,170,IDC_MESSAGE_RECONCILE);
    s.clone_preview=control(s,p,L"EDIT",L"",ES_READONLY|ES_MULTILINE|WS_VSCROLL|ES_AUTOVSCROLL,12,462,494,152,0,WS_EX_CLIENTEDGE);
    p=s.pages[0];label(s,p,L"System Sysop designation (does not grant privileges)",12,274,494,24);
    s.sysop_detail=control(s,p,L"EDIT",L"",ES_READONLY|ES_MULTILINE|WS_VSCROLL,12,306,494,80,0,WS_EX_CLIENTEDGE);
    s.sysop_filter=edit(p,L"Find eligible account (current-name prefix)",12,400,320,IDC_SYSTEM_SYSOP_FILTER,false);button(p,L"Find",350,420,72,IDC_SYSTEM_SYSOP_FIND);button(p,L"Next",432,420,74,IDC_SYSTEM_SYSOP_NEXT);
    s.sysop_choice=control(s,p,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,12,458,494,240,IDC_SYSTEM_SYSOP_CHOICE);
    button(p,L"Assign",12,500,150,IDC_SYSTEM_SYSOP_ASSIGN);button(p,L"Reload",174,500,150,IDC_SYSTEM_SYSOP_RELOAD);button(p,L"Reconcile",336,500,170,IDC_MESSAGE_RECONCILE);
    p=s.pages[8];label(s,p,L"Lines Per Message (1–65,535; no unlimited)",12,628,494,24);
    s.level_message_lines=control(s,p,L"EDIT",L"",WS_TABSTOP|ES_NUMBER|ES_AUTOHSCROLL,12,658,140,25,IDC_LEVEL_MESSAGE_LINES,WS_EX_CLIENTEDGE);SendMessageW(s.level_message_lines,EM_SETLIMITTEXT,5,0);
    s.level_message_save=button(p,L"Save Message Limit",174,656,190,IDC_LEVEL_MESSAGE_SAVE);button(p,L"Reconcile",376,656,130,IDC_MESSAGE_RECONCILE);
    label(s,p,L"New active levels start at 200. Stored messages are never truncated. Open drafts check the current limit when saved.",12,700,494,45);
}
