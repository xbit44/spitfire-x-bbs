#include "linux_web_ftn_source.hpp"
#include "sfx/application/ftn.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sfx::apps::runtime::web {
namespace {
namespace host=platform::linux_host;
using Code=FtnEchoListDownloadCode;
constexpr std::size_t maximum=4*1024*1024;
bool valid_source(const std::string& source,bool url){return url?application::valid_ftn_echo_list_url(source):!source.empty()&&source.size()<=4096&&source.front()=='/'&&source.find('\0')==std::string::npos;}
FtnEchoListDownload read_local(const std::string& source){
    host::FileDescriptor input(open(source.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));struct stat before{};
    if(!input||fstat(input.get(),&before)!=0||!S_ISREG(before.st_mode)||before.st_size<0)return {};
    if(static_cast<std::uint64_t>(before.st_size)>maximum)return {Code::too_large,{}};
    std::string bytes;std::array<char,16384> buffer{};
    for(;;){const auto n=read(input.get(),buffer.data(),buffer.size());if(n<0&&errno==EINTR)continue;if(n<0)return {};if(!n)break;
        if(static_cast<std::size_t>(n)>maximum-bytes.size())return {Code::too_large,{}};
        bytes.append(buffer.data(),static_cast<std::size_t>(n));}
    struct stat after{};if(fstat(input.get(),&after)!=0||before.st_size!=after.st_size||before.st_mtim.tv_sec!=after.st_mtim.tv_sec||before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec||bytes.size()!=static_cast<std::uint64_t>(after.st_size))return {};
    return {Code::ok,std::move(bytes)};
}
}
int run_ftn_echo_source_worker(std::string source,bool url){
    if(!valid_source(source,url))return static_cast<int>(Code::invalid_url);
    const std::atomic<bool> cancel{false};const auto result=url?download_ftn_echo_list(std::move(source),cancel):read_local(source);
    if(result.code!=Code::ok)return static_cast<int>(result.code);
    std::size_t sent{};while(sent<result.bytes.size()){const auto n=write(STDOUT_FILENO,result.bytes.data()+sent,result.bytes.size()-sent);if(n<0&&errno==EINTR)continue;if(n<=0)return static_cast<int>(Code::transport);sent+=static_cast<std::size_t>(n);}
    return 0;
}
FtnEchoListDownload read_ftn_echo_source(std::string source,bool url,const std::atomic<bool>& cancel){
    if(!valid_source(source,url))return {Code::invalid_url,{}};
    if(cancel.load())return {Code::cancelled,{}};
    int descriptors[2];if(pipe2(descriptors,O_CLOEXEC)!=0)return {};
    host::FileDescriptor input(descriptors[0]),output(descriptors[1]),null(open("/dev/null",O_RDWR|O_CLOEXEC));
    if(!null||fcntl(input.get(),F_SETFL,O_NONBLOCK)!=0)return {};
    struct rlimit files{};if(getrlimit(RLIMIT_NOFILE,&files)!=0||files.rlim_cur>INT_MAX)return {};
    const auto close_limit=files.rlim_cur;char program[]="sfxweb",url_mode[]="--ftn-list-url",file_mode[]="--ftn-list-file",locale[]="LANG=C";
    char* arguments[]{program,url?url_mode:file_mode,source.data(),nullptr};char* environment[]{locale,nullptr};
    const auto child=fork();if(child<0)return {};
    if(!child){
        if(dup2(output.get(),STDOUT_FILENO)<0||dup2(null.get(),STDIN_FILENO)<0||dup2(null.get(),STDERR_FILENO)<0||fcntl(0,F_SETFD,0)!=0||fcntl(1,F_SETFD,0)!=0||fcntl(2,F_SETFD,0)!=0||!host::drop_child_capabilities()||!host::restrict_child_to_threads())_exit(127);
        // No shell, inherited credentials/environment, listener or board handles.
#ifdef SYS_close_range
        if(syscall(SYS_close_range,3U,~0U,0U)!=0)
#endif
            for(rlim_t descriptor=3;descriptor<close_limit;++descriptor)close(static_cast<int>(descriptor));
        execve("/proc/self/exe",arguments,environment);_exit(127);
    }
    output=host::FileDescriptor{};
    struct Child {pid_t pid;bool reaped{};~Child(){if(!reaped){kill(pid,SIGKILL);while(waitpid(pid,nullptr,0)<0&&errno==EINTR){}}}} owned{child};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);std::string bytes;std::array<char,16384> buffer{};int status{};bool eof{};
    for(;;){
        if(cancel.load())return {Code::cancelled,{}};
        if(std::chrono::steady_clock::now()>=deadline)return {Code::timeout,{}};
        for(;;){const auto n=read(input.get(),buffer.data(),buffer.size());if(n<0&&errno==EINTR)continue;if(n<0&&errno==EAGAIN)break;if(n<0)return {};if(!n){eof=true;break;}
            if(static_cast<std::size_t>(n)>maximum-bytes.size())return {Code::too_large,{}};
            bytes.append(buffer.data(),static_cast<std::size_t>(n));}
        if(owned.reaped&&eof)return {Code::ok,std::move(bytes)};
        const auto waited=owned.reaped?0:waitpid(child,&status,WNOHANG);if(waited==child){owned.reaped=true;if(!WIFEXITED(status))return {};const auto code=WEXITSTATUS(status);if(code>static_cast<int>(Code::transport))return {};if(code)return {static_cast<Code>(code),{}};if(eof)return {Code::ok,std::move(bytes)};}
        else if(waited<0&&errno!=EINTR)return {};
        if(owned.reaped&&eof)return {Code::ok,std::move(bytes)};
        pollfd poller{input.get(),POLLIN,0};(void)poll(&poller,1,20);
    }
}
}
