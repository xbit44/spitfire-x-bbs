// Shared mouse gesture for the overview, persistent navigation and category list.
// Mouse movement changes feedback only; one protected move is sent on release.
std::optional<DoorEditorState::TreeItem> door_surface_item(State& s,HWND window,HTREEITEM handle=nullptr,int row=-1){
    if(window==s.navigation)return door_navigation_item(s,handle?handle:TreeView_GetSelection(window));
    if(window==s.doors.categories)return door_tree_item(s,handle);
    if(window==s.doors.entries&&s.doors.catalog){
        if(row<0)row=static_cast<int>(SendMessageW(window,LB_GETCURSEL,0,0));
        if(row<0)return {};const auto number=SendMessageW(window,LB_GETITEMDATA,row,0);
        for(const auto& e:s.doors.catalog->entries)if(e.menu==s.doors.menu&&e.number==number)return DoorEditorState::TreeItem{nullptr,e};
    }return {};
}
void door_drag_clear_marker(State& s){
    auto& d=s.doors;if(d.drag_surface==d.entries)DrawInsert(GetParent(d.entries),d.entries,-1);
    else if(d.drag_surface){TreeView_SetInsertMark(d.drag_surface,nullptr,FALSE);TreeView_SelectDropTarget(d.drag_surface,nullptr);}
    if(d.drag_marker)ShowWindow(d.drag_marker,SW_HIDE);
}
void door_drag_end(State& s){
    auto& d=s.doors;d.dragging=false;d.drag_list_index=-1;
    if(d.drag_source)KillTimer(d.drag_source,86);door_drag_clear_marker(s);
    if(d.drag_preview){DestroyWindow(d.drag_preview);d.drag_preview=nullptr;}
    if(d.drag_marker){DestroyWindow(d.drag_marker);d.drag_marker=nullptr;}
    if(GetCapture()==d.drag_source)ReleaseCapture();d.drag_source=d.drag_surface=nullptr;d.drag_item.reset();d.drag_drop.reset();d.drag_target=nullptr;
}
// Keep the drag image in the compositor. Moving an ordinary opaque STATIC
// repeatedly erases the controls beneath it and repaints its blue background.
HWND door_drag_image(State& s,const std::wstring& caption,int width,int height,bool marker){
    const auto window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,L"STATIC",caption.c_str(),WS_POPUP,0,0,width,height,s.window,nullptr,s.instance,nullptr);
    if(!window)return nullptr;
    const auto screen=GetDC(nullptr);const auto dc=screen?CreateCompatibleDC(screen):nullptr;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* pixels{};const auto bitmap=dc?CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;bool ready=false;
    if(bitmap){const auto old=SelectObject(dc,bitmap);RECT area{0,0,width,height};const auto& palette=s.appearance.palette();const auto brush=CreateSolidBrush(marker?RGB(255,255,255):palette.selection);
        if(brush){FillRect(dc,&area,brush);DeleteObject(brush);if(!marker){SetBkMode(dc,TRANSPARENT);SetTextColor(dc,palette.selected_text);const auto font=SelectObject(dc,s.ui_font);area.left+=MulDiv(8,s.dpi,96);area.right-=MulDiv(8,s.dpi,96);DrawTextW(dc,caption.c_str(),-1,&area,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);SelectObject(dc,font);}
            GdiFlush();auto* colour=static_cast<DWORD*>(pixels);for(int i=0;i<width*height;++i)colour[i]|=0xff000000;
            POINT origin{};SIZE size{width,height};BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};ready=UpdateLayeredWindow(window,screen,nullptr,&size,dc,&origin,0,&blend,ULW_ALPHA)!=FALSE;}
        SelectObject(dc,old);DeleteObject(bitmap);}
    if(dc)DeleteDC(dc);if(screen)ReleaseDC(nullptr,screen);if(!ready){DestroyWindow(window);return nullptr;}return window;
}
void door_drag_list_marker(State& s,const RECT& row,int position){
    auto& d=s.doors;RECT client{};GetClientRect(d.entries,&client);const int thickness=std::max(2,MulDiv(2,s.dpi,96)),width=std::max(1,static_cast<int>(client.right)-8);
    if(!d.drag_marker)d.drag_marker=door_drag_image(s,L"",width,thickness,true);
    if(d.drag_marker){POINT point{4,position>0?row.bottom:row.top};ClientToScreen(d.entries,&point);RECT old{};GetWindowRect(d.drag_marker,&old);
        if(old.left!=point.x||old.top!=point.y||!IsWindowVisible(d.drag_marker))SetWindowPos(d.drag_marker,nullptr,point.x,point.y,0,0,SWP_NOZORDER|SWP_NOSIZE|SWP_NOACTIVATE|(IsWindowVisible(s.window)?SWP_SHOWWINDOW:0));}
}
void door_drag_begin(State& s,HWND source,const DoorEditorState::TreeItem& item){
    auto& d=s.doors;if(d.pending||!d.catalog||item.group||(item.entry.kind==app::DoorEntryKind::submenu&&!item.entry.target)||!resolve_dirty(s))return;
    if(!item.unplaced&&std::none_of(d.catalog->entries.begin(),d.catalog->entries.end(),[&](const auto& e){return e.menu==item.entry.menu&&e.number==item.entry.number&&e.kind==item.entry.kind&&e.target==item.entry.target;})){set_status(s,L"The menu changed. Select the item again before dragging.");return;}
    door_drag_end(s);d.drag_source=source;d.drag_item=item;d.drag_revision=d.catalog->revision;d.dragging=true;d.drag_position=1;d.drag_feedback_updates=0;d.drag_scroll_at=0;d.drag_hover_at=GetTickCount64();
    const auto caption=L"Move: "+utf8_to_wide(item.entry.title);
    d.drag_preview=door_drag_image(s,caption,MulDiv(560,s.dpi,96),MulDiv(30,s.dpi,96),false);
    SetFocus(source);if(GetCapture()!=source)SetCapture(source);SetTimer(source,86,80,nullptr);
}
void door_drag_target_at(State&,HWND,POINT);
void door_drag_move(State& s,POINT screen){
    auto& d=s.doors;if(!d.dragging)return;
    if(d.drag_preview){const int pad=MulDiv(18,s.dpi,96);RECT size{};GetWindowRect(d.drag_preview,&size);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(screen,MONITOR_DEFAULTTONEAREST),&monitor);
        const int x=std::max(monitor.rcWork.left,std::min(screen.x+pad,monitor.rcWork.right-(size.right-size.left))),y=screen.y+pad+(screen.y+pad+size.bottom-size.top>monitor.rcWork.bottom?-(size.bottom-size.top)-2*pad:0);
        if(size.left!=x||size.top!=y||!IsWindowVisible(d.drag_preview))SetWindowPos(d.drag_preview,nullptr,x,y,0,0,SWP_NOZORDER|SWP_NOSIZE|SWP_NOACTIVATE|(IsWindowVisible(s.window)?SWP_SHOWWINDOW:0));}
    const auto surface=WindowFromPoint(screen);POINT point=screen;ScreenToClient(surface,&point);door_drag_target_at(s,surface,point);
}
void door_drag_target_at(State& s,HWND surface,POINT point){
    auto& d=s.doors;if(!d.dragging)return;
    std::optional<DoorEditorState::TreeItem> target;RECT bounds{};int row=-1;
    if(surface==d.categories||surface==s.navigation){TVHITTESTINFO hit{};hit.pt=point;const auto handle=TreeView_HitTest(surface,&hit);if(handle)target=door_surface_item(s,surface,handle);if(target)TreeView_GetItemRect(surface,handle,&bounds,TRUE);}
    else if(surface==d.entries){const auto hit=SendMessageW(surface,LB_ITEMFROMPOINT,0,MAKELPARAM(point.x,point.y));if(!HIWORD(hit)){row=LOWORD(hit);target=door_surface_item(s,surface,nullptr,row);if(target)SendMessageW(surface,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&bounds));}}
    int position=1;
    if(target){const auto third=(bounds.bottom-bounds.top)/3;position=point.y<bounds.top+third?-1:point.y>=bounds.bottom-third?1:target->entry.kind==app::DoorEntryKind::submenu?0:1;
        if(target->group||(!target->entry.number&&position!=0))target.reset();
        if(target&&d.drag_item&&d.drag_item->entry.kind==app::DoorEntryKind::submenu){
            const auto destination=position==0?target->entry.target:target->entry.menu;
            bool cycle=destination==d.drag_item->entry.target;
            for(const auto& e:d.catalog->entries)if(e.kind==app::DoorEntryKind::submenu&&e.menu==d.drag_item->entry.target&&e.target==destination)cycle=true;
            if(cycle)target.reset();
        }
    }
    const auto feedback=surface==d.entries||surface==d.categories||surface==s.navigation?surface:nullptr;
    const bool changed=feedback!=d.drag_surface||position!=d.drag_position||bool(target)!=bool(d.drag_drop)||(target&&d.drag_drop&&(target->handle!=d.drag_drop->handle||target->entry.menu!=d.drag_drop->entry.menu||target->entry.number!=d.drag_drop->entry.number));
    if(changed){door_drag_clear_marker(s);d.drag_surface=feedback;d.drag_drop=target;d.drag_position=position;d.drag_hover_at=GetTickCount64();++d.drag_feedback_updates;
        if(target){if(surface==d.entries){if(s.appearance.theme()==win32::DesktopTheme::classic)DrawInsert(GetParent(surface),surface,row+(position>0?1:0));}else {TreeView_SetInsertMarkColor(surface,s.appearance.theme()==win32::DesktopTheme::dark_retro?RGB(255,255,255):RGB(0,0,0));TreeView_SetInsertMark(surface,position?target->handle:nullptr,position>0);TreeView_SelectDropTarget(surface,position?nullptr:target->handle);}
            const auto message=(position==0?L"Move into ":position<0?L"Move before ":L"Move after ")+utf8_to_wide(target->entry.title)+L". Release to move; Escape cancels.";set_status(s,message.c_str());}
        else set_status(s,L"Choose a valid Door or category drop position. Escape cancels.");
    }
    if(target&&surface==d.entries&&s.appearance.theme()==win32::DesktopTheme::dark_retro)door_drag_list_marker(s,bounds,position);
    SetCursor(LoadCursorW(nullptr,target?IDC_SIZEALL:IDC_NO));
}
std::optional<LRESULT> door_drag_message(State& s,HWND window,UINT message,WPARAM w,LPARAM l){
    auto& d=s.doors;
    if(message==WM_NCDESTROY){if(d.drag_source==window)door_drag_end(s);return {};}
    if(d.dragging&&window==d.drag_source){
        if(message==WM_CAPTURECHANGED&&reinterpret_cast<HWND>(l)==window)return 0;
        if((message==WM_KEYDOWN&&w==VK_ESCAPE)||message==WM_CANCELMODE||message==WM_CAPTURECHANGED){door_drag_end(s);return 0;}
        if(message==WM_MOUSEMOVE){POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(window,&p);door_drag_move(s,p);return 0;}
        if(message==WM_TIMER&&w==86){POINT screen{};GetCursorPos(&screen);const auto surface=WindowFromPoint(screen);POINT p=screen;ScreenToClient(surface,&p);RECT r{};GetClientRect(surface,&r);const auto now=GetTickCount64();
            if((surface==d.categories||surface==s.navigation||surface==d.entries)&&now-d.drag_scroll_at>=150){const int edge=MulDiv(18,s.dpi,96);if(p.y<edge||p.y>r.bottom-edge){SendMessageW(surface,WM_VSCROLL,p.y<edge?SB_LINEUP:SB_LINEDOWN,0);d.drag_scroll_at=now;}}
            if(d.drag_drop&&d.drag_surface!=d.entries&&d.drag_position==0&&now-d.drag_hover_at>600){TreeView_Expand(d.drag_surface,d.drag_drop->handle,TVE_EXPAND);d.drag_hover_at=now;}
            door_drag_move(s,screen);return 0;
        }
        if(message==WM_LBUTTONUP){
            const auto source=d.drag_item,target=d.drag_drop;const auto revision=d.drag_revision;const auto position=d.drag_position;const auto back=d.drag_source==d.categories?DoorPage::categories:DoorPage::entries;
            door_drag_end(s);
            if(source&&target&&!d.pending){const auto clipboard=d.clipboard;const auto cut=d.clipboard_cut;const auto old_revision=d.clipboard_revision;d.clipboard=source;d.clipboard_cut=true;d.clipboard_revision=revision;door_tree_paste(s,*target,position,back);d.clipboard=clipboard;d.clipboard_cut=cut;d.clipboard_revision=old_revision;}
            return 0;
        }
    }
    if(window==d.entries){
        if(message==WM_LBUTTONDOWN){
            const auto hit=SendMessageW(window,LB_ITEMFROMPOINT,0,l);d.drag_list_index=HIWORD(hit)?-1:LOWORD(hit);d.drag_anchor={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            RECT row{};if(d.drag_list_index<0||SendMessageW(window,LB_GETITEMRECT,d.drag_list_index,reinterpret_cast<LPARAM>(&row))==LB_ERR||!PtInRect(&row,d.drag_anchor)){d.drag_list_index=-1;return 0;}
            // The native list box otherwise runs its own modal tracking loop,
            // swallowing mouse movement before our subclass can start a drag.
            SetFocus(window);SendMessageW(window,LB_SETCURSEL,d.drag_list_index,0);if(GetCapture()!=window)SetCapture(window);
            SendMessageW(GetParent(window),WM_COMMAND,MAKEWPARAM(IDC_DOOR_ENTRIES,LBN_SELCHANGE),reinterpret_cast<LPARAM>(window));return 0;
        }
        if(message==WM_LBUTTONDBLCLK){d.drag_list_index=-1;if(GetCapture()==window)ReleaseCapture();door_configuration_command(s,IDC_DOOR_ENTRIES,LBN_DBLCLK);return 0;}
        if(message==WM_MOUSEMOVE&&(w&MK_LBUTTON)&&d.drag_list_index>=0&&!d.dragging&&(std::abs(GET_X_LPARAM(l)-d.drag_anchor.x)>=GetSystemMetrics(SM_CXDRAG)||std::abs(GET_Y_LPARAM(l)-d.drag_anchor.y)>=GetSystemMetrics(SM_CYDRAG))){if(const auto item=door_surface_item(s,window,nullptr,d.drag_list_index)){door_drag_begin(s,window,*item);POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(window,&p);door_drag_move(s,p);return 0;}}
        if(message==WM_LBUTTONUP){d.drag_list_index=-1;if(GetCapture()==window)ReleaseCapture();return 0;}
        if(message==WM_CAPTURECHANGED&&reinterpret_cast<HWND>(l)!=window)d.drag_list_index=-1;
        if(message==WM_CANCELMODE){d.drag_list_index=-1;if(GetCapture()==window)ReleaseCapture();return 0;}
    }
    return {};
}
LRESULT CALLBACK door_surface_keys(HWND window,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
    auto& s=*reinterpret_cast<State*>(data);if(const auto result=door_drag_message(s,window,message,w,l))return *result;
    if(message==WM_NCDESTROY)RemoveWindowSubclass(window,door_surface_keys,id);
    if((message==WM_GETDLGCODE||message==WM_KEYDOWN)&&w==VK_DELETE&&s.current_page==26&&
        (s.doors.view==DoorPage::categories||s.doors.view==DoorPage::entries)){
      const auto item=window==s.navigation?door_navigation_item(s,TreeView_GetSelection(window)):door_action_item(s);
      if(item&&item->entry.kind==app::DoorEntryKind::submenu){
        if(message==WM_GETDLGCODE&&w==VK_DELETE)return DefSubclassProc(window,message,w,l)|DLGC_WANTMESSAGE;
        if(message==WM_KEYDOWN&&w==VK_DELETE){if(!s.doors.pending&&!s.doors.conflict)door_request_delete_category(s,item->entry.target,s.doors.view);return 0;}
      }
    }
    return DefSubclassProc(window,message,w,l);
}
