#include "sfx/platform/win32/service_secrets.hpp"
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <windows.h>
#include <iostream>
#include <stdexcept>
namespace p=sfx::platform::win32;
namespace {unsigned checks{};void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}}
int main(){try{
    const auto owner=p::current_process_user_sid_string();
    bool owner_activation_refused=false;
    try{p::activate_service_file_owner(owner);}catch(...){owner_activation_refused=true;}
    check(owner_activation_refused,"ordinary Desktop cannot activate System owner assignment");
    HANDLE thread_token{};
    check(!OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&thread_token)&&GetLastError()==ERROR_NO_TOKEN,
        "ordinary test starts without thread impersonation");
    {p::ServiceFileOwnerScope scope(owner);check(scope.valid(),"inactive owner scope preserves Desktop behavior");}
    check(!OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&thread_token)&&GetLastError()==ERROR_NO_TOKEN,
        "Desktop metadata scope leaves thread token unchanged");
    constexpr auto text="owned-synthetic-service-secret";
    p::configure_service_secret_scope(false);
    const auto old=p::protect_rlogin_secret(text);
    check(old&&old->at(4)==1,"ordinary Desktop protection retains user-bound v1");
    const auto old_clear=p::unprotect_rlogin_secret(*old);
    check(old_clear&&old_clear->view()==text,"original Desktop credential remains readable");
    p::configure_service_secret_scope(true);
    const auto machine=p::protect_rlogin_secret(text);
    check(machine&&machine->at(4)==2,"explicit service scope writes distinguishable v2");
    p::configure_service_secret_scope(false);
    const auto fallback=p::unprotect_rlogin_secret(*machine);
    check(fallback&&fallback->view()==text,"return to Desktop reads existing service v2 without password reset");
    p::ServiceSecretBridge bridge;bridge[*old]=*machine;p::configure_service_secret_scope(true,bridge);
    check(p::service_secret_replacement(*old)==machine,"bridge retains exact original ciphertext key");
    auto invalid=*machine;invalid[4]=1;bridge[*old]=invalid;
    bool refused=false;try{p::configure_service_secret_scope(true,bridge);}catch(...){refused=true;}
    check(refused,"bridge cannot recurse into another v1 record");
    check(p::service_secret_machine_scope(),"failed bridge replacement preserves previous active context");
    for(const auto kind:{p::FtnCredentialKind::session,p::FtnCredentialKind::packet,p::FtnCredentialKind::areafix,p::FtnCredentialKind::tic}){
        p::configure_service_secret_scope(false);
        const auto user=p::protect_ftn_secret("owned-fixture","TESTNET","TESTLINK",kind,"fixture");
        check(user&&user->at(4)==1,"each FTN credential kind retains Desktop v1");
        const auto user_clear=p::unprotect_ftn_secret("owned-fixture","TESTNET","TESTLINK",kind,*user);
        check(user_clear&&user_clear->view()=="fixture","each original FTN purpose reads exact value");
        p::configure_service_secret_scope(true);
        const auto service=p::protect_ftn_secret("owned-fixture","TESTNET","TESTLINK",kind,"fixture");
        check(service&&service->at(4)==2,"each FTN kind has explicit machine v2");
        p::configure_service_secret_scope(false);
        const auto clear=p::unprotect_ftn_secret("owned-fixture","TESTNET","TESTLINK",kind,*service);
        check(clear&&clear->view()=="fixture","each machine FTN kind remains readable after Desktop fallback");
        check(!p::unprotect_ftn_secret("different-installation","TESTNET","TESTLINK",kind,*service),"machine scope retains installation entropy binding");
        check(!p::unprotect_ftn_secret("owned-fixture","OTHER","TESTLINK",kind,*service),"machine scope retains network binding");
        check(!p::unprotect_ftn_secret("owned-fixture","TESTNET","OTHER",kind,*service),"machine scope retains link binding");
        auto corrupt=*service;corrupt.back()^=0x40;
        check(!p::unprotect_ftn_secret("owned-fixture","TESTNET","TESTLINK",kind,corrupt),"corruption fails without a replacement credential");
    }
    p::configure_service_secret_scope(false);
    check(!p::service_secret_replacement(*old),"clearing context retires in-memory aliases only");
    std::cout<<"PASS "<<checks<<" Service secret checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
