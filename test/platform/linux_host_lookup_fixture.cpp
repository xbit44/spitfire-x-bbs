#include <fcntl.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstring>
#include <string>
int main(int argc,char** argv){
    const bool ptr=argc==3&&std::strcmp(argv[1],"--ptr")==0;
    if(argc!=2&&!(argc==3&&std::strcmp(argv[1],"--forward")==0)&&!ptr)return 2;
    __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3,0};__user_cap_data_struct data[2]{};
    if(syscall(SYS_capget,&header,data)!=0||data[0].effective||data[1].effective||data[0].permitted||data[1].permitted||
        data[0].inheritable||data[1].inheritable||prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=1)return 2;
    for(int fd=3;fd<64;++fd)if(fcntl(fd,F_GETFD)>=0)return 2;
    if(argc==3&&!ptr){
        if(std::strcmp(argv[2],"slow.example")==0){sleep(10);return 0;}
        if(std::strcmp(argv[2],"failed.example")==0)return 2;
        const auto response=std::strcmp(argv[2],"oversized.example")==0?std::string(1024,'x'):
            std::strcmp(argv[2],"bad.example")==0?std::string("127.0.0.1\ninvalid\n"):std::string("127.0.0.1\n192.0.2.1\n");
        return write(STDOUT_FILENO,response.data(),response.size())==static_cast<ssize_t>(response.size())?0:2;
    }
    const auto source=argv[ptr?2:1];
    if(std::strcmp(source,"192.0.2.4")==0){sleep(10);return 0;}
    if(std::strcmp(source,"192.0.2.5")==0)return 2;
    const auto response=std::strcmp(source,"192.0.2.2")==0?std::string("bad\x1bname"):
        std::strcmp(source,"192.0.2.3")==0?std::string(1024,'a'):ptr?std::string("Display.Example."):std::string("Verified.Example.");
    return write(STDOUT_FILENO,response.data(),response.size())==static_cast<ssize_t>(response.size())?0:2;
}
