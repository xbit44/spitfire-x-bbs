#include <windows.h>
#include <wincrypt.h>
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include "sfx/platform/win32/service_secrets.hpp"
#include "sfx/platform/win32/portable_credentials.hpp"
#include <algorithm>
#include <array>

namespace sfx::platform::win32 {
namespace {
constexpr std::array<std::uint8_t,5> magic{{'S','F','X','R',1}};
char purpose[]="Spitfire-X outbound RLogin secret v1";
struct NativeSecret {
    DATA_BLOB value{};
    ~NativeSecret(){if(value.pbData){SecureZeroMemory(value.pbData,value.cbData);LocalFree(value.pbData);}}
};
DATA_BLOB entropy(){return {static_cast<DWORD>(sizeof(purpose)),reinterpret_cast<BYTE*>(purpose)};}
}
std::optional<std::vector<std::uint8_t>> protect_rlogin_secret(std::string_view text){
    if(!application::valid_door_text(text,128))return {};
    DATA_BLOB input{static_cast<DWORD>(text.size()),reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))};auto e=entropy();NativeSecret output;
    const bool machine=service_secret_machine_scope();
    if(!CryptProtectData(&input,L"SFX outbound remote credential",&e,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN|(machine?CRYPTPROTECT_LOCAL_MACHINE:0),&output.value)||output.value.cbData>4096-magic.size())return {};
    std::vector<std::uint8_t> result(magic.begin(),magic.end());result[4]=machine?2:1;
    result.insert(result.end(),output.value.pbData,output.value.pbData+output.value.cbData);return result;
}
std::optional<application::RloginSecret> unprotect_rlogin_secret(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()<=magic.size()||bytes.size()>4096||!std::equal(magic.begin(),magic.begin()+4,bytes.begin())||(bytes[4]!=1&&bytes[4]!=2))return {};
    DATA_BLOB input{static_cast<DWORD>(bytes.size()-magic.size()),const_cast<BYTE*>(bytes.data()+magic.size())};auto e=entropy();NativeSecret output;
    if(!CryptUnprotectData(&input,nullptr,&e,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output.value)) {
        if(bytes[4]==1)if(const auto replacement=service_secret_replacement(bytes))return unprotect_rlogin_secret(*replacement);
        return {};
    }
    if(output.value.cbData>128)return {};
    const std::string_view text(reinterpret_cast<const char*>(output.value.pbData),output.value.cbData);
    if(!application::valid_door_text(text,128))return {};return application::RloginSecret(text);
}
std::optional<std::vector<std::uint8_t>> protect_rlogin_secret(const std::filesystem::path& root,std::string_view text){
    if(!application::valid_door_text(text,128))return {};auto key=load_portable_board_key(root);if(!key)return {};
    return seal_portable_credential(key.value->key,key.value->board,{'S','F','X','R'},"RLOGIN",text);
}
std::optional<application::RloginSecret> unprotect_rlogin_secret(const std::filesystem::path& root,const std::vector<std::uint8_t>& bytes){
    const PortableCredentialReader reader(root);if(reader.legacy_allowed()&&bytes.size()>4&&bytes[4]!=3)return unprotect_rlogin_secret(bytes);
    auto result=reader.open({'S','F','X','R'},"RLOGIN",bytes);
    if(!result||!application::valid_door_text(result->view(),128))return {};return result;
}
} // namespace sfx::platform::win32
