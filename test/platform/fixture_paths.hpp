#pragma once

#include <filesystem>
#include <stdexcept>

#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

namespace sfx::test {

inline std::filesystem::path fixture_checkout()
{
    const std::filesystem::path header(__FILE__);
    if (!header.is_absolute()) {
        throw std::runtime_error("Compile the fixture with an absolute source path");
    }
    auto checkout = header.parent_path().parent_path().parent_path();
    return checkout.make_preferred();
}

#ifndef _WIN32
inline std::filesystem::path fixture_account_home()
{
    const auto* account = getpwuid(geteuid());
    if (!account || !account->pw_dir) {
        throw std::runtime_error("The fixture account needs an absolute home directory");
    }
    const std::filesystem::path home(account->pw_dir);
    if (!home.is_absolute()) {
        throw std::runtime_error("The fixture account needs an absolute home directory");
    }
    return home;
}
#endif

} // namespace sfx::test
