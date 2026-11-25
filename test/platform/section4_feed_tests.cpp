#include "alpha_botgate_feed.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <fstream>
#include <iostream>
#include <thread>
namespace rt=sfx::apps::runtime;
namespace win=sfx::platform::win32;
namespace fs=std::filesystem;
unsigned checks{};
void require(bool value,const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
std::string read(const fs::path& path) { std::ifstream f(path,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
template<class F> void until(F predicate) {
    const auto deadline=GetTickCount64()+5000;
    while (!predicate() && GetTickCount64()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(predicate(),"bounded asynchronous completion");
}
int main(int argc,char** argv) {
    try {
        const auto base=fs::current_path();
        require(base.parent_path().filename()=="build","fixture restricted to build subtree");
        const auto root=base/"section4-feed-fixtures"/win::installation::unique_id();
        const auto sid=win::current_process_user_sid_string();
        win::WindowsStorageDaclPolicy policy(sid);
        require(fs::create_directories(root/"Config"),"fresh fixture");
        require(policy.apply(root/"Config",win::StorageDaclObjectKind::directory).succeeded(),"protected Config");
        for(const auto* role:{"BotGate","BotGate/feeds"}) {
            require(fs::create_directory(root/role),"fresh BotGate role");
            require(policy.apply(root/role,win::StorageDaclObjectKind::directory).succeeded(),"protected BotGate role");
        }
        if (argc==2 && std::string(argv[1])=="--https") {
            rt::AlphaBotGateFeed live(root,sid,{});
            require(live.start(),"native HTTPS worker starts");
            const auto deadline=GetTickCount64()+45000;
            while (GetTickCount64()<deadline && (!live.status().last_attempt || live.status().updating))
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto result=live.status(); live.stop();
            std::cout<<"Native HTTPS result="<<result.result<<" entries="<<result.entries<<" bytes="<<result.bytes_received<<'\n';
            require(result.available && result.entries>0 && result.result=="feed_update_success","actual certificate-validated feed downloaded and atomically cached");
            std::cout<<"PASS live HTTPS checks="<<checks<<'\n'; return 0;
        }
        std::atomic<unsigned> mode{0},entered{0}; std::atomic<bool> release{false};
        auto fetch=[&](const std::atomic<bool>& stop,rt::AlphaBotGateFeed::Progress progress,std::string& error)->std::optional<std::string> {
            ++entered;
            while (!release && !stop) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto candidate=mode==0 ? std::string("192.0.2.1\n192.0.2.2 # metadata\n") :
                mode==1 ? std::string("192.0.2.2\n198.51.100.1\n") : std::string("192.0.2.3\ninvalid\n");
            progress(candidate.size());
            if (mode==0) return "192.0.2.1\n192.0.2.2 # metadata\n";
            if (mode==1) return "192.0.2.2\n198.51.100.1\n";
            if (mode==2) return "192.0.2.3\ninvalid\n";
            error="feed_fixture_network_failure"; return {};
        };
        const auto cache=root/"BotGate"/"feeds"/"BotGate-feed.ipv4";
        {
            rt::AlphaBotGateFeed feed(root,sid,{},fetch);
            require(!feed.status().available && feed.snapshot()->empty(),"no cache is degraded, not fabricated");
            require(feed.start(),"one worker starts");
            until([&] {return entered==1;});
            require(feed.status().updating && !feed.update_now(),"scheduled and manual work cannot overlap");
            require(feed.snapshot()->empty(),"incomplete candidate never activates");
            require(sfx::application::render_botgate_feed_progress(feed.status()).find("update complete")==std::string::npos,
                "in-flight worker never reports complete");
            release=true;
            until([&] {return !feed.status().updating;});
            std::cout<<"First publication: "<<feed.status().result<<'\n';
            require(feed.status().available && feed.snapshot()->size()==2 && fs::exists(cache),"first complete cache activates");
            require(feed.status().bytes_received==std::string("192.0.2.1\n192.0.2.2 # metadata\n").size() &&
                sfx::application::render_botgate_feed_progress(feed.status()).find("update complete")!=std::string::npos,
                "download progress uses wire bytes and completion follows activation");
            mode=1;
            require(feed.update_now(),"manual refresh accepted");
            until([&] {return entered==2 && !feed.status().updating;});
            const auto replacement=feed.snapshot();
            std::cout<<"Replacement result: "<<feed.status().result<<'\n';
            require(replacement->size()==2 && replacement->front()==0xC0000202U,"replacement removes upstream-deleted address");
            const auto exact=read(cache); const auto timestamp=feed.status().last_success;
            for (unsigned failure:{2U,3U}) {
                mode=failure; require(feed.update_now(),"failed refresh starts");
                until([&] {return entered==failure+1 && !feed.status().updating;});
                require(feed.snapshot()==replacement && read(cache)==exact && feed.status().last_success==timestamp,
                    "validation/network failure retains exact last good memory and disk");
                require(sfx::application::render_botgate_feed_progress(feed.status()).find("update complete")==std::string::npos &&
                    sfx::application::render_botgate_feed_progress(feed.status()).find("last good feed retained")!=std::string::npos,
                    "failed worker shows failure not success");
            }
            {
                auto held=win::installation::open_object(cache,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
                mode=1; require(feed.update_now(),"publication conflict starts");
                until([&] {return entered==5 && !feed.status().updating;});
                require(feed.snapshot()==replacement && read(cache)==exact && feed.status().result!="feed_update_success",
                    "sharing conflict retains old memory/disk without partial activation");
            }
            require(feed.update_now(),"publication retries after conflict clears");
            until([&] {return entered==6 && !feed.status().updating;});
            require(feed.status().result=="feed_update_success","publication recovers without Runtime restart");
            feed.stop(); require(!feed.update_now(),"stopped owner rejects updates");
        }
        {
            rt::AlphaBotGateFeed restarted(root,sid,{},fetch);
            require(restarted.status().available && restarted.snapshot()->size()==2 && restarted.snapshot()->front()==0xC0000202U,
                "validated cache survives restart without resurrecting removed IP");
        }
        for (const auto& file:fs::directory_iterator(root/"BotGate"/"feeds")) require(file.path()==cache || file.path().filename()=="migration-v1.marker","no partial publication debris");
        // Simulate a pre-correction cache: migrate once, keep its original bytes,
        // and never resurrect it after the canonical cache is deliberately removed.
        const auto legacy=root/"Config"/"BotGate-feed.ipv4";
        require(fs::copy_file(cache,legacy),"preserve legacy cache fixture");
        require(policy.apply(legacy,win::StorageDaclObjectKind::file).succeeded(),"private legacy cache");
        require(fs::remove(cache) && fs::remove(root/"BotGate"/"feeds"/"migration-v1.marker"),"reset only fixture migration state");
        {
            rt::AlphaBotGateFeed migrated(root,sid,{},fetch);
            require(migrated.status().available && read(cache)==read(legacy),"legacy cache atomically migrates unchanged");
        }
        require(fs::remove(cache),"delete only canonical fixture cache");
        {
            rt::AlphaBotGateFeed removed(root,sid,{},fetch);
            require(!removed.status().available && removed.snapshot()->empty(),"migration marker prevents old cache resurrection");
        }
        std::cout<<"PASS Section4 feed checks="<<checks<<'\n'; return 0;
    } catch (const std::exception& e) { std::cerr<<"FAIL check "<<checks<<": "<<e.what()<<'\n'; return 1; }
}
