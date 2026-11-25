struct FileContentsRestoreDialog {
    State& state;
    const std::filesystem::path& source;
    const std::vector<archive::ContentArea>& areas;
    bool ready{};
    std::vector<std::int64_t> reviewed_areas;
    std::unique_ptr<win32::OfflineFileContentRestore> restore;
};

std::vector<std::int64_t> selected_contents_areas(HWND window, const FileContentsRestoreDialog& dialog) {
    std::vector<std::int64_t> selected;
    const auto list = GetDlgItem(window, IDC_CONTENTS_RESTORE_AREAS);
    for (std::size_t row = 0; row < dialog.areas.size(); ++row)
        if (ListView_GetCheckState(list, static_cast<int>(row)) == TRUE) selected.push_back(dialog.areas[row].id);
    return selected;
}

void reset_contents_restore_preview(HWND window, FileContentsRestoreDialog& dialog) {
    dialog.restore.reset();
    dialog.reviewed_areas.clear();
    EnableWindow(GetDlgItem(window, IDOK), FALSE);
    const auto has_selection = !selected_contents_areas(window, dialog).empty();
    EnableWindow(GetDlgItem(window, IDC_CONTENTS_RESTORE_PREVIEW), has_selection);
    SetDlgItemTextW(window, IDC_CONTENTS_RESTORE_DETAILS, has_selection
        ? L"Choose Preview Selection to verify these destinations and review the file counts."
        : L"Check one or more areas to preview.");
}

std::wstring contents_restore_details(const FileContentsRestoreDialog& dialog,
                                      const win32::FileContentRestore& preview) {
    struct Counts { std::size_t missing{}, identical{}, conflicts{}; };
    std::map<std::int64_t, Counts> counts;
    for (const auto& item : preview.items()) {
        auto& area = counts[item.area];
        switch (item.state) {
        case win32::FileContentRestoreItem::State::missing: ++area.missing; break;
        case win32::FileContentRestoreItem::State::identical: ++area.identical; break;
        case win32::FileContentRestoreItem::State::conflict: ++area.conflicts; break;
        }
    }
    std::wstring details;
    for (const auto& [id, path] : preview.destinations()) {
        const auto area = std::find_if(dialog.areas.begin(), dialog.areas.end(),
            [id](const auto& candidate) { return candidate.id == id; });
        const auto& total = counts[id];
        if (!details.empty()) details += L"\r\n\r\n";
        details += L"Area " + std::to_wstring(id);
        if (area != dialog.areas.end()) details += L"  " + utf8_to_wide(area->name);
        details += L"\r\nCurrent destination: " + path.native() +
            L"\r\nMissing to restore: " + std::to_wstring(total.missing) +
            L"\r\nIdentical files kept: " + std::to_wstring(total.identical) +
            L"\r\nConflicting files skipped: " + std::to_wstring(total.conflicts);
    }
    return details;
}

void preview_contents_selection(HWND window, FileContentsRestoreDialog& dialog) {
    reset_contents_restore_preview(window, dialog);
    auto selected = selected_contents_areas(window, dialog);
    if (selected.empty()) return;
    const auto previous_cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    struct CursorRestore {
        HCURSOR previous;
        ~CursorRestore() { SetCursor(previous); }
    } cursor_restore{previous_cursor};
    auto details_control = GetDlgItem(window, IDC_CONTENTS_RESTORE_DETAILS);
    SetWindowTextW(details_control, L"Verifying the restore point and current destinations...");
    UpdateWindow(details_control);

    std::string diagnostic;
    auto restore = win32::OfflineFileContentRestore::begin(
        dialog.state.root, dialog.state.owner, dialog.source, selected, diagnostic);
    if (!restore) {
        SetWindowTextW(details_control, (L"Content restore refused: " + utf8_to_wide(diagnostic)).c_str());
        return;
    }
    const auto details = contents_restore_details(dialog, restore->preview());
    if (details.empty() || !SetWindowTextW(details_control, details.c_str()) ||
        static_cast<std::size_t>(GetWindowTextLengthW(details_control)) != details.size()) {
        SetWindowTextW(details_control, L"The verified preview could not be displayed. No files changed.");
        return;
    }
    dialog.reviewed_areas = std::move(selected);
    dialog.restore = std::move(restore);
    EnableWindow(GetDlgItem(window, IDOK), TRUE);
}

