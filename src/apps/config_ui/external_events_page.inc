struct ExternalEventConfigClock final : app::IClock {
    std::int64_t unix_seconds() const noexcept override {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    std::uint64_t monotonic_millis() const noexcept override { return GetTickCount64(); }
};

std::wstring event_calendar_label(unsigned count, unsigned index) {
    constexpr std::array months{L"Jan",L"Feb",L"Mar",L"Apr",L"May",L"Jun",L"Jul",L"Aug",L"Sep",L"Oct",L"Nov",L"Dec"};
    constexpr std::array weekdays{L"Sun",L"Mon",L"Tue",L"Wed",L"Thu",L"Fri",L"Sat"};
    if (count == months.size()) return months[index];
    if (count == weekdays.size()) return weekdays[index];
    return std::to_wstring(index + 1);
}

void show_external_calendar(HWND button, unsigned mask, unsigned count) {
    const auto all = (1U << count) - 1;
    std::wstring out;
    unsigned selected{};
    for (unsigned index = 0; index < count; ++index) {
        if (mask & (1U << index)) {
            ++selected;
            if (!out.empty()) out += L", ";
            out += event_calendar_label(count, index);
        }
    }
    if (!mask) out = L"Choose";
    else if (mask == all) out = L"Any";
    else if (selected > 3) out = std::to_wstring(selected) + L" selected";
    SetWindowTextW(button, (out + L"  \x25be").c_str());
}

void pick_external_calendar(HWND button, unsigned& mask, unsigned count) {
    const auto all = (1U << count) - 1;
    RECT position{};
    GetWindowRect(button, &position);
    for (;;) {
        const auto menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING | (mask == all ? MF_CHECKED : 0), count + 1, L"Any");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (unsigned index = 0; index < count; ++index) {
            const auto label = event_calendar_label(count, index);
            const bool selected = mask != all && (mask & (1U << index));
            const auto column = count == 31 && (index == 10 || index == 20) ? MF_MENUBARBREAK : 0;
            AppendMenuW(menu, MF_STRING | column | (selected ? MF_CHECKED : 0), index + 1, label.c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, count + 2, L"Done");
        const auto selection = static_cast<unsigned>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
            position.left, position.bottom, 0, GetAncestor(button, GA_ROOT), nullptr));
        DestroyMenu(menu);
        if (!selection || selection == count + 2) return;
        mask = selection == count + 1 ? all : (mask == all ? 0 : mask) ^ (1U << (selection - 1));
        show_external_calendar(button, mask, count);
        if (selection == count + 1) return;
    }
}

std::optional<unsigned> external_event_number(std::wstring_view value, bool allow_blank = false) {
    if (value.empty()) return allow_blank ? std::optional{0U} : std::nullopt;
    unsigned number{};
    for (const auto c : value) {
        if (c < L'0' || c > L'9' || number > (UINT_MAX - (c - L'0')) / 10) return {};
        number = number * 10 + (c - L'0');
    }
    return number;
}

std::wstring external_time_text(unsigned minute) {
    const auto minute_text = std::to_wstring(minute % 60);
    return (minute < 600 ? L"0" : L"") + std::to_wstring(minute / 60) + L":" +
        (minute_text.size() == 1 ? L"0" : L"") + minute_text;
}

void update_external_event_controls(State& state) {
    auto& e = state.external;
    const bool exclusive = Button_GetCheck(e.exclusive) == BST_CHECKED;
    EnableWindow(e.cutoff, exclusive);
    EnableWindow(e.wait, exclusive);
    if (!exclusive) Button_SetCheck(e.cutoff, BST_UNCHECKED);
    EnableWindow(e.time, ComboBox_GetCurSel(e.timing) >= 0);
}

