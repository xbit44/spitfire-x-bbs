#include "sfx/platform/linux/file_scanner.hpp"
#include "sfx/platform/linux/clamav_update.hpp"
#include "sfx/archive/file_zip.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <charconv>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace sfx::platform::linux_host {
namespace {
namespace app=application;
using FD=FileDescriptor;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::uint64_t now(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
bool same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.st_size==b.st_size&&a.st_nlink==b.st_nlink&&a.st_mode==b.st_mode&&a.st_uid==b.st_uid&&a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec&&a.st_ctim.tv_sec==b.st_ctim.tv_sec&&a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;}
bool same(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b){return a.volume==b.volume&&a.file_id==b.file_id&&a.bytes==b.bytes&&a.sha256==b.sha256;}
std::string hash(int fd,const std::function<bool()>& live){
    crypto_hash_sha256_state state;require(crypto_hash_sha256_init(&state)==0,"scan_hash_init");
    std::array<unsigned char,65536> block{};off_t offset{};
    for(;;){require(live(),"scan_preparation_cancelled");const auto n=pread(fd,block.data(),block.size(),offset);
        if(n<0&&errno==EINTR)continue;
        require(n>=0,"scan_hash_read");if(!n)break;require(crypto_hash_sha256_update(&state,block.data(),static_cast<unsigned long long>(n))==0,"scan_hash_update");offset+=n;}
    std::array<unsigned char,crypto_hash_sha256_BYTES> digest{};require(crypto_hash_sha256_final(&state,digest.data())==0,"scan_hash_final");
    std::array<char,crypto_hash_sha256_BYTES*2+1> text{};sodium_bin2hex(text.data(),text.size(),digest.data(),digest.size());return text.data();
}
std::vector<std::string> names(int directory){
    FD copy(openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));require(bool(copy),"scan_directory_reopen");
    DIR* raw=fdopendir(dup(copy.get()));require(raw!=nullptr,"scan_directory_list");std::unique_ptr<DIR,int(*)(DIR*)> list(raw,closedir);std::vector<std::string> result;
    errno=0;while(const auto* entry=readdir(raw)){const std::string name=entry->d_name;if(name=="."||name=="..")continue;require(result.size()<64,"scan_directory_entries");result.push_back(name);errno=0;}
    require(errno==0,"scan_directory_read");return result;
}
struct Pinned {
    FD file;int directory;std::string name;struct stat original{};std::string digest;
    Pinned(int dir,std::string leaf,std::uint64_t maximum,const std::function<bool()>& live):
        file(openat(dir,leaf.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC)),directory(dir),name(std::move(leaf)){
        require(private_regular_file(file.get(),maximum)&&fstat(file.get(),&original)==0,"scan_file_not_private");digest=hash(file.get(),live);verify();
    }
    void verify()const{struct stat held{},named{};require(fstat(file.get(),&held)==0&&fstatat(directory,name.c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&same(original,held)&&same(original,named),"scan_pinned_object_changed");}
};
struct Program {const char* name;std::uint64_t bytes;const char* sha256;};
constexpr Program programs[]{
    {"clamscan",155312,"fc1d30a98d2ed04aef1252a087268505e83a7ef4feb3ab19431180494540e0bd"},
    {"freshclam",73264,"8bf30b7700d5eaea5fece6577a3e1f59ed765334b271503c9d1df0ebe6439761"},
    {"sigtool",10026720,"01ea07213d6c5307af210bed3c125258f569a06c2324255704c3b38602a5a0d3"},
    {"libclamav.so.12",19424360,"c8fb33f7676017384a7ffffbf74869e88d66716b0e39eb9a88cf61c11f0207ca"},
    {"libclammspack.so.0",76104,"a396d01a9601effeb07259d7bb3cefb4420d0e4132bf99fb6eb25f6bd2f1c89f"},
    {"libclamunrar.so.12",249480,"c2cc7065a0642839b90e4b21f197550c9d3d2b374fc6a561416ad009ee524e30"},
    {"libclamunrar_iface.so",14376,"0c199e15feaa0b0ec3887c8e5ec5cec9cc5615b529040d95b36680bdfef71b5c"},
    {"libfreshclam.so.3",17911152,"8658656d380e2a09c1a3c3ef1c8162485300b5ae8e4d0d74aa5f823af59fe250"}
};
struct Child {
    pid_t pid{-1};bool reaped{};int status{};bool* observed{};
    bool poll(){if(reaped)return true;const auto n=waitpid(pid,&status,WNOHANG);if(n==pid){reaped=true;if(observed)*observed=true;}else require(n>=0||errno==EINTR,"scan_child_wait");return reaped;}
    void stop()noexcept{if(pid<0||reaped)return;(void)kill(-pid,SIGKILL);(void)kill(pid,SIGKILL);pid_t n;do{n=waitpid(pid,&status,0);}while(n<0&&errno==EINTR);reaped=n==pid;if(observed)*observed=reaped;}
    ~Child(){stop();}
};
struct Pipe {
    FD read_end,write_end;std::string text;bool eof{};
    Pipe(){int p[2];require(pipe2(p,O_CLOEXEC)==0,"scan_pipe");read_end=FD(p[0]);write_end=FD(p[1]);require(fcntl(p[0],F_SETFL,O_NONBLOCK)==0,"scan_pipe_nonblocking");}
    bool drain(std::size_t& total){std::array<char,4096> bytes{};while(!eof){const auto n=read(read_end.get(),bytes.data(),bytes.size());
        if(n>0){if(static_cast<std::size_t>(n)>app::kFileScanOutputBytes-total)return false;text.append(bytes.data(),static_cast<std::size_t>(n));total+=static_cast<std::size_t>(n);}
        else if(!n)eof=true;else if(errno==EINTR)continue;else if(errno==EAGAIN||errno==EWOULDBLOCK)return true;else throw std::runtime_error("scan_pipe_read");}return true;}
};
bool close_except(const std::vector<int>& allowed,unsigned maximum)noexcept{
    unsigned begin=3;for(std::size_t i=0;i<=allowed.size();++i){const unsigned end=i==allowed.size()?maximum:static_cast<unsigned>(allowed[i]);
        if(end>begin){
#ifdef SYS_close_range
            if(syscall(SYS_close_range,begin,end-1,0)!=0){if(errno!=ENOSYS&&errno!=EINVAL)return false;for(unsigned fd=begin;fd<end;++fd)(void)close(static_cast<int>(fd));}
#else
            for(unsigned fd=begin;fd<end;++fd)(void)close(static_cast<int>(fd));
#endif
        }begin=end+1;}return true;
}
struct Scratch {
    int parent;std::string name;FD directory,budget;struct stat identity{},allocation{};
    explicit Scratch(int owner):parent(owner){
        std::array<unsigned char,16> random{};randombytes_buf(random.data(),random.size());std::array<char,33> text{};sodium_bin2hex(text.data(),text.size(),random.data(),random.size());name="scan-"+std::string(text.data());
        require(mkdirat(parent,name.c_str(),0700)==0,"scan_scratch_create");directory=FD(openat(parent,name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));require(bool(directory)&&fstat(directory.get(),&identity)==0,"scan_scratch_open");
    }
    void reserve(std::uint64_t bytes){
        budget=FD(openat(directory.get(),"budget",O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));require(bool(budget)&&fstat(budget.get(),&allocation)==0,"scan_budget_create");
        // Anonymous after exclusive creation: storage remains physically reserved
        // while either parent or scanner holds the FD, and kernel-close releases
        // it even if the supervising Runtime dies. Never leave a crash-sized
        // named reservation behind for a later session to misidentify or adopt.
        require(unlinkat(directory.get(),"budget",0)==0,"scan_budget_unlink");
        require(fallocate(budget.get(),0,0,static_cast<off_t>(bytes))==0,"scan_budget_reserve");struct statvfs space{};require(fstatvfs(directory.get(),&space)==0&&space.f_bavail>=kFilePayloadFreeReserve/space.f_frsize+1,"scan_disk_reserve");
    }
    ~Scratch(){
        // Reap is ordered before this destructor. Closing the anonymous budget
        // releases its allocation. If unlink-before-allocation failed, remove
        // only the original empty named file below; never remove a replacement.
        // unexpected vendor temporary files and failed evidence stay in place.
        struct stat current{},named{};
        if(budget&&fstat(budget.get(),&current)==0&&fstatat(directory.get(),"budget",&named,AT_SYMLINK_NOFOLLOW)==0&&current.st_dev==allocation.st_dev&&current.st_ino==allocation.st_ino&&same(current,named)&&current.st_nlink==1){if(ftruncate(budget.get(),0)==0)(void)unlinkat(directory.get(),"budget",0);}
        if(directory&&fstatat(parent,name.c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&named.st_dev==identity.st_dev&&named.st_ino==identity.st_ino)(void)unlinkat(parent,name.c_str(),AT_REMOVEDIR);
    }
};
}
LinuxFileScanner::LinuxFileScanner(std::filesystem::path root):root_(std::move(root)){}
NativeFileScanResult LinuxFileScanner::scan(FilePayloadLease& input,std::uint64_t maximum,const std::function<bool()>& cancelled,const std::function<void(std::uint64_t)>& activity,std::uint32_t deadline)const{
    NativeFileScanResult result;const auto began=now();
    try{
        require(geteuid()!=0&&maximum>0&&maximum<=app::kMaximumCallerFileBytes&&deadline>0&&deadline<=180000,"scan_policy_invalid");
        const auto live=[&]{return now()-began<deadline&&(!cancelled||!cancelled());};require(live(),"scan_preparation_cancelled");
        auto work=open_owned_directory(root_/"work/clamav");
        FD gate(openat(work.get(),"operation.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600));require(private_regular_file(gate.get(),0),"scan_gate_private");
        while(flock(gate.get(),LOCK_EX|LOCK_NB)!=0){require(errno==EWOULDBLOCK||errno==EAGAIN,"scan_gate_failed");require(live(),"scan_preparation_cancelled");if(activity)activity(now()-began);std::this_thread::sleep_for(std::chrono::milliseconds(25));}
        auto engine=open_owned_directory(root_/"bin/clamav"),definitions=open_owned_directory(root_/"data/clamav/db");
        for(const auto& name:names(engine.get()))require(name=="Licenses"||std::any_of(std::begin(programs),std::end(programs),[&](const auto& p){return name==p.name;}),"scan_unpinned_engine_entry");
        std::vector<Pinned> images;for(const auto& p:programs){images.emplace_back(engine.get(),p.name,p.bytes,live);require(static_cast<std::uint64_t>(images.back().original.st_size)==p.bytes&&images.back().digest==p.sha256,"scan_engine_hash");}
        require((images.front().original.st_mode&S_IXUSR)!=0,"scan_engine_not_executable");
        const auto listed=names(definitions.get());for(const auto& name:listed)require(name=="main.cvd"||name=="main.cld"||name=="daily.cvd"||name=="daily.cld"||name=="bytecode.cvd"||name=="bytecode.cld"||name=="freshclam.dat","scan_unexpected_database");
        std::vector<Pinned> databases;
        for(const auto* base:{"main","daily","bytecode"}){std::string selected;for(const auto* suffix:{".cvd",".cld"}){const std::string name=std::string(base)+suffix;if(std::find(listed.begin(),listed.end(),name)!=listed.end()){require(selected.empty(),"scan_duplicate_database");selected=name;}}
            require(!selected.empty(),"scan_database_missing");databases.emplace_back(definitions.get(),selected,512ULL*1024*1024,live);}
        const auto original=input.identity([&](auto,auto){return live();});require(original.bytes<=maximum,"scan_input_limit");auto payload=input.send_transfer_descriptor();
        const auto expanded=std::min<std::uint64_t>(maximum*4,app::kMaximumCallerFileBytes);
        const auto prefix=input.read(0,4);
        if(prefix.size()==4&&prefix[0]=='P'&&prefix[1]=='K'){
            // ClamAV can silently truncate an ordinary ZIP member at its
            // configured maximum. A positively parsed metadata budget breach
            // must hold even if that vendor invocation would report clean.
            // This is NOT a replacement archive policy or content validation:
            // unsupported/ZIP64 metadata retains the existing scanner path.
            const archive::FileZipSource source{original.bytes,[&](auto at,auto count){return input.read(at,count);},[&]{return !live();}};
            const auto inspection=archive::inspect_file_zip(source);
            if(inspection.result.ok()){
                std::uint64_t total=original.bytes;std::size_t files{};
                for(const auto& entry:inspection.entries)if(!entry.directory){
                    require(++files<=1000&&entry.bytes<=maximum&&total<=expanded&&entry.bytes<=expanded-total,"scan_zip_limit");total+=entry.bytes;
                }
            }
            require(live(),"scan_preparation_cancelled");
        }
        Scratch scratch(work.get());scratch.reserve(expanded*2);
        const auto fdpath=[](int fd){return "/proc/self/fd/"+std::to_string(fd);};const auto expected=fdpath(payload.get());
        std::vector<std::string> arguments{"clamscan","--stdout","--allmatch","--official-db-only=yes","--fail-if-cvd-older-than=7","--tempdir="+fdpath(scratch.directory.get()),"--alert-encrypted=yes","--alert-exceeds-max=yes","--max-filesize="+std::to_string(maximum),"--max-scansize="+std::to_string(expanded),"--max-files=1000","--max-recursion=16","--max-scantime=120000","--follow-dir-symlinks=0","--follow-file-symlinks=2"};
        // The terminal component must be a directory, not the /proc FD symlink.
        // Keep ClamAV's directory-level daily-CVD freshness check: selecting each
        // database separately incorrectly applies that age check to stable main.
        arguments.push_back("--database="+fdpath(definitions.get())+"/.");
        arguments.push_back("--");arguments.push_back(expected);
        std::vector<char*> argv;for(auto& a:arguments)argv.push_back(a.data());argv.push_back(nullptr);
        std::string libraries="LD_LIBRARY_PATH="+fdpath(engine.get()),temp="TMPDIR="+fdpath(scratch.directory.get());char locale[]="LC_ALL=C";char* env[]{locale,libraries.data(),temp.data(),nullptr};
        Pipe output,error;FD null(open("/dev/null",O_RDONLY|O_CLOEXEC));require(bool(null),"scan_null");
        std::vector<int> allowed{engine.get(),definitions.get(),scratch.directory.get(),scratch.budget.get(),gate.get(),images.front().file.get(),payload.get()};std::sort(allowed.begin(),allowed.end());
        rlimit open_limit{};require(getrlimit(RLIMIT_NOFILE,&open_limit)==0&&open_limit.rlim_max<=1048576&&allowed.back()<static_cast<int>(open_limit.rlim_max),"scan_descriptor_limit");
        for(const auto& image:images)image.verify();
        for(const auto& database:databases)database.verify();
        require(live(),"scan_preparation_cancelled");
        const auto parent=getpid();Child child;child.observed=&result.process_reaped;child.pid=fork();require(child.pid>=0,"scan_fork");
        if(child.pid==0){
            const rlimit no_children{0,0},no_core{0,0},memory{3ULL*1024*1024*1024,3ULL*1024*1024*1024},file_limit{expanded*2,expanded*2};
            if(setpgid(0,0)||prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent||dup2(null.get(),0)<0||dup2(output.write_end.get(),1)<0||dup2(error.write_end.get(),2)<0||fchdir(scratch.directory.get())||!drop_child_capabilities()||setrlimit(RLIMIT_NPROC,&no_children)||setrlimit(RLIMIT_CORE,&no_core)||setrlimit(RLIMIT_AS,&memory)||setrlimit(RLIMIT_FSIZE,&file_limit))_exit(126);
            for(int fd:allowed)if(fd!=images.front().file.get()&&fcntl(fd,F_SETFD,0)<0)_exit(126);
            if(!close_except(allowed,static_cast<unsigned>(open_limit.rlim_max)))_exit(126);
            fexecve(images.front().file.get(),argv.data(),env);_exit(127);
        }
        result.process_reaped=false;output.write_end=FD();error.write_end=FD();app::FileScanEvidence evidence;evidence.started=true;evidence.expected_output_path=expected;std::size_t captured{};bool overflow{};std::uint64_t last_activity=UINT64_MAX;
        for(;;){
            if(!output.drain(captured)||!error.drain(captured)){overflow=true;break;}
            if(child.poll()){evidence.exited=true;if(WIFEXITED(child.status))evidence.exit_code=static_cast<std::uint32_t>(WEXITSTATUS(child.status));else evidence.exit_code=0xffffffffU;result.exit_code=evidence.exit_code;
                overflow=!output.drain(captured)||!error.drain(captured);evidence.output_complete=!overflow&&output.eof&&error.eof;break;}
            const auto elapsed=now()-began;if(cancelled&&cancelled()){evidence.cancelled=true;break;}if(elapsed>=deadline){evidence.timed_out=true;break;}
            if(activity&&elapsed/500!=last_activity){last_activity=elapsed/500;activity(elapsed);}
            pollfd ready[]{ {output.read_end.get(),POLLIN,0},{error.read_end.get(),POLLIN,0} };(void)poll(ready,2,25);
        }
        child.stop();require(result.process_reaped,"scan_child_exit_unconfirmed");
        require(same(original,input.identity()),"scan_input_changed");for(const auto& image:images)image.verify();for(const auto& database:databases)database.verify();
        // Completion does not extend the caller's authority or deadline. A
        // revoked caller can race the child's exit and final custody checks.
        if(cancelled&&cancelled())evidence.cancelled=true;
        if(now()-began>=deadline)evidence.timed_out=true;
        evidence.standard_output=output.text;evidence.standard_error=error.text;
        result.result=overflow?app::FileScanResult{app::FileScanVerdict::incomplete,{},"Scanner output exceeded limit"}:app::classify_clamav_scan(evidence);
    }catch(const std::exception& error){const std::string_view phase=error.what();result.diagnostic=phase.substr(0,5)=="scan_"&&phase.size()<80?std::string(phase):"scan_native_failure";result.result={app::FileScanVerdict::error,{},"Scanner unavailable or failed; retained privately"};
        if(phase=="scan_zip_limit")result.result={app::FileScanVerdict::incomplete,{},"ZIP exceeds configured scan limits; held privately"};
        else if(cancelled&&cancelled())result.result={app::FileScanVerdict::incomplete,{},"Scan cancelled before acceptance"};else if(now()-began>=deadline)result.result={app::FileScanVerdict::timeout,{},"Scanner deadline exceeded"};
    }catch(...){result.diagnostic="scan_native_failure";result.result={app::FileScanVerdict::error,{},"Scanner unavailable or failed; retained privately"};}
    if(!result.process_reaped){result.diagnostic="scan_child_exit_unconfirmed";result.result={app::FileScanVerdict::error,{},"Scanner exit unconfirmed; retained privately"};}
    result.elapsed_milliseconds=now()-began;return result;
}
}

// Shares the pinned vendor-image and native child-custody implementation above.
#include "clamav_update.inc"
