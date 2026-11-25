#pragma once
#include <cstdint>
#include <limits>
#include <string_view>

namespace sfx::platform::win32 {
inline constexpr wchar_t edit_user_message[]=L"SFXConfig.EditUser.v1";
inline std::int64_t edit_user_argument(std::wstring_view command) {
    constexpr std::wstring_view prefix=L"--edit-user ";
    if(command.substr(0,prefix.size())!=prefix) return 0;
    command.remove_prefix(prefix.size());
    if(command.empty() || command.size()>19) return 0;
    std::int64_t id=0;
    for(wchar_t c:command) {
        if(c<L'0' || c>L'9' || id>(std::numeric_limits<std::int64_t>::max()-(c-L'0'))/10) return 0;
        id=id*10+(c-L'0');
    }
    return id;
}
inline std::int64_t edit_user_message_id(std::uint32_t low,std::uint32_t high) {
    const auto id=(static_cast<std::uint64_t>(high)<<32)|low;
    return id<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ? static_cast<std::int64_t>(id) : 0;
}
} // namespace sfx::platform::win32
