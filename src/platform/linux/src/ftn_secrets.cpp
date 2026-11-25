#include "sfx/platform/linux/ftn_secrets.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include <algorithm>
namespace sfx::platform::linux_host {
namespace {
bool valid_text(std::string_view text,FtnCredentialKind kind){
    const auto maximum=kind==FtnCredentialKind::packet?8U:128U;
    if(text.empty()||text.size()>maximum)return false;
    for(unsigned char c:text)if(c<32||c>126)return false;
    return true;
}
}
std::string portable_ftn_purpose(std::string_view network,std::string_view link,FtnCredentialKind kind){
    if(!application::valid_ftn_code(network)||!application::valid_ftn_code(link)||static_cast<unsigned>(kind)>3)return {};
    std::string value="FTN/";value+=network;value+='/';value+=link;value+='/';value+=static_cast<char>('0'+static_cast<unsigned>(kind));
    for(auto& c:value)if(c>='a'&&c<='z')c-=32;
    return value;
}

std::optional<std::vector<std::uint8_t>> protect_ftn_secret(std::string_view installation,std::string_view network,std::string_view link,FtnCredentialKind kind,std::string_view text){
    const auto purpose=portable_ftn_purpose(network,link,kind);
    if(purpose.empty()||!valid_text(text,kind))return {};
    auto key=load_portable_board_key(std::filesystem::u8path(installation));if(!key)return {};
    return portable::seal_portable_credential(key.value->key,key.value->board,{'S','F','X','F'},purpose,text);
}
std::optional<application::RloginSecret> unprotect_ftn_secret(std::string_view installation,std::string_view network,std::string_view link,FtnCredentialKind kind,const std::vector<std::uint8_t>& bytes){
    const auto purpose=portable_ftn_purpose(network,link,kind);if(purpose.empty())return {};
    auto key=load_portable_board_key(std::filesystem::u8path(installation));if(!key)return {};
    auto value=portable::open_portable_credential(key.value->key,key.value->board,{'S','F','X','F'},purpose,bytes);
    if(!value||!valid_text(value->view(),kind))return {};
    return value;
}
bool valid_ftn_protected_credentials(std::string_view installation,const application::FtnConfiguration& configuration){
    for(const auto& link:configuration.links){
        const auto& p=link.credentials;
        for(const auto entry:{std::pair<FtnCredentialKind,const std::vector<std::uint8_t>*>{FtnCredentialKind::session,&p.session},{FtnCredentialKind::packet,&p.packet},{FtnCredentialKind::areafix,&p.areafix},{FtnCredentialKind::tic,&p.tic}})
            if(!entry.second->empty()&&!unprotect_ftn_secret(installation,link.network,link.code,entry.first,*entry.second))return false;
    }
    return true;
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
