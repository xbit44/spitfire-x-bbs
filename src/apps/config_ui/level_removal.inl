// Removal-only review: routine user editing never exposes replacement policy.
enum : int { kLevelRemovalReplacement = 31 };

struct LevelRemovalDialog {
    const app::LocalSecuritySnapshot& catalog;
    const app::LocalSecurityChange& requested;
    HWND replacement{}, effects{};
    std::optional<app::LocalSecurityChange> reviewed;
    bool done{}, accepted{};
};

void review_replacement_level(HWND window, LevelRemovalDialog& dialog) {
    dialog.reviewed.reset();
    EnableWindow(GetDlgItem(window, IDOK), FALSE);
    const auto selection = SendMessageW(dialog.replacement, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) {
        SetWindowTextW(dialog.effects, L"Choose a replacement to review the resulting policy.");
        return;
    }
    const auto value = SendMessageW(dialog.replacement, CB_GETITEMDATA, selection, 0);
    if (value < 1 || value > 999) return;
    auto change = dialog.requested;
    change.replacement = static_cast<std::uint16_t>(value);
    if (change.new_user_level == change.level) change.new_user_level = change.replacement;
    if (change.sysop_threshold == change.level) change.sysop_threshold = change.replacement;
    if (!app::prepare_local_security_change(dialog.catalog, change)) {
        SetWindowTextW(dialog.effects, L"These defaults cannot be saved. Cancel and choose active New User and Sysop levels.");
        return;
    }
    const auto policy = L"New User level: " + std::to_wstring(change.new_user_level) +
        L"    Sysop threshold: " + std::to_wstring(change.sysop_threshold) +
        L"\r\nCallers at or above that threshold have Sysop authority. Reduced access applies to online callers. "
        L"New Sysop access requires a fresh login and SY: authentication.";
    if (SetWindowTextW(dialog.effects, policy.c_str())) {
        dialog.reviewed = std::move(change);
        EnableWindow(GetDlgItem(window, IDOK), TRUE);
    }
}

