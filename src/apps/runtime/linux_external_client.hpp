#pragma once
#include "sfx/platform/linux/private_files.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace sfx::apps::runtime::web {
struct DesktopClientProfile {
    std::uint64_t revision{1};
    unsigned type{}; // 0: cleared, 1: SyncTERM, 2: custom literal argv
    std::string executable;
    std::vector<std::string> arguments;
};
DesktopClientProfile parse_desktop_client(const nlohmann::json&);
nlohmann::json desktop_client_json(const DesktopClientProfile&);
DesktopClientProfile read_desktop_client(const std::filesystem::path& root);
void write_desktop_client(const std::filesystem::path& root,const DesktopClientProfile&,std::uint64_t expected);
DesktopClientProfile desktop_client_update(const nlohmann::json&);
nlohmann::json desktop_client_page(const DesktopClientProfile&,bool local,const std::filesystem::path& root={},const std::filesystem::path& executable={});
std::vector<std::string> desktop_client_arguments(const DesktopClientProfile&,std::uint16_t port);
struct PinnedDesktopClient {
    platform::linux_host::FileDescriptor file;
    std::uint64_t device{},inode{};
    std::array<unsigned char,32> digest{};
    bool script{};
};
PinnedDesktopClient pin_desktop_client(const DesktopClientProfile&);
bool same_desktop_client(const PinnedDesktopClient&,const PinnedDesktopClient&) noexcept;
// Forks the pinned program with literal argv and inherited desktop environment.
// Returns the child PID after exec, or throws after reaping a failed exec.
int spawn_desktop_client(const PinnedDesktopClient&,const DesktopClientProfile&,std::uint16_t port);
} // namespace sfx::apps::runtime::web
