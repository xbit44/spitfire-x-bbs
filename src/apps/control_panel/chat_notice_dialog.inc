struct NoticeDialog {
    AppState& state;
    std::optional<app::NodeAssignmentIdentity> target;
    std::wstring target_name;
    app::ChatConfigurationResult rooms;
    std::function<app::ChatResult(const app::ChatNoticeRequest&)> send;
    bool reviewed{};
};
std::optional<app::ChatNoticeRequest> notice_draft(HWND dialog,NoticeDialog& data){
    app::ChatNoticeRequest r;r.target=data.target;
    const auto choice=SendDlgItemMessageW(dialog,IDC_NOTICE_SCOPE,CB_GETCURSEL,0,0);if(choice==CB_ERR)return {};
    if(!r.target)r.room=static_cast<app::ChatRoomId>(SendDlgItemMessageW(dialog,IDC_NOTICE_SCOPE,CB_GETITEMDATA,choice,0));
    const auto wide=window_text(GetDlgItem(dialog,IDC_NOTICE_TEXT));if(wide.empty()||wide.size()>512)return {};
    BOOL replaced{};const int size=WideCharToMultiByte(437,WC_NO_BEST_FIT_CHARS,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,&replaced);
    if(size<1||size>512||replaced)return {};r.text.resize(static_cast<std::size_t>(size));
    if(!WideCharToMultiByte(437,WC_NO_BEST_FIT_CHARS,wide.data(),static_cast<int>(wide.size()),r.text.data(),size,nullptr,&replaced)||replaced)return {};
    // Notices are one bounded line, so line breaks are rejected rather than hidden.
    if(app::encode_chat_notice(r).empty())return {};return r;
}
INT_PTR CALLBACK notice_dialog_proc(HWND dialog,UINT message,WPARAM wparam,LPARAM lparam){
    auto* data=reinterpret_cast<NoticeDialog*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(message==WM_INITDIALOG){data=reinterpret_cast<NoticeDialog*>(lparam);SetWindowLongPtrW(dialog,DWLP_USER,reinterpret_cast<LONG_PTR>(data));
        SendDlgItemMessageW(dialog,IDC_NOTICE_TEXT,EM_SETLIMITTEXT,512,0);
        const auto add=[&](const std::wstring& name,app::ChatRoomId id){const auto row=SendDlgItemMessageW(dialog,IDC_NOTICE_SCOPE,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));SendDlgItemMessageW(dialog,IDC_NOTICE_SCOPE,CB_SETITEMDATA,row,id);};
        if(data->target)add(data->target_name,0);else{add(L"All Callers",0);if(data->rooms.configuration)for(const auto& room:data->rooms.configuration->rooms)if(room.enabled)add(L"Chat Room: "+utf8_to_wide(room.name),room.id);}
        SendDlgItemMessageW(dialog,IDC_NOTICE_SCOPE,CB_SETCURSEL,0,0);g_appearance.apply_controls(dialog);return TRUE;
    }
    if(!data)return FALSE;LRESULT themed{};
    if(g_appearance.control_message(dialog,message,wparam,lparam,themed)){
        // Dialog procedures return notification results through DWLP_MSGRESULT.
        // Returning CDRF_SKIPDEFAULT directly lets the default button painter
        // overwrite the dark-theme caption after custom drawing.
        if(message==WM_NOTIFY){SetWindowLongPtrW(dialog,DWLP_MSGRESULT,themed);return TRUE;}
        return themed;
    }
    if(message==WM_COMMAND){const auto id=LOWORD(wparam),event=HIWORD(wparam);
        if(id==IDCANCEL){EndDialog(dialog,IDCANCEL);return TRUE;}
        if((id==IDC_NOTICE_TEXT&&event==EN_CHANGE)||(id==IDC_NOTICE_SCOPE&&event==CBN_SELCHANGE)){data->reviewed=false;SetDlgItemTextW(dialog,IDOK,L"Review");return TRUE;}
        if(id==IDOK){const auto draft=notice_draft(dialog,*data);if(!draft){SetDlgItemTextW(dialog,IDC_NOTICE_RESULT,L"Enter one line of 1-512 CP437 characters. No notice was sent.");return TRUE;}
            if(!data->reviewed){data->reviewed=true;SetDlgItemTextW(dialog,IDC_NOTICE_RESULT,L"Check the destination and notice above. Send delivers it at each caller's next safe screen.");SetDlgItemTextW(dialog,IDOK,L"Send");return TRUE;}
            auto request=*draft;auto result=data->send(request);
            if(result==app::ChatResult::dnd&&request.target&&MessageBoxW(dialog,L"This caller has Do Not Disturb on. Override for this notice only?",L"Send System Notice",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES){request.override_dnd=true;result=data->send(request);}
            if(result==app::ChatResult::ok){EndDialog(dialog,IDOK);return TRUE;}
            const auto detail=utf8_to_wide(std::string(app::chat_result_text(result)));SetDlgItemTextW(dialog,IDC_NOTICE_RESULT,detail.c_str());data->reviewed=false;SetDlgItemTextW(dialog,IDOK,L"Review");return TRUE;
        }
    }
    if(message==WM_CLOSE){EndDialog(dialog,IDCANCEL);return TRUE;}return FALSE;
}
void open_notice_dialog(AppState& state,std::optional<app::NodeAssignmentIdentity> target={},std::wstring name={}){
    NoticeDialog data{state,target,std::move(name),state.runtime.chat_configuration(),[&](const auto& r){return state.runtime.send_notice(r);}};
    const auto result=DialogBoxParamW(state.instance,MAKEINTRESOURCEW(IDD_CHAT_NOTICE),state.window,notice_dialog_proc,reinterpret_cast<LPARAM>(&data));
    if(result==IDOK){state.status_text=L"System notice queued for safe delivery.";SendMessageW(state.status_bar,SB_SETTEXTW,0,reinterpret_cast<LPARAM>(state.status_text.c_str()));}
}
