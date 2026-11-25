#include "sfx/platform/linux/child_process.hpp"
#include <cerrno>
#include <cstddef>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace sfx::platform::linux_host {
bool drop_child_capabilities() noexcept {
    if(prctl(PR_CAP_AMBIENT,PR_CAP_AMBIENT_CLEAR_ALL,0,0,0)!=0&&errno!=EINVAL)return false;
    __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3,0};
    __user_cap_data_struct data[2]{};
    return syscall(SYS_capset,&header,data)==0&&prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0;
}
bool restrict_child_to_threads() noexcept {
#if defined(__x86_64__) && !defined(__ILP32__)
    // clone3's flags live behind a pointer that classic BPF cannot inspect.
    // ENOSYS selects glibc's clone fallback, where the flags are direct args.
    // Reject alternate x32 ABI numbers and non-AMD64 syscall architectures.
    constexpr unsigned required=CLONE_THREAD|CLONE_VM|CLONE_SIGHAND;
    const sock_filter filters[]{
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_X86_64,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,nr)),
        BPF_JUMP(BPF_JMP|BPF_JSET|BPF_K,0x40000000U,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|ENOSYS),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_fork,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_vfork,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_unshare,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_setns,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_clone3,0,1),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|ENOSYS),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_clone,0,4),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(seccomp_data,args[0])),
        BPF_STMT(BPF_ALU|BPF_AND|BPF_K,required),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,required,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)
    };
    const sock_fprog program{static_cast<unsigned short>(sizeof(filters)/sizeof(filters[0])),const_cast<sock_filter*>(filters)};
    return prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program)==0;
#else
    return false;
#endif
}
} // namespace sfx::platform::linux_host
