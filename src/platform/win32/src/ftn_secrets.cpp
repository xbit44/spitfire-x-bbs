#include <windows.h>
#include <wincrypt.h>
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/platform/win32/service_secrets.hpp"
#include "sfx/platform/win32/portable_credentials.hpp"
#include <algorithm>
#include <array>

namespace sfx::platform::win32 {
namespace {
constexpr std::array<std::uint8_t,5> magic{{'S','F','X','F',1}};
struct NativeSecret {DATA_BLOB value{};~NativeSecret(){if(value.pbData){SecureZeroMemory(value.pbData,value.cbData);LocalFree(value.pbData);}}};
std::string purpose(std::string_view installation,std::string_view network,std::string_view link,FtnCredentialKind kind){
    if(installation.empty()||installation.size()>1024||!application::valid_ftn_code(network)||!application::valid_ftn_code(link)||static_cast<unsigned>(kind)>3)return {};
    std::string out="Spitfire-X FTN credential v1";
    for(const auto part:{installation,network,link}){out+='\0';out+=part;}out+='\0';out+=static_cast<char>('0'+static_cast<unsigned>(kind));return out;
}
bool valid_text(std::string_view text,FtnCredentialKind kind){
    const auto max=kind==FtnCredentialKind::packet?8U:128U;
    if(text.empty()||text.size()>max)return false;for(unsigned char c:text)if(c<32||c>126)return false;return true;
}
DATA_BLOB blob(std::string& s){return {static_cast<DWORD>(s.size()),reinterpret_cast<BYTE*>(s.data())};}
}
std::optional<std::vector<std::uint8_t>> protect_legacy_ftn_secret(std::string_view installation,std::string_view network,std::string_view link,FtnCredentialKind kind,std::string_view text){
    auto entropy=purpose(installation,network,link,kind);if(entropy.empty()||!valid_text(text,kind))return {};
    DATA_BLOB input{static_cast<DWORD>(text.size()),reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))};auto e=blob(entropy);NativeSecret output;
    const bool machine=service_secret_machine_scope();
    if(!CryptProtectData(&input,L"SFX FTN credential",&e,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN|(machine?CRYPTPROTECT_LOCAL_MACHINE:0),&output.value)||output.value.cbData>4096-magic.size())return {};
    std::vector<std::uint8_t> result(magic.begin(),magic.end());result[4]=machine?2:1;
    result.insert(result.end(),output.value.pbData,output.value.pbData+output.value.cbData);return result;
}
std::optional<application::RloginSecret> unprotect_legacy_ftn_secret(std::string_view installation,std::string_view network,std::string_view link,FtnCredentialKind kind,const std::vector<std::uint8_t>& bytes){
    auto entropy=purpose(installation,network,link,kind);
    if(entropy.empty()||bytes.size()<=magic.size()||bytes.size()>4096||!std::equal(magic.begin(),magic.begin()+4,bytes.begin())||(bytes[4]!=1&&bytes[4]!=2))return {};
    DATA_BLOB input{static_cast<DWORD>(bytes.size()-magic.size()),const_cast<BYTE*>(bytes.data()+magic.size())};auto e=blob(entropy);NativeSecret output;
    if(!CryptUnprotectData(&input,nullptr,&e,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output.value)) {
        if(bytes[4]==1)if(const auto replacement=service_secret_replacement(bytes))
            return unprotect_legacy_ftn_secret(installation,network,link,kind,*replacement);
        return {};
    }
    const std::string_view text(reinterpret_cast<const char*>(output.value.pbData),output.value.cbData);if(!valid_text(text,kind))return {};
    return application::RloginSecret(text);
}
std::string portable_ftn_purpose(std::string_view network,std::string_view link,FtnCredentialKind kind){
    if(!application::valid_ftn_code(network)||!application::valid_ftn_code(link)||static_cast<unsigned>(kind)>3)return {};
    std::string value="FTN/";value+=network;value+='/';value+=link;value+='/';value+=static_cast<char>('0'+static_cast<unsigned>(kind));
    for(auto& c:value)if(c>='a'&&c<='z')c-=32;return value;
}
std::optional<std::vector<std::uint8_t>> protect_ftn_secret(std::string_view installation,std::string_view network,std::string_view link,FtnCredentialKind kind,std::string_view text){
    if(!valid_text(text,kind))return {};auto key=load_portable_board_key(std::filesystem::u8path(installation));if(!key)return {};
    return seal_portable_credential(key.value->key,key.value->board,{'S','F','X','F'},portable_ftn_purpose(network,link,kind),text);
}
std::optional<application::RloginSecret> unprotect_ftn_secret(std::string_view installation,std::string_view network,std::string_view link,FtnCredentialKind kind,const std::vector<std::uint8_t>& bytes){
    const PortableCredentialReader reader(std::filesystem::u8path(installation));
    if(reader.legacy_allowed()&&bytes.size()>4&&bytes[4]!=3)return unprotect_legacy_ftn_secret(installation,network,link,kind,bytes);
    auto result=reader.open({'S','F','X','F'},portable_ftn_purpose(network,link,kind),bytes);
    if(!result||!valid_text(result->view(),kind))return {};return result;
}
bool valid_ftn_protected_credentials(std::string_view installation,const application::FtnConfiguration& c){
    const PortableCredentialReader reader(std::filesystem::u8path(installation));
    for(const auto& link:c.links){const auto& p=link.credentials;
        for(const auto entry:{std::pair<FtnCredentialKind,const std::vector<std::uint8_t>*>{FtnCredentialKind::session,&p.session},{FtnCredentialKind::packet,&p.packet},{FtnCredentialKind::areafix,&p.areafix},{FtnCredentialKind::tic,&p.tic}})
            if(!entry.second->empty()){
                auto clear=reader.legacy_allowed()?unprotect_legacy_ftn_secret(installation,link.network,link.code,entry.first,*entry.second):reader.open({'S','F','X','F'},portable_ftn_purpose(link.network,link.code,entry.first),*entry.second);
                if(!clear||!valid_text(clear->view(),entry.first))return false;
            }
    }return true;
}
bool valid_changed_ftn_credentials(std::string_view installation,const application::FtnConfiguration& current,const application::FtnConfiguration& candidate){
    auto changed=candidate;
    for(auto& link:changed.links){
        const auto purpose=portable_ftn_purpose(link.network,link.code,FtnCredentialKind::session);
        const auto old=std::find_if(current.links.begin(),current.links.end(),[&](const auto& prior){return portable_ftn_purpose(prior.network,prior.code,FtnCredentialKind::session)==purpose;});
        if(old==current.links.end())continue;
        const auto retain=[](auto& value,const auto& prior){if(value==prior)value.clear();};
        retain(link.credentials.session,old->credentials.session);retain(link.credentials.packet,old->credentials.packet);
        retain(link.credentials.areafix,old->credentials.areafix);retain(link.credentials.tic,old->credentials.tic);
    }
    return valid_ftn_protected_credentials(installation,changed);
}
}
