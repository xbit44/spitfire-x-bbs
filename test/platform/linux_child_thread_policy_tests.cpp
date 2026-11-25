#include "sfx/platform/linux/child_process.hpp"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <iostream>
#include <linux/sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
namespace host=sfx::platform::linux_host;
int main(){
    if(geteuid()==0)return 2;
    const auto original=prctl(PR_GET_SECCOMP,0,0,0,0);const auto child=fork();if(child<0)return 2;
    if(child==0){
        if(!host::drop_child_capabilities()||!host::restrict_child_to_threads())_exit(10);
        std::atomic<bool> thread_ran{false};try{std::thread worker([&]{thread_ran=true;});worker.join();}catch(...){_exit(11);}
        if(!thread_ran.load())_exit(12);
        errno=0;if(syscall(SYS_fork)!=-1||errno!=EPERM)_exit(13);
        errno=0;if(syscall(SYS_vfork)!=-1||errno!=EPERM)_exit(14);
        errno=0;if(syscall(SYS_clone,SIGCHLD,nullptr,nullptr,nullptr,0)!=-1||errno!=EPERM)_exit(15);
        clone_args request{};request.exit_signal=SIGCHLD;
        errno=0;if(syscall(SYS_clone3,&request,sizeof(request))!=-1||errno!=ENOSYS)_exit(16);
        errno=0;if(syscall(SYS_unshare,CLONE_NEWUSER)!=-1||errno!=EPERM)_exit(17);
        errno=0;if(syscall(SYS_setns,-1,0)!=-1||errno!=EPERM)_exit(18);
        if(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=1)_exit(19);
        _exit(0);
    }
    int status{};if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=0){std::cerr<<"FAIL thread policy child status "<<status<<'\n';return 1;}
    if(prctl(PR_GET_SECCOMP,0,0,0,0)!=original)return 1;
    const auto normal=fork();if(normal<0)return 1;if(normal==0)_exit(0);
    if(waitpid(normal,&status,0)!=normal||status!=0)return 1;
    std::cout<<"PASS 11 native thread/process policy checks; Runtime parent unchanged\n";return 0;
}
