#include "alpha_operational_journal.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace app=sfx::application;
namespace host=sfx::platform::linux_host;
namespace runtime=sfx::apps::runtime;
namespace fs=std::filesystem;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
app::Identifier128 id(unsigned n){app::Identifier128 value;value.bytes[0]=static_cast<std::uint8_t>(n);return value;}
std::string read(const fs::path& path){auto directory=host::open_owned_directory(path.parent_path());
    const auto bytes=host::read_private_file(directory.get(),path.filename().string(),app::kDurableJournalMaximumFileBytes);
    return {bytes.begin(),bytes.end()};}
void put(const fs::path& path,std::string_view bytes,bool replace){auto dir=host::open_owned_directory(path.parent_path());
    host::publish_private_file(dir.get(),path.filename().string(),{bytes.begin(),bytes.end()},replace);}
app::OperationalEvent event(unsigned node,std::uint64_t generation,std::uint64_t attempt){
    app::OperationalEvent e;e.kind=app::OperationalKind::stage_changed;auto& c=e.context;
    c.node=static_cast<std::uint16_t>(node);c.generation=generation;c.attempt=attempt;
    c.transport=app::OperationalTransport::ssh;c.source_ip="127.0.0.1";c.stage=app::CallerStage::main_menu;
    c.account_id=node;c.caller="Caller "+std::to_string(node);c.durable_session_id=id(node+10);c.correlation_id=id(node+20);return e;
}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"normal user and new fixture required");const fs::path root=argv[1];
    check(root.is_absolute()&&!fs::exists(root)&&fs::create_directory(root)&&chmod(root.c_str(),0700)==0,"private new root");
    check(fs::create_directory(root/"logs")&&chmod((root/"logs").c_str(),0700)==0,"private log directory");
    const auto path=root/"logs"/"sfxruntime.log";const std::string legacy="100|INFO|runtime|historical marker\r\n";
    put(path,legacy,false);
    {
        runtime::AlphaOperationalJournal journal(path,std::to_wstring(geteuid()),id(1));
        check(journal.health()==0&&journal.snapshot().empty(),"validate without importing historical epoch");
        std::array<std::thread,4> writers;
        for(unsigned node=1;node<=4;++node)writers[node-1]=std::thread([&,node]{
            const auto attempt=journal.next_attempt();for(unsigned n=0;n<100;++n){
                journal.emit(event(node,n+1,attempt));std::this_thread::sleep_for(std::chrono::milliseconds(40));}
        });
        for(auto& writer:writers)writer.join();
        check(journal.flush()&&journal.health()==0,"actual concurrent worker flush healthy");
        check(journal.snapshot().size()==32&&journal.snapshot().back().sequence==400,"bounded status tail retains delivery order");
        app::OperationalLogCache cache;cache.bind(id(1));std::size_t delivered{};
        do{const auto page=journal.page(cache.cursor());check(page&&cache.accept(*page),"coherent authenticated epoch page");delivered+=page->entries.size();}while(cache.more());
        check(delivered==400&&cache.backlog().entries().size()==400,"all records available beyond status tail");
        const auto bytes=read(path);check(bytes.substr(0,legacy.size())==legacy,"legacy bytes preserved");
        std::istringstream lines(bytes);std::string line;std::uint64_t sequence{};std::set<std::uint64_t> origins,attempts;
        while(std::getline(lines,line)){
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            const auto decoded=app::decode_operational_record(line,++sequence);
            check(decoded.has_value(),"persisted lines independently decode");if(!decoded->event)continue;
            const auto& c=decoded->event->context;check(origins.insert(c.origin_sequence).second&&c.epoch==id(1),"unique epoch origin");
            attempts.insert(c.attempt);check(c.account_id==c.node&&c.caller=="Caller "+std::to_string(c.node)&&c.durable_session_id==id(c.node+10),"caller identity not mixed across producers");
        }
        check(sequence==401&&origins.size()==400&&attempts.size()==4,"four real producers fully persisted");
        std::atomic<bool> stop{},entered{};
        std::thread producer([&]{entered=true;const auto attempt=journal.next_attempt();for(unsigned n=0;!stop&&n<100;++n){journal.emit(event(5,n+1,attempt));std::this_thread::sleep_for(std::chrono::milliseconds(2));}});
        while(!entered)std::this_thread::yield();
        std::string captured;
        const bool stable=journal.capture_backup_file([&]{captured=read(path);std::this_thread::sleep_for(std::chrono::milliseconds(20));return read(path)==captured;});
        stop=true;producer.join();check(stable&&!captured.empty(),"concurrent append producer cannot change log inside short snapshot barrier");
        check(journal.flush()&&journal.health()==0,"writer and pending producer resume and drain after capture");
        const auto after_capture=read(path);check(after_capture.size()>=captured.size()&&after_capture.compare(0,captured.size(),captured)==0,"point-in-time snapshot is exact prefix of continuing healthy log");
        const auto failed=root/"capture-failed.partial";const auto before_failure=read(path);
        check(!journal.capture_backup_file([&]()->bool{put(failed,"retained failed capture bytes",false);throw std::runtime_error("injected capture failure");}),"capture failure reports failure without escaping barrier");
        check(read(failed)=="retained failed capture bytes"&&read(path)==before_failure,"failed private snapshot evidence and original log preserved");
        journal.emit(event(6,1,journal.next_attempt()));check(journal.flush()&&journal.health()==0,"capture exception releases barrier and later logging still succeeds");
    }
    {
        runtime::AlphaOperationalJournal restarted(path,std::to_wstring(geteuid()),id(2));
        check(restarted.health()==0&&restarted.snapshot().empty(),"new epoch starts clean transient history");
        restarted.emit(event(1,1,restarted.next_attempt()));check(restarted.flush()&&restarted.health()==0,"restart appends without destroying old data");
        const auto before=read(path);check(chmod(path.c_str(),0640)==0,"introduce actual insecure file state");
        restarted.emit(event(2,1,restarted.next_attempt()));check(restarted.flush(),"drain refused append");
        check(chmod(path.c_str(),0600)==0,"restore fixture mode");
        check((restarted.health()&app::operational_log_write_failed)&&read(path)==before&&restarted.snapshot().back().event->context.node==2,"real file refusal preserves bytes and live reporting");
        restarted.emit(event(3,1,restarted.next_attempt()));check(restarted.flush()&&read(path)==before,"failed append remains blocked for deliberate recovery");
    }
    const std::string partial=legacy+"SFXLOG2|unfinished";put(path,partial,true);
    {
        runtime::AlphaOperationalJournal broken(path,std::to_wstring(geteuid()),id(3));
        check((broken.health()&app::operational_log_history_invalid)!=0,"partial record detected on restart");
        broken.emit(event(1,1,broken.next_attempt()));check(broken.flush()&&read(path)==partial&&broken.snapshot().size()==1,"partial durable history preserved while live reporting continues");
    }
    check(!fs::exists(root/"logs"/"callers.log")&&!fs::exists(root/"logs"/"heysysop.log"),"operational worker does not fabricate other logs");
    std::cout<<"PASS "<<checks<<" Linux shared Runtime journal checks; four writers/400 records/restart/actual refusal\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
