#include <windows.h>
#include <bcrypt.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include "sfx/platform/win32/ftn_crypto.hpp"
#include <algorithm>
#include <array>
namespace sfx::platform::win32 {
namespace {
int digit(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
}
std::optional<std::string> ftn_cram_challenge(){
    // IREX 2.29 hashes only the first 16 challenge bytes. Generate a complete
    // 128-bit random challenge so legacy peers and full-challenge verifiers agree.
    std::array<unsigned char,16> nonce{};
    if(BCryptGenRandom(nullptr,nonce.data(),static_cast<ULONG>(nonce.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return {};
    std::string hex;hex.reserve(nonce.size()*2);for(const auto c:nonce){hex+="0123456789abcdef"[c>>4];hex+="0123456789abcdef"[c&15];}return hex;
}
std::optional<application::RloginSecret> ftn_cram_response(std::string_view secret,std::string_view challenge){
    if(secret.empty()||secret.size()>128||challenge.size()<16||challenge.size()>128||challenge.size()%2)return {};
    std::array<unsigned char,64> decoded{};for(std::size_t i=0;i<challenge.size()/2;++i){const auto a=digit(challenge[i*2]),b=digit(challenge[i*2+1]);if(a<0||b<0)return {};decoded[i]=static_cast<unsigned char>((a<<4)|b);}
    std::array<unsigned char,16> digest{};std::size_t size{};
    if(!EVP_Q_mac(nullptr,"HMAC",nullptr,"MD5",nullptr,secret.data(),secret.size(),decoded.data(),challenge.size()/2,digest.data(),digest.size(),&size)||size!=digest.size())return {};
    std::array<char,41> response{};constexpr std::string_view prefix="CRAM-MD5-";std::copy(prefix.begin(),prefix.end(),response.begin());
    for(std::size_t i=0;i<digest.size();++i){response[prefix.size()+i*2]="0123456789abcdef"[digest[i]>>4];response[prefix.size()+i*2+1]="0123456789abcdef"[digest[i]&15];}
    application::RloginSecret result(std::string_view(response.data(),response.size()));SecureZeroMemory(response.data(),response.size());SecureZeroMemory(digest.data(),digest.size());return result;
}
bool verify_ftn_cram(std::string_view secret,std::string_view challenge,std::string_view response){
    return check_ftn_cram(secret,challenge,response)==FtnCramCheck::accepted;
}
FtnCramCheck check_ftn_cram(std::string_view secret,std::string_view challenge,std::string_view response){
    if(response.size()==42&&response.back()=='\0')return FtnCramCheck::trailing_nul;
    if(response.size()!=41)return FtnCramCheck::length;
    if(response.substr(0,9)!="CRAM-MD5-")return FtnCramCheck::encoding;
    std::array<char,32> actual{};bool encoding=true;
    for(std::size_t i=0;i<actual.size();++i){const auto c=response[i+9];if(digit(c)<0)encoding=false;actual[i]=c>='A'&&c<='F'?static_cast<char>(c+32):c;}
    const auto expected=encoding?ftn_cram_response(secret,challenge):std::nullopt;
    const auto result=!encoding?FtnCramCheck::encoding:!expected?FtnCramCheck::unavailable:
        CRYPTO_memcmp(actual.data(),expected->view().data()+9,actual.size())==0?FtnCramCheck::accepted:FtnCramCheck::digest_mismatch;
    SecureZeroMemory(actual.data(),actual.size());return result;
}
}
