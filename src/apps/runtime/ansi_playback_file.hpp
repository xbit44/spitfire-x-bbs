#pragma once

#include "sfx/application/ansi_playback_settings.hpp"

#ifdef _WIN32
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#else
#include "sfx/platform/linux/private_files.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace sfx::apps::runtime {

#ifdef _WIN32
inline std::optional<std::string> read_ansi_playback_file(
    const std::filesystem::path& root, const std::wstring& owner) {
    namespace win = platform::win32;
    namespace install = win::installation;
    const auto directory = root / L"Config";
    const auto parent = install::open_object(directory, true, READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE);
    const win::WindowsStorageDaclPolicy policy(owner);
    if (!policy.assess(directory, win::StorageDaclObjectKind::directory).status) return {};
    const auto path = directory / L"ansi-speed.ini";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return std::string{};
        return {};
    }
    // The held file denies writes and replacement during permission checks
    // and capture. Its fixed path and file kind are verified by open_object.
    const auto file = install::open_object(path, false, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
    if (!policy.assess(path, win::StorageDaclObjectKind::file, true).status) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > application::kAnsiPlaybackConfigMaximumBytes)
        return {};
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD count{};
    if (!text.empty() &&
        (!ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &count, nullptr) ||
            count != text.size()))
        return {};
    return text;
}
#else
inline std::optional<std::string> read_ansi_playback_file(const std::filesystem::path& root) {
    namespace host = platform::linux_host;
    const auto directory = host::open_owned_directory(root / "config", false);
    constexpr auto name = "ansi-speed.ini";
    struct stat status{};
    if (fstatat(directory.get(), name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return std::string{};
        return {};
    }
    const auto bytes = host::read_owned_resource(directory.get(), name,
        application::kAnsiPlaybackConfigMaximumBytes);
    return std::string(bytes.begin(), bytes.end());
}
#endif

}  // namespace sfx::apps::runtime
