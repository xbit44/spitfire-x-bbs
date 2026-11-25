#pragma once
#include "sfx/application/ftn.hpp"
#include "sfx/application/rlogin.hpp"
namespace sfx::platform::win32 {
enum class FtnCredentialKind : std::uint8_t { session, packet, areafix, tic };
// New saves use the board's portable authenticated key. Legacy DPAPI decoding
// is retained for explicit, verified conversion while its Windows keys exist.
std::string portable_ftn_purpose(std::string_view network,std::string_view link,FtnCredentialKind);
std::optional<std::vector<std::uint8_t>> protect_legacy_ftn_secret(std::string_view installation,
    std::string_view network,std::string_view link,FtnCredentialKind,std::string_view);
std::optional<application::RloginSecret> unprotect_legacy_ftn_secret(std::string_view installation,
    std::string_view network,std::string_view link,FtnCredentialKind,const std::vector<std::uint8_t>&);
std::optional<std::vector<std::uint8_t>> protect_ftn_secret(std::string_view installation,
    std::string_view network,std::string_view link,FtnCredentialKind,std::string_view);
std::optional<application::RloginSecret> unprotect_ftn_secret(std::string_view installation,
    std::string_view network,std::string_view link,FtnCredentialKind,const std::vector<std::uint8_t>&);
bool valid_ftn_protected_credentials(std::string_view installation,const application::FtnConfiguration&);
// Current catalog comes from the authoritative storage snapshot. An unreadable
// unchanged value may be retained while unrelated settings are corrected.
// New/replaced credentials or changed purposes must authenticate successfully.
bool valid_changed_ftn_credentials(std::string_view installation,const application::FtnConfiguration& current,const application::FtnConfiguration& candidate);
}
