#include "alpha_botgate_rules.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <fstream>
#include <iostream>
namespace win=sfx::platform::win32;
namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace fs=std::filesystem;
unsigned checks{};
void require(bool ok,const char* why) {++checks;if(!ok) throw std::runtime_error(why);}
template<class F> void until(F f) {
    const auto end=GetTickCount64()+6500;
    while(!f() && GetTickCount64()<end) Sleep(25);
    require(f(),"bounded live reload");
}
int main() {
    try {
        auto base=fs::current_path();require(base.parent_path().filename()=="build","build fixture only");
        auto root=base/L"section4-rule-fixtures"/win::installation::unique_id();
        auto owner=win::current_process_user_sid_string();win::WindowsStorageDaclPolicy policy(owner);
        require(fs::create_directories(root/L"BotGate"),"new fixture");
        require(policy.apply(root/L"BotGate",win::StorageDaclObjectKind::directory).succeeded(),"private role");
        app::BotGateConfiguration legacy;legacy.manual_blocks="192.0.2.1";
        win::initialize_botgate_files(root,owner,legacy);
        require(win::read_botgate_rule(root,owner,1)=="192.0.2.1\n","legacy migrated once");
        win::save_botgate_rule(root,owner,1,"192.0.2.*\n!192.0.2.3\n");
        win::initialize_botgate_files(root,owner,legacy);
        auto initial=win::load_botgate_files(root,owner);
        require(initial.ready && initial.refusal("192.0.2.4","",0)=="ip.can","canonical file active");
        require(initial.refusal("192.0.2.3","",0).empty(),"same-file exception");
        win::save_botgate_rule(root,owner,0,"192.0.2.4\n");
        require(win::load_botgate_files(root,owner).refusal("192.0.2.4","",0).empty(),"OG exemption wins over block");
        bool refused=false;try {win::save_botgate_rule(root,owner,1,"bad-ip");}catch(...) {refused=true;}
        require(refused && win::read_botgate_rule(root,owner,1).find("192.0.2.*")==0,"invalid save preserves existing");
        const auto geo=root/L"BotGate"/L"geo"/L"fixture.txt";
        const auto write=[&](const fs::path& path,const std::string& text) {
            std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<text;require(out.good(),"fixture write");out.close();
            require(policy.assess(path,win::StorageDaclObjectKind::file,true).status.succeeded(),"owner-created inherited private file accepted without ACL repair");
        };
        {
            rt::AlphaBotGateRules live(root,owner,legacy);
            require(live.snapshot()->ready,"initial immutable snapshot");
            write(geo,"deny from 198.51.100.0/24\n");
            until([&]{return live.snapshot()->refusal("198.51.100.1","",0)=="geo/fixture.txt";});
            const auto good=live.snapshot();
            write(geo,"invalid country data\n");
            until([&]{return live.status()=="botgate_geo_parse_failed";});
            require(live.snapshot()==good,"invalid country retains exact last good snapshot");
            require(fs::remove(geo),"remove only owned fixture file");
            until([&]{return live.snapshot()->geo.empty();});
            write(root/L"BotGate"/L"can"/L"ip.can","203.0.113.9\n");
            until([&]{return live.snapshot()->refusal("203.0.113.9","",0)=="ip.can";});
            require(live.snapshot()->refusal("192.0.2.1","",0).empty(),"replacement removes obsolete manual block");
            auto now=static_cast<std::int64_t>(std::time(nullptr));
            for(unsigned i=0;i<20;++i) require(live.arrive("198.51.100.8",100,now),"first twenty arrivals");
            require(!live.arrive("198.51.100.8",100,now),"rate limit creates temporary ban");
            until([&]{return live.snapshot()->refusal("198.51.100.8","",now)=="temp_ip.can";});
        }
        require(win::load_botgate_files(root,owner).refusal("198.51.100.8","",static_cast<std::int64_t>(std::time(nullptr)))=="temp_ip.can","temporary ban survives new owner");
        std::cout<<"PASS rule-file checks="<<checks<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
}
