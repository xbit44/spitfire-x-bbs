#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <vector>
#include <algorithm>
namespace sfx::application {
using PortableCredentialGeneration=std::array<std::uint8_t,32>;
inline constexpr std::size_t kPortableCredentialGenerationOffset=5;
inline constexpr std::size_t kPortableCredentialNonceOffset=5+32;
inline constexpr std::size_t kPortableCredentialCiphertextOffset=5+32+24;
inline constexpr std::size_t kPortableCredentialMinimumBytes=kPortableCredentialCiphertextOffset+16;
inline constexpr std::size_t kPortableCredentialMaximumBytes=kPortableCredentialMinimumBytes+128;
// Shape-only metadata read. Native decryption authenticates this generation
// with board/family/purpose; it is not password or packet authorization.
inline std::optional<PortableCredentialGeneration> retained_portable_ftn_generation(const std::vector<std::uint8_t>& record)noexcept{
    constexpr std::array<std::uint8_t,5> prefix{'S','F','X','F',3};
    if(record.size()<kPortableCredentialMinimumBytes||record.size()>kPortableCredentialMaximumBytes||!std::equal(prefix.begin(),prefix.end(),record.begin()))return {};
    PortableCredentialGeneration result{};std::copy_n(record.begin()+kPortableCredentialGenerationOffset,result.size(),result.begin());return result;
}
}
