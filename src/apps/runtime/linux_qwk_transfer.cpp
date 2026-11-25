#include "linux_qwk_transfer.hpp"
#include "linux_qwk_reply_spool.hpp"
#include "qwk_packet_completion.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/archive/qwk_archive.hpp"
#include "sfx/application/message_receive.hpp"
#include <sys/statvfs.h>
#include "message_receive_completion.hpp"
#include "sfx/application/qwk.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <set>
#include <signal.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace sfx::apps::runtime {
namespace {
namespace app=application;
namespace host=platform::linux_host;
using FD=host::FileDescriptor;
using Direction=qwk_packet_detail::Direction;
constexpr std::string_view helper_hash="cc5cabb21216fe4b99b2582560e89280bb785782aa13f441c4476c3426826c7a";
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
struct stat status(int fd){struct stat s{};require(fstat(fd,&s)==0,"packet_stat");return s;}
bool same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
std::string snapshot(int fd,std::size_t limit,const std::function<bool()>& cancelled={}){
    require(host::private_regular_file(fd,limit),"packet_private_object");const auto before=status(fd);
    std::string bytes(static_cast<std::size_t>(before.st_size),'\0');std::size_t at{};
    while(at<bytes.size()){
        require(!cancelled||!cancelled(),"packet_snapshot_cancelled");
        const auto n=pread(fd,bytes.data()+at,std::min<std::size_t>(4096,bytes.size()-at),static_cast<off_t>(at));
        if(n<0&&errno==EINTR)continue;
        require(n>0,"packet_read");at+=static_cast<std::size_t>(n);
    }
    const auto after=status(fd);
    require(same(before,after)&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&
        before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&
        before.st_ctim.tv_nsec==after.st_ctim.tv_nsec,"packet_object_changed");return bytes;
}
FD pin_helper(const std::filesystem::path& root){
    auto directory=host::open_owned_directory(root/"bin/tools");
    FD helper(openat(directory.get(),"sfxqwktransfer",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
    require(bool(helper),"packet_helper_missing");const auto bytes=snapshot(helper.get(),4*1024*1024);
    std::array<unsigned char,32> hash{};std::array<char,65> hex{};
    crypto_hash_sha256(hash.data(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());
    sodium_bin2hex(hex.data(),hex.size(),hash.data(),hash.size());
    require(helper_hash==hex.data()&&(status(helper.get()).st_mode&07777)==0700,"packet_helper_identity");return helper;
}
struct Child {
    pid_t pid{-1};bool reaped{};int result{};
    ~Child(){stop();}
    bool poll(){
        if(reaped)return true;
        const auto n=waitpid(pid,&result,WNOHANG);
        if(n==pid)reaped=true;else require(n>=0||errno==EINTR,"packet_wait");return reaped;
    }
    void stop()noexcept{
        if(pid<0||reaped)return;
        (void)kill(-pid,SIGKILL);(void)kill(pid,SIGKILL);
        pid_t n;do{n=waitpid(pid,&result,0);}while(n<0&&errno==EINTR);reaped=n==pid;
    }
};
struct Queue {
    std::string data;std::size_t first{};bool paused{};
    std::size_t size()const{return data.size()-first;}
    std::size_t capacity(){if(size()>=app::kMessageReceiveHighWater)paused=true;else if(size()<app::kMessageReceiveLowWater)paused=false;return paused?0:std::min<std::size_t>(4096,app::kMessageReceiveQueueBytes-size());}
    void append(const void* p,std::size_t n){require(n<=app::kMessageReceiveQueueBytes-size(),"packet_queue_bound");if(first){data.erase(0,first);first=0;}data.append(static_cast<const char*>(p),n);}
    std::string_view front()const{return std::string_view(data).substr(first,std::min<std::size_t>(4096,size()));}
    void consume(std::size_t n){require(n<=size(),"packet_queue_consume");first+=n;if(first==data.size()){data.clear();first=0;}}
};
// Child side only; keep the precomputed allowlist and close every other FD.
// close_range is an optimization, with a bounded all-FD fallback for old Linux.
bool close_except(const std::vector<int>& allowed,unsigned maximum)noexcept{
    unsigned begin=3;
    for(std::size_t i=0;i<=allowed.size();++i){
        const unsigned end=i==allowed.size()?maximum:static_cast<unsigned>(allowed[i]);
        if(end>begin){
#ifdef SYS_close_range
            if(syscall(SYS_close_range,begin,end-1,0)!=0){
                if(errno!=ENOSYS&&errno!=EINVAL)return false;
                for(unsigned fd=begin;fd<end;++fd)(void)close(static_cast<int>(fd));
            }
#else
            for(unsigned fd=begin;fd<end;++fd)(void)close(static_cast<int>(fd));
#endif
        }
        begin=end+1;
    }
    return true;
}
bool unchanged(const struct stat& a,const struct stat& b){
    return same(a,b)&&a.st_mode==b.st_mode&&a.st_uid==b.st_uid&&a.st_gid==b.st_gid&&b.st_nlink==1&&a.st_size==b.st_size&&
        a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec&&a.st_ctim.tv_sec==b.st_ctim.tv_sec&&a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;
}
struct Work {
    struct Captured{std::string name;FD file;struct stat identity;};
    std::filesystem::path path;FD parent,directory;std::string name;
    struct stat parent_identity{},identity{};std::vector<Captured> captured;bool confirmed{},created{};
    std::function<void(std::string,std::string)> problem;
    void report(const char* diagnostic)noexcept{try{if(problem&&created)problem(name,diagnostic);}catch(...){}}
    explicit Work(const std::filesystem::path& root,std::function<void(std::string,std::string)> notify):parent(host::open_owned_directory(root/"work")),problem(std::move(notify)){
        try{
        require(sodium_init()>=0,"packet_rng");struct statvfs space{};
        require(fstatvfs(parent.get(),&space)==0&&space.f_frsize>0&&space.f_bavail>=(128ULL*1024*1024+space.f_frsize-1)/space.f_frsize,"packet_work_space");
        std::array<unsigned char,16> random{};std::array<char,33> hex{};randombytes_buf(random.data(),random.size());sodium_bin2hex(hex.data(),hex.size(),random.data(),random.size());
        name="qwk-"+std::string(hex.data());path=root/"work"/name;parent_identity=status(parent.get());
        require(mkdirat(parent.get(),name.c_str(),0700)==0,"packet_work_create");created=true;
        directory=FD(openat(parent.get(),name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));require(directory&&fsync(parent.get())==0,"packet_work_open");identity=status(directory.get());
        }catch(...){report("packet_work_creation_incomplete");throw;}
    }
    void verify()const{
        auto named_parent=host::open_owned_directory(path.parent_path());auto named_directory=host::open_owned_directory(path);
        const auto current=status(named_directory.get());require(same(parent_identity,status(named_parent.get()))&&same(identity,current)&&
            same(identity,status(directory.get()))&&(current.st_mode&07777)==0700,"packet_work_changed");
    }
    void exact_names(const std::set<std::string>& expected)const{
        verify();FD scan(openat(directory.get(),".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));require(bool(scan),"packet_work_scan");
        const auto descriptor=dup(scan.get());require(descriptor>=0,"packet_work_scan_dup");DIR* stream=fdopendir(descriptor);
        if(!stream){close(descriptor);throw std::runtime_error("packet_work_scan_open");}
        struct Close{DIR* value;~Close(){closedir(value);}}close_stream{stream};std::set<std::string> found;
        for(;;){errno=0;const auto entry=readdir(stream);if(!entry){require(errno==0,"packet_work_scan_failed");break;}
            const std::string item(entry->d_name);if(item=="."||item=="..")continue;require(expected.count(item)!=0&&found.insert(item).second,"packet_extra_object");}
        require(found==expected,"packet_object_count");
    }
    void capture(std::string name,int descriptor){
        FD duplicate(fcntl(descriptor,F_DUPFD_CLOEXEC,3));require(bool(duplicate),"packet_capture_duplicate");
        const auto identity=status(duplicate.get());require(host::private_regular_file(duplicate.get(),app::kQwkCompressedBytes)&&(identity.st_mode&07777)==0600,"packet_capture_private");
        captured.push_back({std::move(name),std::move(duplicate),identity});
    }
    void check_captured()const{
        verify();for(const auto& item:captured){struct stat named{};require(fstatat(directory.get(),item.name.c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&
            unchanged(item.identity,named)&&unchanged(item.identity,status(item.file.get())),"packet_cleanup_changed");}
    }
    void confirm(){check_captured();confirmed=true;}
    ~Work(){
        try{
            if(!confirmed){report("packet_work_unconfirmed");return;}
            check_captured();std::set<std::string> names;for(const auto& item:captured)names.insert(item.name);exact_names(names);
            for(const auto& item:captured)require(unlinkat(directory.get(),item.name.c_str(),0)==0,"packet_cleanup_unlink");
            require(fsync(directory.get())==0,"packet_cleanup_flush");verify();
            require(unlinkat(parent.get(),name.c_str(),AT_REMOVEDIR)==0&&fsync(parent.get())==0,"packet_cleanup_directory");
        }catch(...){report("packet_work_cleanup_incomplete");}
    }
};
std::uint64_t monotonic_millis(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
struct TransferResult {app::QwkTransferCode code{app::QwkTransferCode::failed};app::QwkTransferRefusal refusal{};std::shared_ptr<app::IQwkReplySource> source;std::shared_ptr<std::atomic<bool>> cancellation;std::function<void()> confirm_completed;};
}
struct LinuxQwkPacketTransfer::Impl {
    std::filesystem::path root;app::DecodedQwkTransferChannel channel;std::function<void(std::string_view)> observer;std::string unavailable;
    std::function<void(std::string,std::string)> work_problem;
    Impl(std::filesystem::path r,app::DecodedQwkTransferChannel c,std::function<void(std::string_view)> observe,std::function<void(std::string,std::string)> problem):root(std::move(r)),channel(std::move(c)),observer(std::move(observe)),work_problem(std::move(problem)){}
    void observed(std::string_view phase)noexcept{try{if(observer)observer(phase);}catch(...){}}
    FD pin(const app::QwkTransferRequest& request){
        require(geteuid()!=0&&root.is_absolute()&&root==root.lexically_normal()&&root.native().find('\0')==std::string::npos&&
            channel.acquire&&channel.release&&channel.read&&channel.write&&channel.now&&channel.absolute_deadline&&channel.authorized&&
            app::valid_qwk_board_id(request.board_id)&&app::canonical_qwk_board_id(request.board_id)==request.board_id&&request.board_revision>0,"packet_request_channel");
        auto owner=host::open_owned_directory(root);auto bin=host::open_owned_directory(root/"bin");auto work=host::open_owned_directory(root/"work");
        return pin_helper(root);
    }
    TransferResult run(const app::QwkTransferRequest& request,Direction direction,std::string_view zip){
        bool leased{},synchronized{};
        struct Release{app::DecodedQwkTransferChannel& channel;bool& leased;bool& synchronized;~Release(){if(leased)try{channel.release(synchronized);}catch(...){}}}release{channel,leased,synchronized};
        try{
            auto helper=pin(request);require(channel.authorized(request),"packet_authority");const bool sending=direction==Direction::send;
            require(!sending||(!zip.empty()&&zip.size()<=app::kQwkCompressedBytes),"packet_send_bounds");
            auto work=std::make_shared<Work>(root,work_problem);FD send_file;
            if(sending){host::publish_private_file(work->directory.get(),"payload.qwk",{zip.begin(),zip.end()},false);
                send_file=FD(openat(work->directory.get(),"payload.qwk",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));require(send_file&&snapshot(send_file.get(),app::kQwkCompressedBytes)==zip,"packet_send_exact");}
            int sockets[2];require(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets)==0,"packet_socketpair");FD parent(sockets[0]),endpoint(sockets[1]);
            require(fcntl(parent.get(),F_SETFL,fcntl(parent.get(),F_GETFL)|O_NONBLOCK)==0,"packet_nonblocking");
            FD null(open("/dev/null",O_RDWR|O_CLOEXEC));require(bool(null),"packet_null");
            std::string option=sending?"--sfx-packet-send-v1":"--sfx-packet-receive-v1",descriptor=std::to_string(endpoint.get()),board=request.board_id;
            char program[]="sfxqwktransfer",locale[]="LC_ALL=C";char* arguments[]{program,option.data(),descriptor.data(),sending?board.data():nullptr,nullptr};char* environment[]{locale,nullptr};
            std::vector<int> allowed{helper.get(),endpoint.get()};std::sort(allowed.begin(),allowed.end());
            rlimit open_limit{};require(getrlimit(RLIMIT_NOFILE,&open_limit)==0&&open_limit.rlim_max<=1048576&&allowed.back()<static_cast<int>(open_limit.rlim_max),"packet_fd_limit");
            require(channel.authorized(request)&&channel.acquire(),"packet_acquire");leased=true;
            const auto began=channel.now();auto deadline=began+app::kMessageReceiveTotalMillis;const auto initial=channel.absolute_deadline();if(initial)deadline=std::min(deadline,initial);
            require(deadline>began,"packet_initial_deadline");const auto supervisor=getpid();Child child;child.pid=fork();require(child.pid>=0,"packet_fork");
            if(child.pid==0){
                const rlimit no_children{0,0},no_core{0,0},file_size{app::kQwkCompressedBytes,app::kQwkCompressedBytes};
                if(setpgid(0,0)||prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=supervisor||
                    dup2(null.get(),0)<0||dup2(null.get(),1)<0||dup2(null.get(),2)<0||fchdir(work->directory.get())||
                    !host::drop_child_capabilities()||setrlimit(RLIMIT_NPROC,&no_children)||setrlimit(RLIMIT_CORE,&no_core)||setrlimit(RLIMIT_FSIZE,&file_size))_exit(126);
                if(fcntl(endpoint.get(),F_SETFD,0)<0||!close_except(allowed,static_cast<unsigned>(open_limit.rlim_max)))_exit(126);
                fexecve(helper.get(),arguments,environment);_exit(127);
            }
            // Keep this otherwise unused endpoint until reap. Early buffered
            // caller input must not reset away the helper's final output.
            Queue inbound,outbound;message_receive_detail::ForwardedInput input;qwk_packet_detail::OutputLedger output;
            std::uint64_t last=began;bool eof{},complete{},half_closed{};
            for(;;){
                const auto now=channel.now(),live=channel.absolute_deadline();if(live)deadline=std::min(deadline,live);
                if(now<began||now>=deadline||now<last||now-last>=app::kMessageReceiveIdleMillis||!channel.authorized(request))break;
                bool progressed{};child.poll();
                if(child.reaped&&!half_closed){require(shutdown(endpoint.get(),SHUT_WR)==0,"packet_reaped_half_close");half_closed=true;}
                if(!child.reaped&&!eof&&inbound.size()){
                    const auto bytes=inbound.front();const auto n=::send(parent.get(),bytes.data(),bytes.size(),MSG_NOSIGNAL);
                    if(n>0){require(input.record(bytes.substr(0,static_cast<std::size_t>(n))),"packet_input_count");inbound.consume(static_cast<std::size_t>(n));progressed=true;}
                    else if(n==0||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))break;
                }
                if(!eof&&outbound.capacity()){
                    std::array<char,4096> bytes{};const auto n=recv(parent.get(),bytes.data(),outbound.capacity(),0);
                    if(n>0){outbound.append(bytes.data(),static_cast<std::size_t>(n));progressed=true;}
                    else if(n==0)eof=true;else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)break;
                }
                if(outbound.size()){
                    const auto bytes=outbound.front();const auto result=channel.write(bytes);require(app::valid_interactive_stream_write_result(bytes.size(),result),"packet_caller_write");
                    if(result.outcome==app::InteractiveStreamOutcome::completed){require(output.record(bytes.substr(0,result.bytes_written)),"packet_output_count");outbound.consume(result.bytes_written);progressed=true;}
                    else if(result.outcome!=app::InteractiveStreamOutcome::timed_out)break;
                }
                if(!child.reaped&&!eof&&inbound.capacity()){
                    const auto maximum=inbound.capacity();const auto result=channel.read(maximum);require(app::valid_interactive_stream_read_result(maximum,result),"packet_caller_read");
                    if(result.outcome==app::InteractiveStreamOutcome::completed){inbound.append(result.bytes.data(),result.bytes.size());last=channel.now();progressed=true;}
                    else if(result.outcome!=app::InteractiveStreamOutcome::timed_out)break;
                }
                if(child.reaped&&eof&&!outbound.size()){complete=WIFEXITED(child.result)&&WEXITSTATUS(child.result)==0;break;}
                if(!progressed){pollfd item{parent.get(),POLLIN,0};(void)poll(&item,1,1);}else std::this_thread::yield();
            }
            if(!complete){(void)shutdown(parent.get(),SHUT_RDWR);child.stop();require(child.reaped,"packet_child_not_reaped");observed("packet_relay_incomplete");return {app::QwkTransferCode::disconnected,{},{},{},{}};}
            const auto cancelled=[&]{const auto live=channel.absolute_deadline();if(live)deadline=std::min(deadline,live);const auto now=channel.now();return now<began||now>=deadline||now<last||now-last>=app::kMessageReceiveIdleMillis||!channel.authorized(request);};
            require(child.reaped&&!cancelled(),"packet_final_authority");work->verify();
            const char* payload=sending?"payload.qwk":"payload.rep";struct stat named{};const bool present=fstatat(work->directory.get(),payload,&named,AT_SYMLINK_NOFOLLOW)==0;
            require(present||errno==ENOENT,"packet_payload_stat");std::set<std::string> names{"result.txt","dsz.log"};if(present)names.insert(payload);work->exact_names(names);
            FD received,receipt(openat(work->directory.get(),"result.txt",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)),dsz(openat(work->directory.get(),"dsz.log",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
            if(!sending&&present)received=FD(openat(work->directory.get(),payload,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
            auto bytes=present?snapshot(sending?send_file.get():received.get(),app::kQwkCompressedBytes,cancelled):std::string{};
            if(sending)require(bytes==zip,"packet_send_final_identity");
            const auto proof=qwk_packet_detail::completion(direction,snapshot(receipt.get(),1024,cancelled),snapshot(dsz.get(),1024,cancelled),present,bytes.size());
            require(proof&&output.finished(direction,proof->produced)&&input.completion_tail(proof->consumed,std::string_view(inbound.data).substr(inbound.first))&&!cancelled(),"packet_actual_completion");
            if(present)work->capture(payload,sending?send_file.get():received.get());
            work->capture("result.txt",receipt.get());work->capture("dsz.log",dsz.get());work->check_captured();synchronized=true;
            if(proof->refusal){if(proof->refusal==3)work->confirm();return {proof->refusal==3?app::QwkTransferCode::cancelled:app::QwkTransferCode::refused,
                proof->refusal==1?app::QwkTransferRefusal::extension:proof->refusal==2?app::QwkTransferRefusal::too_large:proof->refusal==4?app::QwkTransferRefusal::skipped:app::QwkTransferRefusal::none,{},{},{}};}
            if(sending)return {app::QwkTransferCode::completed,{},{},{},[work]{work->confirm();}};
            auto archive=sfx::archive::read_qwk_reply_zip(bytes,request.board_id,cancelled);
            if(!archive.result.ok())return {app::QwkTransferCode::refused,app::QwkTransferRefusal::content,{},{},{}};
            std::uint64_t expanded{};std::vector<app::QwkFile> files;
            for(auto& f:archive.files){expanded+=f.bytes.size();files.push_back({std::move(f.name),std::move(f.bytes)});}archive={};
            auto packet=app::decode_qwk_reply(request.board_id,files);files={};
            if(!packet)return {app::QwkTransferCode::refused,app::QwkTransferRefusal::content,{},{},{}};
            require(!cancelled(),"packet_authority_before_seal");auto stop=std::make_shared<std::atomic<bool>>(false);
            // Convert remaining channel time to an independent monotonic limit;
            // this callback never retains a caller stack or Runtime reference.
            const auto now=channel.now();require(now<deadline,"packet_seal_deadline");const auto source_deadline=monotonic_millis()+deadline-now;
            auto sealed=seal_linux_qwk_reply_spool(work->path,packet,bytes.size(),expanded,[stop,source_deadline]{return stop->load()||monotonic_millis()>=source_deadline;},work,work->directory.get(),[work]{work->confirm();});
            if(!sealed||!sealed.value){observed("packet_reply_spool_refused");return {app::QwkTransferCode::failed,{},{},{},{}};}
            require(!cancelled(),"packet_authority_after_seal");return {app::QwkTransferCode::completed,{},*sealed.value,std::move(stop),{}};
        }catch(const std::exception& e){const std::string_view phase=e.what();observed(phase.size()<=64&&phase.substr(0,7)=="packet_"?phase:"packet_local_failure");}
        catch(...){observed("packet_local_failure");}
        return {leased&&!synchronized?app::QwkTransferCode::disconnected:leased?app::QwkTransferCode::failed:app::QwkTransferCode::unavailable,{},{},{},{}};
    }
};
LinuxQwkPacketTransfer::LinuxQwkPacketTransfer(std::filesystem::path root,app::DecodedQwkTransferChannel channel,std::function<void(std::string_view)> observer,std::function<void(std::string,std::string)> work_problem):impl_(std::make_unique<Impl>(std::move(root),std::move(channel),std::move(observer),std::move(work_problem))){}
LinuxQwkPacketTransfer::~LinuxQwkPacketTransfer()=default;
bool LinuxQwkPacketTransfer::available(const app::QwkTransferRequest& request){
    impl_->unavailable.clear();try{auto helper=impl_->pin(request);if(impl_->channel.authorized(request))return true;impl_->unavailable="Packet transfer authority changed. Return to the menu and try again.";}
    catch(...){impl_->unavailable="Packet transfer preflight failed. Ask the Sysop to check Runtime logs and installation access.";}return false;
}
std::string LinuxQwkPacketTransfer::unavailable_reason()const{return impl_->unavailable;}
app::QwkReceiveResult LinuxQwkPacketTransfer::receive(const app::QwkTransferRequest& request){auto r=impl_->run(request,Direction::receive,{});return {r.code,r.refusal,std::move(r.source),std::move(r.cancellation)};}
app::QwkSendResult LinuxQwkPacketTransfer::send(const app::QwkTransferRequest& request,std::string_view zip){auto r=impl_->run(request,Direction::send,zip);return {r.code,r.refusal,std::move(r.confirm_completed)};}
}
