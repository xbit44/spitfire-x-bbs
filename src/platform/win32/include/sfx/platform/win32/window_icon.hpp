#pragma once

#include <windows.h>

namespace sfx::platform::win32 {

// LR_SHARED caches by resource identity, not by the requested dimensions.
// Own each size independently so title-bar and task-switcher icons use their
// native ICO entries. The owner must outlive every window using this handle.
class WindowIcon final {
public:
    WindowIcon(HINSTANCE instance, int resource, int width, int height) noexcept
        : handle_(static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(resource),
              IMAGE_ICON, width, height, 0))) {}
    ~WindowIcon() { if (handle_) DestroyIcon(handle_); }
    WindowIcon(const WindowIcon&) = delete;
    WindowIcon& operator=(const WindowIcon&) = delete;
    HICON get() const noexcept { return handle_; }
private:
    HICON handle_{};
};

} // namespace sfx::platform::win32
