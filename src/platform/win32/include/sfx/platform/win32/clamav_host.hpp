#pragma once
#include <cstdint>
namespace sfx::platform::win32 {
enum class ClamAvHost { unavailable, windows7_no_av, enabled };
// Windows 8/8.1 retain their previous behavior; this change is Win7-only.
constexpr ClamAvHost clamav_host_version(std::uint32_t major,std::uint32_t minor) noexcept {
    return major==6&&minor==1?ClamAvHost::windows7_no_av:
        major>=10||(major==6&&(minor==2||minor==3))?ClamAvHost::enabled:ClamAvHost::unavailable;
}
ClamAvHost clamav_host() noexcept;
inline bool clamav_available() noexcept {return clamav_host()==ClamAvHost::enabled;}
inline bool windows7_no_av() noexcept {return clamav_host()==ClamAvHost::windows7_no_av;}
} // namespace sfx::platform::win32
