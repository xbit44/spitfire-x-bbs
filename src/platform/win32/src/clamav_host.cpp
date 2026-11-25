#include "sfx/platform/win32/clamav_host.hpp"
#include <windows.h>
#include <cstring>
namespace sfx::platform::win32 {
ClamAvHost clamav_host() noexcept {
    // Resolve only from the already-loaded system module: no PATH, environment,
    // registry switch or product command-line override can enable AV on Win7.
    static const auto value=[]() noexcept {
        using Query=LONG (WINAPI*)(OSVERSIONINFOW*);
        const auto module=GetModuleHandleW(L"ntdll.dll");
        const auto address=module?GetProcAddress(module,"RtlGetVersion"):nullptr;
        Query query{};static_assert(sizeof(query)==sizeof(address));
        std::memcpy(&query,&address,sizeof(query));
        OSVERSIONINFOW version{};version.dwOSVersionInfoSize=sizeof(version);
        return query&&query(&version)==0?clamav_host_version(version.dwMajorVersion,version.dwMinorVersion):ClamAvHost::unavailable;
    }();
    return value;
}
} // namespace sfx::platform::win32
