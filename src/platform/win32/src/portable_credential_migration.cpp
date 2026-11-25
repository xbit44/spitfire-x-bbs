#include "sfx/platform/win32/portable_credentials.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include <stdexcept>
namespace sfx::platform::win32 {
PortableCredentialPreparation prepare_portable_credentials(const std::filesystem::path& root,const std::wstring& owner,
    const application::FtnConfiguration& ftn,const application::DoorCatalog& doors){
    PortableCredentialPreparation result;
    auto aliases=read_portable_credential_aliases(root);auto key=load_portable_board_key(root);
    if(!aliases.empty()&&!key)throw std::runtime_error("portable_key_missing_or_invalid_restore_matching_Config_copy");
    struct Pending {application::PortableCredentialGeneration generation;PortableCredentialFamily family;std::string purpose;application::RloginSecret clear;};
    std::vector<Pending> pending;
    const auto inspect=[&](const std::vector<std::uint8_t>& original,PortableCredentialFamily family,std::string purpose,std::string connection,const auto& legacy_decode){
        if(original.empty())return;
        if(original.size()>4&&original[4]==3){
            if(!key)throw std::runtime_error("portable_key_missing_or_invalid_restore_matching_Config_copy");
            if(!open_portable_credential(key.value->key,key.value->board,family,purpose,original))result.issues.push_back({std::move(connection),"portable_credential_unreadable_original_preserved"});return;
        }
        const auto generation=legacy_credential_generation(original);const auto found=aliases.find(generation);
        if(found!=aliases.end()){
            if(!key)throw std::runtime_error("portable_key_missing_or_invalid_restore_matching_Config_copy");
            if(!open_portable_credential(key.value->key,key.value->board,family,purpose,found->second))result.issues.push_back({std::move(connection),"portable_alias_unreadable_original_preserved"});return;
        }
        auto clear=legacy_decode();if(!clear){result.issues.push_back({std::move(connection),"legacy_windows_credential_unreadable_original_preserved"});return;}
        pending.push_back({generation,family,std::move(purpose),std::move(*clear)});
    };
    for(const auto& link:ftn.links){const auto& c=link.credentials;
        for(const auto item:{std::pair{FtnCredentialKind::session,&c.session},std::pair{FtnCredentialKind::packet,&c.packet},std::pair{FtnCredentialKind::areafix,&c.areafix},std::pair{FtnCredentialKind::tic,&c.tic}})
            inspect(*item.second,{'S','F','X','F'},portable_ftn_purpose(link.network,link.code,item.first),portable_ftn_purpose(link.network,link.code,item.first),[&]{return unprotect_legacy_ftn_secret(root.u8string(),link.network,link.code,item.first,*item.second);});
    }
    for(const auto& door:doors.doors)for(const auto* value:{&door.rlogin.protected_credential,&door.rlogin.protected_pepper})
        inspect(*value,{'S','F','X','R'},"RLOGIN","Door "+std::to_string(door.id)+(value==&door.rlogin.protected_credential?" remote password":" hash pepper"),[&]{return unprotect_rlogin_secret(*value);});
    // No v3 record or retained alias reaches this point without its original
    // valid key. Unreadable legacy originals remain untouched and unavailable;
    // they do not prevent other verified conversions or a new connection save.
    if(!key){key=create_portable_board_key(root,owner);if(!key)throw std::runtime_error(key.diagnostic);}
    for(const auto& item:pending){
        if(aliases.find(item.generation)!=aliases.end())continue;
        auto converted=seal_portable_credential(key.value->key,key.value->board,item.family,item.purpose,item.clear.view(),&item.generation);
        if(!converted)throw std::runtime_error("portable_credential_conversion_failed_original_preserved");
        const auto verify=open_portable_credential(key.value->key,key.value->board,item.family,item.purpose,*converted);
        if(!verify||verify->view()!=item.clear.view())throw std::runtime_error("portable_credential_conversion_verification_failed_original_preserved");
        aliases.emplace(item.generation,std::move(*converted));
        ++result.converted;
    }
    publish_portable_credential_aliases(root,owner,aliases);
    return result;
}
}
