#include "sfx/platform/linux/private_files.hpp"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace { bool fail_unlink{},fail_directory_sync{}; unsigned checks{};
void check(bool ok,const char* detail){++checks;if(!ok)throw std::runtime_error(detail);}
template<class F> bool refused(F fn){try{fn();return false;}catch(const std::exception&){return true;}}
}
// Link-time fault injection affects only this test image, never shipping code.
extern "C" long __wrap_syscall(long number,...){
    if(number!=SYS_renameat2)std::abort();
    errno=ENOSYS;return -1;
}
extern "C" int __real_unlinkat(int,const char*,int);
extern "C" int __wrap_unlinkat(int fd,const char* name,int flags){
    if(fail_unlink){errno=EIO;return -1;}return __real_unlinkat(fd,name,flags);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){
    struct stat info{};
    if(fail_directory_sync&&fstat(fd,&info)==0&&S_ISDIR(info.st_mode)){errno=EIO;return -1;}
    return __real_fsync(fd);
}
int main(int argc,char** argv){try{
    namespace fs=std::filesystem;namespace host=sfx::platform::linux_host;
    check(argc==2&&geteuid()!=0,"explicit unprivileged fixture");const fs::path root=argv[1];
    check(root.is_absolute()&&!fs::exists(root)&&fs::create_directory(root)&&chmod(root.c_str(),0700)==0,"fresh private fixture");
    auto dir=host::open_owned_directory(root);const std::vector<std::uint8_t> first{'a'},second{'b'};
    host::publish_private_file(dir.get(),"saved",first,false);
    check(host::read_private_file(dir.get(),"saved",10)==first,"fallback publishes complete single-linked file");
    check(refused([&]{host::publish_private_file(dir.get(),"saved",second,false);}),"fallback collision refuses");
    check(host::read_private_file(dir.get(),"saved",10)==first,"fallback collision preserves original");
    fail_unlink=true;check(refused([&]{host::publish_private_file(dir.get(),"unlink-failure",second,false);}),"temporary retirement failure is not success");fail_unlink=false;
    check(fs::exists(root/"unlink-failure")&&fs::hard_link_count(root/"unlink-failure")==2,"uncertain retirement preserves both names");
    check(refused([&]{(void)host::read_private_file(dir.get(),"unlink-failure",10);}),"uncertain multiply linked state is not accepted");
    fail_directory_sync=true;check(refused([&]{host::publish_private_file(dir.get(),"sync-failure",second,false);}),"directory sync failure is not success");fail_directory_sync=false;
    check(fs::exists(root/"sync-failure"),"sync uncertainty preserves published evidence");
    check(host::read_private_file(dir.get(),"saved",10)==first,"failures preserve earlier settings");
    std::cout<<"PASS "<<checks<<" ENOSYS publication fallback, collision and failure-preservation checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