INT_PTR CALLBACK contents_restore_dialog(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* dialog = reinterpret_cast<FileContentsRestoreDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<FileContentsRestoreDialog*>(lparam);
        SetWindowLongPtrW(window, DWLP_USER, lparam);
        SetDlgItemTextW(window, IDC_CONTENTS_RESTORE_SOURCE, dialog->source.c_str());
        SendDlgItemMessageW(window, IDC_CONTENTS_RESTORE_DETAILS, EM_SETLIMITTEXT,
            std::numeric_limits<int>::max(), 0);
        const auto list = GetDlgItem(window, IDC_CONTENTS_RESTORE_AREAS);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        RECT area{};
        GetClientRect(list, &area);
        LVCOLUMNW column{};
        column.mask = LVCF_WIDTH | LVCF_TEXT;
        column.cx = area.right - GetSystemMetrics(SM_CXVSCROLL);
        column.pszText = const_cast<wchar_t*>(L"File area");
        if (ListView_InsertColumn(list, 0, &column) == -1) {
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
        for (std::size_t row = 0; row < dialog->areas.size(); ++row) {
            const auto& entry = dialog->areas[row];
            auto label = std::to_wstring(entry.id) + L"  " + utf8_to_wide(entry.name);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(row);
            item.pszText = label.data();
            if (ListView_InsertItem(list, &item) == -1) {
                EndDialog(window, IDCANCEL);
                return TRUE;
            }
        }
        dialog->ready = true;
        reset_contents_restore_preview(window, *dialog);
        dialog->state.appearance.apply_controls(window);
        SetFocus(GetDlgItem(window, IDCANCEL));
        return FALSE;
    }
    if (!dialog) return FALSE;
    if (message == WM_NOTIFY && dialog->ready) {
        const auto* notice = reinterpret_cast<const NMHDR*>(lparam);
        if (notice && notice->idFrom == IDC_CONTENTS_RESTORE_AREAS && notice->code == LVN_ITEMCHANGED) {
            const auto* change = reinterpret_cast<const NMLISTVIEW*>(notice);
            if ((change->uChanged & LVIF_STATE) && ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK))
                reset_contents_restore_preview(window, *dialog);
            return FALSE;
        }
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wparam)) {
        case IDC_CONTENTS_RESTORE_PREVIEW:
            try {
                preview_contents_selection(window, *dialog);
            } catch (const std::exception& error) {
                SetDlgItemTextW(window, IDC_CONTENTS_RESTORE_DETAILS,
                    (L"Content restore refused: " + utf8_to_wide(error.what())).c_str());
            }
            return TRUE;
        case IDOK:
            if (dialog->restore && selected_contents_areas(window, *dialog) == dialog->reviewed_areas)
                EndDialog(window, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
    }
    if (message == WM_CLOSE) {
        EndDialog(window, IDCANCEL);
        return TRUE;
    }
    if (message == WM_ERASEBKGND) {
        RECT area{};
        GetClientRect(window, &area);
        FillRect(reinterpret_cast<HDC>(wparam), &area, dialog->state.appearance.background_brush());
        return TRUE;
    }
    LRESULT themed{};
    if (!dialog->state.appearance.control_message(window, message, wparam, lparam, themed)) return FALSE;
    if (message != WM_NOTIFY) return themed;
    SetWindowLongPtrW(window, DWLP_MSGRESULT, themed);
    return TRUE;
}

std::unique_ptr<win32::OfflineFileContentRestore> review_file_contents_restore(
    State& state, const std::filesystem::path& source, const std::vector<archive::ContentArea>& areas) {
    FileContentsRestoreDialog dialog{state, source, areas, false, {}, {}};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_FILE_CONTENTS_RESTORE_REVIEW),
        state.window, contents_restore_dialog, reinterpret_cast<LPARAM>(&dialog));
    if (result == -1) throw std::runtime_error("file_contents_review_unavailable");
    if (result != IDOK) return {};
    return std::move(dialog.restore);
}
