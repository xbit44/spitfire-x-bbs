#pragma once
#include "sfx/platform/win32/service_secrets.hpp"
#include "sfx/platform/win32/portable_credentials.hpp"
#include <filesystem>
#include <string>
namespace sfx::platform::win32 {
// Ordinary installation owner only, under Setup's exclusive installation guard.
// Reads existing credentials and publishes portable aliases; database and
// configuration blobs remain byte exact. No account/password/hash conversion.
PortableCredentialPreparation prepare_portable_connection_credentials(const std::filesystem::path&,const std::wstring& owner);
void prepare_service_secret_bridge(const std::filesystem::path&,const std::wstring& owner);
ServiceSecretBridge load_service_secret_bridge(const std::filesystem::path&,const std::wstring& owner);
}
