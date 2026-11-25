// Presentation only. No Runtime calls, saved-full-screen preference, caller
// input, monitor mode change, taskbar mutation or ownership transition.
bool full_screen_bounds(AppState& state) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(state.window, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const auto& rect = monitor.rcMonitor;
    return SetWindowPos(state.window, HWND_TOP, rect.left, rect.top,
        rect.right - rect.left, rect.bottom - rect.top,
        SWP_FRAMECHANGED | SWP_NOACTIVATE) != FALSE;
}
bool restore_windowed(AppState& state) {
    state.restoring_placement = true;
    SetWindowLongPtrW(state.window, GWL_STYLE, state.windowed_style);
    SetWindowLongPtrW(state.window, GWL_EXSTYLE, state.windowed_ex_style);
    const bool menu = SetMenu(state.window, state.windowed_menu) != FALSE;
    const bool frame = SetWindowPos(state.window, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
    const bool placed = SetWindowPlacement(state.window, &state.windowed_placement) != FALSE;
    const bool restored = menu && frame && placed && GetMenu(state.window) == state.windowed_menu &&
        GetWindowLongPtrW(state.window, GWL_STYLE) == state.windowed_style &&
        GetWindowLongPtrW(state.window, GWL_EXSTYLE) == state.windowed_ex_style;
    if (restored) { state.full_screen = false; state.windowed_menu = nullptr; }
    state.restoring_placement = false;
    DrawMenuBar(state.window);
    return restored;
}
bool toggle_full_screen(AppState& state) {
    if (state.closing) return false;
    if (state.full_screen) return restore_windowed(state);
    if (!GetWindowPlacement(state.window, &state.windowed_placement)) return false;
    state.windowed_style = GetWindowLongPtrW(state.window, GWL_STYLE);
    state.windowed_ex_style = GetWindowLongPtrW(state.window, GWL_EXSTYLE);
    state.windowed_menu = GetMenu(state.window);
    state.full_screen = true; // reentrant resize/DPI handlers now use full monitor geometry
    const auto style = state.windowed_style & ~(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE | WS_MINIMIZE);
    const auto ex_style = state.windowed_ex_style & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(state.window, GWL_STYLE, style);
    SetWindowLongPtrW(state.window, GWL_EXSTYLE, ex_style);
    if (!SetMenu(state.window, nullptr) || GetWindowLongPtrW(state.window, GWL_STYLE) != style ||
        GetWindowLongPtrW(state.window, GWL_EXSTYLE) != ex_style || !full_screen_bounds(state)) {
        (void)restore_windowed(state);
        return false;
    }
    return true;
}
void request_full_screen(AppState& state) {
    if (!toggle_full_screen(state))
        MessageBoxW(state.window, L"The window presentation could not be changed. Runtime and callers are unchanged.",
            L"Spitfire-X BBS", MB_OK | MB_ICONWARNING);
}
bool handle_full_screen_key(AppState& state, const MSG& message) {
    if (message.message == WM_SYSKEYDOWN && message.wParam == VK_RETURN &&
        (message.lParam & (1L << 29))) {
        if (!(message.lParam & (1L << 30))) request_full_screen(state);
        return true; // repeats and Alt+Enter never reach any terminal input path
    }
    return state.full_screen && (message.message == WM_SYSCHAR ||
        (message.message == WM_SYSKEYUP && (message.wParam == VK_MENU || message.wParam == VK_RETURN)));
}
