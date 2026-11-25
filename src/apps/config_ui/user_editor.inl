// Familiar account editing; all reads/writes use the existing protected
// Runtime relay or exclusive offline application boundary, never GUI SQL.
bool resolve_dirty(State& state);

// A modal confirmation/refusal may restore focus and send selection messages.
// Never open a second dirty-navigation dialog inside an account operation.
struct AccountActionGuard {
    State& state;
    explicit AccountActionGuard(State& value):state(value){state.account_action_active=true;}
    ~AccountActionGuard(){state.account_action_active=false;}
};
void report_account_result(State& state,const app::LocalAccountResult& result) {
    const auto message=account_result_text(result);
    set_status(state,message);
    if(result.code!=app::LocalAccountResultCode::ok) SetWindowTextW(state.account_detail,message);
}

void user_tab(State& state) {
    const int chosen=TabCtrl_GetCurSel(state.user_tabs);
    for(int tab=0;tab<2;++tab) for(HWND item:state.user_tab_controls[tab])
        ShowWindow(item,tab==chosen ? SW_SHOW : SW_HIDE);
}
void user_save_enabled(State& state) {
    const bool selected=state.creating_user || (state.account_selection.code==app::LocalAccountResultCode::ok &&
        state.account_selection.accounts.size()==1 && state.account_selection.accounts[0].state!=app::AccountState::tombstoned);
    EnableWindow(state.account_apply,selected && page_dirty(state));
}
void populate_user_editor(State& state) {
    state.populating_user=true;
    const auto* account=state.account_selection.code==app::LocalAccountResultCode::ok &&
        state.account_selection.accounts.size()==1 ? &state.account_selection.accounts[0] : nullptr;
    const bool editable=state.creating_user || (account && account->state!=app::AccountState::tombstoned);
    const auto profile=account ? account->profile : app::AccountProfile{};
    const std::array<std::string,7> fields{{account ? account->display_name.value_or("") : "",
        profile.real_name.value_or(""),profile.location.value_or(""),profile.phone.value_or(""),
        profile.email.value_or(""),profile.birth_date.value_or(""),profile.sex ? std::string(1,*profile.sex) : ""}};
    for(std::size_t i=0;i<fields.size();++i) {
        SetWindowTextW(state.user_fields[i],utf8_to_wide(fields[i]).c_str()); EnableWindow(state.user_fields[i],editable);
    }
    SetWindowTextW(state.user_level,std::to_wstring(account ? account->security_level.value_or(0) :
        state.account_page.policy.new_user_level).c_str());
    EnableWindow(state.user_level,editable && !state.creating_user);
    Button_SetCheck(state.user_purge,account && account->purge_allowed.value_or(false) ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(state.user_purge,editable && !state.creating_user);
    for(HWND item:{state.account_value,state.account_confirm}) {SetWindowTextW(item,L"");EnableWindow(item,editable);}
    EnableWindow(state.user_reset,editable && !state.creating_user);
    EnableWindow(state.user_delete,account && editable); EnableWindow(state.user_toggle,account && editable);
    SetWindowTextW(state.user_toggle,account && account->state!=app::AccountState::active ? L"Reactivate User" : L"Deactivate User");
    std::wstring summary=state.creating_user ? L"New User — number assigned on Save" : L"Select a user.";
    if(account) summary=L"User "+std::to_wstring(account->account_id)+L" — "+utf8_to_wide(account->display_name.value_or("(deleted)"))+
        L"\r\n"+(account->state==app::AccountState::active ? L"Active" : account->state==app::AccountState::tombstoned ? L"Deleted" : L"Inactive")+
        std::wstring(L" | ")+(state.account_selection.online ? L"Online" : L"Offline");
    SetWindowTextW(state.account_summary,summary.c_str());
    if(state.current_page==6) {
        auto title=std::wstring(kWindowTitle);
        if(account) title+=L" — User #"+std::to_wstring(account->account_id)+L" — "+utf8_to_wide(account->display_name.value_or("(deleted)"));
        else if(state.creating_user) title+=L" — New User";
        SetWindowTextW(state.window,title.c_str());
    }
    const int selected=ListView_GetNextItem(state.account_list,-1,LVNI_SELECTED);
    EnableWindow(state.account_previous,selected>0 || state.account_query.offset>0);
    EnableWindow(state.account_next,selected+1<static_cast<int>(state.account_page.accounts.size()) || state.account_page.has_more);
    SetWindowTextW(state.account_detail,state.creating_user ?
        L"Enter a name and initial password twice under Security, then Save. The configured New User level is assigned." :
        L"Edit this user's record, then Save. Shared levels and defaults are maintained separately under Security Levels.");
    user_tab(state); state.populating_user=false; remember_page(state); user_save_enabled(state);
}
void reload_selected_user(State& state, app::AccountId id) {
    state.creating_user=false;
    state.account_selection=query_accounts(state,{id,"",0});
    populate_user_editor(state);
    const int selected=ListView_GetNextItem(state.account_list,-1,LVNI_SELECTED);
    if(selected>=0 && static_cast<std::size_t>(selected)<state.account_page.accounts.size() && state.account_selection.accounts.size()==1) {
        state.account_page.accounts[selected]=state.account_selection.accounts[0];
        auto caption=utf8_to_wide(state.account_selection.accounts[0].display_name.value_or("(deleted)"));
        ListView_SetItemText(state.account_list,selected,0,caption.data());
    }
}
void save_user_editor(State& state) {
    if(state.account_action_active) return;
    AccountActionGuard guard(state);
    const bool creating=state.creating_user;
    if(!creating && state.account_selection.accounts.size()!=1) return;
    app::LocalAccountRequest command;
    struct Erase {app::LocalAccountRequest& value;~Erase(){app::erase_local_account_secrets(value);}} erase{command};
    command.action=creating ? app::LocalAccountAction::create_user : app::LocalAccountAction::save_user;
    command.name=wide_to_utf8(text(state.user_fields[0])).value_or("");
    const auto optional=[&](int i)->std::optional<std::string>{auto v=wide_to_utf8(text(state.user_fields[i]));return v && !v->empty() ? v : std::nullopt;};
    command.profile.real_name=optional(1); command.profile.location=optional(2); command.profile.phone=optional(3);
    command.profile.email=optional(4); command.profile.birth_date=optional(5);
    if(const auto sex=optional(6)) {if(sex->size()!=1){set_status(state,L"Refused — Sex must be blank, M, F or X.");return;}command.profile.sex=(*sex)[0];}
    command.expected_revision=1;
    if(creating) {
        if(!consume_password(state.account_value,command.password) || !consume_password(state.account_confirm,command.confirmation) ||
            !password_fields_valid(state,command,command.name)) {
            set_status(state,L"Refused — enter and confirm the same policy-compliant initial password under Security.");return;
        }
    } else {
        const auto& account=state.account_selection.accounts[0];
        if(GetWindowTextLengthW(state.account_value)||GetWindowTextLengthW(state.account_confirm)) {
            set_status(state,L"Refused — use Reset Password for a password change; the user-record draft is retained.");return;
        }
        command.account_id=account.account_id; command.expected_revision=account.revision;
        command.expected_policy_revision=state.account_selection.policy.revision;
        command.security_level=port_value(state.user_level).value_or(0);
        command.purge_allowed=Button_GetCheck(state.user_purge)==BST_CHECKED;
        if(command.name!=account.display_name.value_or("") || command.security_level!=account.security_level) {
            if(MessageBoxW(state.window,L"Save this user's name/access change now? Reduced access applies to online callers. Renaming or newly granting Sysop access requires that caller to log in again.",
                L"Save User",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)!=IDYES) return;
        }
    }
    const auto name=command.name; const auto id=command.account_id;
    const auto result=administer_account(state,command);
    if(result.committed) {
        if(creating) {SetWindowTextW(state.account_filter,utf8_to_wide(name).c_str());refresh_accounts(state,true);}
        else reload_selected_user(state,id);
    }
    report_account_result(state,result); user_save_enabled(state);
}
void user_consequential_action(State& state, app::LocalAccountAction action) {
    if(!resolve_dirty(state) || state.account_selection.accounts.size()!=1) return;
    AccountActionGuard guard(state);
    const auto account=state.account_selection.accounts[0];
    const wchar_t* consequence=action==app::LocalAccountAction::tombstone ?
        L"Delete this user? Login will be blocked and any online sessions disconnected. A deleted-account record and retained session/audit history remain; this does not purge history." :
        action==app::LocalAccountAction::lock ? L"Deactivate this user? Any online sessions will disconnect immediately and login will be blocked until reactivated." :
        L"Reactivate this user? Normal password, access and network checks still apply.";
    if(MessageBoxW(state.window,(L"User "+std::to_wstring(account.account_id)+L" — "+utf8_to_wide(account.display_name.value_or("(deleted)"))+L"\n\n"+consequence).c_str(),
        L"Confirm User Action",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)!=IDYES) return;
    app::LocalAccountRequest command; command.action=action;command.account_id=account.account_id;command.expected_revision=account.revision;
    const auto result=administer_account(state,command);
    if(result.committed) reload_selected_user(state,account.account_id);
    report_account_result(state,result);
}
void reset_user_password(State& state) {
    if(state.creating_user || state.account_selection.accounts.size()!=1) return;
    // Password draft is separate from the ordinary record; never quietly save
    // or discard a profile/access draft to perform a credential operation.
    for(const auto& before:state.baseline) if(before.kind!=3 && before.control!=state.account_filter) {
        const auto values=page_values(state);
        for(const auto& after:values) if(after.control==before.control && after.value!=before.value) {
            set_status(state,L"Save or discard user-record edits before resetting the password. Password and record drafts remain.");return;
        }
    }
    app::LocalAccountRequest command; const auto& account=state.account_selection.accounts[0];
    struct Erase {app::LocalAccountRequest& value;~Erase(){app::erase_local_account_secrets(value);}} erase{command};
    command.action=app::LocalAccountAction::issue_temporary_password;command.account_id=account.account_id;command.expected_revision=account.revision;
    const bool first=consume_password(state.account_value,command.password), second=consume_password(state.account_confirm,command.confirmation);
    if(!first||!second||!password_fields_valid(state,command,account.display_name.value_or(""))){
        set_status(state,L"Refused — enter and confirm the same policy-compliant temporary password.");return;
    }
    AccountActionGuard guard(state);
    if(MessageBoxW(state.window,L"Issue a temporary password and disconnect this user's sessions? It expires after 72 hours or first use. The caller must replace it at login.",
        L"Reset Password",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)!=IDYES) return;
    const auto id=account.account_id;const auto result=administer_account(state,command);
    if(result.committed) reload_selected_user(state,id);
    report_account_result(state,result);user_save_enabled(state);
}
void navigate_user(State& state, bool next) {
    if(!resolve_dirty(state)) return;
    int selected=ListView_GetNextItem(state.account_list,-1,LVNI_SELECTED);
    const int count=static_cast<int>(state.account_page.accounts.size());
    int destination=selected+(next?1:-1);
    if(destination>=0 && destination<count) {
        state.populating_accounts=true;
        ListView_SetItemState(state.account_list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_SetItemState(state.account_list,destination,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        state.populating_accounts=false;select_account(state);ListView_EnsureVisible(state.account_list,destination,FALSE);
    } else if(next && state.account_page.has_more && state.account_query.offset<=1000000-app::kLocalAccountPageSize) {
        state.account_query.offset+=static_cast<std::uint32_t>(app::kLocalAccountPageSize);refresh_accounts(state,false);
    } else if(!next && state.account_query.offset) {
        state.account_query.offset-=static_cast<std::uint32_t>(app::kLocalAccountPageSize);refresh_accounts(state,false);
        state.populating_accounts=true; ListView_SetItemState(state.account_list,0,0,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_SetItemState(state.account_list,static_cast<int>(state.account_page.accounts.size())-1,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        state.populating_accounts=false;select_account(state);
    }
}
void create_accounts_page(State& state) {
    const auto p=state.pages[6]=page(state);
    state.user_new=control(state,p,L"BUTTON",L"New User",WS_TABSTOP,12,8,80,28,IDC_USER_NEW);
    state.account_apply=control(state,p,L"BUTTON",L"Save",WS_TABSTOP,98,8,58,28,IDC_ACCOUNT_APPLY);
    state.user_reload=control(state,p,L"BUTTON",L"Reload",WS_TABSTOP,162,8,68,28,IDC_USER_RELOAD);
    state.user_delete=control(state,p,L"BUTTON",L"Delete User",WS_TABSTOP,236,8,92,28,IDC_USER_DELETE);
    state.user_toggle=control(state,p,L"BUTTON",L"Deactivate User",WS_TABSTOP,334,8,172,28,IDC_USER_TOGGLE);
    state.account_filter=control(state,p,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,12,48,180,25,IDC_ACCOUNT_FILTER,WS_EX_CLIENTEDGE);
    SendMessageW(state.account_filter,EM_SETLIMITTEXT,30,0);
    SendMessageW(state.account_filter,EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(L"Name prefix or user number"));
    control(state,p,L"BUTTON",L"Find",WS_TABSTOP,202,46,70,28,IDC_ACCOUNT_FIND);
    state.account_previous=control(state,p,L"BUTTON",L"Previous",WS_TABSTOP,280,46,110,28,IDC_ACCOUNT_PREVIOUS);
    state.account_next=control(state,p,L"BUTTON",L"Next",WS_TABSTOP,398,46,108,28,IDC_ACCOUNT_NEXT);
    state.account_summary=label(state,p,L"Select a user.",12,84,494,42);
    state.account_list=control(state,p,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,12,138,175,294,IDC_ACCOUNT_LIST,WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(state.account_list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
    LVCOLUMNW column{LVCF_TEXT|LVCF_WIDTH};column.cx=MulDiv(155,state.dpi,96);column.pszText=const_cast<wchar_t*>(L"Users");ListView_InsertColumn(state.account_list,0,&column);
    state.user_tabs=control(state,p,WC_TABCONTROLW,L"",WS_TABSTOP,202,134,304,28,IDC_USER_TABS);
    for(const auto* name:{L"Personal",L"Security"}) {TCITEMW tab{};tab.mask=TCIF_TEXT;tab.pszText=const_cast<wchar_t*>(name);TabCtrl_InsertItem(state.user_tabs,TabCtrl_GetItemCount(state.user_tabs),&tab);}
    const wchar_t* names[]={L"Name",L"Real name",L"Location",L"Phone",L"Email",L"Birth date",L"Sex (M/F/X)"};
    const int limits[]={30,64,64,32,254,10,1};
    for(int i=0;i<7;++i) {
        state.user_tab_controls[0].push_back(label(state,p,names[i],202,178+i*34,106,22));
        state.user_fields[i]=control(state,p,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,310,176+i*34,196,24,IDC_USER_NAME+i,WS_EX_CLIENTEDGE);
        SendMessageW(state.user_fields[i],EM_SETLIMITTEXT,limits[i],0);state.user_tab_controls[0].push_back(state.user_fields[i]);
    }
    auto& sec=state.user_tab_controls[1];
    SendMessageW(state.user_fields[5],EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(L"YYYY-MM-DD"));
    sec.push_back(label(state,p,L"Assigned level (1–999)",202,178,206,22));
    state.user_level=control(state,p,L"EDIT",L"",WS_TABSTOP|ES_NUMBER,420,176,86,24,IDC_USER_LEVEL,WS_EX_CLIENTEDGE);sec.push_back(state.user_level);SendMessageW(state.user_level,EM_SETLIMITTEXT,3,0);
    state.user_purge=control(state,p,L"BUTTON",L"Allow automatic purge",WS_TABSTOP|BS_AUTOCHECKBOX,202,212,304,24,IDC_USER_PURGE);sec.push_back(state.user_purge);
    sec.push_back(label(state,p,L"New user: initial password. Existing user: temporary reset password.",202,244,304,38));
    sec.push_back(label(state,p,L"Password",202,292,106,22));
    state.account_value=control(state,p,L"EDIT",L"",WS_TABSTOP|ES_PASSWORD|ES_AUTOHSCROLL,310,290,196,24,IDC_ACCOUNT_VALUE,WS_EX_CLIENTEDGE);sec.push_back(state.account_value);
    sec.push_back(label(state,p,L"Confirm",202,326,106,22));
    state.account_confirm=control(state,p,L"EDIT",L"",WS_TABSTOP|ES_PASSWORD|ES_AUTOHSCROLL,310,324,196,24,IDC_ACCOUNT_CONFIRM,WS_EX_CLIENTEDGE);sec.push_back(state.account_confirm);
    for(HWND edit:{state.account_value,state.account_confirm}) SendMessageW(edit,EM_SETLIMITTEXT,app::kPasswordMaximum,0);
    state.user_reset=control(state,p,L"BUTTON",L"Reset Password...",WS_TABSTOP,202,362,196,28,IDC_USER_RESET);sec.push_back(state.user_reset);
    state.account_detail=control(state,p,L"EDIT",L"",ES_READONLY|ES_MULTILINE|WS_VSCROLL,12,444,494,62,0,WS_EX_CLIENTEDGE);
    populate_user_editor(state);
}
