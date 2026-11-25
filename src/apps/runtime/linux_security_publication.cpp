#include "linux_security_publication.hpp"
#include <sodium.h>
#include <array>
#include <stdexcept>
namespace sfx::apps::runtime {
std::string LinuxSecurityPublication::fingerprint(std::string_view text){
    std::array<unsigned char,crypto_hash_sha256_BYTES> digest{};
    std::array<char,crypto_hash_sha256_BYTES*2+1> hex{};
    if(sodium_init()<0||crypto_hash_sha256(digest.data(),reinterpret_cast<const unsigned char*>(text.data()),text.size())!=0)
        throw std::runtime_error("security_policy_hash_unavailable");
    sodium_bin2hex(hex.data(),hex.size(),digest.data(),digest.size());
    return hex.data();
}
}
