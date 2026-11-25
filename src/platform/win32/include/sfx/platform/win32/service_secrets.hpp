#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <vector>
namespace sfx::platform::win32 {
using ServiceSecretBridge=std::map<std::vector<std::uint8_t>,std::vector<std::uint8_t>>;
// Explicit per-process policy selected from a verified installed mode record.
// The bridge contains ciphertext only; existing stored v1 blobs stay exact.
void configure_service_secret_scope(bool machine_scope,ServiceSecretBridge bridge={});
bool service_secret_machine_scope();
std::optional<std::vector<std::uint8_t>> service_secret_replacement(const std::vector<std::uint8_t>&);
}