void fill_external_event(State& state, const app::ExternalEvent& event) {
    auto& e = state.external;
    e.selected = event.id;
    if (!event.id) {
        e.filling = true;
        ListView_SetItemState(e.list, -1, 0, LVIS_SELECTED);
        e.filling = false;
    }
    SetWindowTextW(e.name, utf8_to_wide(event.name).c_str());
    SetWindowTextW(e.program, utf8_to_wide(event.program).c_str());
    SetWindowTextW(e.directory, utf8_to_wide(event.directory).c_str());
    SetWindowTextW(e.arguments, utf8_to_wide(event.arguments).c_str());
    e.month_mask = event.months;
    e.date_mask = event.dates;
    e.weekday_mask = event.weekdays;
    show_external_calendar(e.months, e.month_mask, 12);
    show_external_calendar(e.dates, e.date_mask, 31);
    show_external_calendar(e.weekdays, e.weekday_mask, 7);
    ComboBox_SetCurSel(e.timing, !event.id ? -1 : event.runs_per_day ? 1 : 0);
    SetWindowTextW(e.time, !event.id ? L"" : event.runs_per_day ? std::to_wstring(event.runs_per_day).c_str() :
        external_time_text(event.minute).c_str());
    Button_SetCheck(e.enabled, event.enabled ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(e.exclusive, event.exclusive ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(e.cutoff, event.end_calls ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(e.startup, event.at_start ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(e.missed, event.run_missed ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(e.wait, event.wait_minutes ? std::to_wstring(event.wait_minutes).c_str() : L"");
    SetWindowTextW(e.warning, event.warn_minutes ? std::to_wstring(event.warn_minutes).c_str() : L"");
    EnableWindow(e.remove, e.loaded && event.id);
    EnableWindow(e.save, e.loaded);
    update_external_event_controls(state);
    remember_page(state);
}

void read_external_events(State& state) {
    auto& e = state.external;
    const auto raw = app::read_external_event_file(state.root, state.owner, app::ExternalEventFile::catalog);
    const auto catalog = !raw ? std::nullopt : raw->empty() ? std::optional{app::ExternalEventCatalog{}} :
        app::parse_external_events(*raw);
    e.loaded = catalog.has_value() && state.mode != State::ConnectionMode::blocked && !state.system_recovery_only;
    e.catalog = catalog.value_or(app::ExternalEventCatalog{});
    e.filling = true;
    ListView_DeleteAllItems(e.list);
    ExternalEventConfigClock clock;
    const auto rules = win32::WindowsRegistryBbsTimeZoneRuleProvider::create(state.configuration.saved.bbs_time_zone);
    std::optional<app::BbsLocalTimeAdapters> zone;
    if (rules) zone.emplace(rules->configuration(), *rules, clock);
    int select = 0;
    for (int row = 0; row < static_cast<int>(e.catalog.events.size()); ++row) {
        const auto& event = e.catalog.events[row];
        auto name = utf8_to_wide(event.name);
        const auto next = zone ? app::external_event_slot(event, *zone, clock.unix_seconds()) : std::nullopt;
        auto shown = next ? bbs_local_time(state, *next) : event.enabled ? L"Unavailable" : L"—";
        auto time = event.runs_per_day ? std::to_wstring(event.runs_per_day) + L" runs per day" : external_time_text(event.minute);
        if (event.at_start) time += L" + BBS startup";
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = name.data();
        ListView_InsertItem(e.list, &item);
        ListView_SetItemText(e.list, row, 1, time.data());
        ListView_SetItemText(e.list, row, 2, shown.data());
        auto enabled = yes_no(event.enabled);
        ListView_SetItemText(e.list, row, 3, enabled.data());
        if (event.id == e.selected) select = row;
    }
    if (!e.catalog.events.empty()) ListView_SetItemState(e.list, select, LVIS_SELECTED, LVIS_SELECTED);
    e.filling = false;
    fill_external_event(state, e.catalog.events.empty() ? app::ExternalEvent{} : e.catalog.events[select]);
    if (!catalog) set_status(state, L"External Events could not be read. The saved list is protected.");
}

bool save_external_event(State& state, bool erase) {
    auto& e = state.external;
    if (state.current_page != 5 || !e.loaded || state.mode == State::ConnectionMode::blocked ||
        state.system_recovery_only || state.navigation_loading) return false;
    app::ExternalEvent event;
    event.id = e.selected;
    if (erase) {
        const auto selected = std::find_if(e.catalog.events.begin(), e.catalog.events.end(),
            [&](const auto& row) { return row.id == event.id; });
        if (selected == e.catalog.events.end() ||
            MessageBoxW(state.window, (L"Delete the schedule for “" + utf8_to_wide(selected->name) +
                L"”? A run already in progress will finish.").c_str(),
                L"Delete External Event", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return false;
    } else {
        const auto name = wide_to_utf8(text(e.name));
        const auto program = wide_to_utf8(text(e.program));
        const auto directory = wide_to_utf8(text(e.directory));
        const auto argument_text = text(e.arguments);
        const auto arguments = argument_text.empty() ? std::optional{std::string{}} : wide_to_utf8(argument_text);
        const auto wait = external_event_number(text(e.wait), true);
        const auto warning = external_event_number(text(e.warning), true);
        const auto timing = ComboBox_GetCurSel(e.timing);
        const auto time = text(e.time);
        bool parsed_time{};
        if (timing == 0 && time.size() == 5 && time[2] == L':') {
            const auto hours = external_event_number(std::wstring_view(time).substr(0, 2));
            const auto minutes = external_event_number(std::wstring_view(time).substr(3));
            parsed_time = hours && minutes && *hours < 24 && *minutes < 60;
            if (parsed_time) event.minute = *hours * 60 + *minutes;
        } else if (timing == 1) {
            event.runs_per_day = external_event_number(time).value_or(0);
            parsed_time = event.runs_per_day > 0 && event.runs_per_day <= 1440;
        }
        if (!name || !program || !directory || !arguments || !wait || !warning || !parsed_time ||
            !std::filesystem::path(text(e.program)).is_absolute() || !std::filesystem::path(text(e.directory)).is_absolute()) {
            set_status(state, L"Enter full paths, choose a timing mode and enter HH:MM or a run count of 1–1,440.");
            return false;
        }
        event.name = *name; event.program = *program; event.directory = *directory; event.arguments = *arguments;
        event.enabled = Button_GetCheck(e.enabled) == BST_CHECKED;
        event.exclusive = Button_GetCheck(e.exclusive) == BST_CHECKED;
        event.end_calls = Button_GetCheck(e.cutoff) == BST_CHECKED;
        event.at_start = Button_GetCheck(e.startup) == BST_CHECKED;
        event.run_missed = Button_GetCheck(e.missed) == BST_CHECKED;
        event.wait_minutes = *wait; event.warn_minutes = *warning;
        event.months = e.month_mask;
        event.dates = e.date_mask;
        event.weekdays = e.weekday_mask;
        if (!event.months || !event.dates || !event.weekdays) {
            SetFocus(!event.months ? e.months : !event.dates ? e.dates : e.weekdays);
            set_status(state, !event.months ? L"Choose Months or Any." : !event.dates ?
                L"Choose Days of month or Any." : L"Choose Days of week or Any.");
            return false;
        }
        if (!app::valid_external_event(event)) {
            set_status(state, L"Enter an event name, a matching calendar selection and a wait limit for an exclusive event.");
            return false;
        }
    }
    const auto raw = app::read_external_event_file(state.root, state.owner, app::ExternalEventFile::catalog);
    auto saved = !raw ? std::nullopt : raw->empty() ? std::optional{app::ExternalEventCatalog{}} :
        app::parse_external_events(*raw);
    if (!saved || saved->revision != e.catalog.revision) {
        set_status(state, L"External Events changed elsewhere or could not be read. Reopen the page to reload the list.");
        return false;
    }
    const auto id = event.id ? event.id : saved->next_id;
    if (!app::change_external_events(*saved, event, erase, ExternalEventConfigClock{}.unix_seconds()) ||
        !app::publish_external_event_file(state.root, state.owner, app::ExternalEventFile::catalog,
            app::serialize_external_events(*saved))) {
        set_status(state, L"External Event save could not be verified. Review the saved list before retrying.");
        return false;
    }
    e.selected = erase ? 0 : id;
    read_external_events(state);
    set_status(state, erase ? L"Event schedule deleted." : L"External Event saved.");
    return true;
}

void browse_external_event(State& state, bool directory) {
    if (directory) {
        BROWSEINFOW browser{};
        browser.hwndOwner = state.window;
        browser.lpszTitle = L"Choose the event’s working folder";
        browser.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (auto item = SHBrowseForFolderW(&browser)) {
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(item, path)) SetWindowTextW(state.external.directory, path);
            CoTaskMemFree(item);
        }
    } else {
        wchar_t path[1024]{};
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = state.window;
        dialog.lpstrFile = path;
        dialog.nMaxFile = static_cast<DWORD>(std::size(path));
        dialog.lpstrFilter = L"Programs and scripts\0*.exe;*.bat;*.cmd;*.com\0All files\0*.*\0";
        dialog.Flags = OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&dialog)) SetWindowTextW(state.external.program, path);
    }
}

void create_external_events_page(State& state) {
    auto& e = state.external;
    const auto parent = state.pages[5] = page(state);
    auto edit = [&](int x, int y, int width) {
        return control(state, parent, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, x,y,width,26,0,WS_EX_CLIENTEDGE);
    };
    auto check = [&](const wchar_t* caption, int x, int y, int width, int id = 0) {
        return control(state,parent,L"BUTTON",caption,WS_TABSTOP | BS_AUTOCHECKBOX,x,y,width,24,id);
    };
    auto button_at = [&](const wchar_t* caption,int x,int y,int width,int id) {
        return control(state,parent,L"BUTTON",caption,WS_TABSTOP | BS_PUSHBUTTON,x,y,width,29,id);
    };
    label(state,parent,L"External Events",12,9,180,24);
    button_at(L"New Event",654,7,98,IDC_EXTERNAL_EVENT_NEW);
    e.remove = button_at(L"Delete Event",766,7,112,IDC_EXTERNAL_EVENT_DELETE);
    e.list = control(state,parent,WC_LISTVIEWW,L"",WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        12,42,866,132,IDC_EXTERNAL_EVENT_LIST,WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(e.list,LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
    struct Column { const wchar_t* title; int width; };
    const Column columns[]{{L"Event",255},{L"Schedule",207},{L"Next scheduled run",310},{L"Enabled",73}};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH};
        column.pszText = const_cast<wchar_t*>(columns[i].title);
        column.cx = MulDiv(columns[i].width, state.dpi, 96);
        ListView_InsertColumn(e.list, i, &column);
    }
    label(state,parent,L"Command",12,188,155,22);
    label(state,parent,L"Event name",12,218,177,20);
    e.name=edit(12,240,306); e.enabled=check(L"Enabled",332,240,93);
    label(state,parent,L"Program or script",12,277,300,20);
    e.program=edit(12,299,306); button_at(L"Browse...",332,298,93,IDC_EXTERNAL_EVENT_PROGRAM);
    label(state,parent,L"Working folder",12,336,300,20);
    e.directory=edit(12,358,306); button_at(L"Browse...",332,357,93,IDC_EXTERNAL_EVENT_DIRECTORY);
    label(state,parent,L"Arguments (optional)",12,395,300,20);
    e.arguments=edit(12,417,413);
    label(state,parent,L"Caller access",12,466,270,22);
    e.exclusive=check(L"Take all nodes offline for this event",12,493,413,IDC_EXTERNAL_EVENT_EXCLUSIVE);
    e.cutoff=check(L"End calls at the scheduled time",12,522,413);
    label(state,parent,L"Wait limit for callers and Doors (minutes)",12,555,291,22);
    e.wait=edit(318,552,107);
    label(state,parent,L"Schedule (BBS local time)",449,188,413,22);
    label(state,parent,L"Months",449,218,126,20);
    label(state,parent,L"Days of month",593,218,132,20);
    label(state,parent,L"Days of week",738,218,136,20);
    e.months=button_at(L"Choose",449,240,130,IDC_EXTERNAL_EVENT_MONTHS);
    e.dates=button_at(L"Choose",593,240,130,IDC_EXTERNAL_EVENT_DATES);
    e.weekdays=button_at(L"Choose",738,240,140,IDC_EXTERNAL_EVENT_WEEKDAYS);
    label(state,parent,L"Timing",449,317,256,20);
    label(state,parent,L"HH:MM or count",738,317,138,20);
    e.timing=control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP | CBS_DROPDOWNLIST,449,339,274,155,IDC_EXTERNAL_EVENT_TIMING);
    ComboBox_AddString(e.timing,L"At a specific time"); ComboBox_AddString(e.timing,L"Runs per day");
    e.time=edit(738,339,140);
    label(state,parent,L"Startup",449,390,200,22);
    e.startup=check(L"Run when the BBS starts",449,414,395);
    e.missed=check(L"Run missed event when the BBS starts",449,443,395);
    label(state,parent,L"Alerts",449,489,200,22);
    label(state,parent,L"Warn if still running after (minutes)",449,527,284,22);
    e.warning=edit(738,524,140);
    label(state,parent,L"Leave blank to wait without a warning.",449,560,428,22);
    const auto hosting = install::read_service_selection(state.root);
    if (hosting && hosting->enabled)
        label(state,parent,L"Runs as LocalSystem",449,603,298,22);
    e.save = button_at(L"Save Changes",12,602,147,IDC_EXTERNAL_EVENT_SAVE);
}
