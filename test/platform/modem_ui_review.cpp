// Native rendering/command test of the actual CP translation unit. No Runtime
// is launched; only synthetic display snapshots and in-memory UI persistence.
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include <iostream>
namespace sfx::platform::win32 {
std::filesystem::path review_root() { return std::filesystem::path(SFX_UI_REVIEW_SCRATCH) / L"modem-ui-synthetic"; }
std::optional<application::ControlPanelUiPreferences> review_saved;
application::StorageStatus review_publish(const std::filesystem::path&,
    const std::wstring&, const application::ControlPanelUiPreferences& value) {
    review_saved = value; return application::StorageStatus::success();
}
std::optional<application::ControlPanelUiPreferences> review_read(const std::filesystem::path&) noexcept {
    return review_saved;
}
}
#define publish_control_panel_ui_preferences review_publish
#define read_control_panel_ui_preferences review_read
#define alpha_installation_root review_root
#define SFX_CONTROL_PANEL_SYNTHETIC_STATUS 1
#define wWinMain unused_product_entry
#include "../../src/apps/control_panel/main.cpp"
#undef wWinMain
#undef alpha_installation_root
#undef read_control_panel_ui_preferences
#undef publish_control_panel_ui_preferences

bool review_capture_context{}, review_context_received{};
LRESULT CALLBACK review_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CONTEXTMENU && review_capture_context) {
        review_context_received = true;
        return 0; // owned fixture observes dispatch without entering a modal menu loop
    }
    try { return window_proc(window, message, wparam, lparam); }
    catch (const std::exception& error) {
        std::cerr << "Callback " << message << " command=" << wparam << ": " << error.what() << std::endl;
        ExitProcess(90);
    }
}

