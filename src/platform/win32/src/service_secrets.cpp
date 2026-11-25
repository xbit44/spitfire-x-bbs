#include "sfx/platform/win32/service_secrets.hpp"
#include <mutex>
#include <algorithm>
#include <stdexcept>
namespace sfx::platform::win32 {
namespace {std::mutex secret_mutex;bool machine{};ServiceSecretBridge replacements;}
void configure_service_secret_scope(bool value,ServiceSecretBridge bridge) {
    for(const auto& item:bridge)if(item.first.size()<=5||item.second.size()<=5||item.first.size()>4096||item.second.size()>4096||
        item.first[4]!=1||item.second[4]!=2||!std::equal(item.first.begin(),item.first.begin()+4,item.second.begin()))
        throw std::runtime_error("service_secret_bridge_version_invalid");
    std::lock_guard<std::mutex> lock(secret_mutex);machine=value;replacements=std::move(bridge);
}
bool service_secret_machine_scope() {std::lock_guard<std::mutex> lock(secret_mutex);return machine;}
std::optional<std::vector<std::uint8_t>> service_secret_replacement(const std::vector<std::uint8_t>& old) {
    std::lock_guard<std::mutex> lock(secret_mutex);const auto i=replacements.find(old);
    if(i==replacements.end())return {};return i->second;
}
}
