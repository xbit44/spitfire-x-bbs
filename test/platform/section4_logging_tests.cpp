#include "alpha_operational_journal.hpp"
#include "sfx/platform/win32/durable_journal_files.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace app=sfx::application;
namespace win=sfx::platform::win32;
namespace rt=sfx::apps::runtime;
namespace fs=std::filesystem;
unsigned checks{};
void require(bool value,const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
app::Identifier128 id(unsigned n) { app::Identifier128 value; value.bytes[0]=static_cast<std::uint8_t>(n); return value; }
std::string read(const fs::path& path) { std::ifstream f(path,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
std::vector<fs::path> archives(const fs::path& root,const std::string& prefix) {
    std::vector<fs::path> result;
    for (const auto& item:fs::directory_iterator(root/"Logs"))
        if (item.path().filename().string().find(prefix+"-")==0) result.push_back(item.path());
    return result;
}
void run(const fs::path& root) {
    const auto sid=win::current_process_user_sid_string();
    win::WindowsStorageDaclPolicy policy(sid);
    require(fs::create_directories(root/"Logs"),"fresh fixture");
    require(policy.apply(root/"Logs",win::StorageDaclObjectKind::directory).succeeded(),"private Logs");
    const auto path=root/"Logs/SFXRuntime.log";
    const std::string row="100|INFO|runtime|old preserved history\r\n";
    std::string old;
    while (old.size()<1024*1024) old+=row;
    { std::ofstream f(path,std::ios::binary); f<<old; require(f.good(),"seed valid prior full log"); }
    require(policy.apply(path,win::StorageDaclObjectKind::file).succeeded(),"protect prior log");
    {
        std::mutex attention_mutex; std::vector<app::AttentionRecord> attention;
        rt::AlphaOperationalJournal journal(path,sid,id(1));
        journal.attention_sink([&](app::AttentionRecord value) {
            std::lock_guard<std::mutex> lock(attention_mutex); attention.push_back(std::move(value));
        });
        app::LoggingConfiguration limits; limits.runtime={1,2};
        require(journal.configure(limits),"validated size and count");
        journal.emit({app::OperationalKind::configuration_saved,{}, {}});
        require(journal.flush() && !journal.health(),"async append rotates old one-MiB history instead of stopping");
        auto prior=archives(root,"SFXRuntime");
        require(prior.size()==1 && read(prior.front())==old,"archive preserves exact old bytes");
        require(read(path).find("SFXLOG6|")==0,"new active file continues recording");
        const auto start=GetTickCount64();
        for (unsigned i=0;i<10000;++i) {
            app::OperationalContext c;
            c.attempt=journal.next_attempt(); c.transport=app::OperationalTransport::telnet;
            c.source_ip="192.0.2.1"; c.diagnostic_code="manual_ip_rule";
            journal.emit({app::OperationalKind::botgate_blocked,{},std::move(c)});
        }
        require(GetTickCount64()-start<5000,"caller-side flood does not wait on disk writes");
        app::BotGateFeedStatus gate;journal.gate_status(gate);
        require(gate.protection_counts[4]==10000 && gate.protection_started>0,"Runtime totals retain every event despite flood aggregation");
        journal.gate_count(4);journal.gate_active(true);journal.gate_status(gate);
        require(gate.protection_counts[4]==10001 && gate.protection_counts[6]==1,"silent block and active count");
        journal.gate_active(false);journal.gate_active(false);journal.gate_status(gate);
        require(gate.protection_counts[6]==0,"active never underflows");
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        require(journal.flush(),"bounded flood-summary drain");
        require(read(path).find("192.0.2.1")!=std::string::npos,"source context retained");
        bool summary=false;
        for (const auto& entry:journal.snapshot()) if (entry.event && entry.event->kind==app::OperationalKind::flood_aggregate) {
            summary=true; require(entry.event->context.resource_id>0,"aggregate has useful count");
        }
        require(summary,"flood produces explicit aggregate instead of silent loss");
        {
            std::lock_guard<std::mutex> lock(attention_mutex);
            require(!attention.empty() && attention.size()<=3,"attention flood routing is bounded and aggregated");
            require(attention.front().kind==app::AttentionKind::botgate_security && attention.front().identity==app::AttentionIdentity::none,
                "BotGate attention never claims account authentication");
            require(app::render_attention_journal(attention.front(),[](std::int64_t) {
                return std::optional<std::string>("2000-01-01 00:00:00"); }).has_value(),"attention record retains valid fixed format");
        }
        journal.attention_sink({});
    }
    {
        win::WindowsDurableJournalFiles files(root,sid);
        app::LoggingConfiguration limits; limits.runtime.archives=2; limits.callers.archives=3; limits.attention.archives=1;
        require(files.configure(limits),"per-log retention configuration");
        for (const auto kind:{app::DurableJournalKind::runtime,app::DurableJournalKind::callers,app::DurableJournalKind::attention}) {
            for (unsigned i=0;i<5;++i) {
                auto state=files.inspect(kind); require(state.has_value(),"inspect canonical active log");
                const std::string text=kind==app::DurableJournalKind::runtime ? row : kind==app::DurableJournalKind::callers ?
                    "fixture caller\r\n\r\n---\r\n" : "fixture attention\r\n";
                require(files.append(kind,*state,text),"append full record");
                require(files.archive_and_reopen(kind),"common verified rotation");
            }
        }
        require(archives(root,"SFXRuntime").size()==2 && archives(root,"CALLERS").size()==3 &&
            archives(root,"HEYSYSOP").size()==1,"independent retention prunes only oldest completed archives");
    }
    const auto damaged=root/"damaged";
    require(fs::create_directories(damaged/"Logs"),"damaged fixture");
    require(policy.apply(damaged/"Logs",win::StorageDaclObjectKind::directory).succeeded(),"damaged fixture boundary");
    const auto bad=damaged/"Logs/SFXRuntime.log";
    { std::ofstream f(bad); f<<"SFXLOG6|unfinished"; }
    require(policy.apply(bad,win::StorageDaclObjectKind::file).succeeded(),"damaged file protection");
    {
        rt::AlphaOperationalJournal journal(bad,sid,id(2));
        journal.emit({app::OperationalKind::configuration_saved,{}, {}});
        require(journal.flush() && (journal.health()&app::operational_log_history_invalid),"partial history visible");
        require(read(bad)=="SFXLOG6|unfinished" && journal.snapshot().size()==1,"partial history preserved while Live Logs continue");
    }
}
int main() {
    try {
        const auto base=fs::current_path();
        require(base.parent_path().filename()=="build","fixture limited to repository build subtree");
        run(base/"section4-log-fixtures"/win::installation::unique_id());
        std::cout<<"PASS Section4 native logging checks="<<checks<<'\n'; return 0;
    } catch (const std::exception& e) { std::cerr<<"FAIL check "<<checks<<": "<<e.what()<<'\n'; return 1; }
}
