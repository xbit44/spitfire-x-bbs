#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace sfx::apps::runtime {
bool load_linux_ssh_host_key(const std::filesystem::path& root,
    const std::wstring& owner, std::string& diagnostic,
    std::uint64_t& generation_attempts, std::uint64_t& load_attempts);
}
