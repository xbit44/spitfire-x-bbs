#pragma once
#include "sfx/application/rlogin.hpp"
#include "sfx/application/authentication_security.hpp"
#include <algorithm>

namespace sfx::application {
// Worker-owned, move-only password memory. No database/configuration serializer
// and no global cache. A current account and byte-identical verifier are required
// every time it is used; a changed verifier destroys the stale session secret.
class CallerLoginSecret final {
public:
    void clear() noexcept {secret_.clear();account_=0;credential_.reset();}
    void capture(AccountId account,const CredentialEnvelope& credential,std::string_view password){
        clear();
        if(account<=0 || credential.password_change_required || !valid_credential_envelope(credential) ||
            password.empty() || password.size()>kPasswordMaximum ||
            !std::all_of(password.begin(),password.end(),[](unsigned char c){return c>=32&&c<=126;}))return;
        credential_=credential;secret_=RloginSecret(password);account_=account;
    }
    std::optional<RloginSecret> for_account(AccountId account,const CredentialEnvelope& current){
        if(account_!=account||!credential_||!same(*credential_,current)){clear();return {};}
        return RloginSecret(secret_.view());
    }
    bool empty()const noexcept{return secret_.view().empty();}
private:
    static bool same(const CredentialEnvelope& a,const CredentialEnvelope& b){
        return a.envelope_format_version==b.envelope_format_version&&a.scheme_identifier==b.scheme_identifier&&
            a.parameter_encoding_version==b.parameter_encoding_version&&a.canonical_parameters==b.canonical_parameters&&
            a.salt==b.salt&&a.verifier==b.verifier&&a.pepper_key_identifier==b.pepper_key_identifier&&
            a.created_utc_seconds==b.created_utc_seconds&&a.password_change_required==b.password_change_required;
    }
    AccountId account_{};
    std::optional<CredentialEnvelope> credential_;
    RloginSecret secret_;
};
}
