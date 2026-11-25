#pragma once
#include "sfx/application/rlogin.hpp"
#include "sfx/application/portable_credential.hpp"
#include <array>

namespace sfx::platform::portable {
inline constexpr std::size_t kPortableCredentialMaximum=128;
using PortableBoardIdentity=std::array<std::uint8_t,16>;
using PortableCredentialFamily=std::array<std::uint8_t,4>;
struct PortableCredentialKey {
    std::array<std::uint8_t,32> bytes{};
    PortableCredentialKey()=default;
    PortableCredentialKey(const PortableCredentialKey&)=delete;
    PortableCredentialKey& operator=(const PortableCredentialKey&)=delete;
    PortableCredentialKey(PortableCredentialKey&& other)noexcept:bytes(other.bytes){application::erase_rlogin_secret(other.bytes.data(),other.bytes.size());}
    PortableCredentialKey& operator=(PortableCredentialKey&& other)noexcept{if(this!=&other){application::erase_rlogin_secret(bytes.data(),bytes.size());bytes=other.bytes;application::erase_rlogin_secret(other.bytes.data(),other.bytes.size());}return *this;}
    ~PortableCredentialKey(){application::erase_rlogin_secret(bytes.data(),bytes.size());}
};
// No Windows account, machine, path or DPAPI input is used by this envelope.
// Key creation/storage/migration are separate explicit board operations.
std::optional<std::vector<std::uint8_t>> seal_portable_credential(const PortableCredentialKey&,
    const PortableBoardIdentity&,PortableCredentialFamily,std::string_view purpose,std::string_view plaintext,
    const application::PortableCredentialGeneration* verified_legacy_generation=nullptr);
std::optional<application::RloginSecret> open_portable_credential(const PortableCredentialKey&,
    const PortableBoardIdentity&,PortableCredentialFamily,std::string_view purpose,const std::vector<std::uint8_t>&);
}
