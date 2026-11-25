struct OperatorChatDialog {
    AppState& state;app::Identifier128 view;std::uint64_t token{},ack{},sent{};
    bool active{},creating{true};std::deque<std::wstring> history;
    std::function<app::ChatOperatorSnapshot(const app::ChatOperatorRequest&)> exchange;
    app::ChatOperatorSnapshot call(const app::ChatOperatorRequest& request){return exchange?exchange(request):state.runtime.chat_operator(request);}
};
void operator_chat_refresh(HWND window,OperatorChatDialog& d){
    app::ChatOperatorRequest request;request.action=app::ChatOperatorAction::poll;request.view=d.view;request.token=d.token;request.sequence=d.ack;
    const auto result=d.call(request);
    const bool was_active=d.active;d.active=result.code==app::ChatResult::ok&&result.active;
    for(const auto& line:result.lines)if(line.sequence>d.ack){
        // Runtime text is CP437, with controls removed before it reaches this view.
        const int count=MultiByteToWideChar(437,0,line.text.data(),static_cast<int>(line.text.size()),nullptr,0);
        std::wstring text(static_cast<std::size_t>(std::max(count,0)),L' ');
        if(count)MultiByteToWideChar(437,0,line.text.data(),static_cast<int>(line.text.size()),text.data(),count);
        d.history.push_back(std::move(text));while(d.history.size()>128)d.history.pop_front();d.ack=line.sequence;
    }
    std::wstring text;for(const auto& line:d.history)text+=line+L"\r\n";
    if(window_text(GetDlgItem(window,IDC_OPERATOR_HISTORY))!=text){SetDlgItemTextW(window,IDC_OPERATOR_HISTORY,text.c_str());SendDlgItemMessageW(window,IDC_OPERATOR_HISTORY,EM_SETSEL,text.size(),text.size());SendDlgItemMessageW(window,IDC_OPERATOR_HISTORY,EM_SCROLLCARET,0,0);}
    SetDlgItemTextW(window,IDC_OPERATOR_STATUS,result.code!=app::ChatResult::ok?L"Runtime connection unavailable. Chat will close safely.":result.pending?L"Connecting at the caller's safe screen...":result.active?(result.paused?L"Private Sysop chat - caller's calling time is paused.":L"Answering caller's page - normal calling time applies."):L"Chat ended.");
    EnableWindow(GetDlgItem(window,IDOK),d.active);EnableWindow(GetDlgItem(window,IDC_OPERATOR_TEXT),d.active);
    if(d.active&&!was_active&&GetForegroundWindow()==window)SetFocus(GetDlgItem(window,IDC_OPERATOR_TEXT));
    if(result.code!=app::ChatResult::ok||(!result.pending&&!result.active)){SetDlgItemTextW(window,IDCANCEL,L"Close");KillTimer(window,1);}
}
INT_PTR CALLBACK operator_chat_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
    auto* d=reinterpret_cast<OperatorChatDialog*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG){d=reinterpret_cast<OperatorChatDialog*>(lparam);SetWindowLongPtrW(window,DWLP_USER,reinterpret_cast<LONG_PTR>(d));
        SendDlgItemMessageW(window,IDC_OPERATOR_TEXT,EM_SETLIMITTEXT,512,0);g_appearance.apply_controls(window);SetTimer(window,1,350,nullptr);operator_chat_refresh(window,*d);SetFocus(GetDlgItem(window,IDC_OPERATOR_TEXT));return FALSE;}
    if(!d)return FALSE;LRESULT themed{};
    if(g_appearance.control_message(window,message,wparam,lparam,themed)){if(message==WM_NOTIFY){SetWindowLongPtrW(window,DWLP_MSGRESULT,themed);return TRUE;}return themed;}
    if(message==WM_TIMER){operator_chat_refresh(window,*d);return TRUE;}
    if(message==WM_COMMAND&&LOWORD(wparam)==IDOK){
        if(!d->active)return TRUE;const auto wide=window_text(GetDlgItem(window,IDC_OPERATOR_TEXT));BOOL replaced{};
        const auto size=WideCharToMultiByte(437,WC_NO_BEST_FIT_CHARS,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,&replaced);
        if(size<1||size>512||replaced)return TRUE;std::string text(static_cast<std::size_t>(size),' ');
        if(!WideCharToMultiByte(437,WC_NO_BEST_FIT_CHARS,wide.data(),static_cast<int>(wide.size()),text.data(),size,nullptr,&replaced)||replaced||app::chat_safe_text(text)!=text)return TRUE;
        app::ChatOperatorRequest request;request.action=app::ChatOperatorAction::send;request.view=d->view;request.token=d->token;request.sequence=d->sent+1;request.text=std::move(text);
        const auto result=d->call(request);
        if(result.code==app::ChatResult::ok){++d->sent;SetDlgItemTextW(window,IDC_OPERATOR_TEXT,L"");operator_chat_refresh(window,*d);}
        else SetDlgItemTextW(window,IDC_OPERATOR_STATUS,utf8_to_wide(std::string(app::chat_result_text(result.code))).c_str());return TRUE;
    }
    if(message==WM_CLOSE||(message==WM_COMMAND&&LOWORD(wparam)==IDCANCEL)){DestroyWindow(window);return TRUE;}
    if(message==WM_DESTROY){KillTimer(window,1);app::ChatOperatorRequest request;request.action=app::ChatOperatorAction::end;request.view=d->view;request.token=d->token;(void)d->call(request);return TRUE;}
    if(message==WM_NCDESTROY){auto& windows=d->state.operator_windows;windows.erase(std::remove(windows.begin(),windows.end(),window),windows.end());SetWindowLongPtrW(window,DWLP_USER,0);if(!d->creating)delete d;return TRUE;}
    return FALSE;
}
void open_operator_chat(AppState& state,const app::NodeAssignmentIdentity& target,bool answer){
    app::ChatOperatorRequest request;request.action=answer?app::ChatOperatorAction::answer:app::ChatOperatorAction::start;request.target=target;
    if(!win32::fill_secure_random(request.view.bytes.data(),request.view.bytes.size()))return;
    if(answer){const auto current=state.runtime.chat_operator({});for(const auto& page:current.pages)if(page.caller.node==target.node)request.page=page.request;if(!request.page){MessageBoxW(state.window,L"The page is no longer pending.",L"Answer Page",MB_OK);return;}}
    auto result=state.runtime.chat_operator(request);
    if(result.code==app::ChatResult::dnd&&MessageBoxW(state.window,L"This caller has Do Not Disturb on. Override for this chat only?",L"Chat with Caller",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES){request.override_dnd=true;result=state.runtime.chat_operator(request);}
    if(result.code!=app::ChatResult::ok){MessageBoxW(state.window,utf8_to_wide(std::string(app::chat_result_text(result.code))).c_str(),L"Chat with Caller",MB_OK);return;}
    auto* data=new OperatorChatDialog{state,request.view,result.token};
    const HWND window=CreateDialogParamW(state.instance,MAKEINTRESOURCEW(IDD_OPERATOR_CHAT),state.window,operator_chat_proc,reinterpret_cast<LPARAM>(data));
    if(!window){app::ChatOperatorRequest end;end.action=app::ChatOperatorAction::end;end.token=result.token;end.view=request.view;(void)state.runtime.chat_operator(end);delete data;return;}
    data->creating=false;
    state.operator_windows.push_back(window);SetWindowTextW(window,(L"Chat with "+utf8_to_wide(result.name)).c_str());ShowWindow(window,SW_SHOWNORMAL);
}