int wmain(int argc, wchar_t** argv) {
    unsigned checks = 0;
    const auto require = [&](bool ok, const char* label) {
        ++checks; if (!ok) throw std::runtime_error(label);
    };
    const auto original = g_appearance.theme();
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--relay-only") {
            using Phase = ConfigurationAdminExchange::Phase;
            ConfigurationAdminRelay relay;
            require(!relay.begin(0) && !relay.begin(1), "unregistered window message has no request authority");
            {
                auto request = std::make_shared<ConfigurationAdminExchange>();
                require(request->completed != nullptr, "completion event allocated");
                request->expire();
                require(!request->begin() && request->phase.load() == Phase::cancelled,
                    "expired queued work cannot run later");
            }
            {
                auto worker = std::make_shared<ConfigurationAdminExchange>();
                std::weak_ptr<ConfigurationAdminExchange> lifetime = worker;
                auto gui = worker;
                require(gui->begin() && !gui->begin(), "request may begin only once");
                worker->expire();
                require(gui->phase.load() == Phase::running, "timeout never destroys already running work");
                worker.reset();
                require(!lifetime.expired(), "GUI owner outlives timed-out worker reference");
                gui->response.payload = {1,2,3}; gui->handled = true; gui->finish();
                require(WaitForSingleObject(gui->completed,0) == WAIT_OBJECT_0 &&
                    gui->phase.load(std::memory_order_acquire) == Phase::completed && gui->handled,
                    "actual event and release/acquire publish complete response");
                gui->expire();
                require(!gui->begin() && gui->phase.load() == Phase::completed, "completed request cannot replay");
                gui.reset();
                require(lifetime.expired(), "last owner releases complete exchange and event");
            }
            DWORD before{}, after{};
            require(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "initial handle count");
            for (unsigned index = 0; index < 100; ++index) {
                auto request = std::make_shared<ConfigurationAdminExchange>();
                bool ran = false;
                std::thread gui([&] { ran = request->begin(); if (ran) request->finish(); });
                request->expire();
                gui.join();
                require((ran && request->phase.load() == Phase::completed) ||
                    (!ran && request->phase.load() == Phase::cancelled),
                    "concurrent expiry and GUI start have one valid linearized outcome");
            }
            require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE && after == before,
                "raced exchanges leave no event or worker handle leak");
            std::cout << "CONFIGURATION RELAY PASS checks=" << checks << '\n';
            return 0;
        }
        const bool selection_only = argc == 4 && std::wstring_view(argv[3]) == L"--selection-only";
        const bool logging_only = argc == 4 && std::wstring_view(argv[3]) == L"--logging-only";
        const bool saver_only = argc == 4 && std::wstring_view(argv[3]) == L"--saver-only";
        require(argc == 3 || selection_only || logging_only || saver_only ||
            (argc == 4 && std::wstring_view(argv[3]) == L"--reporting-only"), "output path and DPI required");
        const auto output = std::filesystem::absolute(argv[1]).lexically_normal();
        require(output.parent_path() == std::filesystem::path(SFX_UI_REVIEW_SCRATCH) &&
            output.filename().wstring().find(L"modem-ui-") == 0 && !std::filesystem::exists(output), "fresh exact review output");
        require(std::filesystem::create_directories(output), "create evidence only");
        const auto dpi = static_cast<UINT>(std::stoul(argv[2]));
        require(dpi == 96 || dpi == 144, "bounded DPI");
        g_automation_dpi = dpi;
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
        auto storage = std::make_unique<AppState>();
        auto& state = *storage; state.instance = GetModuleHandleW(nullptr);
        if (saver_only) {
            // Check capacity before any high-node draw: the regression was an
            // out-of-bounds per-node position access, not a PRNG/color defect.
            require(state.saver_placement.size() == app::kAlphaNodePoolCapacity + 1,
                "screensaver position storage covers every supported node including 128");
        }
        state.preferences.system_monitor_visible = true;
        state.owner_node_view = load_owner_ansi(state.instance, IDR_OWNER_NODE_VIEW_ANSI);
        state.owner_screen_saver = load_owner_ansi(state.instance, IDR_OWNER_SCREEN_SAVER_ANSI);
        state.terminal_font = load_embedded_cp437_font(state.instance, IDR_MOEBIUS_CP437_FONT);
        require(state.owner_node_view && state.owner_screen_saver && state.terminal_font, "production ANSI/font resources");
        const auto register_class = [&](const wchar_t* name, WNDPROC procedure) {
            WNDCLASSW value{}; value.hInstance = state.instance; value.lpfnWndProc = procedure;
            value.lpszClassName = name; value.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            require(RegisterClassW(&value) != 0, "native class registers");
        };
        register_class(kWindowClass, review_window_proc); register_class(kTerminalClass, terminal_proc);
        register_class(kMonitorClass, monitor_proc);
        register_class(kStatisticsClass, statistics_proc);
        std::cerr << "Creating native controls" << std::endl;
        const HWND window = CreateWindowW(kWindowClass, L"SFX native modem/palette review — synthetic nodes",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 30, 30, MulDiv(1180,dpi,96), MulDiv(750,dpi,96),
            nullptr, nullptr, state.instance, &state);
        require(window != nullptr, "actual CP window and child controls");
        std::cerr << "Native controls created" << std::endl;
        KillTimer(window, kRefreshTimer);
        state.have_status = true; state.status.bbs_name = "Spitfire-X BBS";
        state.status.state = app::AlphaRuntimePresentationState::ready;
        state.status.runtime_epoch.bytes[0] = 1; state.status.telnet_active = true; state.status.ssh_active = true;
        state.status.configuration_loaded = true; state.status.storage.state = app::StorageRuntimeState::ready;
        for (const std::uint16_t number : std::array<std::uint16_t,3>{{1,12,128}}) {
            app::AlphaNodeStatus node; node.number = number;
            node.available = node.enabled = true; node.state = sfx::core::NodeState::ready;
            node.activity = "Idle"; node.details = "Ready for connection";
            state.status.nodes.push_back(node);
        }
        SetWindowTextW(state.subheading, L"Spitfire-X BBS  •  READY  •  Synthetic review nodes");
        state.status_text = L"Native presentation qualification — no Runtime, callers or installation writes";
        if (saver_only) {
            state.status.nodes.clear();
            for (std::uint16_t number = 1; number <= app::kAlphaNodePoolCapacity; ++number) {
                app::AlphaNodeStatus node; node.number = number;
                node.available = node.enabled = true; node.state = sfx::core::NodeState::ready;
                state.status.nodes.push_back(node);
                state.saver_placement[number].seed(42U + number);
            }
            const HDC screen = GetDC(state.terminal_view), paint = CreateCompatibleDC(screen);
            const HBITMAP bitmap = CreateCompatibleBitmap(screen, 1400, 1000);
            require(paint && bitmap, "screensaver memory paint surface");
            const auto previous_bitmap = SelectObject(paint, bitmap);
            const auto render = [&] {
                // Use the real child message and shipping draw_terminal path.
                SendMessageW(state.terminal_view, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(paint), PRF_CLIENT);
                return state.automation.painted_art;
            };
            for (const auto theme : {win32::DesktopTheme::classic, win32::DesktopTheme::dark_retro}) {
                require(g_appearance.save(theme), "select screensaver review palette");
                for (std::uint16_t number = 1; number <= app::kAlphaNodePoolCapacity; ++number) {
                    state.displayed_node = number;
                    state.node_view_interaction[number] = GetTickCount64();
                    const bool boundary = number == 1 || number == 4 || number == 5 || number == 15 || number == 128;
                    for (const auto scale : {app::NodeViewScale::auto_fit, app::NodeViewScale::percent100,
                            app::NodeViewScale::percent125, app::NodeViewScale::percent150, app::NodeViewScale::percent200}) {
                        if (!boundary && scale != app::NodeViewScale::auto_fit) continue;
                        state.node_view_scale = scale;
                        for (const auto size : {SIZE{1000,700}, SIZE{320,200}}) {
                            if (!boundary && size.cx != 1000) continue;
                            SetWindowPos(state.terminal_view, nullptr, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOZORDER);
                            RECT previous{-1,-1,-1,-1};
                            for (std::uint64_t step = 0; step < 4; ++step) {
                                state.automation.saver_steps[number] = step;
                                const RECT art = render();
                                require(state.automation.painted_saver && art.right > art.left && art.bottom > art.top,
                                    "idle node draws complete screensaver artwork");
                                require(art.left >= 0 && art.top >= 0 && art.right <= size.cx && art.bottom <= size.cy,
                                    "screensaver remains inside viewport after resize at every scale");
                                require(art.left != previous.left || art.top != previous.top,
                                    "next thirty-second step changes position for every node");
                                require(state.automation.painted_saver_color == (step + number - 1U) % 3U,
                                    "authored red white blue sequence unchanged");
                                const RECT repaint = render();
                                require(EqualRect(&art, &repaint), "ordinary repaint never moves screensaver early");
                                previous = art;
                            }
                        }
                    }
                    state.automation.saver_steps[number].reset();
                    auto& node = state.status.nodes[number - 1];
                    const auto now = GetTickCount64();
                    state.node_view_interaction[number] = now;
                    require(!screen_saver_active(state, node, now + 179999) &&
                        screen_saver_active(state, node, now + 180000), "real three-minute activation boundary");
                    state.node_view_interaction[number] = now - 181000;
                    const RECT timed_first = render();
                    state.node_view_interaction[number] -= 30000;
                    const RECT timed_next = render();
                    require(timed_first.left != timed_next.left || timed_first.top != timed_next.top,
                        "unforced shipping clock path moves at next thirty-second interval");
                    const auto before_input = state.node_view_interaction[number];
                    SendMessageW(state.terminal_view, WM_KEYDOWN, 'A', 0);
                    SendMessageW(state.terminal_view, WM_LBUTTONDOWN, 0, 0);
                    require(state.node_view_interaction[number] == before_input, "ordinary key and click leave screensaver running");
                    SendMessageW(state.terminal_view, WM_KEYDOWN, VK_HOME, 0);
                    render();
                    require(!state.automation.painted_saver, "HOME immediately returns to idle node view");
                    state.node_view_interaction[number] = now - 181000;
                    node.screen_active = node.screen_included = true;
                    render();
                    require(!state.automation.painted_saver, "live caller screen always replaces idle screensaver");
                    node.screen_active = node.screen_included = false;
                    node.enabled = false;
                    require(!screen_saver_active(state,node,now), "disabled node never starts screensaver");
                    node.enabled = true;
                }
            }
            SelectObject(paint, previous_bitmap); DeleteObject(bitmap); DeleteDC(paint); ReleaseDC(state.terminal_view,screen);
            DestroyWindow(window);
            require(g_appearance.save(original), "restore operator palette after screensaver review");
            std::ofstream report(output / L"RESULT.txt");
            report << "PASS checks=" << checks << " failed=0 skipped=0 dpi=" << dpi
                << "\nSynthetic nodes 1-128; real CP painter/input; no Runtime or installed capture.\n";
            std::cout << "SCREENSAVER UI PASS checks=" << checks << " dpi=" << dpi << '\n';
            return 0;
        }
        if (logging_only) {
            require(TabCtrl_GetItemCount(state.tab)==5,"five fixed tabs include BotGate");
            TabCtrl_SetCurSel(state.tab,4);show_selected_page(state,false,false);
            require(IsWindowVisible(state.botgate_view) && IsWindowEnabled(state.botgate_view) && state.displayed_node==0,"BotGate is not a node tab");
            RECT gate_tab{};TabCtrl_GetItemRect(state.tab,4,&gate_tab);int hit=-1;
            require(!tab_close_hit_test(state.tab,{gate_tab.right-12,(gate_tab.top+gate_tab.bottom)/2},hit),"fixed BotGate tab cannot close");
            // Exercise the production painter into a memory bitmap. BotGate
            // must share both active and inactive colors/geometry with Nodes.
            RECT rail{};GetClientRect(state.tab,&rail);
            const HDC screen=GetDC(state.tab),paint=CreateCompatibleDC(screen);
            const HBITMAP bitmap=CreateCompatibleBitmap(screen,rail.right,rail.bottom);
            const auto previous_bitmap=SelectObject(paint,bitmap);
            for(const auto theme:{win32::DesktopTheme::classic,win32::DesktopTheme::dark_retro}) {
                require(g_appearance.save(theme),"select palette for tab painter");
                for(int selected:{0,4}) {
                    TabCtrl_SetCurSel(state.tab,selected);draw_tab_rail(state.tab,paint,state);
                    for(int index:{0,4}) {
                        RECT item{};TabCtrl_GetItemRect(state.tab,index,&item);
                        require(GetPixel(paint,item.left+3,item.top+4)==(index==selected ? kPanelColor : kSurfaceColor),
                            "same tab fill with no gold separator");
                        require(GetPixel(paint,item.left,item.top+4)==(index==selected ? kGoldColor : kBorderColor),
                            "same tab border and no extra inset");
                    }
                }
            }
            require(g_appearance.save(original),"restore operator palette after tab painter");
            SelectObject(paint,previous_bitmap);DeleteObject(bitmap);DeleteDC(paint);ReleaseDC(state.tab,screen);
            TabCtrl_SetCurSel(state.tab, 2); show_selected_page(state, false, false);
            require(!IsWindowVisible(state.botgate_view) && !IsWindowEnabled(state.botgate_view),"BotGate hidden on Live Logs");
            state.logs_paused = true; // no IPC: commands and controls only, synthetic status
            const std::array<HWND,5> order{state.tab,state.log_view,state.log_pause_button,
                state.log_clear_button,state.log_filter};
            for (std::size_t i=0;i<order.size();++i) {
                SetFocus(order[i]);
                require(handle_node_page_key(state,order[i],WM_KEYDOWN,VK_TAB),"Live Logs Tab handled");
                require(GetFocus()==order[(i+1)%order.size()],"stable visible keyboard traversal");
            }
            for (int i=0;i<5;++i) {
                SendMessageW(state.log_filter,CB_SETCURSEL,i,0);
                SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_LOG_FILTER,CBN_SELCHANGE),
                    reinterpret_cast<LPARAM>(state.log_filter));
                require(state.selected_log_filter==i && !state.log_view_initialized,"filter command invalidates presentation");
            }
            state.status.logs={app::make_operational_entry(123,100,{app::OperationalKind::runtime_started,{}, {}})};
            SetWindowTextW(state.log_view,L"fixture previous view");
            require(handle_node_page_key(state,state.log_clear_button,WM_KEYDOWN,VK_RETURN),"Enter activates Clear View");
            require(state.logs_cleared_through==123 && window_text(state.log_view).find(L"Persistent operational logs were not deleted")!=std::wstring::npos,
                "clear uses authoritative sequence cutoff and explicit view-only notice");
            require(state.status.logs.size()==1 && state.status.logs.front().sequence==123,"clearing does not mutate Runtime history");
            DestroyWindow(window);
            std::ofstream report(output/L"RESULT.txt");
            report<<"PASS checks="<<checks<<" failed=0 skipped=0 dpi="<<dpi<<"\nNative production control/command fixture; synthetic Runtime, no installed UI or pixels.\n";
            std::cout<<"SECTION4 UI CONTROL PASS checks="<<checks<<" dpi="<<dpi<<'\n'; return 0;
        }
        if (selection_only) {
            const HMENU bbs = GetSubMenu(GetMenu(window), 1);
            wchar_t menu_label[80]{};
            GetMenuStringW(bbs, 3, menu_label, 80, MF_BYPOSITION);
            require(std::wstring(menu_label) == L"Graceful Shut Down", "graceful shutdown menu name");
            GetMenuStringW(bbs, ID_BBS_CANCEL_DRAIN, menu_label, 80, MF_BYCOMMAND);
            require(std::wstring(menu_label) == L"Cancel Graceful Shut Down", "graceful shutdown cancel name");
            require(GetMenuItemCount(GetSubMenu(bbs, 3)) == 3, "three existing bounded shutdown intervals");
            const auto nodes_page = [&] {
                while (!state.open_node_views.empty()) close_node_view_at(state, 5);
                TabCtrl_SetCurSel(state.tab, 0); show_selected_page(state, false, false);
                ListView_SetItemState(state.node_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            };
            for (const unsigned count : {15U, 128U}) {
                state.status.nodes.clear();
                for (unsigned number = 1; number <= count; ++number) {
                    app::AlphaNodeStatus node; node.number = static_cast<std::uint16_t>(number);
                    node.available = node.enabled = true; node.state = sfx::core::NodeState::ready;
                    state.status.nodes.push_back(node);
                }
                refresh_node_rows(state);
                for (const unsigned number : {1U, 4U, 5U, 15U, 128U}) {
                    if (number > count) continue;
                    for (unsigned action = 0; action < 3; ++action) {
                        nodes_page();
                        ListView_SetItemState(state.node_list, number - 1,
                            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                        const std::vector<std::uint16_t> expected{static_cast<std::uint16_t>(number)};
                        require(selected_node_numbers(state) == expected && node_command_targets(state) == expected,
                            "selected row maps to exact active node including higher nodes");
                        HMENU menu = CreatePopupMenu(); populate_node_menu(menu, state);
                        require((GetMenuState(menu, ID_NODE_OPEN, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)) == 0 &&
                            (GetMenuState(menu, ID_NODE_LOGON, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)) == 0 &&
                            (GetMenuState(menu, ID_NODE_DISABLE, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)) == 0,
                            "actual context menu offers open/logon/disable for selected ready node");
                        DestroyMenu(menu);
                        if (action == 0) {
                            NMHDR notification{state.node_list, IDC_NODE_LIST, NM_DBLCLK};
                            SendMessageW(window, WM_NOTIFY, IDC_NODE_LIST, reinterpret_cast<LPARAM>(&notification));
                        } else if (action == 1) SendMessageW(window, WM_COMMAND, IDC_OPEN_NODE, 0);
                        else SendMessageW(state.node_list, WM_KEYDOWN, VK_RETURN, 0);
                        require(state.open_node_views == expected && state.displayed_node == number &&
                            IsWindowVisible(state.terminal_view), "double-click/button/Enter opens exact node view");
                        SendMessageW(window, WM_COMMAND, ID_NODE_OPEN, 0);
                        require(state.open_node_views == expected, "open again never duplicates node tab");
                    }
                }
                nodes_page();
                std::vector<std::uint16_t> expected{4,5,15};
                if (count == 128) expected.push_back(128);
                for (const auto number : expected)
                    ListView_SetItemState(state.node_list, number - 1, LVIS_SELECTED, LVIS_SELECTED);
                require(node_command_targets(state) == expected, "multi-selection retains high and low node targets");
                select_context_node(state.node_list, 4);
                require(node_command_targets(state) == expected, "right-click selected member preserves group");
                ListView_EnsureVisible(state.node_list, 4, FALSE);
                RECT clicked{}; ListView_GetItemRect(state.node_list, 4, &clicked, LVIR_LABEL);
                SendMessageW(state.node_list, WM_RBUTTONDOWN, MK_RBUTTON,
                    MAKELPARAM(clicked.left + 2, (clicked.top + clicked.bottom) / 2));
                require(node_command_targets(state) == expected, "native right-button-down preserves selected group");
                review_capture_context = true; review_context_received = false;
                SendMessageW(state.node_list, WM_RBUTTONUP, 0,
                    MAKELPARAM(clicked.left + 2, (clicked.top + clicked.bottom) / 2));
                review_capture_context = false;
                require(review_context_received && node_command_targets(state) == expected,
                    "native right-button-up requests context menu with group intact");
                HMENU group_menu = CreatePopupMenu(); populate_node_menu(group_menu, state);
                require(GetMenuState(group_menu,ID_NODE_EDIT_USER,MF_BYCOMMAND)&MF_GRAYED,"Edit User is disabled for multiple selected nodes");
                wchar_t group_label[80]{};
                GetMenuStringW(group_menu, ID_NODE_DISABLE, group_label, 80, MF_BYCOMMAND);
                require(std::wstring(group_label) == L"Disable Selected Nodes" &&
                    !(GetMenuState(group_menu, ID_NODE_DISABLE, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)),
                    "group context menu offers enabled Disable Selected Nodes");
                DestroyMenu(group_menu);
                for (const auto lifecycle : {app::AlphaRuntimePresentationState::draining,
                        app::AlphaRuntimePresentationState::stopping}) {
                    state.status.state = lifecycle;
                    state.status.nodes[3].caller_connected = true;
                    state.status.nodes[3].assignment_sequence = 42;
                    state.status.nodes[4].enabled = false; // mixed group: neither mutation is allowed
                    HMENU blocked = CreatePopupMenu(); populate_node_menu(blocked, state);
                    const auto disabled = [&](UINT command) {
                        return (GetMenuState(blocked, command, MF_BYCOMMAND) & (MF_DISABLED | MF_GRAYED)) != 0;
                    };
                    require(disabled(ID_NODE_ENABLE) && disabled(ID_NODE_DISABLE),
                        "non-ready Runtime disables both node mutations for a mixed selected group");
                    require(!disabled(ID_NODE_OPEN), "inspection remains available during shutdown");
                    require(disabled(ID_NODE_DISCONNECT) == (lifecycle != app::AlphaRuntimePresentationState::draining),
                        "assigned caller disconnect allowed during drain but not stopped");
                    wchar_t explanation[128]{};
                    GetMenuStringW(blocked, GetMenuItemCount(blocked)-1, explanation, 128, MF_BYPOSITION);
                    require(std::wstring(explanation).find(lifecycle == app::AlphaRuntimePresentationState::draining ?
                        L"Graceful Shut Down" : L"not ready") != std::wstring::npos,
                        "disabled control has a human-readable Runtime-state explanation");
                    require(!node_ready_for_local_logon(state, 15), "local logon not offered during shutdown");
                    SendMessageW(window, WM_COMMAND, ID_NODE_DISABLE, 0);
                    SendMessageW(window, WM_COMMAND, ID_NODE_ENABLE, 0);
                    DestroyMenu(blocked);
                }
                state.status.state = app::AlphaRuntimePresentationState::ready;
                state.status.nodes[3].caller_connected = false;
                state.status.nodes[3].assignment_sequence = 0;
                state.status.nodes[4].enabled = true;
                SendMessageW(window, WM_COMMAND, ID_NODE_OPEN, 0);
                require(state.open_node_views == expected, "multi-selection opens every exact target");
                nodes_page();
                ListView_SetItemState(state.node_list, 3, LVIS_SELECTED, LVIS_SELECTED);
                select_context_node(state.node_list, 4);
                require(node_command_targets(state) == std::vector<std::uint16_t>{5},
                    "right-click outside group selects only the clicked node");
                const auto original_node=state.status.nodes[4];
                auto& caller=state.status.nodes[4];
                HMENU edit_menu=CreatePopupMenu();populate_node_menu(edit_menu,state);
                require(GetMenuState(edit_menu,ID_NODE_EDIT_USER,MF_BYCOMMAND)&MF_GRAYED,"idle or prelogin node cannot open a user editor");
                caller.state=sfx::core::NodeState::online;caller.caller_connected=true;
                caller.account_id=4294967297LL;caller.screen_owner.node=5;caller.screen_owner.generation=42;
                populate_node_menu(edit_menu,state);
                require(!(GetMenuState(edit_menu,ID_NODE_EDIT_USER,MF_BYCOMMAND)&MF_GRAYED) && same_edit_caller(state,&caller),
                    "one authenticated caller enables Edit User with exact 64-bit account identity");
                ++caller.screen_owner.generation;
                require(!same_edit_caller(state,&caller),"node reuse cannot edit replacement caller from stale menu");
                --caller.screen_owner.generation;caller.account_id=2;
                require(!same_edit_caller(state,&caller),"changed authenticated account refuses stale menu");
                caller=original_node;DestroyMenu(edit_menu);
                nodes_page();
                ListView_SetItemState(state.node_list, 0, LVIS_SELECTED, LVIS_SELECTED);
                for (const LPARAM invalid : {LPARAM{0}, LPARAM{129}, LPARAM{65537}, LPARAM{-1}}) {
                    LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = 0; item.lParam = invalid;
                    ListView_SetItem(state.node_list, &item);
                    require(selected_node_numbers(state).empty(), "invalid or narrowing-alias node ID rejected");
                }
                refresh_node_rows(state);
            }
            nodes_page();
            for (const auto theme : {win32::DesktopTheme::dark_retro, win32::DesktopTheme::classic,
                    win32::DesktopTheme::dark_retro}) {
                SendMessageW(window, WM_COMMAND, theme == win32::DesktopTheme::classic ?
                    win32::DesktopAppearance::classic_command : win32::DesktopAppearance::dark_command, 0);
                for (const int logical_width : {760, 1180}) {
                    SetWindowPos(window, nullptr, 0, 0, MulDiv(logical_width,dpi,96), MulDiv(750,dpi,96),
                        SWP_NOMOVE | SWP_NOZORDER);
                    RECT list{}, monitor{}, button{}, status{};
                    GetWindowRect(state.node_list,&list); GetWindowRect(state.monitor_view,&monitor);
                    GetWindowRect(state.open_node_button,&button); GetWindowRect(state.status_bar,&status);
                    require(list.bottom == monitor.bottom && monitor.bottom < button.top && button.bottom < status.top,
                        "monitor and node list end above full-width action row and unchanged status bar");
                    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_UPDATENOW);
                    const auto& monitor_text = state.automation.monitor_content;
                    require(monitor_text.find("text\tDisks\t") != std::string::npos &&
                        monitor_text.find("text\tUptime\t") != std::string::npos &&
                        monitor_text.find("card\t5\t") != std::string::npos &&
                        monitor_text.find("card\t6\t") == std::string::npos &&
                        monitor_text.find("Volume free space") == std::string::npos,
                        "six cards end at Uptime, Disks retained and Volume card removed");
                    POINT left{button.right + scaled(window,10), (button.top+button.bottom)/2};
                    POINT right{(monitor.left+monitor.right)/2,left.y};
                    ScreenToClient(window,&left); ScreenToClient(window,&right);
                    const HDC dc = GetDC(window);
                    require(GetPixel(dc,left.x,left.y) == kBackgroundColor && GetPixel(dc,right.x,right.y) == kBackgroundColor,
                        "entire action row has same theme background under list and monitor");
                    ReleaseDC(window,dc);
                }
            }
            require(capture_window_bmp(window,output/L"dark-action-row.bmp"),"corrected action row capture");
            DestroyWindow(window); require(g_appearance.save(original),"restore operator theme");
            std::ofstream report(output/L"RESULT.txt");
            report << "PASS checks=" << checks << " failed=0 skipped=0 dpi=" << dpi
                << "\nActual CP controls/handlers with 15/128 synthetic nodes; no Runtime or installation writes.\n";
            std::cout << "SELECTION/ACTION ROW PASS checks=" << checks << " dpi=" << dpi << '\n';
            return 0;
        }
        if (argc == 4) {
            state.status.state=app::AlphaRuntimePresentationState::ready;
            state.status.bbs_time_zone="Pacific Standard Time";
            state.status.statistics.successful_logons={true,3,123};
            state.status.statistics.caller_seconds={true,3671,987654};
            state.status.statistics.new_users={true,1,42};
            state.status.reporting.available=state.status.administration_history.available=true;
            for (unsigned i=0;i<state.status.reporting.counts.size();++i) state.status.reporting.counts[i]=i*100+1;
            for (unsigned i=1;i<=32;++i) {
                app::OperationalContext context; context.epoch=state.status.runtime_epoch; context.origin_sequence=i;
                state.status.reporting.recent.push_back(app::make_operational_entry(i,1'700'000'000+i,
                    {i%2 ? app::OperationalKind::drain_started : app::OperationalKind::throttle_cleared,{},context}));
                app::Identifier128 id; id.bytes[0]=static_cast<std::uint8_t>(i);
                state.status.administration_history.recent.push_back({true,8,0,0,1'700'000'100-i,id});
            }
            SetWindowPos(window,nullptr,0,0,MulDiv(760,dpi,96),MulDiv(520,dpi,96),SWP_NOMOVE|SWP_NOZORDER);
            for (const auto theme : {win32::DesktopTheme::classic,win32::DesktopTheme::dark_retro}) {
                const auto previous=g_appearance.palette(); require(g_appearance.save(theme),"report theme saves");
                apply_desktop_theme(state,previous);
                const auto name=theme==win32::DesktopTheme::classic ? L"classic" : L"dark";
                TabCtrl_SetCurSel(state.tab,3); show_selected_page(state,false,false);
                require(IsWindowVisible(state.stats_view),"actual Statistics page displayed");
                RECT stats_client{}; GetClientRect(state.stats_view,&stats_client);
                const HDC stats_dc=GetDC(state.stats_view);
                const auto prior_font=SelectObject(stats_dc,state.ui_font);
                const int label_width=(stats_client.right-scaled(window,24))/3-scaled(window,7);
                bool labels_fit=true;
                for (unsigned i=0;i<state.status.reporting.counts.size();++i) {
                    const auto label=utf8_to_wide(std::string(app::operational_counter_label(static_cast<app::OperationalCounter>(i))));
                    SIZE size{};
                    labels_fit=labels_fit && GetTextExtentPoint32W(stats_dc,label.c_str(),static_cast<int>(label.size()),&size) && size.cx<=label_width;
                }
                SelectObject(stats_dc,prior_font); ReleaseDC(state.stats_view,stats_dc);
                require(labels_fit,"every operational counter label fits minimum window without ellipsis");
                require(capture_window_bmp(window,output/(std::wstring(name)+L"-statistics-minimum.bmp")),"statistics minimum capture");
                TabCtrl_SetCurSel(state.tab,1); show_selected_page(state,false,false);
                update_scheduler_page(state);
                require((GetWindowLongPtrW(state.events_view,GWL_STYLE)&ES_READONLY)!=0,"Events remains a read-only control");
                const auto text=window_text(state.events_view);
                require(text.find(L"persisted, newest 32")!=text.npos && text.find(L"throttling_cleared")!=text.npos &&
                    text.find(L"this Runtime only")!=text.npos,"Events distinguishes real data lifetimes");
                SendMessageW(state.events_view,EM_SETSEL,50,90);
                SendMessageW(state.events_view,EM_LINESCROLL,0,15);
                const auto top=SendMessageW(state.events_view,EM_GETFIRSTVISIBLELINE,0,0);
                for (unsigned i=0;i<10;++i) update_scheduler_page(state);
                DWORD begin=0,end=0; SendMessageW(state.events_view,EM_GETSEL,reinterpret_cast<WPARAM>(&begin),reinterpret_cast<LPARAM>(&end));
                require(begin==50 && end==90 && SendMessageW(state.events_view,EM_GETFIRSTVISIBLELINE,0,0)==top,
                    "idle Events updates preserve scroll and selected text");
                SendMessageW(state.events_view,EM_SETSEL,0,0);
                SendMessageW(state.events_view,EM_LINESCROLL,0,-1000);
                require(capture_window_bmp(window,output/(std::wstring(name)+L"-events-minimum.bmp")),"events minimum capture");
            }
            DestroyWindow(window); require(g_appearance.save(original),"restore actual operator theme after reporting fixture");
            std::ofstream report(output/L"RESULT.txt");
            report << "PASS checks=" << checks << " failed=0 skipped=0 dpi=" << dpi << "\nSynthetic reporting values; actual production painters/read-only Events control.\n";
            std::cout << "REPORTING UI PASS checks=" << checks << " dpi=" << dpi << '\n';
            return 0;
        }
        refresh_node_rows(state);
        require(Header_GetItemCount(state.list_header) == 6 && !IsWindowVisible(state.modem_strip), "missing option defaults off");
        for (const auto choice : {std::make_pair(ID_VIEW_MONITOR_MEDIUM,320),
                std::make_pair(ID_VIEW_MONITOR_WIDE,400),std::make_pair(ID_VIEW_MONITOR_COMPACT,251)}) {
            SendMessageW(window,WM_COMMAND,choice.first,0);
            require(state.preferences.system_monitor_width == choice.second && win32::review_saved &&
                win32::review_saved->system_monitor_width == choice.second,"monitor width saved");
            RECT list{},monitor{}; GetWindowRect(state.node_list,&list); GetWindowRect(state.monitor_view,&monitor);
            require(list.right <= monitor.left && monitor.right-monitor.left == scaled(window,choice.second),
                "width resizes allocated space without overlay");
        }
        for (const auto theme : {win32::DesktopTheme::dark_retro, win32::DesktopTheme::classic,
                win32::DesktopTheme::dark_retro, win32::DesktopTheme::classic, win32::DesktopTheme::dark_retro}) {
            SendMessageW(window, WM_COMMAND, theme == win32::DesktopTheme::classic ?
                win32::DesktopAppearance::classic_command : win32::DesktopAppearance::dark_command, 0);
            RECT monitor{}; GetClientRect(state.monitor_view, &monitor);
            require(monitor.bottom > scaled(window, 399), "sidebar owns the unused lower area");
            const HDC dc = GetDC(state.monitor_view);
            const auto color = GetPixel(dc, monitor.right / 2, monitor.bottom - 8);
            RECT header{}; GetWindowRect(state.list_header,&header);
            const auto cpu_background = GetPixel(dc,scaled(window,6),header.bottom-header.top+scaled(window,7));
            ReleaseDC(state.monitor_view, dc);
            require(color == kPanelColor, "theme command immediately paints entire lower sidebar without resize");
            require(cpu_background == kSurfaceColor,
                "CPU card matches Network card background in both themes without stale blue fill");
        }
        for (const auto theme : {win32::DesktopTheme::classic, win32::DesktopTheme::dark_retro}) {
            const auto previous = g_appearance.palette(); require(g_appearance.save(theme), "theme saves");
            apply_desktop_theme(state, previous);
            require(kPanelColor == (theme == win32::DesktopTheme::classic ? RGB(240,240,240) : RGB(27,30,32)),
                "soft Classic content base and unchanged Dark Retro base");
            require(kAlternateSurfaceColor == (theme == win32::DesktopTheme::classic ? RGB(224,224,224) : RGB(30,34,36)),
                "darker Classic stripes and unchanged Dark Retro stripes");
            require(g_appearance.palette().cpu == (theme == win32::DesktopTheme::classic ? kSurfaceColor : RGB(37,59,104)),
                "Classic CPU card has neutral card background; Dark Retro CPU unchanged");
            require(ListView_GetBkColor(state.node_list)==kPanelColor && ListView_GetTextBkColor(state.node_list)==kPanelColor,
                "empty table and row base use the shared soft palette after theme switch");
            const auto name = theme == win32::DesktopTheme::classic ? L"classic" : L"dark";
            for (const bool on : {true,false,true}) {
                if (state.preferences.modem_lights != on) SendMessageW(window, WM_COMMAND, ID_VIEW_MODEM_LIGHTS, 0);
                require(state.preferences.modem_lights == on && win32::review_saved && win32::review_saved->modem_lights == on, "on and off persist");
                require(Header_GetItemCount(state.list_header) == (on ? 7 : 6), "toggle reclaims column");
                if (on) {
                    require(ListView_GetColumnWidth(state.node_list,1) == scaled(window,126), "fixed Modem column");
                    RECT first{}, last{}; ListView_GetSubItemRect(state.node_list,0,1,LVIR_BOUNDS,&first);
                    ListView_GetSubItemRect(state.node_list,2,1,LVIR_BOUNDS,&last);
                    require(first.left == last.left && first.right == last.right, "Node1 and Node128 LED alignment");
                    const wchar_t* names[]={L"AA",L"CD",L"RD",L"SD",L"SC",L"MR"};
                    for(int signal=0;signal<6;++signal) {
                        const auto pair=modem_pair_rect(window,first,signal);
                        const auto led=modem_led_rect(window,first,signal,true);
                        require(pair.left>=first.left && pair.right<=first.right && led.left>=pair.left && led.right<=pair.right,
                            "six labelled pairs fit unchanged Modem column");
                        require(led.right-led.left==scaled(window,8),"larger eight-pixel table lights");
                        require(led.bottom+scaled(window,9)<=first.bottom,"smaller label and larger light fit unchanged node row height");
                        for(const int y:{led.top+1,first.bottom-1}) {
                            POINT point{(pair.left+pair.right)/2,y};ClientToScreen(state.node_list,&point);
                            require(modem_tooltip_at_point(state,point).find(names[signal])!=std::wstring::npos,"whole light and label pair has correct explanation");
                        }
                    }
                }
            }
            require(capture_window_bmp(window, output / (std::wstring(name) + L"-nodes.bmp")), "nodes capture");
            state.open_node_views = {1}; TCITEMW tab{}; tab.mask=TCIF_TEXT; tab.pszText=const_cast<wchar_t*>(L"Node 1");
            TabCtrl_InsertItem(state.tab,5,&tab); TabCtrl_SetCurSel(state.tab,5); show_selected_page(state,false,false);
            RECT client{}; GetClientRect(window,&client); layout(state,client.right,client.bottom);
            update_node_header(state);
            require(window_text(state.node_header).find(L"Ready for connection") == std::wstring::npos, "redundant ready detail removed");
            RECT terminal_on{}, strip{}; GetWindowRect(state.terminal_view,&terminal_on); GetWindowRect(state.modem_strip,&strip);
            require(IsWindowVisible(state.modem_strip) && terminal_on.bottom <= strip.top, "strip outside terminal canvas");
            require(strip.left == terminal_on.left, "strip at bottom left, not right");
            require(capture_window_bmp(window, output / (std::wstring(name) + L"-node.bmp")), "node view capture");
            const auto cells = state.automation.last_painted_terminal.cells;
            SendMessageW(window,WM_COMMAND,ID_VIEW_MODEM_LIGHTS,0);
            RECT terminal_off{}; GetWindowRect(state.terminal_view,&terminal_off);
            require(!IsWindowVisible(state.modem_strip) && terminal_off.bottom-terminal_on.bottom == scaled(window,27), "off returns entire strip height");
            require(capture_window_bmp(window,output/(std::wstring(name)+L"-node-off.bmp")), "off capture");
            require(std::equal(cells.begin(), cells.end(), state.automation.last_painted_terminal.cells.begin(),
                [](const auto& a, const auto& b) { return a.character == b.character && a.attribute == b.attribute; }),
                "same authored ANSI cells with or without lights");
            state.open_node_views.clear(); TabCtrl_DeleteItem(state.tab,5); TabCtrl_SetCurSel(state.tab,0); show_selected_page(state,false,false);
            for (bool gray : {false,true,false,true}) {
                SendMessageW(window,WM_COMMAND,gray ? ID_VIEW_MONITOR_GRAYSCALE : ID_VIEW_MONITOR_COLOR,0);
                require(monitor_appearance_menu_matches(state,gray), "actual menu radio follows actual mode");
            }
        }
        DestroyWindow(window);
        require(g_appearance.save(original), "restore original shared operator theme");
        std::ofstream report(output / L"RESULT.txt");
        report << "PASS checks=" << checks << " failed=0 skipped=0 dpi=" << dpi << "\nSynthetic display; actual production controls/painters/commands. Persistence boundary mocked in memory.\n";
        std::cout << "MODEM UI PASS checks=" << checks << " dpi=" << dpi << '\n';
        return 0;
    } catch (const std::exception& error) {
        (void)g_appearance.save(original);
        std::cerr << "MODEM UI FAIL checks=" << checks << " " << error.what() << '\n'; return 1;
    }
}
