// Included only in explicit BUILD_TESTING + SFX_ALPHA_TEST_AUTOMATION builds.
// Exercises the real UI functions and authenticated connection, not a mock
// status injection path. No caller credentials are accepted by this fixture.
void run_multinode_automation(AppState& state) {
    const auto directory = state.automation.multinode_directory;
    std::ifstream request(directory / L"request.txt");
    std::uint64_t serial = 0;
    std::string operation;
    unsigned node = 0;
    if (!(request >> serial >> operation >> node)) return;
    request.close(); // Do not hold the completed command file across its acknowledgement.
    if (serial <= state.automation.multinode_request) return;
    state.automation.multinode_request = serial;
    const auto pre_command_native_paints = state.automation.native_terminal_paints;
    const auto pre_command_native_saver = state.automation.native_saver_painted;
    const auto prefix = directory / std::to_wstring(serial);
    const auto file = [&](const wchar_t* suffix) { return std::filesystem::path(prefix.native() + suffix); };
    bool ok = node <= 4;
    bool cleared = false;
    state.automation.confirmation_heading.clear();
    state.automation.confirmation_detail.clear();
    if (ok && operation == "reset") {
        clear_node_presentation(state);
        cleared = state.status.nodes.empty() && !state.have_status && state.displayed_node == 0 &&
            state.open_node_views.empty() && TabCtrl_GetItemCount(state.tab) == 4 &&
            ListView_GetItemCount(state.node_list) == 0 &&
            ListView_GetNextItem(state.node_list, -1, LVNI_SELECTED) == -1;
        for (std::uint16_t i = 1; i <= 4; ++i)
            cleared = cleared && !state.indicators.bold(i, GetTickCount64()) &&
                !state.observed_screen_active[i] && !state.node_view_interaction[i];
        ok = cleared;
    } else if (ok && operation == "restart") {
        // The synthetic producer belongs to the first fixture Runtime only;
        // otherwise the replacement inherits the CP environment and reseeds.
        SetEnvironmentVariableW(L"SFX_UI6_LOG_FIXTURE", nullptr);
        SetEnvironmentVariableW(L"SFX_UI6_LARGE_LOG_FIXTURE", nullptr);
        std::string diagnostic;
        ok = state.runtime.restart(diagnostic);
    } else if (ok && operation == "lose") {
        state.runtime.test_disconnect_control();
    } else if (ok && operation == "exit") {
        ok = close_runtime(state, false);
    } else if (ok && operation == "telnet-profile") {
        std::wifstream profile_file(directory / L"client-profile.txt");
        std::wstring executable, output, release, label;
        ok = static_cast<bool>(std::getline(profile_file, executable) &&
            std::getline(profile_file, output) && std::getline(profile_file, release) &&
            std::getline(profile_file, label));
        if (ok) {
            app::ExternalTelnetClientProfile profile;
            profile.type = app::ExternalTelnetClientType::custom;
            profile.executable = std::move(executable);
            profile.custom_arguments = {L"--output=" + output, L"--release=" + release,
                L"--label=" + label, L"--host={HOST}", L"--port={PORT}"};
            ok = save_telnet_client_profile(profile);
        }
    } else if (ok && operation == "telnet-profile-clear") {
        ok = clear_telnet_client_profile();
    } else if (ok && operation == "telnet-profile-missing") {
        ok = clear_telnet_client_profile();
        update_status(state);
        const auto before = state.have_status ?
            state.status.lifecycle.active_work.caller_sessions : 0;
        g_suppress_external_client_warning = true;
        if (ok) logon_to_node(state, static_cast<std::uint16_t>(node));
        g_suppress_external_client_warning = false;
        update_status(state);
        ok = ok && state.have_status &&
            state.status.lifecycle.active_work.caller_sessions == before &&
            node_ready_for_local_logon(state, static_cast<std::uint16_t>(node));
    } else if (ok && operation == "telnet-logon") {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        const HANDLE sentinel = CreateEventW(&security, TRUE, FALSE, nullptr);
        ok = sentinel != nullptr;
        if (ok) {
            const auto text = std::to_wstring(reinterpret_cast<std::uintptr_t>(sentinel));
            ok = SetEnvironmentVariableW(L"SFX_F06_INHERITABLE_HANDLE", text.c_str()) != FALSE;
            if (ok) logon_to_node(state, static_cast<std::uint16_t>(node));
            (void)SetEnvironmentVariableW(L"SFX_F06_INHERITABLE_HANDLE", nullptr);
            CloseHandle(sentinel);
        }
    } else if (ok && operation == "local-reserve") {
        const auto result = state.runtime.start_local_session(static_cast<std::uint16_t>(node));
        state.automation.last_local_code = result.response.code;
        state.automation.last_local_diagnostic = result.response.diagnostic_code;
        state.automation.last_local_host = result.host;
        state.automation.last_local_port = result.port;
        state.automation.last_local_timeout = result.timeout_milliseconds;
        state.automation.last_local_reservation =
            result.response.code == app::LocalControlResponseCode::ok
            ? std::optional<app::NodeAssignmentIdentity>(result.reservation) : std::nullopt;
        ok = result.response.code == app::LocalControlResponseCode::ok;
    } else if (ok && operation == "local-cancel") {
        ok = state.automation.last_local_reservation.has_value();
        if (ok) {
            const auto result = state.runtime.cancel_local_session({
                *state.automation.last_local_reservation,
                app::AlphaLocalSessionCancelReason::operator_cancelled});
            state.automation.last_local_code = result.code;
            state.automation.last_local_diagnostic = result.diagnostic_code;
            ok = result.code == app::LocalControlResponseCode::ok;
        }
    }
    if (operation != "exit") update_status(state);
    if (ok && operation == "show") ShowWindow(state.window, SW_SHOWNOACTIVATE);
    if (ok && operation == "fullscreen") {
        MSG key{}; key.hwnd = state.window; key.message = WM_SYSKEYDOWN;
        key.wParam = VK_RETURN; key.lParam = 1L << 29;
        ok = handle_full_screen_key(state, key);
    }
    if (ok && operation == "fullscreen-command") {
        wchar_t label[64]{};
        ok = !state.full_screen && GetMenuStringW(GetMenu(state.window), ID_VIEW_FULL_SCREEN,
            label, 64, MF_BYCOMMAND) && std::wstring(label) == L"Full Screen\tAlt+Enter";
        if (ok) {
            SendMessageW(state.window, WM_COMMAND, ID_VIEW_FULL_SCREEN, 0);
            ok = state.full_screen && GetMenu(state.window) == nullptr;
        }
    }
    if (ok && operation == "maximize") ShowWindow(state.window, SW_MAXIMIZE);
    if (ok && operation == "restore") ShowWindow(state.window, SW_RESTORE);
    if (ok && operation == "fullscreen-menu-check") {
        ok = state.full_screen;
        if (ok) {
            MSG repeat{}; repeat.message = WM_SYSKEYDOWN; repeat.wParam = VK_RETURN;
            repeat.lParam = (1L << 29) | (1L << 30);
            ok = handle_full_screen_key(state, repeat) && state.full_screen;
            MSG alt{}; alt.message = WM_SYSCHAR; alt.wParam = 'f';
            ok = handle_full_screen_key(state, alt) && ok;
            SendMessageW(state.window, WM_SYSCOMMAND, SC_KEYMENU, 'f');
            ok = GetMenu(state.window) == nullptr && ok;
        }
    }
    if (ok && operation == "scale") {
        SendMessageW(state.window, WM_COMMAND, ID_VIEW_SCALE_AUTO + node, 0);
        ok = state.node_view_scale == kNodeViewScales[node] && read_node_view_scale() == state.node_view_scale;
    } else if (ok && (operation == "saver-start" || operation == "saver-next" || operation == "home")) {
        const auto selected = state.displayed_node;
        ok = selected > 0 && selected <= 4;
        if (ok && operation == "saver-start") {
            state.saver_placement[selected].seed(42U + selected);
            state.automation.saver_steps[selected] = 0;
        } else if (ok && operation == "saver-next") {
            ok = state.automation.saver_steps[selected].has_value();
            if (ok) ++*state.automation.saver_steps[selected];
        } else if (ok) {
            state.automation.saver_steps[selected].reset();
            SendMessageW(state.terminal_view, WM_KEYDOWN, VK_HOME, 0);
        }
    } else if (ok && (operation == "open" || operation == "disable" || operation == "enable")) {
        ok = node > 0 && node <= state.status.nodes.size();
        if (ok) {
            select_panel_page(state.tab, 0); show_selected_page(state, false);
            ListView_SetItemState(state.node_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(state.node_list, static_cast<int>(node - 1),
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            if (operation == "open") open_selected_node(state);
            else {
                SendMessageW(state.window, WM_COMMAND,
                    operation == "disable" ? ID_NODE_DISABLE : ID_NODE_ENABLE, 0);
            }
        }
    } else if (ok && operation == "right-click") {
        select_panel_page(state.tab, 0); show_selected_page(state, false);
        RECT item{};
        ok = node > 0 && ListView_GetItemRect(state.node_list,
            static_cast<int>(node - 1), &item, LVIR_LABEL);
        if (ok) {
            POINT point{item.left + scaled(state.window, 12), (item.top + item.bottom) / 2};
            ClientToScreen(state.node_list, &point);
            PostMessageW(state.window, WM_CANCELMODE, 0, 0);
            SendMessageW(state.window, WM_CONTEXTMENU,
                reinterpret_cast<WPARAM>(state.node_list), MAKELPARAM(point.x, point.y));
        }
    } else if (ok && (operation == "click" || operation == "ctrl-click" || operation == "shift-click" ||
            operation == "ctrl-a" || operation == "enter" || operation == "down" || operation == "shift-down")) {
        // Native list control receives the same mouse/key messages as Windows.
        // Only this UI thread's keyboard state is scoped; no global input injection.
        select_panel_page(state.tab, 0); show_selected_page(state, false);
        SetFocus(state.node_list);
        std::array<BYTE, 256> previous{}, keys{};
        ok = GetKeyboardState(previous.data()) != FALSE;
        keys = previous;
        keys[VK_CONTROL] = (operation == "ctrl-a" || operation == "ctrl-click") ? 0x80 : 0;
        keys[VK_SHIFT] = (operation == "shift-click" || operation == "shift-down") ? 0x80 : 0;
        ok = ok && SetKeyboardState(keys.data()) != FALSE;
        if (ok && operation.find("click") != std::string::npos) {
            RECT item{};
            ok = node > 0 && ListView_GetItemRect(state.node_list, static_cast<int>(node - 1), &item, LVIR_LABEL);
            if (ok) {
                const auto point = MAKELPARAM(item.left + scaled(state.window, 12), (item.top + item.bottom) / 2);
                const WPARAM modifiers = MK_LBUTTON | (keys[VK_CONTROL] ? MK_CONTROL : 0) |
                    (keys[VK_SHIFT] ? MK_SHIFT : 0);
                // A native list mouse-down may enter its drag-detection loop.
                // Queue the release first so that nested loop cannot wait on us.
                PostMessageW(state.node_list, WM_LBUTTONUP, modifiers & ~MK_LBUTTON, point);
                SendMessageW(state.node_list, WM_LBUTTONDOWN, modifiers, point);
                MSG release{};
                while (PeekMessageW(&release, state.node_list, WM_LBUTTONUP, WM_LBUTTONUP, PM_REMOVE))
                    DispatchMessageW(&release);
            }
        } else if (ok) {
            const WPARAM key = operation == "ctrl-a" ? 'A' : operation == "enter" ? VK_RETURN : VK_DOWN;
            SendMessageW(state.node_list, WM_KEYDOWN, key, 1);
            SendMessageW(state.node_list, WM_KEYUP, key, 1);
        }
        ok = SetKeyboardState(previous.data()) != FALSE && ok;
    } else if (ok && (operation == "batch-open" || operation == "batch-enable" || operation == "batch-disable")) {
        SendMessageW(state.window, WM_COMMAND, operation == "batch-open" ? ID_NODE_OPEN :
            operation == "batch-enable" ? ID_NODE_ENABLE : ID_NODE_DISABLE, 0);
    } else if (ok && (operation == "tab-key" || operation == "shift-tab-key" || operation == "button-enter")) {
        std::array<BYTE, 256> previous{}, keys{};
        ok = GetKeyboardState(previous.data()) != FALSE; keys = previous;
        keys[VK_SHIFT] = operation == "shift-tab-key" ? 0x80 : 0;
        ok = SetKeyboardState(keys.data()) != FALSE && ok;
        ok = handle_node_page_key(state, GetFocus(), WM_KEYDOWN,
            operation == "button-enter" ? VK_RETURN : VK_TAB) && ok;
        ok = SetKeyboardState(previous.data()) != FALSE && ok;
    } else if (ok && (operation == "close" || operation == "switch")) {
        const auto found = std::find(state.open_node_views.begin(), state.open_node_views.end(), node);
        ok = found != state.open_node_views.end();
        if (ok) {
            const int tab = 4 + static_cast<int>(found - state.open_node_views.begin());
            if (operation == "close") close_node_view_at(state, tab);
            else { select_panel_page(state.tab, tab); show_selected_page(state); }
        }
    } else if (ok && (operation == "nodes" || operation == "gray" || operation == "color")) {
        select_panel_page(state.tab, 0);
        state.preferences.system_monitor_visible = true;
        if (operation != "nodes") SendMessageW(state.window, WM_COMMAND,
            operation == "gray" ? ID_VIEW_MONITOR_GRAYSCALE : ID_VIEW_MONITOR_COLOR, 0);
        show_selected_page(state, false);
        sample_visible_monitor(state);
    } else if (ok && (operation == "logs" || operation == "log-pause" || operation == "log-clear" ||
            operation == "log-top" || operation == "log-bottom")) {
        select_panel_page(state.tab, 2); show_selected_page(state, false);
        if (operation == "log-pause" || operation == "log-clear")
            SendMessageW(state.window, WM_COMMAND,
                operation == "log-pause" ? IDC_LOG_PAUSE : IDC_LOG_CLEAR, 0);
        else if (operation == "log-top" || operation == "log-bottom")
            SendMessageW(state.log_view, WM_VSCROLL, operation == "log-top" ? SB_TOP : SB_BOTTOM, 0);
    } else if (ok && operation == "long") {
        // Presentation-only clipping probe, separate from authoritative captures.
        for (int i = 0; i < ListView_GetItemCount(state.node_list); ++i) {
            std::wstring text(128, L'W');
            for (int column = 2; column < 6; ++column)
                ListView_SetItemText(state.node_list, i, column, text.data());
        }
    } else if (ok && operation == "pixel-room") {
        // Large enough to measure 200% at every DPI, without a DPI-dependent
        // multiplication of the terminal's requested device pixels.
        SetWindowPos(state.window, nullptr, 0, 0, 1800, 1400,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (ok && (operation == "size" || operation == "log-compact" || operation == "minimum")) {
        SetWindowPos(state.window, nullptr, 0, 0,
            scaled(state.window, operation == "size" ? 1380 : operation == "minimum" ? 760 : 820),
            scaled(state.window, operation == "size" ? 850 : operation == "minimum" ? 520 : 560),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (operation != "exit") {
        RECT area{}; GetClientRect(state.window, &area);
        layout(state, area.right, area.bottom);
        RedrawWindow(state.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    if (state.have_status) {
        const auto encoded = app::encode_runtime_status(state.status);
        std::ofstream wire(file(L".status"), std::ios::binary);
        wire.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        ok = ok && wire.good() && !encoded.empty();
    }
    if (operation == "capture" || operation == "long" || operation == "gray" || operation == "color") {
        ok = capture_window_bmp(state.window, file(L".bmp")) && ok;
        std::ofstream monitor_data(file(L".monitor"));
        monitor_data << state.automation.monitor_content;
        if (state.displayed_node) {
            ok = capture_terminal_cells(state, file(L".cells")) && ok;
            ok = capture_live_fields(state, file(L".fields")) && ok;
        }
    }
    // Hidden fixture windows do not receive ordinary WM_PAINT. Exercise the
    // actual child paint path before recording geometry, without moving focus.
    if (operation != "exit" && state.displayed_node) {
        RECT client{}; GetClientRect(state.terminal_view, &client);
        const auto source = GetDC(state.terminal_view);
        const auto memory = CreateCompatibleDC(source);
        const auto bitmap = CreateCompatibleBitmap(source, std::max(1L, client.right), std::max(1L, client.bottom));
        const auto previous = SelectObject(memory, bitmap);
        draw_terminal(state.terminal_view, state, memory);
        SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(state.terminal_view, source);
    }
    std::ofstream meta(file(L".json"));
    const auto quoted_ui = [](const std::wstring& value) {
        std::string result = "\"";
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned char c : wide_to_utf8(value).value_or("")) {
            if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
            else { if (c == '\\' || c == '"') result += '\\'; result += static_cast<char>(c); }
        }
        return result + '"';
    };
    LONG warning_color = -1;
    if (state.rich_edit_module != nullptr) {
        FINDTEXTEXW find{}; find.chrg = {0, -1}; find.lpstrText = const_cast<wchar_t*>(L"WARNING:");
        constexpr WPARAM find_forward = 0x00000001; // FR_DOWN, common-dialog search flag.
        if (SendMessageW(state.log_view, EM_FINDTEXTEXW, find_forward, reinterpret_cast<LPARAM>(&find)) >= 0) {
            CHARRANGE previous{};
            POINT previous_scroll{};
            SendMessageW(state.log_view, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&previous_scroll));
            SendMessageW(state.log_view, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&previous));
            SendMessageW(state.log_view, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&find.chrgText));
            CHARFORMAT2W format{}; format.cbSize = sizeof(format);
            SendMessageW(state.log_view, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
            if (format.dwMask & CFM_COLOR) warning_color = static_cast<LONG>(format.crTextColor);
            SendMessageW(state.log_view, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&previous));
            SendMessageW(state.log_view, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&previous_scroll));
        }
    }
    RECT viewport{}, clear_button{};
    GetClientRect(state.terminal_view, &viewport);
    GetClientRect(state.log_clear_button, &clear_button);
    SIZE clear_label{};
    const auto label_dc = GetDC(state.log_clear_button);
    const auto label_font = SelectObject(label_dc, state.ui_font);
    GetTextExtentPoint32W(label_dc, L"Clear View", 10, &clear_label);
    SelectObject(label_dc, label_font); ReleaseDC(state.log_clear_button, label_dc);
    SCROLLINFO log_scroll{sizeof(log_scroll), SIF_ALL};
    GetScrollInfo(state.log_view, SB_VERT, &log_scroll);
    const auto rect_json = [](const RECT& r) {
        return "[" + std::to_string(r.left) + "," + std::to_string(r.top) + "," +
            std::to_string(r.right) + "," + std::to_string(r.bottom) + "]";
    };
    RECT outer{}; GetWindowRect(state.window, &outer);
    WINDOWPLACEMENT placement{sizeof(placement)}; GetWindowPlacement(state.window, &placement);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(state.window, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto frame_json = [&](const HWND child) {
        RECT frame{}; GetWindowRect(child, &frame); OffsetRect(&frame, -outer.left, -outer.top);
        return rect_json(frame);
    };
    meta << "{\"ok\":" << ok << ",\"cleared\":" << cleared
        << ",\"window_frame\":" << rect_json(outer)
        << ",\"normal_placement\":" << rect_json(placement.rcNormalPosition)
        << ",\"monitor_rect\":" << rect_json(monitor.rcMonitor)
        << ",\"uptime_available\":" << state.runtime_metrics.uptime_available
        << ",\"bbs_uptime\":" << state.runtime_metrics.uptime_milliseconds
        << ",\"os_uptime\":" << GetTickCount64()
        << ",\"node_view_scale\":" << static_cast<unsigned>(state.node_view_scale)
        << ",\"saved_node_view_scale\":" << static_cast<unsigned>(read_node_view_scale())
        << ",\"scale_menu_checked\":[";
    for (UINT i = 0; i < 5; ++i) {
        if (i) meta << ',';
        meta << !!(GetMenuState(state.scale_menu, ID_VIEW_SCALE_AUTO+i, MF_BYCOMMAND) & MF_CHECKED);
    }
    meta << "],\"viewport\":" << rect_json(viewport)
        << ",\"terminal_frame\":" << frame_json(state.terminal_view)
        << ",\"monitor_frame\":" << frame_json(state.monitor_view)
        << ",\"node_header_frame\":" << frame_json(state.list_header)
        << ",\"clear_frame\":" << frame_json(state.log_clear_button)
        << ",\"painted_canvas\":" << rect_json(state.automation.painted_canvas)
        << ",\"painted_art\":" << rect_json(state.automation.painted_art)
        << ",\"painted_saver\":" << state.automation.painted_saver
        << ",\"full_screen\":" << state.full_screen
        << ",\"menu_visible\":" << (GetMenu(state.window) != nullptr)
        << ",\"maximized\":" << (IsZoomed(state.window) != FALSE)
        << ",\"native_paints\":" << state.automation.native_terminal_paints
        << ",\"native_saver\":" << state.automation.native_saver_painted
        << ",\"pre_command_native_paints\":" << pre_command_native_paints
        << ",\"pre_command_native_saver\":" << pre_command_native_saver
        << ",\"idle_tick\":" << (state.displayed_node ? state.node_view_interaction[state.displayed_node] : 0)
        << ",\"now_tick\":" << GetTickCount64()
        << ",\"log_cache_count\":" << state.log_cache.backlog().entries().size()
        << ",\"log_cache_bytes\":" << state.log_cache.backlog().bytes()
        << ",\"log_cache_oldest\":" << state.log_cache.backlog().oldest()
        << ",\"log_cursor\":" << state.log_cache.cursor().after
        << ",\"log_more\":" << state.log_cache.more()
        << ",\"saver_color\":" << state.automation.painted_saver_color
        << ",\"clear_button\":" << rect_json(clear_button)
        << ",\"clear_label\":[" << clear_label.cx << ',' << clear_label.cy << ']'
        << ",\"log_scroll\":[" << log_scroll.nMin << ',' << log_scroll.nMax << ','
        << log_scroll.nPage << ',' << log_scroll.nPos << ']'
        << ",\"pid\":" << GetCurrentProcessId() << ",\"runtime_pid\":" << state.runtime.test_process_id()
        << ",\"displayed\":" << state.displayed_node << ",\"selected\":"
        << ListView_GetNextItem(state.node_list, -1, LVNI_SELECTED)
        << ",\"grayscale\":" << state.preferences.system_monitor_grayscale
        << ",\"header\":" << quoted_ui(window_text(state.node_header))
        << ",\"footer\":" << quoted_ui(state.status_text)
        << ",\"events\":" << quoted_ui(window_text(state.events_view))
        << ",\"logs\":" << quoted_ui(window_text(state.log_view))
        << ",\"logs_paused\":" << state.logs_paused
        << ",\"warning_color\":" << warning_color
        << ",\"log_first_line\":" << SendMessageW(state.log_view, EM_GETFIRSTVISIBLELINE, 0, 0)
        << ",\"confirmation\":" << quoted_ui(state.automation.confirmation_heading)
        << ",\"detail\":" << quoted_ui(state.automation.confirmation_detail)
        << ",\"selected_nodes\":[";
    bool selected_first = true;
    for (const auto number : selected_node_numbers(state)) {
        if (!selected_first) meta << ',';
        selected_first = false; meta << number;
    }
    populate_node_menu(state.node_menu, state);
    const auto enabled_menu = [&](UINT command) {
        const UINT flags = GetMenuState(state.node_menu, command, MF_BYCOMMAND);
        return flags != static_cast<UINT>(-1) && !(flags & (MF_DISABLED | MF_GRAYED));
    };
    std::array<wchar_t, 96> logon_label{};
    (void)GetMenuStringW(state.node_menu, ID_NODE_LOGON, logon_label.data(),
        static_cast<int>(logon_label.size()), MF_BYCOMMAND);
    meta << "],\"focused_node\":" << ListView_GetNextItem(state.node_list, -1, LVNI_FOCUSED) + 1
        << ",\"selection_mark\":" << ListView_GetSelectionMark(state.node_list) + 1
        << ",\"list_has_focus\":" << (GetFocus() == state.node_list)
        << ",\"button_has_focus\":" << (GetFocus() == state.open_node_button)
        << ",\"tab_has_focus\":" << (GetFocus() == state.tab)
        << ",\"multiselect\":" << !(GetWindowLongPtrW(state.node_list, GWL_STYLE) & LVS_SINGLESEL)
        << ",\"tabstop\":" << !!(GetWindowLongPtrW(state.node_list, GWL_STYLE) & WS_TABSTOP)
        << ",\"enable_available\":" << enabled_menu(ID_NODE_ENABLE)
        << ",\"disable_available\":" << enabled_menu(ID_NODE_DISABLE)
        << ",\"logon_available\":" << enabled_menu(ID_NODE_LOGON)
        << ",\"logon_label\":" << quoted_ui(logon_label.data())
        << ",\"local_code\":" << static_cast<unsigned>(state.automation.last_local_code)
        << ",\"local_diagnostic\":" << quoted_ui(
            utf8_to_wide(state.automation.last_local_diagnostic))
        << ",\"local_host\":" << quoted_ui(utf8_to_wide(state.automation.last_local_host))
        << ",\"local_port\":" << state.automation.last_local_port
        << ",\"local_timeout\":" << state.automation.last_local_timeout
        << ",\"open_label\":" << quoted_ui(window_text(state.open_node_button))
        << ",\"tabs\":[";
    bool first = true;
    for (auto number : state.open_node_views) { if (!first) meta << ','; first = false; meta << number; }
    meta << "],\"bold\":[";
    for (std::uint16_t i = 1; i <= 4; ++i) { if (i > 1) meta << ','; meta << state.indicators.bold(i, GetTickCount64()); }
    meta << "],\"painted_bold\":[";
    for (unsigned i = 0; i < 4; ++i) {
        if (i) meta << ',';
        meta << '[';
        for (unsigned j = 0; j < 6; ++j) { if (j) meta << ','; meta << state.automation.painted_bold[i][j]; }
        meta << ']';
    }
    meta << "],\"rows\":[";
    for (int i = 0; i < ListView_GetItemCount(state.node_list); ++i) {
        if (i) meta << ',';
        meta << '[';
        for (int column = 0; column < 6; ++column) {
            if (column) meta << ',';
            std::array<wchar_t, 256> value{};
            ListView_GetItemText(state.node_list, i, column, value.data(), static_cast<int>(value.size()));
            meta << quoted_ui(value.data());
        }
        meta << ']';
    }
    meta << "]}\n";
    meta.close();
    std::ofstream done(directory / L"complete.txt");
    done << serial << '\n';
}
