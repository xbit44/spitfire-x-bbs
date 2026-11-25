#include "linux_rlogin.hpp"
#include "sfx/application/hostname_lookup.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>

namespace sfx::apps::runtime {
std::vector<std::array<std::uint8_t,4>> resolve_linux_gateway_ipv4(const std::filesystem::path& root,
    std::string_view hostname,const std::function<bool()>& running){
    namespace host=platform::linux_host;
    try{
        if(geteuid()==0||!running||!running())return {};
        const auto normalized=application::normalize_hostname_hint(hostname);if(!normalized)return {};
        const std::string source(normalized->bytes.data(),normalized->size);
        auto directory=host::open_owned_directory(root/"bin");
        host::FileDescriptor helper(openat(directory.get(),"sfxhostlookup",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
        if(!host::private_regular_file(helper.get(),16*1024*1024))return {};
        std::array<unsigned char,4> magic{};struct stat executable{};
        if(pread(helper.get(),magic.data(),magic.size(),0)!=4||magic!=std::array<unsigned char,4>{0x7f,'E','L','F'}||fstat(helper.get(),&executable)!=0||(executable.st_mode&S_IXUSR)==0)return {};
        int descriptors[2];if(pipe2(descriptors,O_CLOEXEC|O_NONBLOCK)!=0)return {};
        host::FileDescriptor output(descriptors[0]),writer(descriptors[1]),null(open("/dev/null",O_RDWR|O_CLOEXEC));if(!null)return {};
        char program[]="sfxhostlookup",forward[]="--forward",locale[]="LC_ALL=C";
        char* arguments[]{program,forward,const_cast<char*>(source.c_str()),nullptr};char* environment[]{locale,nullptr};
        const auto child=fork();if(child<0)return {};
        if(child==0){
            if(dup2(null.get(),STDIN_FILENO)<0||dup2(writer.get(),STDOUT_FILENO)<0||dup2(null.get(),STDERR_FILENO)<0||
                fchdir(directory.get())!=0||!host::drop_child_capabilities())_exit(126);
            fexecve(helper.get(),arguments,environment);_exit(127);
        }
        writer=host::FileDescriptor{};
        struct Child{pid_t pid;bool joined{};~Child(){if(!joined){kill(pid,SIGKILL);int status{};while(waitpid(pid,&status,0)<0&&errno==EINTR){}}}}owned{child};
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        std::string text;bool eof{};int status{};
        while(running()&&std::chrono::steady_clock::now()<deadline){
            std::array<char,256> bytes{};const auto count=read(output.get(),bytes.data(),bytes.size());
            if(count>0){if(static_cast<std::size_t>(count)>256-text.size())return {};text.append(bytes.data(),static_cast<std::size_t>(count));}
            else if(count==0)eof=true;else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)return {};
            if(!owned.joined){const auto waited=waitpid(child,&status,WNOHANG);if(waited==child)owned.joined=true;else if(waited<0&&errno!=EINTR)return {};}
            if(owned.joined&&eof){
                if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)return {};
                std::vector<std::array<std::uint8_t,4>> addresses;std::size_t begin{};
                while(begin<text.size()){
                    const auto end=text.find('\n',begin);if(end==text.npos||end-begin>15||addresses.size()>=16)return {};
                    std::array<std::uint8_t,4> address{};const auto value=text.substr(begin,end-begin);
                    if(inet_pton(AF_INET,value.c_str(),address.data())!=1)return {};
                    addresses.push_back(address);begin=end+1;
                }
                return addresses;
            }
            pollfd descriptor{output.get(),POLLIN,0};(void)poll(&descriptor,1,20);
        }
    }catch(...){}
    return {};
}
}
