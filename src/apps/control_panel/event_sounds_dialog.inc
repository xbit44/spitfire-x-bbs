struct EventSoundsDialog {
    std::optional<app::AlphaConfigurationSnapshot> snapshot;
    std::function<std::optional<app::AlphaConfigurationSnapshot>()> load;
    std::function<app::AlphaConfigurationUpdateResult(const app::AlphaConfigurationUpdateRequest&)> save;
    std::function<std::optional<std::wstring>(HWND)> browse;
    std::function<int(HWND)> confirm_exit;
    win32::EventSoundPlayer player;
    std::size_t selected{};
    int scroll{};
    bool initializing{};
    HFONT font{};
    ~EventSoundsDialog(){if(font)DeleteObject(font);}
};
HWND sound_field(HWND dialog,int id){return GetDlgItem(GetDlgItem(dialog,IDC_SOUND_ROWS),id);}
int sound_px(HWND dialog,int value){return MulDiv(value,static_cast<int>(window_dpi(dialog)),96);}
void sound_status(HWND dialog,const std::wstring& message){SetDlgItemTextW(dialog,IDC_SOUND_STATUS,message.c_str());}
void sound_font(HWND dialog,EventSoundsDialog& d,UINT dpi){
    const auto font=CreateFontW(-MulDiv(9,static_cast<int>(dpi),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    if(!font)return;SendMessageW(dialog,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
    EnumChildWindows(dialog,[](HWND child,LPARAM f)->BOOL{SendMessageW(child,WM_SETFONT,f,TRUE);return TRUE;},reinterpret_cast<LPARAM>(font));
    if(d.font)DeleteObject(d.font);d.font=font;
}
void sound_rows_layout(HWND dialog,EventSoundsDialog& d){
    const auto body=GetDlgItem(dialog,IDC_SOUND_ROWS);RECT area{};GetClientRect(body,&area);
    const int row=sound_px(dialog,28),total=row*static_cast<int>(app::kSoundEventCount);
    d.scroll=std::clamp(d.scroll,0,std::max(0,total-static_cast<int>(area.bottom)));
    SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS,0,total-1,static_cast<UINT>(std::max(1L,area.bottom)),d.scroll,0};
    SetScrollInfo(body,SB_VERT,&si,TRUE);GetClientRect(body,&area);
    const int label=sound_px(dialog,184),browse=sound_px(dialog,28),gap=sound_px(dialog,8),height=sound_px(dialog,23);
    for(std::size_t i=0;i<app::kSoundEventCount;++i){const int at=static_cast<int>(i),y=at*row-d.scroll;
        MoveWindow(sound_field(dialog,IDC_SOUND_ROW_FIRST+at),0,y+sound_px(dialog,2),label,height,TRUE);
        MoveWindow(sound_field(dialog,IDC_SOUND_PATH_FIRST+at),label+gap,y,std::max(40,static_cast<int>(area.right)-label-browse-3*gap),height,TRUE);
        MoveWindow(sound_field(dialog,IDC_SOUND_BROWSE_FIRST+at),area.right-browse,y,browse,height,TRUE);
    }
    InvalidateRect(body,nullptr,TRUE);
}
void sound_layout(HWND dialog,EventSoundsDialog& d){
    RECT a{};GetClientRect(dialog,&a);const int margin=sound_px(dialog,12),footer=sound_px(dialog,84),top=sound_px(dialog,36);
    MoveWindow(GetDlgItem(dialog,IDC_SOUND_HINT),margin,sound_px(dialog,10),a.right-2*margin,sound_px(dialog,20),TRUE);
    MoveWindow(GetDlgItem(dialog,IDC_SOUND_ROWS),margin,top,a.right-2*margin,std::max(30,static_cast<int>(a.bottom)-top-footer),TRUE);
    MoveWindow(GetDlgItem(dialog,IDC_SOUND_STATUS),margin,a.bottom-footer+sound_px(dialog,6),a.right-2*margin,sound_px(dialog,40),TRUE);
    const int gap=sound_px(dialog,8),y=a.bottom-sound_px(dialog,36),height=sound_px(dialog,24);
    int x=margin;
    for(const auto& button:std::array<std::pair<int,int>,3>{{{IDC_SOUND_TEST,94},{IDC_SOUND_STOP,60},{IDC_SOUND_CLEAR,94}}}){
        const auto width=sound_px(dialog,button.second);MoveWindow(GetDlgItem(dialog,button.first),x,y,width,height,TRUE);x+=width+gap;
    }
    const int exit_width=sound_px(dialog,68),save_width=sound_px(dialog,100);
    MoveWindow(GetDlgItem(dialog,IDCANCEL),a.right-margin-exit_width,y,exit_width,height,TRUE);
    MoveWindow(GetDlgItem(dialog,IDOK),a.right-margin-exit_width-gap-save_width,y,save_width,height,TRUE);
    sound_rows_layout(dialog,d);
}
void sound_select(HWND dialog,EventSoundsDialog& d,std::size_t selected){
    d.selected=selected;const auto body=GetDlgItem(dialog,IDC_SOUND_ROWS);
    CheckRadioButton(body,IDC_SOUND_ROW_FIRST,IDC_SOUND_ROW_FIRST+static_cast<int>(app::kSoundEventCount)-1,IDC_SOUND_ROW_FIRST+static_cast<int>(selected));
    RECT area{};GetClientRect(body,&area);const int row=sound_px(dialog,28),y=static_cast<int>(selected)*row;
    if(y<d.scroll)d.scroll=y;else if(y+row>d.scroll+area.bottom)d.scroll=std::max(0,y+row-static_cast<int>(area.bottom));
    sound_rows_layout(dialog,d);
    if(selected==static_cast<std::size_t>(app::SoundEvent::inbound_binkp))sound_status(dialog,L"BinkP assignment can be tested and saved. Live inbound BinkP alerts are not available yet.");
}
LRESULT CALLBACK sound_body_proc(HWND body,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR){
    const auto dialog=GetParent(body);auto* d=reinterpret_cast<EventSoundsDialog*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(d && message==WM_VSCROLL){SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(body,SB_VERT,&si);
        const int row=sound_px(dialog,28);switch(LOWORD(wparam)){
            case SB_LINEUP:d->scroll-=row;break;case SB_LINEDOWN:d->scroll+=row;break;
            case SB_PAGEUP:d->scroll-=si.nPage;break;case SB_PAGEDOWN:d->scroll+=si.nPage;break;
            case SB_THUMBTRACK:d->scroll=si.nTrackPos;break;case SB_TOP:d->scroll=0;break;case SB_BOTTOM:d->scroll=si.nMax;break;
        }sound_rows_layout(dialog,*d);return 0;
    }
    if(d && message==WM_MOUSEWHEEL){d->scroll-=GET_WHEEL_DELTA_WPARAM(wparam)/WHEEL_DELTA*sound_px(dialog,28);sound_rows_layout(dialog,*d);return 0;}
    if(message==WM_COMMAND || message==WM_NOTIFY || message==WM_CTLCOLORSTATIC || message==WM_CTLCOLOREDIT || message==WM_CTLCOLORBTN)
        return SendMessageW(dialog,message,wparam,lparam);
    return DefSubclassProc(body,message,wparam,lparam);
}
std::optional<std::wstring> browse_sound(HWND dialog){
    std::array<wchar_t,260> path{};OPENFILENAMEW pick{sizeof(pick)};pick.hwndOwner=dialog;
    pick.lpstrFilter=L"WAV files (*.wav)\0*.wav\0\0";pick.lpstrFile=path.data();pick.nMaxFile=static_cast<DWORD>(path.size());
    pick.lpstrTitle=L"Choose Event Sound";pick.lpstrDefExt=L"wav";
    pick.Flags=OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT;
    if(!GetOpenFileNameW(&pick))return {};return std::wstring(path.data());
}
bool sound_dirty(HWND dialog,const EventSoundsDialog& d){
    if(!d.snapshot)return false;
    for(std::size_t i=0;i<app::kSoundEventCount;++i)
        if(window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST+static_cast<int>(i)))!=utf8_to_wide(d.snapshot->saved.event_sounds.paths[i]))return true;
    return false;
}
bool sound_save(HWND dialog,EventSoundsDialog& d){
    d.player.stop();if(!d.snapshot)return false;auto candidate=d.snapshot->saved;
    for(std::size_t i=0;i<app::kSoundEventCount;++i){
        const auto path=window_text(sound_field(dialog,IDC_SOUND_PATH_FIRST+static_cast<int>(i)));const auto text=wide_to_utf8(path);
        const auto event=utf8_to_wide(std::string(app::kSoundEventNames[i]));
        if(!text || !app::valid_event_sound_path(*text)){
            sound_select(dialog,d,i);sound_status(dialog,event+L": enter a full local .wav path or leave it blank. No changes saved.");return false;
        }
        if(!path.empty()){const auto file=win32::read_event_sound(path);if(!file){sound_select(dialog,d,i);sound_status(dialog,event+L": "+file.detail+L" No changes saved.");return false;}}
        candidate.event_sounds.paths[i]=*text;
    }
    if(!sound_dirty(dialog,d)){sound_status(dialog,L"No assignment changes to save.");return true;}
    const auto expected=candidate.configuration_revision;
    if(expected==std::numeric_limits<std::uint64_t>::max()){sound_status(dialog,L"Configuration revision unavailable. No changes saved.");return false;}
    candidate.configuration_revision=expected+1;const auto result=d.save({expected,candidate});
    if(result.code!=app::LocalControlResponseCode::ok){sound_status(dialog,utf8_to_wide(result.detail)+L" No changes saved.");return false;}
    auto verified=d.load();
    if(!verified || verified->saved.configuration_revision<candidate.configuration_revision || !(verified->saved.event_sounds==candidate.event_sounds)){
        sound_status(dialog,L"Save was sent, but the saved assignments could not be verified. Reopen Event Sounds to check them.");
        d.snapshot.reset();EnableWindow(GetDlgItem(dialog,IDOK),FALSE);return false;
    }
    d.snapshot=std::move(verified);sound_status(dialog,L"Event sound assignments saved. "+utf8_to_wide(result.detail));return true;
}
void sound_exit(HWND dialog,EventSoundsDialog& d){
    if(sound_dirty(dialog,d)){
        const auto choice=d.confirm_exit?d.confirm_exit(dialog):MessageBoxW(dialog,L"Save your event sound changes before exiting?",L"Event Sounds",MB_YESNOCANCEL|MB_ICONQUESTION);
        if((choice!=IDYES && choice!=IDNO) || (choice==IDYES && !sound_save(dialog,d)))return;
    }
    d.player.stop();EndDialog(dialog,IDCANCEL);
}
INT_PTR CALLBACK event_sounds_proc(HWND dialog,UINT message,WPARAM wparam,LPARAM lparam){
    auto* d=reinterpret_cast<EventSoundsDialog*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(message==WM_INITDIALOG){d=reinterpret_cast<EventSoundsDialog*>(lparam);SetWindowLongPtrW(dialog,DWLP_USER,reinterpret_cast<LONG_PTR>(d));
        d->initializing=true;const auto body=GetDlgItem(dialog,IDC_SOUND_ROWS);
        SetWindowSubclass(body,sound_body_proc,1,0);const auto font=SendMessageW(dialog,WM_GETFONT,0,0);
        for(std::size_t i=0;i<app::kSoundEventCount;++i){const int row=static_cast<int>(i);const auto name=utf8_to_wide(std::string(app::kSoundEventNames[i]));
            const auto radio=CreateWindowExW(0,L"BUTTON",name.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTORADIOBUTTON|BS_NOTIFY|(i?0:WS_GROUP),0,0,0,0,body,reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SOUND_ROW_FIRST+row)),GetModuleHandleW(nullptr),nullptr);
            const auto edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,body,reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SOUND_PATH_FIRST+row)),GetModuleHandleW(nullptr),nullptr);
            const auto browse=CreateWindowExW(0,L"BUTTON",L"...",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,body,reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SOUND_BROWSE_FIRST+row)),GetModuleHandleW(nullptr),nullptr);
            for(const auto child:{radio,edit,browse})SendMessageW(child,WM_SETFONT,font,FALSE);
            SendMessageW(edit,EM_SETLIMITTEXT,259,0);
        }
        d->snapshot=d->load();if(d->snapshot)for(std::size_t i=0;i<app::kSoundEventCount;++i)SetWindowTextW(sound_field(dialog,IDC_SOUND_PATH_FIRST+static_cast<int>(i)),utf8_to_wide(d->snapshot->saved.event_sounds.paths[i]).c_str());
        else {sound_status(dialog,L"Saved settings could not be read. Reopen when Runtime is ready. Nothing can be saved.");EnableWindow(GetDlgItem(dialog,IDOK),FALSE);}
        sound_font(dialog,*d,window_dpi(dialog));g_appearance.apply_controls(dialog);d->initializing=false;CheckRadioButton(body,IDC_SOUND_ROW_FIRST,IDC_SOUND_ROW_FIRST+9,IDC_SOUND_ROW_FIRST);
        RECT frame{};GetWindowRect(dialog,&frame);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(dialog,MONITOR_DEFAULTTONEAREST),&monitor);
        const auto width=std::min(frame.right-frame.left,monitor.rcWork.right-monitor.rcWork.left-32),height=std::min(frame.bottom-frame.top,monitor.rcWork.bottom-monitor.rcWork.top-32);
        SetWindowPos(dialog,nullptr,monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2,monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
        sound_layout(dialog,*d);return TRUE;
    }
    if(!d)return FALSE;LRESULT themed{};
    if(g_appearance.control_message(dialog,message,wparam,lparam,themed)){if(message==WM_NOTIFY){SetWindowLongPtrW(dialog,DWLP_MSGRESULT,themed);return TRUE;}return themed;}
    if(message==WM_SIZE){sound_layout(dialog,*d);return TRUE;}
    if(message==WM_GETMINMAXINFO){auto* size=reinterpret_cast<MINMAXINFO*>(lparam);size->ptMinTrackSize={sound_px(dialog,600),sound_px(dialog,300)};return TRUE;}
    if(message==WM_DPICHANGED){sound_font(dialog,*d,HIWORD(wparam));const auto* r=reinterpret_cast<RECT*>(lparam);SetWindowPos(dialog,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);sound_layout(dialog,*d);return TRUE;}
    if(message==WM_COMMAND){const int id=LOWORD(wparam),notice=HIWORD(wparam);
        if(id==IDCANCEL){sound_exit(dialog,*d);return TRUE;}
        if(id>=IDC_SOUND_ROW_FIRST && id<IDC_SOUND_ROW_FIRST+10 && (notice==BN_CLICKED || notice==BN_SETFOCUS)){sound_select(dialog,*d,id-IDC_SOUND_ROW_FIRST);return TRUE;}
        if(id>=IDC_SOUND_PATH_FIRST && id<IDC_SOUND_PATH_FIRST+10 && notice==EN_SETFOCUS && !d->initializing){sound_select(dialog,*d,id-IDC_SOUND_PATH_FIRST);return TRUE;}
        if(id>=IDC_SOUND_PATH_FIRST && id<IDC_SOUND_PATH_FIRST+10 && notice==EN_CHANGE && !d->initializing){sound_status(dialog,L"Unsaved assignment changes. Save Changes keeps them.");return TRUE;}
        if(id>=IDC_SOUND_BROWSE_FIRST && id<IDC_SOUND_BROWSE_FIRST+10 && notice==BN_CLICKED){
            const auto row=static_cast<std::size_t>(id-IDC_SOUND_BROWSE_FIRST);
            sound_select(dialog,*d,row);const auto chosen=d->browse?d->browse(dialog):browse_sound(dialog);
            // Native modal activation can temporarily focus the first radio.
            // Keep the clicked event authoritative on Open, Cancel and error.
            sound_select(dialog,*d,row);SetFocus(sound_field(dialog,id));
            if(chosen){const auto utf8=wide_to_utf8(*chosen);const auto file=win32::read_event_sound(*chosen);
                // Keep the actual selection visible, including a rejected file.
                // Save must validate this draft instead of silently saving the old path.
                SetWindowTextW(sound_field(dialog,IDC_SOUND_PATH_FIRST+static_cast<int>(row)),chosen->c_str());
                if(!utf8 || !app::valid_event_sound_path(*utf8) || !file){sound_status(dialog,utf8_to_wide(std::string(app::kSoundEventNames[row]))+L": "+(file.detail.empty()?L"Choose a supported local WAV path.":file.detail)+L" Not saved.");return TRUE;}
                sound_status(dialog,L"WAV selected. Test previews it; Save Changes keeps the assignment.");}
            return TRUE;
        }
        const auto selected=sound_field(dialog,IDC_SOUND_PATH_FIRST+static_cast<int>(d->selected));
        if(id==IDC_SOUND_TEST){const auto path=window_text(selected);if(path.empty()){d->player.stop();sound_status(dialog,L"This event is silent. Browse for a WAV to preview it.");return TRUE;}
            std::wstring detail;d->player.preview(path,detail);sound_status(dialog,detail);return TRUE;}
        if(id==IDC_SOUND_STOP){d->player.stop();sound_status(dialog,L"Preview stopped.");return TRUE;}
        if(id==IDC_SOUND_CLEAR){d->player.stop();SetWindowTextW(selected,L"");sound_status(dialog,L"Assignment cleared. Save Changes keeps it silent; the WAV file is unchanged.");return TRUE;}
        if(id==IDOK){sound_save(dialog,*d);return TRUE;}
    }
    if(message==WM_CLOSE){sound_exit(dialog,*d);return TRUE;}
    if(message==WM_DESTROY){d->player.stop();return TRUE;}return FALSE;
}
void open_event_sounds(AppState& state){
    state.event_sound_player.stop();state.pending_sounds.clear();state.sound_preview_open=true;
    struct Resume {AppState& state;~Resume(){state.sound_preview_open=false;state.sound_warning.clear();}} resume{state};
    EventSoundsDialog data;data.load=[&]{return state.runtime.configuration_snapshot();};
    data.save=[&](const auto& request){return state.runtime.update_configuration(request);};
    DialogBoxParamW(state.instance,MAKEINTRESOURCEW(IDD_EVENT_SOUNDS),state.window,event_sounds_proc,reinterpret_cast<LPARAM>(&data));
}
