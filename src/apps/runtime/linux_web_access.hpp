#pragma once
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sfx::apps::runtime::web {
struct WebAccessSettings {
    std::uint64_t revision{1};bool enabled{};std::string address,server_name;
    std::uint16_t port{8443};std::vector<std::string> approved;
    bool custom_certificate{};
};
// Numeric exact addresses only: no DNS, proxy headers, wildcard or CIDR trust.
std::string web_ip(std::string_view);
std::string web_server_name(std::string_view);
std::string web_access_error(std::string_view);
WebAccessSettings parse_web_access(const nlohmann::json&);
nlohmann::json web_access_json(const WebAccessSettings&);
WebAccessSettings read_web_access(const std::filesystem::path& root);
void write_web_access(const std::filesystem::path& root,const WebAccessSettings&,std::uint64_t expected);
bool web_ip_allowed(const WebAccessSettings&,std::string_view);
struct WebTlsIdentity {
    std::shared_ptr<SSL_CTX> context;
    std::string fingerprint,certificate;
};
// Generation/replacement is an explicit local management action. Loading never
// silently changes the trusted identity or substitutes a missing custom key.
WebTlsIdentity load_web_tls(const std::filesystem::path& root,const WebAccessSettings&);
void generate_web_tls(const std::filesystem::path& root,std::string_view address,bool replace);
void import_web_tls(const std::filesystem::path& root,std::string_view address,
    const std::filesystem::path& certificate,const std::filesystem::path& private_key);
} // namespace sfx::apps::runtime::web
