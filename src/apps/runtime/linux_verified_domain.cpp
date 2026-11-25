#include "linux_verified_domain.hpp"
#include "sfx/application/hostname_lookup.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/platform/linux/child_process.hpp"
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>

namespace sfx::apps::runtime {
namespace {
VerifiedDomain run_lookup(const std::filesystem::path& root,std::uint32_t ip,const std::function<bool()>& cancelled,bool verified)noexcept{
    namespace host=platform::linux_host;
    try{
        if((cancelled&&cancelled())||geteuid()==0)return {};
        auto directory=host::open_owned_directory(root/"bin");
        host::FileDescriptor helper(openat(directory.get(),"sfxhostlookup",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
        if(!host::private_regular_file(helper.get(),16*1024*1024))return {};
        std::array<unsigned char,4> magic{};struct stat executable{};
        if(pread(helper.get(),magic.data(),magic.size(),0)!=4||magic!=std::array<unsigned char,4>{0x7f,'E','L','F'}||fstat(helper.get(),&executable)!=0||(executable.st_mode&S_IXUSR)==0)return {};
        int descriptors[2];if(pipe2(descriptors,O_CLOEXEC|O_NONBLOCK)!=0)return {};
        host::FileDescriptor output(descriptors[0]),writer(descriptors[1]),null(open("/dev/null",O_RDWR|O_CLOEXEC));if(!null)return {};
        const std::string source=std::to_string(ip>>24)+'.'+std::to_string((ip>>16)&255)+'.'+std::to_string((ip>>8)&255)+'.'+std::to_string(ip&255);
        char program[]="sfxhostlookup",locale[]="LC_ALL=C",ptr[]="--ptr";char* arguments[]{program,verified?const_cast<char*>(source.c_str()):ptr,verified?nullptr:const_cast<char*>(source.c_str()),nullptr};char* environment[]{locale,nullptr};
        const auto child=fork();if(child<0)return {};
        if(child==0){
            if(dup2(null.get(),STDIN_FILENO)<0||dup2(writer.get(),STDOUT_FILENO)<0||dup2(null.get(),STDERR_FILENO)<0||
                fchdir(directory.get())!=0||!host::drop_child_capabilities())_exit(126);
            fexecve(helper.get(),arguments,environment);_exit(127);
        }
        writer=host::FileDescriptor{};
        struct Child{pid_t pid;bool joined{};~Child(){if(!joined){kill(pid,SIGKILL);int status{};while(waitpid(pid,&status,0)<0&&errno==EINTR){}}}}owned{child};
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);std::string text;bool eof{};int status{};
        while((!cancelled||!cancelled())&&std::chrono::steady_clock::now()<deadline){
            std::array<char,256> bytes{};const auto count=read(output.get(),bytes.data(),bytes.size());
            if(count>0){if(static_cast<std::size_t>(count)>application::kHostnameMaximum-text.size())return {};text.append(bytes.data(),static_cast<std::size_t>(count));}
            else if(count==0)eof=true;
            else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)return {};
            if(!owned.joined){const auto waited=waitpid(child,&status,WNOHANG);if(waited==child)owned.joined=true;else if(waited<0&&errno!=EINTR)return {};}
            if(owned.joined&&eof){if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)return {};if(text.empty())return {true,{}};
                const auto normalized=application::normalize_hostname_hint(text);if(!normalized)return {};return {true,std::string(normalized->bytes.data(),normalized->size)};}
            pollfd descriptor{output.get(),POLLIN,0};(void)poll(&descriptor,1,20);
        }
        return {};
    }catch(...){return {};}
}
}
VerifiedDomain verify_linux_peer_domain(const std::filesystem::path& root,std::uint32_t ip,const std::atomic<bool>& stopping)noexcept{return run_lookup(root,ip,[&]{return stopping.load();},true);}
std::optional<application::HostnameHint> lookup_linux_hostname(const std::filesystem::path& root,std::uint32_t ip,const std::function<bool()>& cancelled)noexcept{
    const auto result=run_lookup(root,ip,cancelled,false);if(!result.completed)return {};
    if(result.name.empty())return application::HostnameHint{};
    return application::normalize_hostname_hint(result.name);
}
}
