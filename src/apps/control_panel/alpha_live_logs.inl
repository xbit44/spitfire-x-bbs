// UI-thread paging. One <=60 KiB response per timer turn; never request another
// four-screen status snapshot during catch-up. Runtime/caller workers stay independent.
void refresh_live_logs(AppState& state) {
    KillTimer(state.window, kLogPageTimer);
    if (!state.have_status || state.closing || state.logs_paused || panel_page(state.tab) != 2 || state.log_page_failed) return;
    const auto newest = state.status.logs.empty() ? 0 : state.status.logs.back().sequence;
    if (!state.log_view_initialized || state.log_cache.more() || newest > state.log_cache.cursor().after) {
        try {
            const auto page = state.runtime.log_page(state.log_cache.cursor());
            if (!page || !state.log_cache.accept(*page)) {
                state.log_page_failed = true;
                SetWindowTextW(state.log_view, L"Live Logs unavailable: invalid or lost authoritative history response.");
                return;
            }
        } catch (...) {
            state.log_page_failed = true;
            SetWindowTextW(state.log_view, L"Live Logs unavailable: bounded presentation allocation failed.");
            return;
        }
        if (state.log_cache.more()) {
            SetTimer(state.window, kLogPageTimer, 25, nullptr);
            return; // publish a coherent caught-up view; do not repeatedly format a growing backlog
        }
    }
    const auto& entries = state.log_cache.backlog().entries();
    const std::uint64_t first_log_sequence = entries.empty()
        ? 0 : entries.front().sequence;
    const std::uint64_t last_log_sequence = entries.empty()
        ? 0 : entries.back().sequence;
    const bool log_changed = !state.log_view_initialized ||
        first_log_sequence != state.rendered_log_first_sequence ||
        last_log_sequence != state.rendered_log_last_sequence ||
        entries.size() != state.rendered_log_count ||
        state.logs_cleared_through != state.rendered_log_clear_through ||
        state.rendered_log_zone != state.status.bbs_time_zone ||
        state.rendered_log_health != state.status.log_health ||
        state.rendered_hostname_exhausted != state.status.hostname_exhausted;
    // Pause freezes the displayed buffer and inspection position. New health
    // warnings remain immediately visible in the existing application footer;
    // Resume renders their full notices with the newest authoritative entries.
    if (log_changed) {
        (void)state.log_time.configure(state.status.bbs_time_zone);
        std::wstring logs = L"Time: " + utf8_to_wide(state.status.bbs_time_zone) +
            L" (BBS local; structured diagnostics retain UTC)\r\n";
        std::vector<std::pair<CHARRANGE, COLORREF>> colors;
        for (const auto& encoded : entries) {
            const auto entry = *app::decode_operational_record(encoded.record, encoded.sequence);
            if (entry.sequence <= state.logs_cleared_through) continue;
            if (state.selected_log_filter) {
                if (app::operational_filter_group(entry)!=static_cast<unsigned>(state.selected_log_filter)) continue;
            }
            const LONG begin = log_selection_position(logs);
            logs += utf8_to_wide(app::render_operational_line(entry,
                state.log_time.format(entry.utc_seconds))) + L"\r\n";
            const COLORREF color = entry.level == "ERROR" ? kErrorColor :
                (entry.level == "WARNING" ? kAmberColor : kTextColor);
            colors.push_back({{begin, log_selection_position(logs)}, color});
        }
        if (state.log_cache.gap()) logs += L"Earlier entries were evicted from this Runtime\u2019s bounded history.\r\n";
        const LONG warning_begin = log_selection_position(logs);
        logs += log_health_notice(state.status.log_health, state.status.hostname_exhausted);
        if (state.status.log_health || state.status.hostname_exhausted)
            colors.push_back({{warning_begin, log_selection_position(logs)}, kAmberColor});
        state.rendered_log_colors = colors;
        SendMessageW(state.log_view, WM_SETREDRAW, FALSE, 0);
        SetWindowTextW(state.log_view, logs.c_str());
        if (state.rich_edit_module != nullptr) {
            CHARFORMAT2W base{};
            base.cbSize = sizeof(base);
            base.dwMask = CFM_COLOR;
            base.crTextColor = kTextColor;
            SendMessageW(state.log_view, EM_SETCHARFORMAT, SCF_ALL,
                reinterpret_cast<LPARAM>(&base));
            for (const auto& colored : colors) {
                SendMessageW(state.log_view, EM_EXSETSEL, 0,
                    reinterpret_cast<LPARAM>(const_cast<CHARRANGE*>(&colored.first)));
                CHARFORMAT2W format{};
                format.cbSize = sizeof(format);
                format.dwMask = CFM_COLOR;
                format.crTextColor = colored.second;
                SendMessageW(state.log_view, EM_SETCHARFORMAT, SCF_SELECTION,
                    reinterpret_cast<LPARAM>(&format));
            }
        }
        {
            // Follow is unconditional while running. Pause is the explicit
            // inspection mode; scrolling upward must not silently disable follow.
            CHARRANGE end{-1, -1};
            SendMessageW(state.log_view, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
            SendMessageW(state.log_view, EM_SCROLLCARET, 0, 0);
            SendMessageW(state.log_view, WM_VSCROLL, SB_BOTTOM, 0);
        }
        SendMessageW(state.log_view, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(state.log_view, nullptr, FALSE);
        state.rendered_log_first_sequence = first_log_sequence;
        state.rendered_log_last_sequence = last_log_sequence;
        state.rendered_log_count = entries.size();
        state.rendered_log_health = state.status.log_health;
        state.rendered_hostname_exhausted = state.status.hostname_exhausted;
        state.rendered_log_zone = state.status.bbs_time_zone;
        state.rendered_log_clear_through = state.logs_cleared_through;
        state.log_view_initialized = true;
    }
}
