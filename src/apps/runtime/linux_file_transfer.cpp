#include "linux_file_transfer.hpp"
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
using Direction=file_transfer_detail::Direction;
constexpr std::string_view helper_hash="02991f394251dff2bdee1c5892dfb165df038a126b419e7f246e059d7c8859eb";
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
struct stat status(int fd){struct stat s{};require(fstat(fd,&s)==0,"file_transfer_stat");return s;}
bool same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
std::string snapshot(int fd,std::size_t limit){
    require(host::private_regular_file(fd,limit),"file_transfer_private_object");const auto before=status(fd);
    std::string bytes(static_cast<std::size_t>(before.st_size),'\0');std::size_t at{};
    while(at<bytes.size()){
        const auto n=pread(fd,bytes.data()+at,bytes.size()-at,static_cast<off_t>(at));
        if(n<0&&errno==EINTR)continue;
        require(n>0,"file_transfer_read");at+=static_cast<std::size_t>(n);
    }
    const auto after=status(fd);
    require(same(before,after)&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&
        before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&
        before.st_ctim.tv_nsec==after.st_ctim.tv_nsec,"file_transfer_object_changed");return bytes;
}
FD pin_helper(const std::filesystem::path& root){
    auto directory=host::open_owned_directory(root/"bin/tools");
    FD helper(openat(directory.get(),"sfxfiletransfer",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
    require(bool(helper),"file_transfer_helper_missing");const auto bytes=snapshot(helper.get(),4*1024*1024);
    std::array<unsigned char,32> hash{};std::array<char,65> hex{};
    crypto_hash_sha256(hash.data(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());
    sodium_bin2hex(hex.data(),hex.size(),hash.data(),hash.size());
    require(helper_hash==hex.data()&&(status(helper.get()).st_mode&S_IXUSR),"file_transfer_helper_identity");return helper;
}
std::uint64_t exact_object(int fd,const app::FileObjectIdentity& expected){
    require(host::private_regular_file(fd,app::kMaximumCallerFileBytes),"file_transfer_payload_private");const auto s=status(fd);
    require(std::to_string(s.st_dev)==expected.volume&&std::to_string(s.st_ino)==expected.file_id,"file_transfer_payload_identity");
    return static_cast<std::uint64_t>(s.st_size);
}
std::string encoded_name(std::string_view name){
    require(!name.empty()&&name.size()<=128&&app::valid_file_name(app::qwk_cp437_to_utf8(name)),"file_transfer_name");
    std::string encoded;
    for(unsigned char c:name){
        require(c>=32&&c!=127&&std::string_view("/\\:<>\"|?*").find(static_cast<char>(c))==std::string_view::npos,"file_transfer_name_structure");
        encoded+="0123456789ABCDEF"[c>>4];encoded+="0123456789ABCDEF"[c&15];
    }
    return encoded;
}
struct Work {
    FD parent,directory;std::string name,configuration,receipt;struct stat bound{};bool completed{};
    explicit Work(const std::filesystem::path& root):parent(host::open_owned_directory(root/"work")){
        require(sodium_init()>=0,"file_transfer_rng");std::array<unsigned char,16> random{};std::array<char,33> hex{};
        randombytes_buf(random.data(),random.size());sodium_bin2hex(hex.data(),hex.size(),random.data(),random.size());name="file-"+std::string(hex.data());
        require(mkdirat(parent.get(),name.c_str(),0700)==0,"file_transfer_work_create");
        directory=FD(openat(parent.get(),name.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        require(bool(directory)&&fsync(parent.get())==0,"file_transfer_work_open");bound=status(directory.get());
    }
    void verify()const{
        struct stat named{};require(fstatat(parent.get(),name.c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&same(bound,named)&&
            named.st_uid==geteuid()&&(named.st_mode&0777)==0700,"file_transfer_work_changed");
    }
    void confirm(){
        if(completed)return;
        verify();
        for(const auto& item:{std::pair<const char*,const std::string&>{"job.cfg",configuration},{"result.txt",receipt}}){
            const auto bytes=host::read_private_file(directory.get(),item.first,131072);
            require(std::string(bytes.begin(),bytes.end())==item.second,"file_transfer_cleanup_changed");
        }
        require(unlinkat(directory.get(),"job.cfg",0)==0&&unlinkat(directory.get(),"result.txt",0)==0&&
            fsync(directory.get())==0,"file_transfer_cleanup_failed");
        verify();require(unlinkat(parent.get(),name.c_str(),AT_REMOVEDIR)==0&&fsync(parent.get())==0,"file_transfer_directory_cleanup_failed");completed=true;
    }
    void verify_contents()const{
        FD scan(openat(directory.get(),".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));require(bool(scan),"file_transfer_work_scan");
        const auto descriptor=dup(scan.get());require(descriptor>=0,"file_transfer_work_scan_dup");
        DIR* stream=fdopendir(descriptor);if(!stream){close(descriptor);throw std::runtime_error("file_transfer_work_scan_open");}
        struct Close{DIR* value;~Close(){closedir(value);}}close_stream{stream};
        unsigned count{};
        for(;;){
            errno=0;const auto entry=readdir(stream);if(!entry){require(errno==0,"file_transfer_work_scan_failed");break;}
            const std::string_view name(entry->d_name);if(name=="."||name=="..")continue;
            require((name=="job.cfg"||name=="result.txt")&&++count<=2,"file_transfer_extra_control_object");
        }
        require(count==2,"file_transfer_control_count");
    }
};
struct Child {
    pid_t pid{-1};bool reaped{};int result{};
    ~Child(){stop();}
    bool poll(){
        if(reaped)return true;
        const auto n=waitpid(pid,&result,WNOHANG);
        if(n==pid)reaped=true;else require(n>=0||errno==EINTR,"file_transfer_wait");return reaped;
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
    void append(const void* p,std::size_t n){require(n<=app::kMessageReceiveQueueBytes-size(),"file_transfer_queue_bound");if(first){data.erase(0,first);first=0;}data.append(static_cast<const char*>(p),n);}
    std::string_view front()const{return std::string_view(data).substr(first,std::min<std::size_t>(4096,size()));}
    void consume(std::size_t n){require(n<=size(),"file_transfer_queue_consume");first+=n;if(first==data.size()){data.clear();first=0;}}
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
bool recover(LinuxFileTransferChannel& channel,std::uint64_t deadline,std::size_t buffered){
    if(!channel.authorized()||(channel.begin_recovery&&!channel.begin_recovery())||buffered>262144)return false;
    const auto began=channel.now();const auto end=std::min(deadline,began+5000);
    const auto valid=[&]{const auto now=channel.now(),live=channel.absolute_deadline();return channel.authorized()&&now>=began&&now<end&&(!live||now<live);};
    constexpr std::string_view cancel="\x18\x18\x18\x18\x18\x18\x18\x18\b\b\b\b\b\b\b\b";std::size_t sent{};
    while(sent<cancel.size()){
        if(!valid())return false;
        const auto bytes=cancel.substr(sent);const auto result=channel.write(bytes);
        if(!app::valid_interactive_stream_write_result(bytes.size(),result))return false;
        if(result.outcome==app::InteractiveStreamOutcome::completed)sent+=result.bytes_written;
        else if(result.outcome!=app::InteractiveStreamOutcome::timed_out)return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto quiet=channel.now();std::size_t drained=buffered;
    while(valid()){
        const auto result=channel.read(4096);if(!app::valid_interactive_stream_read_result(4096,result))return false;
        if(result.outcome==app::InteractiveStreamOutcome::completed){if(result.bytes.size()>262144-drained)return false;drained+=result.bytes.size();quiet=channel.now();}
        else if(result.outcome==app::InteractiveStreamOutcome::timed_out){const auto now=channel.now();if(now>=quiet&&now-quiet>=1000)return valid();}
        else return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}
}
LinuxFileTransfer::LinuxFileTransfer(std::filesystem::path root,LinuxFileTransferChannel channel):root_(std::move(root)),channel_(std::move(channel)){}
bool LinuxFileTransfer::available(){
    try{auto helper=pin_helper(root_);return geteuid()!=0&&channel_.authorized&&channel_.authorized();}catch(...){return false;}
}
LinuxFileTransferResult LinuxFileTransfer::run(Direction direction,const std::vector<LinuxFileTransferItem>& items,app::TransferProtocolPreference protocol){
    bool leased{},synchronized{};
    struct Release{LinuxFileTransferChannel& channel;bool& leased;bool& synchronized;~Release(){if(leased)try{channel.release(synchronized);}catch(...){}}}release{channel_,leased,synchronized};
    try{
        require(geteuid()!=0&&channel_.acquire&&channel_.release&&channel_.authorized&&channel_.read&&channel_.write&&channel_.now&&channel_.absolute_deadline,"file_transfer_channel");
        const auto code=file_transfer_detail::protocol_code(protocol);const bool sending=direction==Direction::send;
        require(code&&!items.empty()&&items.size()<=app::kFileQueueMaximum&&(code!='X'||items.size()==1)&&channel_.authorized(),"file_transfer_manifest");
        auto helper=pin_helper(root_);auto work=std::make_shared<Work>(root_);
        std::vector<FD> payloads;std::vector<file_transfer_detail::ExpectedFile> expected;
        std::set<std::string> names;std::set<std::pair<std::string,std::string>> objects;
        std::string configuration=sending?"SFX-FILE-SEND 1\n":"SFX-FILE-RECEIVE 1\n";configuration+=std::to_string(items.size())+'\n';
        for(const auto& item:items){
            require(item.maximum>0&&item.maximum<=app::kMaximumCallerFileBytes,"file_transfer_item_bound");const auto encoded=encoded_name(item.wire_name);
            auto folded=item.wire_name;for(char& c:folded)if(c>='a'&&c<='z')c-=32;
            require(names.insert(folded).second&&objects.insert({item.native_object.volume,item.native_object.file_id}).second,"file_transfer_duplicate");
            FD fd(fcntl(item.descriptor,F_DUPFD_CLOEXEC,3));require(bool(fd),"file_transfer_descriptor");const auto flags=fcntl(fd.get(),F_GETFL);
            require(flags>=0&&(flags&O_ACCMODE)==(sending?O_RDONLY:O_WRONLY)&&!(flags&O_APPEND),"file_transfer_descriptor_access");
            const auto bytes=exact_object(fd.get(),item.native_object);require(bytes<=item.maximum&&(sending||bytes==0),"file_transfer_initial_size");
            expected.push_back({item.maximum,bytes});configuration+=std::to_string(fd.get())+' '+std::to_string(item.maximum)+' '+encoded+'\n';payloads.push_back(std::move(fd));
        }
        host::publish_private_file(work->directory.get(),"job.cfg",{configuration.begin(),configuration.end()},false);work->configuration=configuration;
        int sockets[2];require(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets)==0,"file_transfer_socketpair");FD parent(sockets[0]),endpoint(sockets[1]);
        require(fcntl(parent.get(),F_SETFL,fcntl(parent.get(),F_GETFL)|O_NONBLOCK)==0,"file_transfer_nonblocking");
        FD null(open("/dev/null",O_RDWR|O_CLOEXEC));require(bool(null),"file_transfer_null");
        std::string option=std::string("--sfx-file-")+(sending?"send":"receive")+(code=='Z'?"-v1":"-v2");
        std::string descriptor=std::to_string(endpoint.get()),protocol_argument(1,code);char program[]="sfxfiletransfer",locale[]="LC_ALL=C";
        char* arguments[]{program,option.data(),descriptor.data(),code=='Z'?nullptr:protocol_argument.data(),nullptr};char* environment[]{locale,nullptr};
        std::vector<int> allowed{helper.get(),endpoint.get()};for(const auto& fd:payloads)allowed.push_back(fd.get());std::sort(allowed.begin(),allowed.end());
        rlimit open_limit{};require(getrlimit(RLIMIT_NOFILE,&open_limit)==0&&open_limit.rlim_max<=1048576&&allowed.back()<static_cast<int>(open_limit.rlim_max),"file_transfer_fd_limit");
        require(channel_.authorized()&&channel_.acquire(),"file_transfer_acquire");leased=true;
        const auto began=channel_.now();auto deadline=began+app::kMessageReceiveTotalMillis;const auto initial=channel_.absolute_deadline();if(initial)deadline=std::min(deadline,initial);
        require(deadline>began,"file_transfer_initial_deadline");const auto supervisor=getpid();
        Child child;child.pid=fork();require(child.pid>=0,"file_transfer_fork");
        if(child.pid==0){
            const rlimit no_children{0,0},no_core{0,0};
            if(setpgid(0,0)||prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=supervisor||
               dup2(null.get(),0)<0||dup2(null.get(),1)<0||dup2(null.get(),2)<0||fchdir(work->directory.get())||
               !host::drop_child_capabilities()||setrlimit(RLIMIT_NPROC,&no_children)||setrlimit(RLIMIT_CORE,&no_core))_exit(126);
            for(int fd:allowed)if(fd!=helper.get()&&fcntl(fd,F_SETFD,0)<0)_exit(126);
            if(!close_except(allowed,static_cast<unsigned>(open_limit.rlim_max)))_exit(126);
            fexecve(helper.get(),arguments,environment);_exit(127);
        }
        endpoint=FD();Queue inbound,outbound;message_receive_detail::ForwardedInput input;file_transfer_detail::OutputLedger output;
        std::uint64_t last=began;bool eof{},complete{};
        for(;;){
            const auto now=channel_.now(),live=channel_.absolute_deadline();if(live)deadline=std::min(deadline,live);
            if(now<began||now>=deadline||now<last||now-last>=app::kMessageReceiveIdleMillis||!channel_.authorized())break;
            bool progressed{};child.poll();
            if(!child.reaped&&!eof&&inbound.size()){
                const auto bytes=inbound.front();const auto n=send(parent.get(),bytes.data(),bytes.size(),MSG_NOSIGNAL);
                if(n>0){require(input.record(bytes.substr(0,static_cast<std::size_t>(n))),"file_transfer_input_count");inbound.consume(static_cast<std::size_t>(n));progressed=true;}
                else if(n==0||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))break;
            }
            if(!eof&&outbound.capacity()){
                std::array<char,4096> bytes{};const auto n=recv(parent.get(),bytes.data(),outbound.capacity(),0);
                if(n>0){outbound.append(bytes.data(),static_cast<std::size_t>(n));progressed=true;}
                else if(n==0)eof=true;else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)break;
            }
            if(outbound.size()){
                const auto bytes=outbound.front();const auto result=channel_.write(bytes);require(app::valid_interactive_stream_write_result(bytes.size(),result),"file_transfer_caller_write");
                if(result.outcome==app::InteractiveStreamOutcome::completed){require(output.record(bytes.substr(0,result.bytes_written)),"file_transfer_output_count");outbound.consume(result.bytes_written);progressed=true;if(sending)last=channel_.now();}
                else if(result.outcome!=app::InteractiveStreamOutcome::timed_out)break;
            }
            if(!child.reaped&&!eof&&inbound.capacity()){
                const auto maximum=inbound.capacity();const auto result=channel_.read(maximum);require(app::valid_interactive_stream_read_result(maximum,result),"file_transfer_caller_read");
                if(result.outcome==app::InteractiveStreamOutcome::completed){inbound.append(result.bytes.data(),result.bytes.size());last=channel_.now();progressed=true;}
                else if(result.outcome!=app::InteractiveStreamOutcome::timed_out)break;
            }
            if(child.reaped&&eof&&!outbound.size()){complete=WIFEXITED(child.result)&&WEXITSTATUS(child.result)==0;break;}
            if(!progressed){pollfd descriptor_poll{parent.get(),POLLIN,0};(void)poll(&descriptor_poll,1,1);}else std::this_thread::yield();
        }
        if(!complete){
            (void)shutdown(parent.get(),SHUT_RDWR);child.stop();require(child.reaped,"file_transfer_child_not_reaped");
            synchronized=recover(channel_,deadline,inbound.size());return {synchronized?LinuxFileTransferCode::failed:LinuxFileTransferCode::disconnected,{},{},{}};
        }
        const auto valid=[&]{
            const auto live=channel_.absolute_deadline();if(live)deadline=std::min(deadline,live);
            const auto now=channel_.now();return now>=began&&now<deadline&&now>=last&&
                now-last<app::kMessageReceiveIdleMillis&&channel_.authorized();
        };
        require(child.reaped&&valid(),"file_transfer_final_authority");
        for(std::size_t i=0;i<items.size();++i){const auto bytes=exact_object(payloads[i].get(),items[i].native_object);require(bytes<=items[i].maximum&&(!sending||bytes==expected[i].actual),"file_transfer_final_size");expected[i].actual=bytes;}
        work->verify();work->verify_contents();
        const auto config=host::read_private_file(work->directory.get(),"job.cfg",131072),receipt=host::read_private_file(work->directory.get(),"result.txt",file_transfer_detail::maximum_receipt);
        require(std::string(config.begin(),config.end())==configuration,"file_transfer_manifest_changed");work->receipt.assign(receipt.begin(),receipt.end());
        const auto proof=file_transfer_detail::completion(direction,work->receipt,expected,protocol);
        require(proof&&output.finished(direction,protocol,proof->produced)&&input.completion_tail(proof->consumed,std::string_view(inbound.data).substr(inbound.first))&&valid(),"file_transfer_complete_proof");
        synchronized=true;return {LinuxFileTransferCode::completed,proof->files,proof->refused,[work]{work->confirm();}};
    }catch(...){return {leased&&!synchronized?LinuxFileTransferCode::disconnected:LinuxFileTransferCode::unavailable,{},{},{}};}
}
}
