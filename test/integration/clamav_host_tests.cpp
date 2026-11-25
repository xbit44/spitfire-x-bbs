#include "sfx/platform/win32/clamav_host.hpp"
#include <iostream>
#include <stdexcept>
namespace win=sfx::platform::win32;
int main(){try{
    unsigned n{};const auto check=[&](bool x){++n;if(!x)throw std::runtime_error("host policy mismatch");};
    check(win::clamav_host_version(6,1)==win::ClamAvHost::windows7_no_av);
    for(const auto major:{10U,11U,12U})check(win::clamav_host_version(major,0)==win::ClamAvHost::enabled);
    check(win::clamav_host_version(6,2)==win::ClamAvHost::enabled);check(win::clamav_host_version(6,3)==win::ClamAvHost::enabled);
    check(win::clamav_host_version(0,0)==win::ClamAvHost::unavailable);check(win::clamav_host_version(5,1)==win::ClamAvHost::unavailable);
    check(win::clamav_available()&&!win::windows7_no_av());
    std::cout<<"PASS host policy "<<n<<" checks; actual supported host plus synthetic version matrix\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
