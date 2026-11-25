#pragma once
#include "sfx/application/rlogin.hpp"
#include <filesystem>

namespace sfx::platform::win32 {
// Rootless overloads are legacy DPAPI conversion/qualification only. Product
// saves/reads supply the actual installation root for portable board protection.
std::optional<std::vector<std::uint8_t>> protect_rlogin_secret(std::string_view);
std::optional<application::RloginSecret> unprotect_rlogin_secret(const std::vector<std::uint8_t>&);
std::optional<std::vector<std::uint8_t>> protect_rlogin_secret(const std::filesystem::path&,std::string_view);
std::optional<application::RloginSecret> unprotect_rlogin_secret(const std::filesystem::path&,const std::vector<std::uint8_t>&);
} // namespace sfx::platform::win32
