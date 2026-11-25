// Presentation only. Both locations consume the same Runtime-derived model.
RECT modem_pair_rect(HWND window, RECT area, int index) {
    const int left = area.left + scaled(window, 5) + index * scaled(window, 18);
    return {left, area.top, left + scaled(window, 18), area.bottom};
}

RECT modem_led_rect(HWND window, RECT area, int index, bool labelled = false) {
    if (labelled) {
        const auto pair = modem_pair_rect(window, area, index);
        const int diameter = scaled(window, 8);
        const int left = pair.left + (pair.right - pair.left - diameter) / 2;
        const int top = area.top + scaled(window, 1);
        return {left, top, left + diameter, top + diameter};
    }
    const int diameter = scaled(window, 10), step = scaled(window, 18);
    const int left = area.left + scaled(window, 9) + index * step;
    const int top = area.top + (area.bottom - area.top - diameter) / 2;
    return {left, top, left + diameter, top + diameter};
}

void draw_modem_lights(AppState& state, HDC dc, RECT area,
    const app::AlphaNodeStatus* node, COLORREF background, bool labelled = false) {
    fill_rectangle(dc, area, background);
    const auto value = node ? state.modem_indicators.lights(*node, GetTickCount64()) : app::ModemLights{};
    const std::array<bool, 6> active{{value.aa, value.cd, value.rd, value.sd, value.sc, value.mr}};
    const bool classic = g_appearance.theme() == win32::DesktopTheme::classic;
    const int saved_dc = SaveDC(dc);
    const HFONT labels = labelled ? CreateFontW(-scaled(state.window, 9), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI") : nullptr;
    if (labels) SelectObject(dc, labels);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, classic ? RGB(78,84,90) : RGB(128,138,148));
    constexpr const wchar_t* names[] = {L"AA", L"CD", L"RD", L"SD", L"SC", L"MR"};
    for (int index = 0; index < 6; ++index) {
        const auto area_led = modem_led_rect(state.window, area, index, labelled);
        const COLORREF color = active[index] ? (index == 4 ? RGB(34,192,72) : RGB(242,44,35))
            : classic ? RGB(124,69,65) : RGB(83,32,29);
        const HBRUSH brush = CreateSolidBrush(color);
        const HPEN pen = CreatePen(PS_SOLID, 1, classic ? RGB(81,52,50) : RGB(128,56,48));
        const auto old_brush = SelectObject(dc, brush), old_pen = SelectObject(dc, pen);
        Ellipse(dc, area_led.left, area_led.top, area_led.right, area_led.bottom);
        SelectObject(dc, old_brush); SelectObject(dc, old_pen);
        DeleteObject(brush); DeleteObject(pen);
        if (labelled) {
            auto label = modem_pair_rect(state.window, area, index);
            label.top = area_led.bottom;
            DrawTextW(dc, names[index], -1, &label,
                DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    RestoreDC(dc, saved_dc);
    if (labels) DeleteObject(labels);
}

std::wstring modem_tooltip_at_point(AppState& state,POINT screen) {
    if (!state.preferences.modem_lights || !state.have_status) return {};
    POINT point = screen;
    RECT area{};
    const app::AlphaNodeStatus* node = nullptr;
    bool labelled = false;
    if (IsWindowVisible(state.modem_strip)) {
        ScreenToClient(state.modem_strip, &point);
        GetClientRect(state.modem_strip, &area);
        if (PtInRect(&area, point)) node = node_status(state, state.displayed_node);
    }
    if (!node && IsWindowVisible(state.node_list)) {
        point = screen; ScreenToClient(state.node_list, &point);
        LVHITTESTINFO hit{}; hit.pt = point;
        const auto row = ListView_SubItemHitTest(state.node_list, &hit);
        if (row < 0 || hit.iSubItem != 1 || static_cast<std::size_t>(row) >= state.status.nodes.size()) return {};
        ListView_GetSubItemRect(state.node_list, row, 1, LVIR_BOUNDS, &area);
        node = &state.status.nodes[static_cast<std::size_t>(row)];
        labelled = true;
    }
    if (!node) return {};
    int index = -1;
    for (int i = 0; i < 6; ++i) {
        const auto led = labelled ? modem_pair_rect(state.window, area, i) :
            modem_led_rect(state.window, area, i);
        if (PtInRect(&led, point)) { index = i; break; }
    }
    const auto lights = state.modem_indicators.lights(*node, GetTickCount64());
    const std::wstring prefix = L"Node " + std::to_wstring(node->number) + L" — ";
    switch (index) {
        case 0: return prefix + L"AA — Auto Answer: " + (lights.aa ? L"accepting callers" : L"not available for a new caller");
        case 1: return prefix + L"CD — Connected: " + (lights.cd ? L"caller connected (not a login indicator)" : L"no connection");
        case 2: return prefix + L"RD — Receive: " + (lights.rd ? L"data from caller" : L"no recent caller data");
        case 3: return prefix + L"SD — Send: " + (lights.sd ? L"data to caller" : L"no recent output");
        case 4: return prefix + L"SC — Secure Connection: " + (!lights.cd ? L"No connection" :
            lights.sc ? L"SSH encrypted (not authentication)" : node->connection == "SSH"
                ? L"SSH negotiating; encryption not established" : L"Telnet unencrypted");
        case 5: return prefix + L"MR — Node operational: " + (lights.mr ? L"enabled and operational" : L"down, disabled or unavailable");
        default: return {};
    }
}

std::wstring modem_tooltip_at_cursor(AppState& state) {
    POINT point{};GetCursorPos(&point);return modem_tooltip_at_point(state,point);
}
void insert_modem_column(AppState& state) {
    LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t*>(L"Modem"); column.cx = scaled(state.window, 126);
    ListView_InsertColumn(state.node_list, 1, &column);
}

void relay_modem_mouse(AppState* state, HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_MOUSEMOVE && state && state->modem_tooltip) {
        const auto text = modem_tooltip_at_cursor(*state);
        // One native tool covers each child. Moving to a different lamp must
        // dismiss the previous lamp's text even without leaving that child.
        if (text != state->modem_hover_text) {
            state->modem_hover_text = text;
            SendMessageW(state->modem_tooltip, TTM_POP, 0, 0);
            SendMessageW(state->modem_tooltip, TTM_UPDATE, 0, 0);
        }
    }
    if (state && state->modem_tooltip && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
        MSG event{}; event.hwnd = window; event.message = message;
        event.wParam = wparam; event.lParam = lparam;
        SendMessageW(state->modem_tooltip, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&event));
    }
}

LRESULT CALLBACK modem_hover_proc(HWND window, UINT message, WPARAM wparam,
    LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    relay_modem_mouse(reinterpret_cast<AppState*>(data), window, message, wparam, lparam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, modem_hover_proc, id);
    return DefSubclassProc(window, message, wparam, lparam);
}

void initialize_modem_lights(AppState& state) {
    if (state.preferences.modem_lights) insert_modem_column(state);
    state.modem_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, state.window, nullptr, state.instance, nullptr);
    if (!state.modem_tooltip) return;
    SendMessageW(state.modem_tooltip, TTM_SETMAXTIPWIDTH, 0, scaled(state.window, 380));
    for (HWND child : {state.node_list, state.modem_strip}) {
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND;
        tool.hwnd = state.window;
        tool.uId = reinterpret_cast<UINT_PTR>(child);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(state.modem_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        // The list already has a raw WndProc subclass; never mix that chain
        // with the common-control subclass helpers. It relays mouse events.
        if (child == state.modem_strip)
            SetWindowSubclass(child, modem_hover_proc, 1, reinterpret_cast<DWORD_PTR>(&state));
    }
}

void toggle_modem_lights(AppState& state) {
    state.preferences.modem_lights = !state.preferences.modem_lights;
    if (state.preferences.modem_lights) insert_modem_column(state);
    else ListView_DeleteColumn(state.node_list, 1);
    CheckMenuItem(GetMenu(state.window), ID_VIEW_MODEM_LIGHTS, MF_BYCOMMAND |
        (state.preferences.modem_lights ? MF_CHECKED : MF_UNCHECKED));
    show_selected_page(state, false, false);
    RECT area{}; GetClientRect(state.window, &area); layout(state, area.right, area.bottom);
    refresh_node_rows(state);
    InvalidateRect(state.node_list, nullptr, TRUE);
    InvalidateRect(state.modem_strip, nullptr, TRUE);
    if (!win32::publish_control_panel_ui_preferences(win32::alpha_installation_root(),
        win32::current_process_user_sid_string(), state.preferences).succeeded())
        MessageBoxW(state.window, L"Modem Lights changed for this session but could not be saved.",
            L"Spitfire-X BBS", MB_OK | MB_ICONWARNING);
}
