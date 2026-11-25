#pragma once

#include <windows.h>
#include <string>

namespace sfx::platform::win32 {

enum class DesktopTheme : DWORD { classic = 0, dark_retro = 1 };

struct DesktopPalette {
    COLORREF background, panel, surface, alternate, text, muted, border, grid;
    COLORREF gold, green, cyan, amber, error, selection, selected_text, cpu;
    COLORREF banner, banner_text, banner_gold;
};

// Presentation only: one Windows-operator preference shared by both editors.
// The registered message is only an invalidation hint; its payload is ignored.
class DesktopAppearance final {
public:
    explicit DesktopAppearance(std::wstring key = L"Software\\Spitfire-X BBS\\Appearance",
        bool soft_classic_content = false);
    ~DesktopAppearance();
    DesktopAppearance(const DesktopAppearance&) = delete;
    DesktopAppearance& operator=(const DesktopAppearance&) = delete;
    DesktopTheme theme() const noexcept { return theme_; }
    const DesktopPalette& palette() const noexcept { return palette_; }
    HBRUSH panel_brush() const noexcept { return panel_; }
    HBRUSH background_brush() const noexcept { return background_; }
    HBRUSH banner_brush() const noexcept { return banner_; }
    bool reload();
    bool save(DesktopTheme theme);
    static UINT changed_message();
    HMENU append_menu(HMENU view) const;
    void check_menu(HMENU theme_menu) const;
    void apply_controls(HWND parent);
    bool control_message(HWND parent, UINT message, WPARAM wparam, LPARAM lparam,
        LRESULT& result) const;
    static constexpr UINT classic_command = 40980;
    static constexpr UINT dark_command = 40981;
private:
    void set_palette(DesktopTheme theme);
    std::wstring key_;
    bool soft_classic_content_{false};
    DesktopTheme theme_{DesktopTheme::dark_retro};
    DesktopPalette palette_{};
    HBRUSH background_{nullptr}, panel_{nullptr}, banner_{nullptr};
};

} // namespace sfx::platform::win32