LRESULT CALLBACK level_removal_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* dialog=reinterpret_cast<LevelRemovalDialog*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        dialog=static_cast<LevelRemovalDialog*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(dialog));
    }
    if(dialog && message==WM_COMMAND) {
        const auto command = LOWORD(wparam);
        if (command == kLevelRemovalReplacement &&
            (HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == CBN_SELENDCANCEL)) {
            review_replacement_level(window, *dialog);
            return 0;
        }
        if (command == IDOK || command == IDCANCEL) {
            if (command == IDOK) {
                const auto selection = SendMessageW(dialog->replacement, CB_GETCURSEL, 0, 0);
                if (!dialog->reviewed || selection == CB_ERR ||
                    SendMessageW(dialog->replacement, CB_GETITEMDATA, selection, 0) != dialog->reviewed->replacement) {
                    review_replacement_level(window, *dialog);
                    return 0;
                }
            }
            dialog->accepted = command == IDOK;
            dialog->done = true;
            return 0;
        }
    }
    if(dialog && message==WM_CLOSE) {dialog->done=true;return 0;}
    return DefWindowProcW(window,message,wparam,lparam);
}
bool review_level_removal(State& state,app::LocalSecurityChange& change) {
    const auto target = std::find_if(state.security_catalog.levels.begin(), state.security_catalog.levels.end(),
        [&](const auto& level) { return level.level == change.level && level.state == app::SecurityLevelState::active; });
    if (target == state.security_catalog.levels.end()) {
        set_status(state, L"Refused — select an active security level to remove.");
        return false;
    }
    std::vector<app::AccountRecord> affected;
    bool complete=false;
    for(std::uint32_t offset=0;offset<=1000000;offset+=static_cast<std::uint32_t>(app::kLocalAccountPageSize)) {
        const auto part=query_accounts(state,{0,"",offset});
        if(part.code!=app::LocalAccountResultCode::ok || part.policy.revision!=change.expected_revision) break;
        for(const auto& account:part.accounts) if(account.security_level==change.level && account.state!=app::AccountState::tombstoned) affected.push_back(account);
        if(!part.has_more) {complete=true;break;}
    }
    if(!complete) {set_status(state,L"Refused — the affected users could not be read consistently. Reload Security Levels.");return false;}
    std::vector<std::pair<app::AccountId,std::int64_t>> revisions;
    for(const auto& account:affected) revisions.emplace_back(account.account_id,account.revision);
    const auto definition=app::security_reassignment_definition(std::move(revisions));
    change.affected_accounts_hash=win32::installation::sha256(std::vector<std::uint8_t>(definition.begin(),definition.end()));
    std::transform(change.affected_accounts_hash.begin(),change.affected_accounts_hash.end(),change.affected_accounts_hash.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    WNDCLASSW cls{};cls.lpfnWndProc=level_removal_proc;cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpszClassName=L"SFXConfigLevelRemoval";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);
    RegisterClassW(&cls);
    LevelRemovalDialog dialog{state.security_catalog, change, nullptr, nullptr, {}, false, false};
    const auto scale=[&](int n){return MulDiv(n,state.dpi,96);};
    RECT owner{};GetWindowRect(state.window,&owner);
    const HWND window=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,cls.lpszClassName,L"Remove Security Level",
        WS_CAPTION|WS_SYSMENU|WS_POPUP,owner.left+scale(24),owner.top+scale(24),scale(540),scale(478),state.window,nullptr,cls.hInstance,&dialog);
    if(!window) return false;
    const auto child=[&](const wchar_t* kind,const wchar_t* caption,DWORD style,int x,int y,int w,int h,int id)->HWND {
        const auto result=CreateWindowExW(0,kind,caption,WS_CHILD|WS_VISIBLE|style,scale(x),scale(y),scale(w),scale(h),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),cls.hInstance,nullptr);
        SendMessageW(result,WM_SETFONT,reinterpret_cast<WPARAM>(state.ui_font),TRUE);return result;
    };
    const auto caption=L"Remove level "+std::to_wstring(change.level)+L" ("+utf8_to_wide(target->label)+
        L")? This number cannot be reused.\r\nAffected users: "+std::to_wstring(affected.size())+L".";
    child(L"STATIC",caption.c_str(),0,12,12,504,54,0);
    const auto list=child(L"LISTBOX",L"",WS_TABSTOP|WS_BORDER|WS_VSCROLL|WS_HSCROLL,12,70,504,170,0);
    for(const auto& account:affected) {
        const auto row=L"#"+std::to_wstring(account.account_id)+L"  "+utf8_to_wide(account.display_name.value_or(""));
        if (SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(row.c_str())) < 0) {
            DestroyWindow(window);
            set_status(state,L"Refused — the complete affected-user list could not be displayed.");
            return false;
        }
    }
    if(affected.empty()) SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"No users currently assigned."));
    child(L"STATIC",L"Reassign the listed users and any default using this level to:",0,12,248,504,24,0);
    dialog.replacement=child(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,
        12,274,504,180,kLevelRemovalReplacement);
    for(const auto& level:state.security_catalog.levels) if(level.state==app::SecurityLevelState::active && level.level!=change.level) {
        const auto row=std::to_wstring(level.level)+L"  "+utf8_to_wide(level.label);
        const auto index=SendMessageW(dialog.replacement,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(row.c_str()));
        if (index >= 0) SendMessageW(dialog.replacement,CB_SETITEMDATA,index,level.level);
    }
    dialog.effects=child(L"STATIC",L"",0,12,312,504,79,0);
    const auto remove=child(L"BUTTON",L"Remove Level",WS_TABSTOP|BS_PUSHBUTTON,268,400,120,30,IDOK);
    const auto cancel=child(L"BUTTON",L"Cancel",WS_TABSTOP|BS_DEFPUSHBUTTON,396,400,120,30,IDCANCEL);
    if (!list || !dialog.replacement || !dialog.effects || !remove || !cancel) {
        DestroyWindow(window);
        set_status(state,L"Refused — the removal review could not be displayed.");
        return false;
    }
    review_replacement_level(window, dialog);
    state.appearance.apply_controls(window);
    EnableWindow(state.window,FALSE);ShowWindow(window,SW_SHOW);SetFocus(cancel);
    MSG message{};
    while(!dialog.done) {
        const BOOL result=GetMessageW(&message,nullptr,0,0);
        if(result<=0) {if(!result) PostQuitMessage(static_cast<int>(message.wParam));break;}
        if(!IsDialogMessageW(window,&message)) {TranslateMessage(&message);DispatchMessageW(&message);}
    }
    if(dialog.accepted) change=std::move(*dialog.reviewed);
    EnableWindow(state.window,TRUE);DestroyWindow(window);SetActiveWindow(state.window);
    return dialog.accepted;
}
