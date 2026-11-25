#include "linux_runtime_host.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include <unistd.h>
#include <iostream>
namespace {
namespace app=sfx::application;namespace rt=sfx::apps::runtime;
unsigned checks{};
void check(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
class Clock final:public app::IClock{
public:std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::uint64_t monotonic_millis()const noexcept override{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}};
}
int main(int argc,char** argv){try{
    check(argc==3&&geteuid()!=0&&sfx::platform::linux_host::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"normal-user prepared fixture");
    Clock clock;rt::LinuxRuntimeHost runtime(argv[1],clock,[]{return false;});check(runtime.open().code==app::LocalControlResponseCode::ok,"stopped management opened");
    const auto authenticate=[&](std::string password,const char* source="192.168.1.12"){const auto result=runtime.authenticate_web(password,source);check(password.empty(),"host wipes submitted password on every outcome");return result;};
    const auto original=authenticate("Standalone fixture system secret 71");check(original.verified&&original.epoch==runtime.web_authentication_epoch(),"existing System Password authenticates stopped Web management");
    check(!authenticate("incorrect password").verified&&!authenticate("Standalone fixture system secret 71","not-an-ip").verified,"wrong password and invalid native source refused");
    bool throttled{};
    const auto start=std::chrono::steady_clock::now();
    for(unsigned attempt=0;attempt<40&&!throttled;++attempt){const auto result=authenticate("incorrect password");throttled=result.retry_seconds>0;}
    check(throttled&&std::chrono::steady_clock::now()-start<std::chrono::seconds(15),"durable shared throttle reported without sleeping under host lock");
    check(!authenticate("Standalone fixture system secret 71").verified,"correct password cannot bypass active delay");
    const auto clear=[&]{
        check(runtime.clear_throttle({app::RateLimitScope::source,"192.168.1.12"}).code==app::LocalControlResponseCode::ok,"source delay cleared through existing recovery");
        check(runtime.clear_throttle({app::RateLimitScope::system_password,{}}).code==app::LocalControlResponseCode::ok,"System Password delay cleared through existing recovery");
        check(runtime.clear_throttle({app::RateLimitScope::global,{}}).code==app::LocalControlResponseCode::ok,"global delay cleared through existing recovery");
    };clear();check(authenticate("Standalone fixture system secret 71").verified,"existing local throttle recovery restores Web login");
    check(runtime.start_bbs().code==app::LocalControlResponseCode::ok,"fixture BBS started explicitly");
    check(authenticate("Standalone fixture system secret 71").verified,"running Web authentication shares caller-owned rate coordinator");
    throttled=false;for(unsigned attempt=0;attempt<40&&!throttled;++attempt)throttled=authenticate("incorrect password").retry_seconds>0;
    check(throttled,"live shared throttle enforced");clear();
    auto current=runtime.account_snapshot({});check(current.system_revision>0,"actual System Password revision read");
    app::LocalAccountRequest change;change.action=app::LocalAccountAction::replace_system_password;change.expected_revision=current.system_revision;change.password="Web changed system secret 92";change.confirmation=change.password;
    const auto changed=runtime.administer_account(change);check(changed.code==app::LocalAccountResultCode::ok&&changed.committed,"existing System Password change committed");
    check(runtime.web_authentication_epoch()!=original.epoch,"System Password replacement revokes LAN proof epoch");
    check(!authenticate("Standalone fixture system secret 71").verified&&authenticate("Web changed system secret 92").verified,"old System Password refused and new one accepted");
    check(runtime.status().lifecycle.state==app::RuntimeState::online,"login attempts and password mutation do not restart running BBS");
    const auto before=runtime.web_authentication_epoch();check(runtime.revoke({app::SessionRevocationKind::account_all,1,{}})&&runtime.web_authentication_epoch()==before,"ordinary account revoke does not grant or change LAN epoch");
    check(runtime.stop_bbs().response.code==app::LocalControlResponseCode::ok,"explicit empty BBS stop");
    check(runtime.shutdown().code==app::LocalControlResponseCode::ok,"fixture retires normally");
    std::cout<<"PASS "<<checks<<" native Web System Password and shared throttle checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
