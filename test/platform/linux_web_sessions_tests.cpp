#include "linux_web_sessions.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
namespace web=sfx::apps::runtime::web;
unsigned checks{};
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
class Clock final:public sfx::application::IClock{
public:std::uint64_t now{};std::int64_t unix_seconds()const noexcept override{return 0;}
    std::uint64_t monotonic_millis()const noexcept override{return now;}
};
}
int main(){try{
    check(sodium_init()>=0,"randomness available");Clock clock;std::string published;
    web::BrowserSessions sessions(clock,[&](const std::string& token){published=token;});
    const auto expired=published;clock.now=300000;
    check(!sessions.launch(expired),"five minute token expires");
    check(!sessions.renew(expired),"browser token is not native renewal authority");
    check(!sessions.renew(std::string(64,'0')),"wrong native secret refused");
    auto renewed=sessions.renew(sessions.owner_secret());check(renewed&&*renewed==published&&*renewed!=expired,"native owner renews expired token");
    auto first=sessions.launch(*renewed);check(first.has_value(),"fresh local authorization");
    check(!sessions.launch(*renewed),"launch token single use");
    check(sessions.authorized(first->first,first->second).has_value(),"matching csrf accepted");
    check(!sessions.authorized(first->first,std::string(64,'0')),"incorrect csrf refused");
    const auto again=sessions.renew(sessions.owner_secret());
    const auto reused=sessions.launch(*again,first->first);check(reused==first,"repeated native launch reuses valid browser cookie");
    std::vector<std::pair<std::string,std::string>> active{*first};
    for(unsigned index=0;index<16;++index){++clock.now;const auto token=sessions.renew(sessions.owner_secret());const auto session=sessions.launch(*token);check(session.has_value(),"bounded repeated launcher opens without BBS restart");active.push_back(*session);}
    check(!sessions.authorized(first->first,{}),"oldest local browser revoked at capacity");
    unsigned valid{};for(const auto& session:active)if(sessions.authorized(session.first,{}))++valid;
    check(valid==16,"session capacity remains sixteen");
    sessions.revoke_all();for(const auto& session:active)check(!sessions.authorized(session.first,{}),"restore revokes browser sessions");
    const auto after_restore=sessions.renew(sessions.owner_secret());check(sessions.launch(*after_restore).has_value(),"native launcher recovers after revoke without restart");
    const auto idle=sessions.launch(*sessions.renew(sessions.owner_secret()));clock.now+=1800000;
    check(!sessions.authorized(idle->first,{}),"idle browser expires");
    const auto absolute=sessions.launch(*sessions.renew(sessions.owner_secret()));
    for(unsigned hour=0;hour<8;++hour){clock.now+=1799999;check(sessions.authorized(absolute->first,{}).has_value(),"activity before absolute lifetime");}
    clock.now+=28800000-8*1799999;
    check(!sessions.authorized(absolute->first,{}),"absolute lifetime expires despite activity");
    web::RemoteBrowserSessions remote(clock);const auto lan=remote.establish("192.168.1.12",1);check(lan.has_value(),"verified LAN session created");
    check(remote.authorized(lan->first,lan->second,"192.168.1.12",1).has_value(),"LAN source epoch and csrf accepted");
    check(!remote.authorized(lan->first,lan->second,"192.168.1.13",1),"stolen LAN cookie refused from another source");
    check(!remote.authorized(lan->first,std::string(64,'0'),"192.168.1.12",1),"LAN CSRF required");
    check(!sessions.authorized(lan->first,{})&&!remote.authorized(absolute->first,{},"192.168.1.12",1),"local and LAN session namespaces isolated");
    check(!remote.authorized(lan->first,lan->second,"192.168.1.12",2),"System Password epoch revokes old session");
    check(!remote.authorized(lan->first,lan->second,"192.168.1.12",1),"revoked LAN session cannot return at old epoch");
    check(!remote.establish("192.168.1.12",0)&&!remote.establish("192.168.1.12",UINT64_MAX),"invalid or exhausted verification epoch refused");
    for(unsigned i=0;i<16;++i)check(remote.establish("192.168.1.12",2).has_value(),"bounded verified LAN login");
    check(!remote.establish("192.168.1.12",2),"LAN capacity cannot be increased by repeated login");
    const auto changed=remote.establish("192.168.1.12",3);check(changed.has_value(),"new System Password epoch retires sixteen obsolete slots for a fresh browser");
    check(!remote.establish("192.168.1.12",2)&&remote.authorized(changed->first,{},"192.168.1.12",3).has_value(),"delayed old verification cannot remove newer sessions");
    for(unsigned i=0;i<15;++i)check(remote.establish("192.168.1.12",3).has_value(),"new epoch bounded capacity available");
    check(!remote.establish("192.168.1.12",3),"new epoch capacity remains sixteen");remote.revoke_all();
    const auto fresh=remote.establish("192.168.1.12",3);check(fresh.has_value(),"local recovery revokes LAN sessions");clock.now+=1800000;
    check(!remote.authorized(fresh->first,{},"192.168.1.12",3),"LAN idle session expires");
    std::cout<<"PASS "<<checks<<" local and LAN browser session checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
