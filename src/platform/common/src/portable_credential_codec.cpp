#include "sfx/platform/portable_credential_codec.hpp"
#include <sodium.h>
#include <algorithm>

namespace sfx::platform::portable {
namespace {
constexpr std::size_t nonce_at=application::kPortableCredentialNonceOffset,cipher_at=application::kPortableCredentialCiphertextOffset;
constexpr std::size_t nonce_bytes=crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,tag_bytes=crypto_aead_xchacha20poly1305_ietf_ABYTES;
static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES==32);
static_assert(cipher_at==nonce_at+nonce_bytes);
std::vector<unsigned char> associated_data(const PortableBoardIdentity& board,PortableCredentialFamily family,std::string_view purpose,const application::PortableCredentialGeneration& generation){
    if(purpose.empty()||purpose.size()>1024)return {};
    constexpr std::string_view domain="Spitfire-X portable credential v3";
    std::vector<unsigned char> data(domain.begin(),domain.end());data.insert(data.end(),board.begin(),board.end());data.insert(data.end(),family.begin(),family.end());
    data.insert(data.end(),generation.begin(),generation.end());
    for(unsigned n=0;n<4;++n)data.push_back(static_cast<unsigned char>(purpose.size()>>(8*n)));
    data.insert(data.end(),purpose.begin(),purpose.end());return data;
}
struct Plaintext {std::array<unsigned char,kPortableCredentialMaximum> bytes{};~Plaintext(){sodium_memzero(bytes.data(),bytes.size());}};
}
std::optional<std::vector<std::uint8_t>> seal_portable_credential(const PortableCredentialKey& key,const PortableBoardIdentity& board,
    PortableCredentialFamily family,std::string_view purpose,std::string_view text,const application::PortableCredentialGeneration* legacy_generation){
    if(text.size()>kPortableCredentialMaximum||sodium_init()<0)return {};
    application::PortableCredentialGeneration generation{};if(legacy_generation)generation=*legacy_generation;else randombytes_buf(generation.data(),generation.size());
    const auto ad=associated_data(board,family,purpose,generation);if(ad.empty())return {};
    std::vector<std::uint8_t> record(family.begin(),family.end());record.push_back(3);record.insert(record.end(),generation.begin(),generation.end());record.resize(cipher_at+text.size()+tag_bytes);
    // The pinned libsodium XChaCha20-Poly1305 construction supports a fresh
    // random192-bit nonce for each record. Version/family/board/purpose bind it.
    randombytes_buf(record.data()+nonce_at,nonce_bytes);unsigned long long written{};
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(record.data()+cipher_at,&written,
        reinterpret_cast<const unsigned char*>(text.data()),text.size(),ad.data(),ad.size(),nullptr,record.data()+nonce_at,key.bytes.data())!=0||written!=text.size()+tag_bytes)return {};
    return record;
}
std::optional<application::RloginSecret> open_portable_credential(const PortableCredentialKey& key,const PortableBoardIdentity& board,
    PortableCredentialFamily family,std::string_view purpose,const std::vector<std::uint8_t>& record){
    if(record.size()<cipher_at+tag_bytes||record.size()>cipher_at+tag_bytes+kPortableCredentialMaximum||record[4]!=3||
        !std::equal(family.begin(),family.end(),record.begin())||sodium_init()<0)return {};
    application::PortableCredentialGeneration generation{};std::copy_n(record.begin()+application::kPortableCredentialGenerationOffset,generation.size(),generation.begin());
    const auto ad=associated_data(board,family,purpose,generation);if(ad.empty())return {};Plaintext clear;unsigned long long written{};
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(clear.bytes.data(),&written,nullptr,record.data()+cipher_at,
        record.size()-cipher_at,ad.data(),ad.size(),record.data()+nonce_at,key.bytes.data())!=0||written>clear.bytes.size())return {};
    return application::RloginSecret(std::string_view(reinterpret_cast<const char*>(clear.bytes.data()),static_cast<std::size_t>(written)));
}
}
