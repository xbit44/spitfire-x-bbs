#pragma once

#include <windows.h>
#include <optional>
#include <string>

namespace sfx::platform::win32 {

// Cosmetic, per-Windows-operator state, independent of Control Panel and BBS
// configuration. Invalid/unavailable state falls back visibly, never blocks UI.
struct EditorPlacement {
    RECT rectangle{}; // normal window rectangle in screen coordinates
    bool maximized{};
};

RECT visible_editor_rectangle(RECT requested, RECT work, int minimum_width,
    int minimum_height) noexcept;

class EditorPlacementStore final {
public:
    explicit EditorPlacementStore(std::wstring key =
        L"Software\\Spitfire-X BBS\\Configuration Window");
    std::optional<EditorPlacement> load(int minimum_width, int minimum_height) const;
    bool save(HWND window) const;
private:
    std::wstring key_;
};

} // namespace sfx::platform::win32
