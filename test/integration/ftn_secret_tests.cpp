#include "sfx/platform/win32/ftn_secrets.hpp"
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;
namespace win32=sfx::platform::win32;
namespace {
unsigned checks{};
void check(bool c,const char* message){++checks;if(!c)throw std::runtime_error(message);}
}
int main(){try{
    using Kind=win32::FtnCredentialKind;const std::string installation="SFX-owned-FTN-credential-fixture";
    for(const auto kind:{Kind::session,Kind::packet,Kind::areafix,Kind::tic}){
        const auto sealed=win32::protect_ftn_secret(installation,"my-net","peer",kind,"Fixture7");
        check(sealed&&sealed->size()>8,"real owner DPAPI protects FTN credential");
        auto plain=win32::unprotect_ftn_secret(installation,"my-net","peer",kind,*sealed);
        check(plain&&plain->view()=="Fixture7","real owner DPAPI credential roundtrip");
        check(!win32::unprotect_ftn_secret("other-installation","my-net","peer",kind,*sealed),"installation binding");
        check(!win32::unprotect_ftn_secret(installation,"other-net","peer",kind,*sealed),"network binding");
        check(!win32::unprotect_ftn_secret(installation,"my-net","other-peer",kind,*sealed),"link binding");
        const auto other=kind==Kind::session?Kind::packet:Kind::session;
        check(!win32::unprotect_ftn_secret(installation,"my-net","peer",other,*sealed),"credential purpose binding");
        auto corrupt=*sealed;corrupt.back()^=1;check(!win32::unprotect_ftn_secret(installation,"my-net","peer",kind,corrupt),"corrupt envelope rejected");
        check(!win32::unprotect_ftn_secret(installation,"my-net","peer",kind,{'p','l','a','i','n'}),"arbitrary bytes are not protected credentials");
    }
    check(!win32::protect_ftn_secret(installation,"my-net","peer",Kind::packet,"NineChars"),"packet password byte limit");
    check(win32::protect_ftn_secret(installation,"my-net","peer",Kind::session,"Fixture Space").has_value(),"printable session password preserves spaces");
    app::FtnConfiguration config;app::FtnLink link;link.code="peer";link.network="my-net";link.credentials.session=*win32::protect_ftn_secret(installation,"my-net","peer",Kind::session,"Fixture7");config.links.push_back(link);
    check(win32::valid_ftn_protected_credentials(installation,config),"whole publication validates owner envelopes");
    config.links[0].credentials.tic={1,2,3};check(!win32::valid_ftn_protected_credentials(installation,config),"invalid extra credential fails publication");
    std::cout<<"PASS "<<checks<<" real Windows FTN credential checks\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
