#include "sfx/platform/linux/clamav_update.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/file.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <unistd.h>
namespace host=sfx::platform::linux_host;
namespace fs=std::filesystem;
namespace {
fs::path failure_root;
std::string failure_mode;
ino_t previous_database{};
bool injected{};
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){
    if(!failure_mode.empty()&&!injected){
        std::array<char,4096> path{};const auto n=readlink(("/proc/self/fd/"+std::to_string(fd)).c_str(),path.data(),path.size());
        struct stat info{};const auto active=failure_root/"data/clamav/db";
        if(n>0&&stat(active.c_str(),&info)==0&&info.st_ino!=previous_database){
            const std::string name(path.data(),static_cast<std::size_t>(n));const auto parent=(failure_root/"data/clamav").string();
            if((failure_mode=="--fail-publish"&&name==parent)||(failure_mode=="--fail-history"&&name.rfind(parent+"/.sfx-pending-",0)==0)){
                injected=true;errno=EIO;return -1;
            }
        }
    }
    return __real_fsync(fd);
}
namespace {
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
std::string read(const fs::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
void run(const fs::path& root,bool restored,bool initial){
    check(geteuid()!=0&&root.is_absolute(),"normal native owner and absolute fixture");
    auto status=host::inspect_linux_clamav(root);
    check(status.installed&&status.verified&&status.version=="1.4.7","exact optional pinned component");
    if(restored){check(status.history_available&&status.definitions_verified&&status.databases[1].has_value(),"portable folder copy retains verified definitions without path lock");return;}
    check(status.definitions_present!=initial&&!status.definitions_verified&&!status.history_available,"missing or copied CVDs alone never manufacture successful update history");
    const auto utc=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto result=host::update_linux_clamav(root,utc);
    std::cout<<"update attempted="<<result.attempted<<" success="<<result.succeeded<<" reaped="<<result.process_reaped<<" cleanup="<<result.cleanup_complete<<" exit="<<result.exit_code<<" diagnostic="<<result.diagnostic<<'\n'<<std::flush;
    check(result.attempted&&result.succeeded&&result.process_reaped&&result.cleanup_complete,"real FreshClam and full scanner verification publish durably");
    if(initial)check(result.change==host::LinuxClamAvDefinitionChange::initial,"empty installation gets truthful initial-definition result");
    status=host::inspect_linux_clamav(root);check(status.definitions_verified&&status.history_available&&status.last_success==utc&&status.last_attempt==utc,"saved exact database fingerprints verify");
    check(status.databases[0]&&status.databases[1]&&status.databases[2]&&status.databases[1]->version==result.daily_version,"verified display metadata belongs to exact databases");
    const auto history=read(root/"data/clamav/update-status.v1"),day=read(root/"data/clamav/update-day.v1"),log=read(root/"data/clamav/update-last.log");
    check(!log.empty(),"bounded real vendor log retained");
    result=host::update_linux_clamav(root,utc+1);
    check(!result.attempted&&result.diagnostic=="clamav_update_already_attempted_today","persisted UTC day suppresses repeated vendor calls");
    check(history==read(root/"data/clamav/update-status.v1")&&day==read(root/"data/clamav/update-day.v1"),"skipped automatic attempt preserves history");
    result=host::update_linux_clamav(root,utc+2,host::LinuxClamAvUpdateMode::manual,[]{return true;});
    check(result.cancelled&&!result.attempted&&result.process_reaped&&!result.succeeded,"cancel before launch preserves definitions");
    host::FileDescriptor gate(open((root/"work/clamav/operation.lock").c_str(),O_RDWR|O_CLOEXEC));check(bool(gate)&&flock(gate.get(),LOCK_EX|LOCK_NB)==0,"fixture owns update gate");
    status=host::inspect_linux_clamav(root);check(status.busy&&!status.definitions_verified,"concurrent status says busy rather than trusting a changing set");
    result=host::update_linux_clamav(root,utc+3,host::LinuxClamAvUpdateMode::manual,{}, {},100);
    check(!result.attempted&&result.process_reaped&&!result.succeeded&&result.diagnostic=="clamav_update_timeout","queued update remains deadline bounded");gate=host::FileDescriptor();
    check(history==read(root/"data/clamav/update-status.v1")&&host::inspect_linux_clamav(root).definitions_verified,"cancel and admission timeout retain verified set and history");
    bool saw_child{};result=host::update_linux_clamav(root,utc+4,host::LinuxClamAvUpdateMode::manual,[&]{
        saw_child=!read(fs::path("/proc/self/task")/std::to_string(getpid())/"children").empty();return saw_child;
    });
    check(saw_child&&result.attempted&&result.cancelled&&!result.succeeded&&result.process_reaped&&result.cleanup_complete,"active updater cancellation reaps child and preserves exact prior definitions");
    status=host::inspect_linux_clamav(root);check(status.definitions_verified&&status.last_success==utc&&status.diagnostic=="clamav_update_cancelled","failed latest attempt remains visible beside usable saved definitions");
    fs::path daily;for(const auto& entry:fs::directory_iterator(root/"data/clamav/db"))if(entry.path().stem()=="daily")daily=entry.path();check(!daily.empty(),"actual daily database located");
    const auto original_size=fs::file_size(daily);{std::ofstream file(daily,std::ios::binary|std::ios::app);file.put('X');}
    check(!host::inspect_linux_clamav(root).definitions_verified,"changed database never inherits old successful receipt");
    fs::resize_file(daily,original_size);check(host::inspect_linux_clamav(root).definitions_verified,"exact restored database matches retained receipt");
    const auto saved_history=read(root/"data/clamav/update-status.v1");auto directory=host::open_owned_directory(root/"data/clamav");host::publish_private_file(directory.get(),"update-status.v1",{'b','a','d'},true);
    result=host::update_linux_clamav(root,utc+5,host::LinuxClamAvUpdateMode::manual);
    check(!result.attempted&&!result.succeeded&&result.diagnostic=="clamav_status_invalid","corrupt history refused without vendor request or fabricated success");
    host::publish_private_file(directory.get(),"update-status.v1",{saved_history.begin(),saved_history.end()},true);
    std::size_t jobs{};for(const auto& entry:fs::directory_iterator(root/"work/clamav"))if(entry.path().filename()!="operation.lock")++jobs;
    check(jobs==0,"successful update retires exact old definitions and owned scratch");
}
void unready(const fs::path& root){
    check(geteuid()!=0&&root.is_absolute(),"normal native owner and absolute fixture");
    host::initialize_linux_clamav_data(root);
    const auto status=host::inspect_linux_clamav(root);
    check(status.installed&&status.verified&&status.version=="1.4.7","exact engine verified independently of definitions");
    check(!status.definitions_present&&!status.definitions_verified&&!status.history_available&&
        status.diagnostic=="clamav_database_missing","missing database stays unready without a successful update receipt");
}
void publication_failure(const fs::path& root,const std::string& mode){
    const auto before=host::inspect_linux_clamav(root);check(before.definitions_verified,"verified baseline before injected publication failure");
    struct stat info{};check(stat((root/"data/clamav/db").c_str(),&info)==0,"pin previous definition directory");
    std::vector<std::pair<fs::path,std::uintmax_t>> previous_files;
    for(const auto& entry:fs::directory_iterator(root/"data/clamav/db"))if(entry.path().extension()==".cvd"||entry.path().extension()==".cld")previous_files.emplace_back(entry.path().filename(),entry.file_size());
    check(previous_files.size()==3,"three actual prior CVD or CLD files pinned");
    failure_root=root;failure_mode=mode;previous_database=info.st_ino;
    const auto utc=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto result=host::update_linux_clamav(root,utc,host::LinuxClamAvUpdateMode::manual);
    failure_mode.clear();
    std::cout<<"injection="<<mode<<" reached="<<injected<<" success="<<result.succeeded<<" cleanup="<<result.cleanup_complete<<" diagnostic="<<result.diagnostic<<'\n';
    check(injected&&result.attempted&&!result.succeeded&&result.process_reaped&&!result.cleanup_complete,"publication or receipt failure retains fallback and reports attention");
    check(result.diagnostic==(mode=="--fail-publish"?"clamav_update_publish_flush":"clamav_update_status_unavailable"),"exact injected phase reported");
    bool retained{};for(const auto& entry:fs::directory_iterator(root/"work/clamav"))if(entry.is_directory()){
        const auto saved=entry.path()/"db";if(stat(saved.c_str(),&info)==0&&info.st_ino==previous_database){
            retained=true;for(const auto& file:previous_files)check(fs::file_size(saved/file.first)==file.second,"previous complete database file retained");
        }
    }
    check(retained,"exact pre-exchange directory retained after uncertainty");
}
}
int main(int argc,char** argv){try{check(argc==2||argc==3,"private fixture required");
    const std::string_view mode=argc==3?argv[2]:"";
    if(mode=="--unready")unready(argv[1]);
    else if(mode.rfind("--fail-",0)==0)publication_failure(argv[1],argv[2]);
    else run(argv[1],mode=="--restored",mode=="--initial");
    std::cout<<"PASS "<<checks<<" native ClamAV lifecycle checks\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
