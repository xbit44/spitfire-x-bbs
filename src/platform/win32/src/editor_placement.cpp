#include "sfx/platform/win32/editor_placement.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace sfx::platform::win32 {
namespace {
// One registry value atomically publishes the complete cosmetic record.
// The fixed-width Win32 structure is private, versioned and strictly validated.
struct Record {
    DWORD version{1};
    RECT normal{}, work{};
    DWORD maximized{};
    wchar_t monitor[CCHDEVICENAME]{};
};

bool valid_rectangle(const RECT& r) noexcept {
    return r.left >= -100000 && r.top >= -100000 && r.right <= 100000 &&
        r.bottom <= 100000 && r.left < r.right && r.top < r.bottom;
}

struct MonitorSearch {
    const wchar_t* name;
    HMONITOR found{};
};
BOOL CALLBACK find_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto& search = *reinterpret_cast<MonitorSearch*>(parameter);
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && wcscmp(info.szDevice, search.name) == 0) {
        search.found = monitor;
        return FALSE;
    }
    return TRUE;
}
} // namespace

RECT visible_editor_rectangle(RECT requested, RECT work, int minimum_width,
    int minimum_height) noexcept {
    if (!valid_rectangle(work)) return {};
    if (!valid_rectangle(requested)) requested = work;
    const LONG width = std::min(work.right - work.left,
        std::max(static_cast<LONG>(minimum_width), requested.right - requested.left));
    const LONG height = std::min(work.bottom - work.top,
        std::max(static_cast<LONG>(minimum_height), requested.bottom - requested.top));
    const LONG left = std::clamp(requested.left, work.left, work.right - width);
    const LONG top = std::clamp(requested.top, work.top, work.bottom - height);
    return {left, top, left + width, top + height};
}

EditorPlacementStore::EditorPlacementStore(std::wstring key) : key_(std::move(key)) {}

std::optional<EditorPlacement> EditorPlacementStore::load(
    int minimum_width, int minimum_height) const {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key_.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return std::nullopt;
    Record record{};
    DWORD type{}, bytes = sizeof(record);
    const auto result = RegQueryValueExW(key, L"Placement", nullptr, &type,
        reinterpret_cast<BYTE*>(&record), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_BINARY || bytes != sizeof(record) ||
        record.version != 1 || record.maximized > 1 ||
        record.monitor[CCHDEVICENAME - 1] != L'\0' || !record.monitor[0] ||
        !valid_rectangle(record.normal) || !valid_rectangle(record.work)) return std::nullopt;
    MonitorSearch search{record.monitor};
    EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(&search));
    MONITORINFO info{sizeof(info)};
    const HMONITOR target = search.found ? search.found :
        MonitorFromRect(&record.normal, MONITOR_DEFAULTTONEAREST);
    if (!target || !GetMonitorInfoW(target, &info)) return std::nullopt;
    // Preserve relative placement if a still-present display moved in the
    // virtual desktop. A removed display falls back to the nearest work area.
    if (search.found) OffsetRect(&record.normal,
        info.rcWork.left - record.work.left, info.rcWork.top - record.work.top);
    return EditorPlacement{visible_editor_rectangle(record.normal, info.rcWork,
        minimum_width, minimum_height), record.maximized != 0};
}

bool EditorPlacementStore::save(HWND window) const {
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(window, &placement)) return false;
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) return false;
    Record record{};
    record.normal = placement.rcNormalPosition;
    // WINDOWPLACEMENT uses workspace coordinates for ordinary top-level windows.
    OffsetRect(&record.normal, info.rcWork.left - info.rcMonitor.left,
        info.rcWork.top - info.rcMonitor.top);
    record.work = info.rcWork;
    record.maximized = IsZoomed(window) ||
        (IsIconic(window) && (placement.flags & WPF_RESTORETOMAXIMIZED)) ? 1U : 0U;
    std::copy_n(info.szDevice, CCHDEVICENAME, record.monitor);
    if (!valid_rectangle(record.normal) || !valid_rectangle(record.work)) return false;
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key_.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const auto result = RegSetValueExW(key, L"Placement", 0, REG_BINARY,
        reinterpret_cast<const BYTE*>(&record), sizeof(record));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
} // namespace sfx::platform::win32
