#include "alpha_durable_journal.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/platform/linux/durable_journal_files.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <array>
#include <cerrno>
#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace app=sfx::application;
namespace db=sfx::adapters::storage;
namespace host=sfx::platform::linux_host;
namespace rt=sfx::apps::runtime;
namespace fs=std::filesystem;
namespace {
unsigned checks{};
constexpr std::int64_t now=1788990000;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
app::Identifier128 id(unsigned value){app::Identifier128 result;result.bytes[0]=1;
    result.bytes[12]=static_cast<std::uint8_t>(value>>24);result.bytes[13]=static_cast<std::uint8_t>(value>>16);
    result.bytes[14]=static_cast<std::uint8_t>(value>>8);result.bytes[15]=static_cast<std::uint8_t>(value);return result;}
app::AuditContext audit(unsigned value){return {id(value),id(value+10000),{},now,app::AuditReasonCode::initial_setup};}
std::string read(const fs::path& path){auto directory=host::open_owned_directory(path.parent_path());
    const auto bytes=host::read_private_file(directory.get(),path.filename().string(),app::kDurableJournalMaximumFileBytes);
    return {bytes.begin(),bytes.end()};}
void put(const fs::path& path,std::string_view value){auto directory=host::open_owned_directory(path.parent_path());
    host::publish_private_file(directory.get(),path.filename().string(),{value.begin(),value.end()},false);}
void directory(const fs::path& path){check(fs::create_directory(path)&&chmod(path.c_str(),0700)==0,"new private fixture directory");}
void layout(const fs::path& root){directory(root);for(const auto* name:{"data","logs","config","work"})directory(root/name);}
db::SqliteStorageSettings settings(const fs::path& root){db::SqliteStorageSettings result;result.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;result.data_directory=root/"data";
    result.utc_now=[] {return now;};return result;}
app::AccountId bootstrap(db::SqliteStorage& storage){
    app::CredentialEnvelope credential;credential.envelope_format_version=1;credential.scheme_identifier="snapshot-fixture";
    credential.parameter_encoding_version=1;credential.canonical_parameters={1};credential.salt={1,2};credential.verifier={3,4};credential.created_utc_seconds=now;
    app::StageInitialSysopRequest stage;stage.display_name="Snapshot Sysop";stage.credential=credential;stage.audit=audit(1);
    const auto staged=storage.stage_initial_sysop(stage);check(staged&&staged.account_id.has_value(),"supported account staging");
    const auto policy=storage.security_policy();check(policy&&policy.value.has_value(),"actual security policy");
    app::CompleteFirstRunRequest complete{*staged.account_id,policy.value->revision,policy.value->policy_hash,policy.value->policy_hash,audit(2)};
    check(storage.complete_first_run(complete).succeeded(),"supported fixture setup completion");return *staged.account_id;
}
std::int64_t caller(db::SqliteStorage& storage,app::AccountId account,unsigned number){
    app::StartSessionRequest start{id(number+100),id(number+200),account,1,now};
    start.journal_transport="Telnet";start.journal_source_ip="127.0.0.1";start.journal_ansi=0;
    check(storage.record_session_started(start).succeeded(),"real caller accounting starts");
    check(storage.finalize_session({start.session_id,8,now+8,app::SessionOutcomeCode::normal,id(number+300)}).succeeded(),"real caller finalization");
    const auto pending=storage.pending_caller_journals(false,16);check(pending&&pending.value&&!pending.value->empty(),"finalized caller remains publishable");
    return pending.value->back().caller_number;
}
std::size_t occurrences(std::string_view text,std::string_view needle){std::size_t count{},offset{};
    while((offset=text.find(needle,offset))!=text.npos){++count;offset+=needle.size();}return count;}
template<class F> void eventually(F ready){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!ready()){if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("bounded publisher completion failed");std::this_thread::yield();}}
void complete_and_restore(const fs::path& base){
    const auto root=base/"source";layout(root);host::LinuxLocalStoragePathPolicy paths;host::LinuxSecureIdentifierGenerator identifiers;
    db::SqliteStorage storage(settings(root),paths);check(storage.start().succeeded(),"fresh native SQLite starts");
    const auto account=bootstrap(storage);const auto number=caller(storage,account,1);
    host::LinuxDurableJournalFiles native(root);const auto initial=native.inspect(app::DurableJournalKind::callers);
    check(initial.has_value(),"native caller header initialized");
    const std::string block="PREPARED-CALLER-ONCE\r\n\r\n---\r\n";
    check(storage.prepare_caller_publication({number,initial->identity,initial->bytes,block}).succeeded(),"actual native publication intent saved");
    check(native.append(app::DurableJournalKind::callers,*initial,std::string_view(block).substr(0,9)),"interrupted prefix is genuinely retained");
    auto operational=std::make_shared<rt::AlphaOperationalJournal>(root/"logs/sfxruntime.log",std::to_wstring(geteuid()),identifiers.generate());
    rt::AlphaDurableJournal durable(storage,identifiers,root,std::to_wstring(geteuid()),"UTC",operational);
    check(durable.start(),"durable startup with prepared publication");
    std::string diagnostic,captured_callers,captured_attention;
    const auto snapshot=root/"work/captured.sqlite";
    check(durable.capture_backup_files([&]{return operational->capture_backup_files([&]{
        const auto intent=storage.caller_publication();check(intent&&intent.value&&!(*intent.value),"capture has no inode-bound publication intent");
        check(storage.create_backup(snapshot).status.succeeded()&&chmod(snapshot.c_str(),0600)==0,"SQLite cut under both writer fences");
        captured_callers=read(root/"logs/callers.log");captured_attention=read(root/"logs/heysysop.log");
        operational->emit({app::OperationalKind::runtime_started,{}, {}});
        check(read(root/"logs/callers.log")==captured_callers,"durable bytes remain fixed inside capture");return true;
    });},diagnostic),"startup/caller coordinated cut succeeds");
    check(diagnostic.empty()&&occurrences(captured_callers,block)==1,"prepared caller suffix completes exactly once");
    check(operational->flush(),"operational writer resumes after capture");
    check(!read(root/"logs/sfxruntime.log").empty(),"queued operational record persists after fence release");
    // Startup-style attention may be between dispatch and durable queueing.
    // It holds no operational file-I/O fence while waiting for publication.
    std::mutex gate;std::condition_variable changed;bool entered{},release{},delivered{};
    operational->attention_sink([&](app::AttentionRecord record){
        {std::unique_lock<std::mutex> lock(gate);entered=true;changed.notify_all();
            if(!changed.wait_for(lock,std::chrono::seconds(5),[&]{return release;}))throw std::runtime_error("attention gate timed out");}
        durable.attention(std::move(record));
        {std::lock_guard<std::mutex> lock(gate);delivered=true;}changed.notify_all();
    });
    std::thread startup([&]{operational->problem(app::AttentionKind::work_preserved,"snapshot fixture","startup_attention_after_cut");});
    struct JoinStartup {
        std::thread& thread;std::mutex& mutex;std::condition_variable& changed;bool& release;
        ~JoinStartup(){
            {std::lock_guard<std::mutex> lock(mutex);release=true;}changed.notify_all();
            if(thread.joinable())thread.join();
        }
    } join_startup{startup,gate,changed,release};
    {std::unique_lock<std::mutex> lock(gate);check(changed.wait_for(lock,std::chrono::seconds(5),[&]{return entered;}),"startup attention reaches deterministic dispatch gate");}
    const bool overlap=durable.capture_backup_files([&]{return operational->capture_backup_files([&]{
        {std::lock_guard<std::mutex> lock(gate);release=true;}changed.notify_all();
        check(read(root/"logs/heysysop.log")==captured_attention,"attention cannot append across the coordinated cut");return true;
    });},diagnostic);
    {std::lock_guard<std::mutex> lock(gate);release=true;}changed.notify_all();startup.join();
    check(overlap&&delivered,"capture and startup attention both complete without lock inversion");
    check(operational->flush(),"startup operational reporting drains after capture");
    operational->attention_sink({});
    // An exception must release both locks and retain the partial work bytes.
    check(!durable.capture_backup_files([&]{return operational->capture_backup_files([&]()->bool{
        put(root/"work/retained.partial","failed captured bytes");throw std::runtime_error("injected_snapshot_failure");
    });},diagnostic)&&diagnostic=="injected_snapshot_failure","capture failure is precise and releases both fences");
    check(read(root/"work/retained.partial")=="failed captured bytes","failed new Work remains intact");
    check(durable.capture_backup_files([&]{return operational->capture_backup_files([]{return true;});},diagnostic),"capture progresses after exception");
    durable.stop();
    const auto stopped_callers=read(root/"logs/callers.log"),stopped_attention=read(root/"logs/heysysop.log");
    check(durable.capture_backup_files([&]{return operational->capture_backup_files([&]{
        const auto file=root/"work/stopped-cut.sqlite";
        return storage.create_backup(file).status.succeeded()&&chmod(file.c_str(),0600)==0&&
            read(root/"logs/callers.log")==stopped_callers&&read(root/"logs/heysysop.log")==stopped_attention;
    });},diagnostic),"fully joined maintenance owner can capture real SQLite and fixed logs");
    app::OtherBoardLogRecord stopped_record;stopped_record.board_name="After stop";stopped_record.actor_name="Snapshot Sysop";
    stopped_record.utc_seconds=now;stopped_record.event_id=id(901);
    check(!durable.other_board(stopped_record)&&read(root/"logs/callers.log")==stopped_callers&&
        read(root/"logs/heysysop.log")==stopped_attention,"snapshot-only joined owner still refuses new durable records");
    check(durable.capture_backup_files([&]{return operational->capture_backup_files([]{return true;});},diagnostic),"joined owner remains capture-eligible after refused new record");
    check(operational->flush(),"joined owner refusal reporting drains normally");
    operational.reset();check(storage.shutdown().succeeded(),"original writer and SQLite close normally");
    const auto restored=base/"restored";layout(restored);
    const auto database=read(snapshot);put(restored/"data/sfx.db",database);
    put(restored/"logs/callers.log",captured_callers);put(restored/"logs/heysysop.log",captured_attention);
    struct stat original{},replacement{};
    check(lstat((root/"logs/callers.log").c_str(),&original)==0&&lstat((restored/"logs/callers.log").c_str(),&replacement)==0&&
        (original.st_dev!=replacement.st_dev||original.st_ino!=replacement.st_ino),"restored caller journal is a genuinely new native inode");
    db::SqliteStorage restored_storage(settings(restored),paths);check(restored_storage.start().succeeded(),"captured DB opens at restored root");
    check(restored_storage.validate_backup(restored/"data/sfx.db").succeeded(),"restored schema and SQLite integrity validate");
    auto resumed_operational=std::make_shared<rt::AlphaOperationalJournal>(restored/"logs/sfxruntime.log",std::to_wstring(geteuid()),identifiers.generate());
    rt::AlphaDurableJournal resumed(restored_storage,identifiers,restored,std::to_wstring(geteuid()),"UTC",resumed_operational);
    check(resumed.start(),"publisher resumes against new restored inode");
    check(resumed.capture_backup_files([&]{const auto intent=restored_storage.caller_publication();
        return intent&&intent.value&&!(*intent.value);},diagnostic),"restored publication boundary remains valid");
    check(read(restored/"logs/callers.log")==captured_callers,"restored publisher never duplicates acknowledged caller");
    const auto next=caller(restored_storage,account,2);check(next==2,"restored caller number continues authoritatively");
    resumed.wake();eventually([&]{const auto rows=restored_storage.pending_caller_journals(false,16);return rows&&rows.value&&rows.value->empty();});
    check(occurrences(read(restored/"logs/callers.log"),block)==1,"new caller publication retains original exactly once");
    resumed.stop();resumed_operational.reset();check(restored_storage.shutdown().succeeded(),"restored writers and DB close normally");
}
void refuse_incomplete(const fs::path& base){
    const auto root=base/"refused";layout(root);host::LinuxLocalStoragePathPolicy paths;host::LinuxSecureIdentifierGenerator identifiers;
    db::SqliteStorage storage(settings(root),paths);check(storage.start().succeeded(),"refusal native DB starts");
    const auto number=caller(storage,bootstrap(storage),3);host::LinuxDurableJournalFiles files(root);
    const auto initial=files.inspect(app::DurableJournalKind::callers);check(initial.has_value(),"refusal native log exists");
    check(initial->identity!="1:1","well-formed refusal identity differs from actual native file");
    check(storage.prepare_caller_publication({number,"1:1",initial->bytes,"unpublishable\r\n\r\n---\r\n"}).succeeded(),"retained incompatible native intent fixture");
    auto operational=std::make_shared<rt::AlphaOperationalJournal>(root/"logs/sfxruntime.log",std::to_wstring(geteuid()),identifiers.generate());
    rt::AlphaDurableJournal durable(storage,identifiers,root,std::to_wstring(geteuid()),"UTC",operational);check(durable.start(),"refusal fixture Runtime writer starts");
    bool invoked{};std::string diagnostic;
    check(!durable.capture_backup_files([&]{invoked=true;return true;},diagnostic)&&!invoked&&
        diagnostic=="backup_caller_publication_incomplete_work_preserved","unrestorable active intent refuses before snapshot callback with precise reason");
    check(read(root/"logs/callers.log")=="; SFX CALLERS.LOG v1\r\n","refused capture never truncates or rewrites caller bytes");
    durable.stop();operational.reset();check(storage.shutdown().succeeded(),"refusal fixture closes normally");
}
void append_seed(const fs::path& path,std::string_view row){
    auto parent=host::open_owned_directory(path.parent_path());
    host::FileDescriptor file(openat(parent.get(),path.filename().c_str(),O_WRONLY|O_APPEND|O_NOFOLLOW|O_CLOEXEC));
    check(bool(file)&&host::private_regular_file(file.get(),app::kDurableJournalMaximumFileBytes),"seed holds exact private journal file");
    std::string bytes;while(bytes.size()<1024U*1024U)bytes+=row;
    std::size_t offset{};while(offset<bytes.size()){
        const auto count=write(file.get(),bytes.data()+offset,bytes.size()-offset);
        if(count<0&&errno==EINTR)continue;
        check(count>0,"seed writes complete bounded records");offset+=static_cast<std::size_t>(count);
    }
    check(fsync(file.get())==0,"seed journal bytes durable before writers start");
}
void rotations_after_cut(const fs::path& base){
    const auto root=base/"rotations";layout(root);host::LinuxLocalStoragePathPolicy paths;host::LinuxSecureIdentifierGenerator identifiers;
    db::SqliteStorage storage(settings(root),paths);check(storage.start().succeeded(),"rotation fixture SQLite starts");(void)bootstrap(storage);
    app::LoggingConfiguration limits;limits.runtime={1,1};limits.callers={1,1};limits.attention={1,1};
    host::LinuxDurableJournalFiles files(root);check(files.configure(limits),"real native retention configured to one owned archive per family");
    const std::array<std::pair<app::DurableJournalKind,const char*>,3> families{{
        {app::DurableJournalKind::callers,"callers.log"},{app::DurableJournalKind::attention,"heysysop.log"},{app::DurableJournalKind::runtime,"sfxruntime.log"}}};
    for(const auto& family:families){
        check(files.inspect(family.first).has_value(),"initialize canonical active family");
        const std::string row=family.first==app::DurableJournalKind::runtime?"100|INFO|runtime|rotation seed\r\n":
            family.first==app::DurableJournalKind::callers?"seed caller\r\n\r\n---\r\n":"seed attention\r\n";
        append_seed(root/"logs"/family.second,row);
        check(files.archive_and_reopen(family.first),"native rotate retains first full journal");
        append_seed(root/"logs"/family.second,row);
    }
    auto operational=std::make_shared<rt::AlphaOperationalJournal>(root/"logs/sfxruntime.log",std::to_wstring(geteuid()),identifiers.generate());
    check(operational->configure(limits),"operational writer uses same bounded native retention");
    rt::AlphaDurableJournal durable(storage,identifiers,root,std::to_wstring(geteuid()),"UTC",operational);
    check(durable.configure_logging(limits)&&durable.start(),"durable writer uses same bounded retention");
    std::map<std::string,std::string> captured;std::map<std::string,fs::path> copies;std::string diagnostic;
    check(durable.capture_backup_files([&]{return operational->capture_backup_files([&]{
        const auto database=root/"work/rotation-cut.sqlite";
        check(storage.create_backup(database).status.succeeded()&&chmod(database.c_str(),0600)==0,"rotation set shares real SQLite cut");
        for(const auto& entry:fs::directory_iterator(root/"logs")){
            const auto name=entry.path().filename().string();const auto bytes=read(entry.path());
            const auto copy=root/"work"/("captured-"+std::to_string(captured.size())+".snapshot");put(copy,bytes);
            captured.emplace(name,bytes);copies.emplace(name,copy);
        }
        return true;
    });},diagnostic),"capture includes active and retained rotation set under both fences");
    check(captured.size()==6,"three active plus three retained journal files captured");
    app::OtherBoardLogRecord record;record.board_name="Snapshot rotation board";record.actor_name="Snapshot Sysop";
    record.utc_seconds=now;record.event_id=id(900);record.action=app::ClassicInformationAction::remove_board;
    check(durable.other_board(record),"ordinary durable publication resumes and rotates both full families");
    operational->emit({app::OperationalKind::runtime_started,{}, {}});check(operational->flush(),"ordinary operational publication resumes and rotates full runtime family");
    std::size_t pruned{};
    for(const auto& captured_file:captured){
        check(read(copies.at(captured_file.first))==captured_file.second,"immutable captured bytes survive live append/rotation/retention");
        if(captured_file.first.find('-')!=std::string::npos){
            check(!fs::exists(root/"logs"/captured_file.first),"old captured rotation is actually pruned only after capture resumes writers");++pruned;
        }
    }
    check(pruned==3,"all three managed rotation families exercised real post-cut pruning");
    durable.stop();check(operational->flush(),"rotation writers flush cleanly");operational.reset();check(storage.shutdown().succeeded(),"rotation DB closes normally");
}
}
int main(int argc,char** argv){try{
    check(argc==3&&geteuid()!=0,"fresh private test root and timezone resources, ordinary UID");
    const fs::path root=argv[1];check(root.is_absolute()&&!fs::exists(root),"new absolute fixture only");directory(root);
    check(host::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"pinned native timezone initialized");
    complete_and_restore(root);refuse_incomplete(root);rotations_after_cut(root);
    std::cout<<"PASS "<<checks<<" coordinated Linux journal/SQLite/new-inode restore checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
