// Native owner presentation of bounded observations from the sole Runtime.
struct BinkPConsole {
    HWND window{},session{},status{},send{},receive{},log{},poll{},scan{},queues{},configure{},activity_button{},details{};
    HFONT current_font{};int framed_columns{},scroll_y{};bool laying_out{};
    app::FtnRuntimeResponse current;std::uint64_t selected{},sample_tick{},sample_sent{},sample_received{},send_rate{},receive_rate{};bool rate_known{},queue_view{},follow_active{true};std::uint32_t queue_offset{};
    std::uint64_t send_pulse_until{},receive_pulse_until{},send_pulse_session{},receive_pulse_session{};
    const app::FtnSessionStatus* selected_session()const{const auto found=std::find_if(current.sessions.begin(),current.sessions.end(),[&](const auto& s){return s.serial==selected;});return found==current.sessions.end()?nullptr:&*found;}
    void select_session(){const auto index=SendMessageW(session,CB_GETCURSEL,0,0);follow_active=index==0;
        if(index>0&&static_cast<std::size_t>(index-1)<current.sessions.size())selected=current.sessions[static_cast<std::size_t>(index-1)].serial;
        sample_tick=0;rate_known=false;queue_view=false;queue_offset=0;SetWindowTextW(queues,L"Queues / Held Mail");apply(current);
    }
    static bool text_if_changed(HWND h,const std::wstring& text){const auto size=GetWindowTextLengthW(h);std::wstring old(static_cast<std::size_t>(size)+1,L'\0');GetWindowTextW(h,old.data(),size+1);old.resize(size);if(old==text)return false;SetWindowTextW(h,text.c_str());return true;}
    static std::wstring rate_text(std::uint64_t value){wchar_t out[64]{};if(value>=1048576)swprintf_s(out,L"%.1f MB/s",static_cast<double>(value)/1048576);else if(value>=1024)swprintf_s(out,L"%.1f KB/s",static_cast<double>(value)/1024);else swprintf_s(out,L"%llu B/s",static_cast<unsigned long long>(value));return out;}
    std::wstring action_notice;std::vector<std::wstring> rendered_sessions;int rendered_selection{-1};
    static std::wstring timestamp(std::uint64_t seconds){
        ULARGE_INTEGER ticks{};ticks.QuadPart=(seconds+11644473600ULL)*10000000ULL;
        FILETIME file{ticks.LowPart,ticks.HighPart},local{};SYSTEMTIME time{};
        if(!FileTimeToLocalFileTime(&file,&local)||!FileTimeToSystemTime(&local,&time))return L"Time unavailable";
        wchar_t out[32]{};swprintf_s(out,L"%04u-%02u-%02u %02u:%02u:%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);return out;
    }
    void show_activity(){queue_view=false;queue_offset=0;SetWindowTextW(queues,L"Queues / Held Mail");render_log(true);}
    void notice(std::wstring text){FILETIME now{};GetSystemTimeAsFileTime(&now);ULARGE_INTEGER n{};n.LowPart=now.dwLowDateTime;n.HighPart=now.dwHighDateTime;action_notice=timestamp(n.QuadPart/10000000ULL-11644473600ULL)+L"  "+text;show_activity();}
    void render_log(bool force=false){
        if(queue_view)return;std::wstring text=L"BINKP ACTIVITY (recent events, local time; this Runtime)\r\n";
        for(const auto& a:current.activity)text+=timestamp(a.utc_seconds)+L"  "+utf8_to_wide(a.network+(a.link.empty()?std::string{}:" / "+a.link))+(a.session?L"  #"+std::to_wstring(a.session):std::wstring{})+L"\r\n  "+utf8_to_wide(a.message)+L"\r\n";
        if(current.activity.empty())text+=L"No connection activity recorded yet.\r\n";
        if(!current.classic_error.empty())text+=L"Classic listener: "+utf8_to_wide(current.classic_error)+L"\r\n";
        if(!current.tls_error.empty())text+=L"TLS listener: "+utf8_to_wide(current.tls_error)+L"\r\n";
        if(!current.processing_error.empty())text+=L"Attention: "+utf8_to_wide(current.processing_error)+L"\r\n";
        if(!action_notice.empty())text+=L"\r\n"+action_notice+L"\r\n";
        const auto length=GetWindowTextLengthW(log);std::wstring old(static_cast<std::size_t>(length)+1,L'\0');GetWindowTextW(log,old.data(),length+1);old.resize(length);
        if(old==text){if(force){SendMessageW(log,EM_SETSEL,text.size(),text.size());SendMessageW(log,EM_SCROLLCARET,0,0);}return;}
        DWORD begin{},end{};SendMessageW(log,EM_GETSEL,reinterpret_cast<WPARAM>(&begin),reinterpret_cast<LPARAM>(&end));
        SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS};GetScrollInfo(log,SB_VERT,&scroll);
        const bool following=force||(begin==end&&scroll.nPos+static_cast<int>(scroll.nPage)>=scroll.nMax);
        if(!following)return; // Freeze the displayed snapshot while reading/copying history.
        SetWindowTextW(log,text.c_str());SendMessageW(log,EM_SETSEL,text.size(),text.size());SendMessageW(log,EM_SCROLLCARET,0,0);
    }
    void poll_now(const std::function<app::FtnRuntimeResponse(const app::FtnRuntimeRequest&)>& request){
        notice(L"Selecting connection to poll...");
        for(std::uint32_t offset=0;offset<4096;offset+=64){
            app::FtnRuntimeRequest query;query.action=app::FtnRuntimeAction::connections;query.offset=offset;const auto page=request(query);
            if(page.code!=app::FtnConfigurationCode::ok){notice(L"Could not load FTN connections. Check Runtime status and try again.");return;}
            const auto eligible=std::count_if(page.connections.begin(),page.connections.end(),[](const auto& c){return c.enabled&&c.outbound;});
            if(page.connections.empty()&&!page.more_connections){notice(L"No connections configured. Add and enable an outgoing connection in FTN Settings.");return;}
            if(!eligible&&!page.more_connections&&offset==0){notice(L"No enabled outgoing connections. Check the network and connection settings.");return;}
            int chosen=-1;
            if(offset==0&&!page.more_connections&&eligible==1){for(std::size_t i=0;i<page.connections.size();++i)if(page.connections[i].enabled&&page.connections[i].outbound)chosen=static_cast<int>(i);}
            else{
                HMENU menu=CreatePopupMenu();if(!menu){notice(L"Could not open the connection selector. No poll requested.");return;}
                for(std::size_t i=0;i<page.connections.size();++i){const auto& c=page.connections[i];AppendMenuW(menu,MF_STRING|((c.enabled&&c.outbound)?MF_ENABLED:MF_GRAYED),static_cast<UINT_PTR>(i+1),utf8_to_wide(c.name).c_str());}
                if(page.more_connections)AppendMenuW(menu,MF_STRING,100,L"More Connections...");RECT bounds{};GetWindowRect(poll,&bounds);
                const auto picked=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,bounds.left,bounds.bottom,0,GetParent(window),nullptr);DestroyMenu(menu);
                if(picked==100)continue;if(!picked){notice(L"Poll cancelled; no connection requested.");return;}chosen=static_cast<int>(picked)-1;
            }
            if(chosen<0||static_cast<std::size_t>(chosen)>=page.connections.size()){notice(L"Connection selection unavailable; no poll requested.");return;}
            const auto& c=page.connections[static_cast<std::size_t>(chosen)];app::FtnRuntimeRequest poll_request;poll_request.action=app::FtnRuntimeAction::poll;poll_request.network=c.network;poll_request.link=c.link;
            const auto result=request(poll_request);apply(result);
            notice(result.code==app::FtnConfigurationCode::ok?L"Poll requested: "+utf8_to_wide(c.name)+L". Watch the activity log for the result.":L"Poll unavailable: "+utf8_to_wide(c.name)+L". Check FTN status and connection settings; no delivery confirmed.");return;
        }
        notice(L"Connection list limit reached; no poll requested.");
    }
    void apply(app::FtnRuntimeResponse value){
        // A complete short transfer can fall between two one-second snapshots.
        // Briefly mark its direction without showing invented progress or
        // increasing idle Runtime polling. The finite native timer only paints.
        const auto pulse_tick=GetTickCount64();
        for(const auto& item:value.sessions){const auto old=std::find_if(current.sessions.begin(),current.sessions.end(),[&](const auto& v){return v.serial==item.serial;});
            if(item.sent_files&&(old==current.sessions.end()||item.sent_files>old->sent_files)){send_pulse_session=item.serial;send_pulse_until=pulse_tick+1500;SetTimer(send,15,1500,nullptr);InvalidateRect(send,nullptr,FALSE);}
            if(item.received_files&&(old==current.sessions.end()||item.received_files>old->received_files)){receive_pulse_session=item.serial;receive_pulse_until=pulse_tick+1500;SetTimer(receive,15,1500,nullptr);InvalidateRect(receive,nullptr,FALSE);}
        }
        current=std::move(value);const bool ready=current.code==app::FtnConfigurationCode::ok;
        const bool run=ready&&current.capable&&!current.paused&&!current.serial_recovery_required&&current.mode!=app::FtnMailerMode::disabled&&current.active_revision==current.saved_revision&&current.active_revision>0;
        EnableWindow(poll,run&&current.mode==app::FtnMailerMode::native);EnableWindow(scan,run);EnableWindow(queues,ready);
        std::wstring message=!ready?L"FTN Runtime status unavailable.":current.serial_recovery_required?L"FTN stopped: message ID history needs recovery.":!current.capable?L"FTN is unavailable for this installation. Check Setup and settings.":current.mode==app::FtnMailerMode::disabled?L"FTN disabled. Settings and mail are retained.":current.mode==app::FtnMailerMode::external_binkd?(current.diagnostic=="ftn_external_processing_active"?std::wstring(L"SFX processes mail; BinkD handles connections. Scan exports mail. Handed to BinkD does not confirm delivery."):L"External BinkD: "+utf8_to_wide(current.diagnostic)):current.diagnostic=="ftn_native_active"?L"Native BinkP is running.":current.diagnostic=="ftn_native_listener_failure"?L"Native BinkP: a listener could not start. See session results.":L"Native BinkP is not ready. Check FTN settings.";
        if(ready)message+=L"\r\nSaved revision "+std::to_wstring(current.saved_revision)+L" / active "+std::to_wstring(current.active_revision)+L"; Classic "+(current.classic_listening?std::to_wstring(current.classic_port):L"Off")+L"; TLS "+(current.tls_listening?std::to_wstring(current.tls_port):L"Off")+(current.paused?L"; backup pause":L"");text_if_changed(status,message);
        const auto prior=selected;
        if(follow_active||!selected_session()){
            const app::FtnSessionStatus* latest=nullptr;for(const auto& item:current.sessions)if(!latest||(item.active&&!latest->active)||(item.active==latest->active&&item.serial>latest->serial))latest=&item;
            selected=latest?latest->serial:0;
        }
        if(prior!=selected){sample_tick=0;rate_known=false;send_rate=receive_rate=0;}
        // Leave an open list intact while the operator is choosing history.
        if(!SendMessageW(session,CB_GETDROPPEDSTATE,0,0)){
            std::wstring live=L"Follow active session";if(const auto* item=selected_session();item&&follow_active)live+=L" - "+utf8_to_wide(item->network+" / "+item->link);
            std::vector<std::wstring> labels{live};int selection=0;
            for(std::size_t i=0;i<current.sessions.size();++i){const auto& item=current.sessions[i];labels.push_back(L"#"+std::to_wstring(item.serial)+L" "+utf8_to_wide(item.network+(item.link.empty()?std::string{}:" / "+item.link))+(item.outbound?L" - Outbound":L" - Inbound")+(item.active?L" - Active":item.completed?L" - Completed":L" - Failed"));if(!follow_active&&item.serial==selected)selection=static_cast<int>(i+1);}
            if(labels!=rendered_sessions||selection!=rendered_selection){
                SendMessageW(session,WM_SETREDRAW,FALSE,0);SendMessageW(session,CB_RESETCONTENT,0,0);
                for(const auto& label:labels)SendMessageW(session,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
                SendMessageW(session,CB_SETCURSEL,selection,0);EnableWindow(session,!current.sessions.empty());SendMessageW(session,WM_SETREDRAW,TRUE,0);InvalidateRect(session,nullptr,FALSE);
                rendered_sessions=std::move(labels);rendered_selection=selection;
            }
        }
        const auto* s=selected_session();const auto tick=GetTickCount64();rate_known=s&&sample_tick&&tick>sample_tick&&s->sent_bytes>=sample_sent&&s->received_bytes>=sample_received;if(rate_known){send_rate=(s->sent_bytes-sample_sent)*1000/(tick-sample_tick);receive_rate=(s->received_bytes-sample_received)*1000/(tick-sample_tick);}if(s){sample_tick=tick;sample_sent=s->sent_bytes;sample_received=s->received_bytes;}else sample_tick=0;
        if(!ready){queue_view=false;queue_offset=0;}render_log();framed_columns=0;pending_frame();
    }
    void show_queues(const app::FtnRuntimeResponse& value){
        if(value.code!=app::FtnConfigurationCode::ok||!value.queues){SetWindowTextW(log,L"Queue snapshot unavailable. No totals inferred.");return;}
        const auto& q=*value.queues;queue_view=true;std::wstring text=L"FTN QUEUES / HELD MAIL - recorded custody states\r\nUnscanned local messages: "+std::to_wstring(q.unscanned)+L"\r\nIncoming partial / received / processing / held / processed: ";for(const auto n:q.ingress)text+=std::to_wstring(n)+L"  ";text+=L"\r\nOutbound queued / claimed / awaiting ACK / delivered / held / handed to BinkD: ";for(const auto n:q.outbound)text+=std::to_wstring(n)+L"  ";text+=L"\r\n\r\n";
        const std::array<const wchar_t*,5> inbound{{L"Partial",L"Received",L"Processing",L"Held",L"Processed"}};const std::array<const wchar_t*,6> outbound{{L"Queued",L"Claimed",L"Awaiting ACK",L"Delivered",L"Held",L"Handed to BinkD"}};
        for(const auto& row:q.rows){text+=(row.outbound?L"OUT ":L"IN  ")+utf8_to_wide(row.network+" / "+row.link+" / "+row.file)+L"  "+(row.outbound?outbound[row.state]:inbound[row.state])+L"  "+std::to_wstring(row.bytes)+L" bytes\r\n";if(!row.reason.empty())text+=L"    "+utf8_to_wide(row.reason)+L"\r\n";}if(q.rows.empty())text+=L"No pending or held records on this page.\r\n";SetWindowTextW(log,text.c_str());queue_offset=q.more?queue_offset+64:0;SetWindowTextW(queues,q.more?L"Next Queue Page":L"Refresh Queues");
    }
    void pending_frame(){
        const auto* item=selected_session();
        const auto text=[&](bool sending){const auto* p=item?(sending?&item->send:&item->receive):nullptr;
            std::wstring value=sending?L"SEND":L"RECEIVE";
            if(!p||p->name.empty())return value+L" - "+(item&&item->active?L"Waiting for file":L"Idle")+L"\nNo file transfer";
            value+=L" - "+utf8_to_wide(p->name)+L" ("+std::to_wstring(p->size)+L" bytes)";
            const auto percent=p->size?std::min<std::uint64_t>(100,p->position*100/p->size):(p->active?0:100);
            value+=L"\n"+std::to_wstring(p->position)+L" / "+std::to_wstring(p->size)+L" bytes   "+std::to_wstring(percent)+L"%   ";
            if(!item->active)value+=item->completed?L"Session completed":L"Session ended";
            else if(p->awaiting_ack)value+=L"Awaiting acknowledgement";
            else if(!p->active)value+=L"File finished";
            else value+=rate_known?rate_text(sending?send_rate:receive_rate):L"Measuring speed...";
            return value;
        };
        if(text_if_changed(send,text(true)))InvalidateRect(send,nullptr,FALSE);if(text_if_changed(receive,text(false)))InvalidateRect(receive,nullptr,FALSE);
        std::wstring info=L"Waiting for a BinkP connection.";
        if(item){info=utf8_to_wide(item->network+" / "+item->link)+(item->outbound?L" - Outbound":L" - Inbound")+L" - "+(item->active?L"Active":item->completed?L"Completed":L"Failed");
            if(!item->peer_address.empty())info+=L" | Peer: "+utf8_to_wide(item->peer_address);
            info+=L"\r\n";if(!item->endpoint.empty())info+=L"Configured endpoint: "+utf8_to_wide(item->endpoint)+L" | ";
            info+=L"Sent: "+std::to_wstring(item->sent_files)+L" files / "+std::to_wstring(item->sent_bytes)+L" bytes; Received: "+std::to_wstring(item->received_files)+L" files / "+std::to_wstring(item->received_bytes)+L" bytes";
        }text_if_changed(details,info);
    }
    void paint_transfer(HWND h,HDC dc){
        RECT r{};GetClientRect(h,&r);const auto& p=g_appearance.palette();const auto brush=CreateSolidBrush(p.panel);FillRect(dc,&r,brush);DeleteObject(brush);
        const auto old=SelectObject(dc,current_font?current_font:GetStockObject(SYSTEM_FIXED_FONT));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,p.text);TEXTMETRICW tm{};GetTextMetricsW(dc,&tm);
        const int pad=MulDiv(8,static_cast<int>(window_dpi(window)),96),line=tm.tmHeight+2;std::wstring value(GetWindowTextLengthW(h)+1,L'\0');GetWindowTextW(h,value.data(),static_cast<int>(value.size()));value.resize(wcslen(value.c_str()));
        const auto newline=value.find(L'\n');RECT label{pad,pad,r.right-pad,pad+line};DrawTextW(dc,value.c_str(),static_cast<int>(newline==value.npos?value.size():newline),&label,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        if(newline!=value.npos){label.top+=line;label.bottom+=line;DrawTextW(dc,value.c_str()+newline+1,-1,&label,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);}
        RECT bar{pad,pad+line*2+2,r.right-pad,pad+line*3+2};const auto track=CreateSolidBrush(p.surface);FillRect(dc,&bar,track);DeleteObject(track);
        const auto* item=selected_session();const auto* file=item?(h==send?&item->send:&item->receive):nullptr;
        if(item&&item->active&&file&&(file->active||file->awaiting_ack)&&!file->name.empty()){RECT fill=bar;const double fraction=file->size?std::min(1.0,static_cast<double>(file->position)/file->size):(file->active?0.0:1.0);fill.right=fill.left+static_cast<int>((fill.right-fill.left)*fraction);const auto gold=CreateSolidBrush(p.gold);FillRect(dc,&fill,gold);DeleteObject(gold);}
        const bool recent=item&&item->serial==(h==send?send_pulse_session:receive_pulse_session)&&GetTickCount64()<(h==send?send_pulse_until:receive_pulse_until);
        const auto edge=CreateSolidBrush(recent?p.gold:p.cyan);FrameRect(dc,&bar,edge);DeleteObject(edge);SetTextColor(dc,p.cyan);label={bar.left,bar.bottom+1,bar.right,r.bottom};DrawTextW(dc,L"0",-1,&label,DT_LEFT|DT_SINGLELINE);DrawTextW(dc,L"50",-1,&label,DT_CENTER|DT_SINGLELINE);DrawTextW(dc,L"100%",-1,&label,DT_RIGHT|DT_SINGLELINE);SelectObject(dc,old);
    }
    static LRESULT CALLBACK transfer_proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){auto& self=*reinterpret_cast<BinkPConsole*>(data);if(m==WM_TIMER&&w==15){KillTimer(h,15);InvalidateRect(h,nullptr,FALSE);return 0;}if(m==WM_PAINT){PAINTSTRUCT ps{};const auto dc=BeginPaint(h,&ps);self.paint_transfer(h,dc);EndPaint(h,&ps);return 0;}if(m==WM_PRINTCLIENT){self.paint_transfer(h,reinterpret_cast<HDC>(w));return 0;}if(m==WM_ERASEBKGND)return 1;if(m==WM_NCDESTROY){KillTimer(h,15);RemoveWindowSubclass(h,transfer_proc,id);}return DefSubclassProc(h,m,w,l);}
    void paint_session(HDC dc,RECT r,int index,bool highlighted){const auto& p=g_appearance.palette();const auto brush=CreateSolidBrush(highlighted?p.selection:p.panel);FillRect(dc,&r,brush);DeleteObject(brush);SetTextColor(dc,highlighted?p.selected_text:p.text);SetBkMode(dc,TRANSPARENT);const auto old=SelectObject(dc,current_font?current_font:GetStockObject(SYSTEM_FIXED_FONT));
        const auto length=SendMessageW(session,CB_GETLBTEXTLEN,index,0);if(length>=0){std::wstring label(static_cast<std::size_t>(length)+1,L'\0');SendMessageW(session,CB_GETLBTEXT,index,reinterpret_cast<LPARAM>(label.data()));r.left+=6;r.right-=6;DrawTextW(dc,label.c_str(),-1,&r,DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);}SelectObject(dc,old);
    }
    void paint_selector(HDC dc){RECT r{};GetClientRect(session,&r);const auto& p=g_appearance.palette();const auto border=CreateSolidBrush(p.border);FillRect(dc,&r,border);DeleteObject(border);InflateRect(&r,-1,-1);const auto full=r;const int arrow=MulDiv(24,static_cast<int>(window_dpi(session)),96);r.right-=arrow;paint_session(dc,r,static_cast<int>(SendMessageW(session,CB_GETCURSEL,0,0)),GetFocus()==session);RECT button{r.right,full.top,full.right,full.bottom};const auto surface=CreateSolidBrush(p.panel);FillRect(dc,&button,surface);DeleteObject(surface);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,p.gold);DrawTextW(dc,L"\x25be",1,&button,DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
    static LRESULT CALLBACK session_proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){auto& self=*reinterpret_cast<BinkPConsole*>(data);
        if(m==WM_PAINT){PAINTSTRUCT ps{};auto dc=BeginPaint(h,&ps);self.paint_selector(dc);EndPaint(h,&ps);return 0;}
        if(m==WM_PRINTCLIENT){self.paint_selector(reinterpret_cast<HDC>(w));return 0;}
        if(m==WM_ERASEBKGND)return 1;if(m==WM_NCDESTROY)RemoveWindowSubclass(h,session_proc,id);return DefSubclassProc(h,m,w,l);
    }
    void scroll(int position){SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE};GetScrollInfo(window,SB_VERT,&info);scroll_y=std::clamp(position,0,std::max(0,info.nMax-static_cast<int>(info.nPage)+1));layout();}
    static LRESULT CALLBACK focus_proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){auto& self=*reinterpret_cast<BinkPConsole*>(data);
        if(m==WM_SETFOCUS){const auto result=DefSubclassProc(h,m,w,l);for(int pass=0;pass<3;++pass){RECT r{},page{};GetWindowRect(h,&r);MapWindowPoints(HWND_DESKTOP,self.window,reinterpret_cast<POINT*>(&r),2);GetClientRect(self.window,&page);if(r.top<0)self.scroll(self.scroll_y+r.top);else if(r.bottom>page.bottom)self.scroll(self.scroll_y+r.bottom-page.bottom);else break;}return result;}
        if(m==WM_NCDESTROY)RemoveWindowSubclass(h,focus_proc,id);return DefSubclassProc(h,m,w,l);
    }
    enum : UINT {Sessions=27000,Poll=27001,Scan=27002,Queues=27003,Configure=27004,Activity=27005};
    void layout(){if(laying_out)return;laying_out=true;
        RECT r{};GetClientRect(window,&r);const auto scale=[&](int x){return MulDiv(x,static_cast<int>(window_dpi(window)),96);};const auto dc=GetDC(window);const auto old=SelectObject(dc,current_font?current_font:GetStockObject(SYSTEM_FIXED_FONT));TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);SIZE label{};GetTextExtentPoint32W(dc,L"Queues / Held Mail",18,&label);SelectObject(dc,old);ReleaseDC(window,dc);
        const auto gap=scale(12),line=std::max(scale(26),static_cast<int>(metrics.tmHeight)+scale(8));const auto frame_height=static_cast<int>(metrics.tmHeight+2)*4+scale(18);
        const auto top=gap*3+line*3,detail_top=top+frame_height*2+gap,log_top=detail_top+line*2+gap;
        // A short window scrolls as one page. Reserve scrollbar width for layout
        // decisions so showing it cannot oscillate the action-column count.
        RECT outer{};GetWindowRect(window,&outer);const auto usable=std::max(scale(200),static_cast<int>(outer.right-outer.left)-GetSystemMetrics(SM_CXVSCROLL));const auto columns=(usable-gap*5)/4>=label.cx+scale(24)?4:2;const auto rows=4/columns;
        const auto bottom=std::max(log_top+line*3+gap,static_cast<int>(r.bottom)-rows*line-rows*gap);const auto height=bottom+rows*(line+gap);
        SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS,0,height-1,static_cast<UINT>(std::max(0L,r.bottom)),0,0};scroll_y=std::clamp(scroll_y,0,std::max(0,height-static_cast<int>(r.bottom)));si.nPos=scroll_y;SetScrollInfo(window,SB_VERT,&si,TRUE);GetClientRect(window,&r);const auto width=std::max(scale(200),static_cast<int>(r.right));
        const auto move=[&](HWND h,int x,int y,int w,int hgt){MoveWindow(h,x,y-scroll_y,w,hgt,TRUE);};
        move(status,gap,gap,width-gap*2,line*2);move(session,gap,gap*2+line*2,width-gap*3-scale(160),line*8);SendMessageW(session,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),line-scale(4));SendMessageW(session,CB_SETITEMHEIGHT,0,line-scale(4));move(activity_button,width-gap-scale(160),gap*2+line*2,scale(160),line);
        move(receive,gap,top,width-gap*2,frame_height);move(send,gap,top+frame_height+gap/2,width-gap*2,frame_height);move(details,gap,detail_top,width-gap*2,line*2);
        move(log,gap,log_top,width-gap*2,bottom-log_top-gap);const auto button=(width-gap*(columns+1))/columns;const std::array<HWND,4> actions{{poll,scan,queues,configure}};for(std::size_t i=0;i<actions.size();++i)move(actions[i],gap+(static_cast<int>(i)%columns)*(button+gap),bottom+(static_cast<int>(i)/columns)*(line+gap),button,line);
        framed_columns=0;pending_frame();laying_out=false;
    }
    void font(HFONT value){current_font=value;framed_columns=0;for(auto h:{session,status,send,receive,details,log,poll,scan,queues,activity_button,configure})SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(value),TRUE);layout();}
    static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){auto& self=*reinterpret_cast<BinkPConsole*>(data);LRESULT result{};if(m==WM_VSCROLL){SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(h,SB_VERT,&si);int next=self.scroll_y;switch(LOWORD(w)){case SB_TOP:next=0;break;case SB_BOTTOM:next=si.nMax;break;case SB_LINEUP:next-=26;break;case SB_LINEDOWN:next+=26;break;case SB_PAGEUP:next-=si.nPage;break;case SB_PAGEDOWN:next+=si.nPage;break;case SB_THUMBTRACK:next=si.nTrackPos;break;}self.scroll(next);return 0;}if(m==WM_MOUSEWHEEL){self.scroll(self.scroll_y-GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA*78);return 0;}if(m==WM_DRAWITEM&&w==Sessions){auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(l);self.paint_session(item->hDC,item->rcItem,static_cast<int>(item->itemID),(item->itemState&ODS_SELECTED)!=0);return TRUE;}if(m==WM_MEASUREITEM&&w==Sessions){reinterpret_cast<MEASUREITEMSTRUCT*>(l)->itemHeight=MulDiv(26,static_cast<int>(window_dpi(h)),96);return TRUE;}if(m==WM_CTLCOLORLISTBOX){const auto& p=g_appearance.palette();SetTextColor(reinterpret_cast<HDC>(w),p.text);SetBkColor(reinterpret_cast<HDC>(w),p.panel);return reinterpret_cast<LRESULT>(g_appearance.panel_brush());}if(g_appearance.control_message(h,m,w,l,result))return result;if(m==WM_SIZE){self.layout();return 0;}if(m==WM_COMMAND)return SendMessageW(GetParent(h),m,w,l);if(m==WM_ERASEBKGND){RECT r{};GetClientRect(h,&r);FillRect(reinterpret_cast<HDC>(w),&r,g_appearance.background_brush());return 1;}if(m==WM_NCDESTROY)RemoveWindowSubclass(h,proc,id);return DefSubclassProc(h,m,w,l);}
    bool create(HWND parent,HINSTANCE instance){window=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS|WS_VSCROLL,0,0,10,10,parent,nullptr,instance,nullptr);if(!window||!SetWindowSubclass(window,proc,27000,reinterpret_cast<DWORD_PTR>(this)))return false;
        const auto add=[&](const wchar_t* cls,const wchar_t* text,DWORD style,UINT id){return CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,10,10,window,reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)),instance,nullptr);};
        status=add(L"STATIC",L"FTN Runtime status unavailable.\r\nWaiting for the protected Runtime connection.",SS_LEFT|SS_NOPREFIX,0);session=add(WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,27000);SendMessageW(session,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"No session snapshot available"));SendMessageW(session,CB_SETCURSEL,0,0);EnableWindow(session,FALSE);
        SetWindowSubclass(session,session_proc,27001,reinterpret_cast<DWORD_PTR>(this));
        const auto edit=WS_TABSTOP|ES_READONLY|ES_MULTILINE|ES_AUTOHSCROLL;send=add(L"EDIT",L"",edit,0);receive=add(L"EDIT",L"",edit,0);SetWindowSubclass(send,transfer_proc,27002,reinterpret_cast<DWORD_PTR>(this));SetWindowSubclass(receive,transfer_proc,27003,reinterpret_cast<DWORD_PTR>(this));details=add(L"STATIC",L"Waiting for a BinkP connection.",SS_LEFT|SS_NOPREFIX,0);log=add(L"EDIT",L"SESSION LOG\r\n-----------\r\nNo Runtime session observations are available.",edit|WS_VSCROLL,0);poll=add(L"BUTTON",L"Poll Now",WS_TABSTOP|BS_PUSHBUTTON,Poll);scan=add(L"BUTTON",L"Scan Outbound",WS_TABSTOP|BS_PUSHBUTTON,Scan);queues=add(L"BUTTON",L"Queues / Held Mail",WS_TABSTOP|BS_PUSHBUTTON,Queues);activity_button=add(L"BUTTON",L"Activity Log",WS_TABSTOP|BS_PUSHBUTTON,Activity);configure=add(L"BUTTON",L"FTN Settings...",WS_TABSTOP|BS_PUSHBUTTON,Configure);for(auto h:{session,send,receive,log,poll,scan,queues,activity_button,configure})SetWindowSubclass(h,focus_proc,27004,reinterpret_cast<DWORD_PTR>(this));for(auto h:{poll,scan,queues})EnableWindow(h,FALSE);return status&&session&&send&&receive&&details&&log&&poll&&scan&&queues&&activity_button&&configure;
    }
};
// Preserve existing primary-page identities while inserting BinkP after Nodes.
// Native node-tab hit tests still use physical indices (six fixed tabs).
int panel_page(HWND tab){const auto physical=TabCtrl_GetCurSel(tab);return physical==1?-14:physical>1?physical-1:physical;}
void select_panel_page(HWND tab,int page){TabCtrl_SetCurSel(tab,page==-14?1:page>0?page+1:page);}
