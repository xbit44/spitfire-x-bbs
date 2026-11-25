#pragma once

#include <string>
#include <string_view>

namespace sfx::application {

// BBS names come from the validated immutable installation configuration,
// never from authentication input. Logical newlines are encoded by the terminal.
inline std::string closed_registration_notice(const std::string& bbs_name) {
    return bbs_name + " is not accepting new users at this time.\n\n"
        "Please try again later.\n\nGoodbye.\n";
}

// Board-wide auth metadata: shown to every SSH authentication exchange while
// closed, including existing users. Do not imply that existing logins are closed.
inline std::string closed_registration_ssh_banner(const std::string& bbs_name) {
    return bbs_name + " is not accepting new users at this time.\r\n\r\n"
        "Existing users may still log in.\r\n";
}

inline constexpr std::string_view kNoNodesNotice =
    "No nodes available.\r\n\r\nPlease try again later.\r\n";
inline constexpr std::string_view kDuplicateCallerNotice =
    "You are already online on another node.\n\nGoodbye.\n";

} // namespace sfx::application
