struct FtnRetentionDialog {State* state{};std::wstring text;};
INT_PTR CALLBACK ftn_retention_dialog(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
    auto* dialog=reinterpret_cast<FtnRetentionDialog*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG){dialog=reinterpret_cast<FtnRetentionDialog*>(lparam);SetWindowLongPtrW(window,DWLP_USER,lparam);SendDlgItemMessageW(window,IDC_FTN_RETENTION_TEXT,EM_SETLIMITTEXT,131072,0);SetDlgItemTextW(window,IDC_FTN_RETENTION_TEXT,dialog->text.c_str());dialog->state->appearance.apply_controls(window);SendMessageW(window,DM_SETDEFID,IDCANCEL,0);SetFocus(GetDlgItem(window,IDCANCEL));return FALSE;}
    if(!dialog)return FALSE;LRESULT themed{};if(dialog->state->appearance.control_message(window,message,wparam,lparam,themed)){if(message==WM_NOTIFY){SetWindowLongPtrW(window,DWLP_MSGRESULT,themed);return TRUE;}return themed;}
    if(message==WM_COMMAND&&(LOWORD(wparam)==IDOK||LOWORD(wparam)==IDCANCEL)){EndDialog(window,LOWORD(wparam));return TRUE;}
    if(message==WM_CLOSE){EndDialog(window,IDCANCEL);return TRUE;}
    if(message==WM_ERASEBKGND){RECT area{};GetClientRect(window,&area);FillRect(reinterpret_cast<HDC>(wparam),&area,dialog->state->appearance.background_brush());return TRUE;}
    return FALSE;
}
bool ftn_review_retention(State& s,const app::FtnRetentionPreview& p){
    if(!p.shortened)return true;
    FtnRetentionDialog dialog{&s,L"Saving will shorten protection for "+std::to_wstring(p.shortened)+L" existing processed originals. Imported messages and published files remain. Dates use the BBS time zone.\r\n\r\n"};
    for(const auto& n:p.networks)dialog.text+=utf8_to_wide(n.name)+L" ("+utf8_to_wide(n.network)+L")\r\nDue now: "+std::to_wstring(n.due_now)+L"\r\nExpire later: "+std::to_wstring(n.expire_later)+L"\r\nPreviously kept Forever: "+std::to_wstring(n.from_forever)+L"\r\nNew expiry range: "+bbs_local_time(s,n.earliest)+L" to "+bbs_local_time(s,n.latest)+L"\r\n\r\n";
    dialog.text+=L"Save applies this exact reviewed set and the other edits together. Originals still needed for processing, forwarding or recovery remain held. Later arrivals cannot be added to this review.\r\n\r\nCancel keeps all saved settings and protection dates unchanged; your draft remains unsaved. Keep Off affects future successful processing only.";
    const auto focus=GetFocus();const auto result=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_FTN_RETENTION_REVIEW),s.window,ftn_retention_dialog,reinterpret_cast<LPARAM>(&dialog));if(IsWindow(focus))SetFocus(focus);return result==IDOK;
}
