// Included in the configuration window's private implementation namespace.
// Preview uses the shipping selector and menu frame, but supplies no caller,
// transport, write-to-disk or action authority. Bell/pause operations are inert.
COLORREF menu_vga_color(unsigned value) {
    constexpr std::array<COLORREF,16> colors{{RGB(0,0,0),RGB(0,0,170),RGB(0,170,0),RGB(0,170,170),
        RGB(170,0,0),RGB(170,0,170),RGB(170,85,0),RGB(170,170,170),RGB(85,85,85),RGB(85,85,255),
        RGB(85,255,85),RGB(85,255,255),RGB(255,85,85),RGB(255,85,255),RGB(255,255,85),RGB(255,255,255)}};
    return colors[value&15U];
}
LRESULT CALLBACK menu_preview_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_PAINT && state) {
        PAINTSTRUCT paint{};const auto dc=BeginPaint(window,&paint);
        RECT bounds{};GetClientRect(window,&bounds);FillRect(dc,&bounds,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const int width=std::max(1,static_cast<int>(bounds.right)/80),height=std::max(1,static_cast<int>(bounds.bottom)/28);
        const auto font=CreateFontW(height,width,0,0,FW_NORMAL,FALSE,FALSE,FALSE,OEM_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,NONANTIALIASED_QUALITY,FIXED_PITCH|FF_MODERN,L"Consolas");
        const auto old=SelectObject(dc,font);
        const auto draft=candidate_for_page(*state,17);
        const auto colors=draft?draft->menus:state->configuration.saved.menus;
        const std::array<std::pair<const wchar_t*,unsigned>,3> samples{{
            {L"Default text preview",colors.text_color},{L"Caller input preview",colors.input_color},
            {L"1. Selected light-bar item (direct number entry also works)",colors.selected_foreground}}};
        for(int row=0;row<3;++row) {
            RECT area{0,row*height,bounds.right,(row+1)*height};
            SetTextColor(dc,menu_vga_color(samples[row].second));
            SetBkColor(dc,menu_vga_color(row==2?colors.selected_background:0));
            ExtTextOutW(dc,0,area.top,ETO_OPAQUE|ETO_CLIPPED,&area,samples[row].first,
                static_cast<UINT>(wcslen(samples[row].first)),nullptr);
        }
        for(int row=0;row<25;++row) for(int column=0;column<80;++column) {
            const auto& cell=state->menu_screen.cells[static_cast<std::size_t>(row)*80+column];
            const char byte=static_cast<char>(cell.character);wchar_t character=L' ';
            MultiByteToWideChar(437,0,&byte,1,&character,1);
            SetTextColor(dc,menu_vga_color(cell.attribute));SetBkColor(dc,menu_vga_color(cell.attribute>>4));
            RECT area{column*width,(row+3)*height,(column+1)*width,(row+4)*height};
            ExtTextOutW(dc,area.left,area.top,ETO_OPAQUE|ETO_CLIPPED,&area,&character,1,nullptr);
        }
        SelectObject(dc,old);DeleteObject(font);EndPaint(window,&paint);return 0;
    }
    if(message==WM_ERASEBKGND) return 1;
    return DefWindowProcW(window,message,wparam,lparam);
}
void refresh_menu_preview(State& state) {
    if(!state.menu_preview) return;
    const auto selection=SendMessageW(state.menu_select,CB_GETCURSEL,0,0);
    const auto menu_id=SendMessageW(state.menu_select,CB_GETITEMDATA,static_cast<WPARAM>(selection),0);
    const auto level=port_value(state.menu_level);
    const auto draft=candidate_for_page(state,17);
    if(selection<0 || selection>11 || menu_id<0 || menu_id>static_cast<LRESULT>(app::MenuId::qwk) || menu_id==3 || menu_id==4 || !level || *level>999 || !draft) {
        SetWindowTextW(state.menu_detail,L"Choose a security level from 1 to 999 and distinguishable colors.");return;
    }
    app::MenuSessionSnapshot sample;
    sample.access={*level,Button_GetCheck(state.menu_sysop)==BST_CHECKED,1};
    sample.configuration_revision=1;sample.preferences={1,{},1,1};
    sample.graphics=SendMessageW(state.menu_format,CB_GETCURSEL,0,0)==0?app::GraphicsMode::ansi:app::GraphicsMode::ascii;
    sample.default_color=draft->menus.text_color;sample.input_color=draft->menus.input_color;
    sample.lightbars_enabled=draft->menus.lightbars_enabled;
    sample.selected_foreground=draft->menus.selected_foreground;sample.selected_background=draft->menus.selected_background;
    sample.compact_footer=draft->menus.compact_footer;
    auto& context=sample.context;
    context.bbs_name=draft->bbs_name;context.sysop="Preview Sysop";context.version=SFX_PRODUCT_DISPLAY_VERSION;
    context.date="2026-09-27";context.time="12:34:56";context.datetime="2026-09-27 12:34:56";
    context.node="1";context.nodes=std::to_string(draft->logical_nodes);context.transport="Preview";
    context.columns="80";context.rows="25";context.name="Preview Caller";context.alias="Preview Caller";
    context.first_name="Preview";context.security_level=std::to_string(*level);context.time_on="60";context.time_left="Unlimited";
    app::ControlPanelTerminalMirror mirror;
    std::wstring detail=L"Built-in menu. Sample values only; no caller actions. ";
    auto reader=win32::Win32TrustedDisplayAssetReader::open((state.root/L"Display").wstring());
    app::MenuSessionPorts ports;
    ports.snapshot=[&]{return std::optional<app::MenuSessionSnapshot>{sample};};
    ports.artwork=[&](const app::TrustedDisplaySelectionRequest& request) {
        if(!reader || Button_GetCheck(state.menu_artwork)!=BST_CHECKED)
            return app::TrustedDisplaySelectionResult{app::TrustedDisplaySelectionCode::not_found,{}};
        app::TrustedDisplaySelector selector(*reader);auto selected=selector.select(request);
        if(selected.available()) detail=L"Selected: "+utf8_to_wide(selected.program->selected_logical_asset)+
            L". Sample values only; bell/pause disabled. ";
        else detail=L"No usable matching artwork; built-in fallback. ";
        return selected;
    };
    ports.write=[&](std::string_view bytes){mirror.consume(bytes);return true;};
    ports.display=[&](const app::TerminalDisplayProgram& program) {
        for(const auto& op:program.operations) switch(op.kind) {
            case app::TerminalDisplayOperationKind::cp437_text:
            case app::TerminalDisplayOperationKind::ansi_sequence: mirror.consume(op.bytes);break;
            case app::TerminalDisplayOperationKind::logical_newline:
            case app::TerminalDisplayOperationKind::paging_newline: mirror.consume("\r\n");break;
            case app::TerminalDisplayOperationKind::clear_screen: mirror.clear();break;
            default: break;
        }
        return true;
    };
    ports.read=[](app::MenuInputDecoder&,const std::function<bool()>&)->std::optional<app::MenuInput>{return {};};
    ports.save_preferences=[](const app::UpdateMenuPreferenceRequest&){return false;};
    ports.action=[](app::MenuAction)->std::optional<std::string>{return {};};
    ports.utc_now=[] {return std::int64_t{0};};
    (void)app::run_menu_session(ports,static_cast<app::MenuId>(menu_id));
    state.menu_screen=mirror.snapshot();
    detail+=L"Final 80x25 viewport; long displays scroll. Folder: "+(state.root/L"Display").wstring();
    SetWindowTextW(state.menu_detail,detail.c_str());InvalidateRect(state.menu_preview,nullptr,FALSE);
}
void create_display_menu_page(State& state) {
    const auto parent=state.pages[17]=page(state);
    label(state,parent,L"Board colors apply at the next menu redraw; new logins use them too. Authored ANSI and plain ASCII are unchanged.",12,8,560,38);
    const auto color_box=[&](const wchar_t* name,int y,int id,bool black,bool background) {
        label(state,parent,name,12,y,195,22);
        const auto box=control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,210,y-3,245,260,id);
        for(unsigned i=black?0U:1U;i<(background?8U:16U);++i) {
            const auto title=i==0?std::wstring(L"Black"):utf8_to_wide(app::menu_colors()[i].name);
            SendMessageW(box,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(title.c_str()));
        }
        return box;
    };
    state.menu_text=color_box(L"Default ANSI text:",52,IDC_MENU_TEXT,false,false);
    state.menu_input=color_box(L"Caller input:",84,IDC_MENU_INPUT,false,false);
    state.menu_foreground=color_box(L"Selected light-bar text:",116,IDC_MENU_FOREGROUND,true,false);
    state.menu_background=color_box(L"Selected light-bar background:",148,IDC_MENU_BACKGROUND,true,true);
    label(state,parent,L"Menu Footer:",12,180,195,22);
    state.menu_footer=control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,210,177,245,100,IDC_MENU_FOOTER);
    SendMessageW(state.menu_footer,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"SF Original"));
    SendMessageW(state.menu_footer,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"SFX New"));
    SendMessageW(state.menu_footer,CB_SETCURSEL,0,0);
    state.menu_bars=control(state,parent,L"BUTTON",L"Enable light bars (callers may turn them off)",WS_TABSTOP|BS_AUTOCHECKBOX,12,216,400,25,IDC_MENU_BARS);
    state.menu_save=control(state,parent,L"BUTTON",L"Save",WS_TABSTOP|BS_PUSHBUTTON,12,249,120,28,IDC_MENU_SAVE);
    control(state,parent,L"BUTTON",L"Open Display Folder",WS_TABSTOP|BS_PUSHBUTTON,145,249,170,28,IDC_MENU_FOLDER);
    control(state,parent,L"BUTTON",L"Door Commands...",WS_TABSTOP|BS_PUSHBUTTON,328,249,247,28,IDC_CHAT_COMMANDS);

    label(state,parent,L"Message Pipe Colors",12,292,328,22);
    state.private_pipe_colors=control(state,parent,L"BUTTON",L"Enable pipe colors for private mail and NetMail",
        WS_TABSTOP|WS_DISABLED|BS_CHECKBOX,12,319,343,25,IDC_PRIVATE_PIPE_COLORS);
    label(state,parent,L"Pipe color style",365,292,210,22);
    state.menu_pipe_style=control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,
        365,319,210,110,IDC_MENU_PIPE_STYLE);
    SendMessageW(state.menu_pipe_style,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Synchronet (default)"));
    SendMessageW(state.menu_pipe_style,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Legacy SFX"));
    SendMessageW(state.menu_pipe_style,CB_SETCURSEL,0,0);
    state.private_pipe_reconcile=control(state,parent,L"BUTTON",L"Reconcile",WS_TABSTOP|BS_PUSHBUTTON,12,354,130,28,IDC_MESSAGE_RECONCILE);
    ShowWindow(state.private_pipe_reconcile,SW_HIDE);

    label(state,parent,L"Manage artwork with your ANSI/text editor in Display. Refresh to validate it. Missing or unusable files use built-in menus; Setup restores missing stock files.",12,390,560,44);
    state.menu_select=control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,12,440,205,260,IDC_MENU_SELECT);
    for(unsigned i=0;i<=static_cast<unsigned>(app::MenuId::qwk);++i) {
        if(i==3||i==4)continue;
        const auto name=utf8_to_wide(app::menu_definition(static_cast<app::MenuId>(i)).title);
        const auto item=SendMessageW(state.menu_select,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
        SendMessageW(state.menu_select,CB_SETITEMDATA,static_cast<WPARAM>(item),static_cast<LPARAM>(i));
    }
    SendMessageW(state.menu_select,CB_SETCURSEL,0,0);
    state.menu_format=control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,228,440,95,100,IDC_MENU_FORMAT);
    SendMessageW(state.menu_format,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"ANSI"));
    SendMessageW(state.menu_format,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"ASCII"));
    SendMessageW(state.menu_format,CB_SETCURSEL,0,0);
    label(state,parent,L"Level:",336,443,45,22);
    state.menu_level=control(state,parent,L"EDIT",L"10",WS_TABSTOP|WS_BORDER|ES_NUMBER,383,440,60,25,IDC_MENU_LEVEL);
    SendMessageW(state.menu_level,EM_SETLIMITTEXT,3,0);
    control(state,parent,L"BUTTON",L"Refresh Preview",WS_TABSTOP|BS_PUSHBUTTON,450,440,125,27,IDC_MENU_REFRESH);
    state.menu_sysop=control(state,parent,L"BUTTON",L"Sysop authorization (preview only)",WS_TABSTOP|BS_AUTOCHECKBOX,12,475,290,25,IDC_MENU_SYSOP);
    state.menu_artwork=control(state,parent,L"BUTTON",L"Use matching artwork",WS_TABSTOP|BS_AUTOCHECKBOX,320,475,220,25,IDC_MENU_ARTWORK);
    Button_SetCheck(state.menu_artwork,BST_CHECKED);
    WNDCLASSW preview{};preview.lpfnWndProc=menu_preview_proc;preview.hInstance=state.instance;
    preview.lpszClassName=L"SFXMenuSafePreview";preview.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    RegisterClassW(&preview);
    state.menu_preview=control(state,parent,preview.lpszClassName,L"",0,12,510,560,392);
    SetWindowLongPtrW(state.menu_preview,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&state));
    state.menu_detail=control(state,parent,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,12,912,560,64);
}
