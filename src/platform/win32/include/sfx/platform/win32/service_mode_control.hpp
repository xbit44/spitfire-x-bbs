#pragma once
#include "sfx/platform/win32/service_mode.hpp"
#include <functional>
#include <string_view>
namespace sfx::platform::win32 {
struct ServiceModeChangeResult {bool confirmed{},cancelled{};std::string detail;bool uncertain{};};
ServiceModeChangeResult change_service_mode(const installation::ServiceMode&,bool enabled,HWND,
    const std::function<void()>& pump={},std::optional<DWORD> start_policy={});
// Recognizes only the private installed-Config worker entry. The worker derives
// its root from its own protected image; no root/command/service-name CLI input.
std::optional<int> service_mode_worker(std::wstring_view command) noexcept;
}
