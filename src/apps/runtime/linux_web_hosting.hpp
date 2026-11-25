#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <cstdint>
#include <string>

namespace sfx::apps::runtime::web {
struct HostingSettings {
    std::uint64_t revision{1};
    bool start_bbs_at_boot{};
};
HostingSettings parse_hosting(const nlohmann::json&);
nlohmann::json hosting_json(const HostingSettings&);
HostingSettings read_hosting(const std::filesystem::path& root);
void write_hosting(const std::filesystem::path& root,const HostingSettings&,std::uint64_t expected);
HostingSettings hosting_update(const nlohmann::json&);
nlohmann::json hosting_page(const HostingSettings&,bool service,const std::string& startup_detail);
// Produces reviewable files only. Registration always belongs to the installer
// or an explicit administrator action, never to a browser settings request.
nlohmann::json hosting_service_plan(const std::filesystem::path& executable,
    const std::filesystem::path& root,const std::filesystem::path& tzdata,
    std::uint32_t uid,std::uint32_t gid,bool low_ports);
} // namespace sfx::apps::runtime::web
