#include "alpha_botgate_rules.hpp"
#include "alpha_botgate_feed.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace app=sfx::application;namespace rt=sfx::apps::runtime;namespace host=sfx::platform::linux_host;namespace fs=std::filesystem;
namespace {unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F> bool refused(F fn){try{fn();return false;}catch(const std::exception&){return true;}}
template<class F> bool wait(F fn){for(unsigned i=0;i<400;++i){if(fn())return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return false;}
void directory(const fs::path& path){check(fs::create_directory(path)&&chmod(path.c_str(),0700)==0,"new private directory");}
void file(const fs::path& parent,const char* name,const std::string& text){auto pin=host::open_owned_directory(parent);host::publish_private_file(pin.get(),name,{text.begin(),text.end()},true);}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"explicit unprivileged fixture");const fs::path root=argv[1];check(root.is_absolute()&&!fs::exists(root),"fresh fixture");directory(root);directory(root/"config");
    const auto owner=std::to_wstring(geteuid());app::BotGateConfiguration legacy;legacy.manual_blocks="192.0.2.1";legacy.trusted_sources="192.0.2.2";legacy.domains="blocked.example";
    host::initialize_botgate_files(root,owner,legacy);const auto rules=host::load_botgate_files(root,owner);
    check(rules.ready&&rules.refusal("192.0.2.1","",1)=="ip.can","legacy IP rules migrate and filter");
    check(rules.exempt("192.0.2.2",1)&&rules.refusal("192.0.2.2","blocked.example",1).empty(),"exemption retains precedence");
    check(rules.refusal("192.0.2.3","sub.blocked.example",1)=="host.can","legacy domain includes subdomains");
    check(refused([&]{host::save_botgate_rule(root,owner,1,"bad rule !");}),"invalid candidate rejected");
    check(host::read_botgate_rule(root,owner,1)=="192.0.2.1\n","rejected candidate preserves original");
    check(refused([&]{host::save_botgate_rule(root,owner,4,"");}),"temporary list is not a normal editable rule");
    check(refused([&]{host::read_botgate_rule(root,L"0",1);}),"foreign identity refused");
    file(root/"botgate/geo","country.txt","deny from 198.51.100.0/24\n");
    check(host::load_botgate_files(root,owner).refusal("198.51.100.3","",1)=="geo/country.txt","private geo file participates");
    {
        rt::AlphaBotGateRules live(root,owner,legacy);check(live.snapshot()->ready,"immutable rules ready synchronously");
        file(root/"botgate/can","ip.can","bad rule !");
        check(wait([&]{return live.status()=="botgate_rule_parse_failed";}),"invalid reload reported");
        check(live.snapshot()->refusal("192.0.2.1","",1)=="ip.can","invalid reload retains last good snapshot");
        host::save_botgate_rule(root,owner,1,"192.0.2.5\n");check(wait([&]{return live.snapshot()->refusal("192.0.2.5","",1)=="ip.can";}),"valid reload replaces snapshot");
        const auto utc=static_cast<std::int64_t>(std::time(nullptr));for(unsigned i=0;i<20;++i)check(live.arrive("203.0.113.7",100,utc),"bounded initial arrivals admitted");
        check(!live.arrive("203.0.113.7",100,utc),"arrival burst banned");
    }
    check(host::read_botgate_rule(root,owner,4).find("203.0.113.7")!=std::string::npos,"temporary ban persisted on owned worker retirement");
    std::atomic<unsigned> mode{},fetches{};rt::AlphaBotGateFeed::Fetch fetch=[&](const std::atomic<bool>& stop,auto progress,std::string& error)->std::optional<std::string>{
        ++fetches;const auto selected=mode.load();if(selected==2){while(!stop.load())std::this_thread::sleep_for(std::chrono::milliseconds(5));error="feed_cancelled_or_timed_out";return {};}
        const std::string text=selected==1?"not an address\n":"203.0.113.10\n203.0.113.11\n";progress(text.size());return text;
    };
    {
        rt::AlphaBotGateFeed feed(root,owner,{},fetch);check(!feed.status().available&&feed.start(),"empty feed starts bounded worker");
        check(wait([&]{return feed.status().result=="feed_update_success";}),"fixture feed validated and published");
        check(feed.snapshot()->size()==2&&feed.status().available,"published numeric snapshot available");
        mode=1;check(feed.update_now(),"manual update queued");check(wait([&]{return feed.status().result=="feed_candidate_invalid";}),"invalid feed reported");
        check(feed.snapshot()->size()==2&&feed.status().available,"invalid feed keeps previous snapshot");
        mode=2;const auto previous=fetches.load();check(feed.update_now()&&wait([&]{return fetches.load()>previous;}),"cancellable request entered");
        const auto begin=std::chrono::steady_clock::now();feed.stop();check(std::chrono::steady_clock::now()-begin<std::chrono::seconds(1),"stop joins cancelled feed worker");
    }
    rt::AlphaBotGateFeed restored(root,owner,{},fetch);check(restored.status().available&&restored.snapshot()->size()==2,"persisted feed reloads without network");
    const auto collision=root/"conflict";directory(collision);directory(collision/"botgate");directory(collision/"botgate/can");file(collision/"botgate/can","ip.can","192.0.2.99\n");
    check(refused([&]{host::initialize_botgate_files(collision,owner,legacy);}),"legacy migration conflict retained");
    check(host::read_botgate_rule(collision,owner,1)=="192.0.2.99\n","migration refusal preserves user rule");
    check(symlink("../can/ip.can",(root/"botgate/geo/alias.txt").c_str())==0,"adversarial geo symlink created");
    check(!host::load_botgate_files(root,owner).ready,"symlink candidate refuses whole snapshot");
    std::cout<<"PASS "<<checks<<" Linux BotGate migration, reload, rate, feed cache and cancellation checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
