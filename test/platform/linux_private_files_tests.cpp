#include "sfx/platform/linux/private_files.hpp"
#include <fcntl.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace host=sfx::platform::linux_host;
namespace fs=std::filesystem;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F> bool refused(F fn){try{fn();return false;}catch(const std::exception&){return true;}}
}
int main(int argc,char** argv){try{
    if(argc==3&&std::string(argv[1])=="--closed-fd")return fcntl(std::stoi(argv[2]),F_GETFD)==-1?0:1;
    check(argc==2&&geteuid()!=0,"explicit fresh fixture and normal user required");
    const fs::path root=argv[1];check(root.is_absolute()&&!fs::exists(root),"fresh absolute root");
    check(fs::create_directory(root)&&chmod(root.c_str(),0700)==0,"create private fixture");
    auto directory=host::open_owned_directory(root);
    const std::vector<std::uint8_t> first{'o','l','d'},second{'n','e','w'};
    host::publish_private_file(directory.get(),"settings",first,false);
    check(host::read_private_file(directory.get(),"settings",10)==first,"exclusive durable publication");
    check(refused([&]{host::publish_private_file(directory.get(),"settings",second,false);}),"exclusive collision refused");
    check(host::read_private_file(directory.get(),"settings",10)==first,"collision preserves original");
    host::publish_private_file(directory.get(),"settings",second,true);
    check(host::read_private_file(directory.get(),"settings",10)==second,"atomic replacement");
    check(refused([&]{(void)host::read_private_file(directory.get(),"settings",2);}),"oversized file refused");
    check(refused([&]{(void)host::read_private_file(directory.get(),"../outside",10);}),"parent traversal refused");
    check(mkfifo((root/"fifo").c_str(),0600)==0,"create unexpected private FIFO");
    check(refused([&]{(void)host::read_private_file(directory.get(),"fifo",10);}),"FIFO refused without waiting for a writer");
    check(symlink("settings",(root/"alias").c_str())==0,"create adversarial symlink");
    check(refused([&]{(void)host::read_private_file(directory.get(),"alias",10);}),"file symlink refused");
    check(refused([&]{host::publish_private_file(directory.get(),"alias",first,true);}),"replacement does not follow symlink");
    check(host::read_private_file(directory.get(),"settings",10)==second,"symlink refusal preserves destination");
    check(link((root/"settings").c_str(),(root/"hardlink").c_str())==0,"create hardlink fixture");
    check(refused([&]{(void)host::read_private_file(directory.get(),"settings",10);}),"multiply linked private file refused");
    check(unlink((root/"hardlink").c_str())==0,"retire only owned added test link");
    check(chmod((root/"settings").c_str(),0640)==0,"make fixture group-readable");
    check(refused([&]{(void)host::read_private_file(directory.get(),"settings",10);}),"shared private file refused");
    check(chmod((root/"settings").c_str(),0600)==0,"restore fixture private mode");
    host::LinuxLocalStoragePathPolicy policy;
    check(static_cast<bool>(policy.assess(root).status),"private local storage accepted");
    check(static_cast<bool>(policy.create_database_file_exclusive(root/"store.db")),"exclusive database seed");
    check(!policy.create_database_file_exclusive(root/"store.db"),"existing database not truncated");
    check(static_cast<bool>(policy.protect_and_verify_database_file(root/"store.db")),"private database verified");
    {
        host::FileDescriptor database(open((root/"store.db").c_str(),O_RDWR|O_CLOEXEC));
        struct flock lock{};lock.l_type=F_RDLCK;lock.l_whence=SEEK_SET;lock.l_start=1073741826;lock.l_len=510;
        check(database&&fcntl(database.get(),F_SETLK,&lock)==0,"hold SQLite-style POSIX shared range");
        check(static_cast<bool>(policy.protect_and_verify_database_file(root/"store.db")),"metadata verification while database is locked");
        const auto contender=fork();check(contender>=0,"fork independent database lock contender");
        if(contender==0){lock.l_type=F_WRLCK;const int result=fcntl(database.get(),F_SETLK,&lock);_exit(result==-1&&(errno==EACCES||errno==EAGAIN)?0:1);}
        int result{};check(waitpid(contender,&result,0)==contender&&WIFEXITED(result)&&WEXITSTATUS(result)==0,"metadata verification preserves parent POSIX database lock");
    }
    check(chmod(root.c_str(),0770)==0&&!policy.assess(root).status,"shared directory refused");
    check(chmod(root.c_str(),0700)==0,"restore private directory");
    host::InstallationLock one,two;
    check(one.acquire(root,"runtime.lock")&&!two.acquire(root,"runtime.lock"),"two open descriptions cannot own one installation");
    one.release();check(two.acquire(root,"runtime.lock"),"owner lock transfers after release");
    const pid_t child=fork();check(child>=0,"fork isolated descriptor check");
    if(child==0){const auto fd=std::to_string(directory.get());execl(argv[0],argv[0],"--closed-fd",fd.c_str(),nullptr);_exit(2);}
    int status{};check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"private descriptor not inherited across exec");
    const int parent_privileges=prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0);
    const pid_t caps=fork();check(caps>=0,"fork isolated capability check");
    if(caps==0){
        if(!host::drop_child_capabilities())_exit(3);
        __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3,0};__user_cap_data_struct data[2]{};
        if(syscall(SYS_capget,&header,data)!=0||data[0].effective||data[1].effective||data[0].permitted||data[1].permitted||data[0].inheritable||data[1].inheritable||prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)!=1)_exit(4);
        _exit(0);
    }
    check(waitpid(caps,&status,0)==caps&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"child has empty capability sets and no-new-privileges");
    check(prctl(PR_GET_NO_NEW_PRIVS,0,0,0,0)==parent_privileges,"child restriction did not mutate parent");
    host::LinuxSecureIdentifierGenerator ids;const auto a=ids.generate(),b=ids.generate();
    check(a.valid()&&b.valid()&&a.bytes!=b.bytes,"secure identifiers valid and distinct");
    std::cout<<"PASS "<<checks<<" Linux private-file, storage, lock and unprivileged-child checks; privileged service proof separate\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
