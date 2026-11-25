#include "sfx/platform/win32/desktop_theme.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
double luminance(COLORREF color) {
    const auto channel = [](BYTE value) {
        const double c = value / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(GetRValue(color)) + 0.7152 * channel(GetGValue(color)) +
        0.0722 * channel(GetBValue(color));
}
double contrast(COLORREF a, COLORREF b) {
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

LRESULT CALLBACK listener(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    using Appearance = sfx::platform::win32::DesktopAppearance;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* appearance = reinterpret_cast<Appearance*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (appearance && message != 0 && message == Appearance::changed_message())
        (void)appearance->reload(); // Like both applications: ignore the message payload.
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int main() {
    namespace win = sfx::platform::win32;
    const std::wstring key = L"Software\\Spitfire-X BBS\\Tests\\DesktopTheme-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    try {
        win::DesktopAppearance panel(key), config(key);
        require(panel.theme() == win::DesktopTheme::dark_retro, "missing setting keeps existing dark default");
        WNDCLASSW listener_class{};
        listener_class.lpfnWndProc = listener;
        listener_class.hInstance = GetModuleHandleW(nullptr);
        listener_class.lpszClassName = L"SFXDesktopThemeTestListener";
        require(RegisterClassW(&listener_class) != 0, "notification listener class registers");
        const HWND receiver = CreateWindowW(listener_class.lpszClassName, L"",
            WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
            listener_class.hInstance, &config);
        require(receiver != nullptr, "hidden notification listener created");
        require(panel.save(win::DesktopTheme::classic), "theme change broadcasts a hint");
        const auto deadline = GetTickCount64() + 1000;
        while (config.theme() != win::DesktopTheme::classic && GetTickCount64() < deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            if (config.theme() != win::DesktopTheme::classic) Sleep(5);
        }
        require(config.theme() == win::DesktopTheme::classic, "live peer reloads after posted notification");
        SendMessageW(receiver, win::DesktopAppearance::changed_message(), 1, 1);
        require(config.theme() == win::DesktopTheme::classic, "message payload cannot choose a theme");
        DestroyWindow(receiver);
        UnregisterClassW(listener_class.lpszClassName, listener_class.hInstance);
        for (auto theme : {win::DesktopTheme::classic, win::DesktopTheme::dark_retro}) {
            require(panel.save(theme), "operator selection saves");
            (void)config.reload();
            require(config.theme() == theme, "second application picks up shared choice");
            win::DesktopAppearance restarted(key);
            require(restarted.theme() == theme, "direct launch reads the saved choice");
            const auto& p = restarted.palette();
            if (theme == win::DesktopTheme::classic) {
                require(p.background == RGB(226,229,232) && p.panel == RGB(249,249,247) &&
                    p.banner == RGB(27,42,52), "Classic matches sampled prototype reference colors");
            }
            require(contrast(p.banner_text, p.banner) >= 4.5 && contrast(p.banner_gold, p.banner) >= 4.5,
                "banner text and branding remain readable in both themes");
            for (auto surface : {p.background, p.panel, p.surface, p.alternate})
                require(contrast(p.text, surface) >= 4.5, "body text remains readable on every surface");
            require(contrast(p.selected_text, p.selection) >= 4.5, "selected text remains readable");
            require(contrast(p.muted, p.panel) >= 4.5, "secondary text remains readable");
            require(contrast(p.gold, p.panel) >= 4.5, "heading accent remains readable");
            const HMENU view = CreatePopupMenu();
            const HMENU menu = restarted.append_menu(view);
            const auto chosen = theme == win::DesktopTheme::classic
                ? win::DesktopAppearance::classic_command : win::DesktopAppearance::dark_command;
            require((GetMenuState(menu, chosen, MF_BYCOMMAND) & MF_CHECKED) != 0, "menu matches persisted theme");
            DestroyMenu(view);
        }
        HKEY raw = nullptr;
        require(RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE, &raw) == ERROR_SUCCESS,
            "test key is isolated and writable");
        const DWORD invalid = 99;
        require(RegSetValueExW(raw, L"Theme", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&invalid), sizeof(invalid)) == ERROR_SUCCESS, "malformed value seeded");
        RegCloseKey(raw);
        (void)panel.reload();
        require(panel.theme() == win::DesktopTheme::dark_retro, "unknown value defaults without rewriting it");
        DWORD stored = 0, bytes = sizeof(stored), type = 0;
        require(RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &raw) == ERROR_SUCCESS,
            "malformed preference remains inspectable");
        const auto read_status = RegQueryValueExW(raw, L"Theme", nullptr, &type,
            reinterpret_cast<BYTE*>(&stored), &bytes);
        RegCloseKey(raw);
        require(read_status == ERROR_SUCCESS && type == REG_DWORD && stored == invalid,
            "fallback leaves malformed persisted value untouched");
        require(!panel.save(static_cast<win::DesktopTheme>(99)), "invalid selection cannot persist");
        require(RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()) == ERROR_SUCCESS, "only isolated test preference removed");
        std::cout << "PASS desktop theme: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
        std::cerr << "FAIL desktop theme: " << error.what() << '\n';
        return 1;
    }
}
