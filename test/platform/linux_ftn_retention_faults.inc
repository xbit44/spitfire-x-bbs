// This test executable alone wraps fsync/close. Shipping libraries contain no
// fault seam. Only the explicitly armed inode is affected; SQLite file I/O and
// unrelated descriptors continue to the real system calls.
namespace {
bool expiry_without_birth{};
std::atomic<unsigned> expiry_fault_kind{},expiry_fault_skip{},expiry_fault_hits{};
dev_t expiry_fault_device{};ino_t expiry_fault_inode{};
bool expiry_fault_match(int fd,unsigned kind)noexcept{
    if(expiry_fault_kind.load(std::memory_order_acquire)!=kind)return false;
    struct stat info{};if(fstat(fd,&info)!=0||info.st_dev!=expiry_fault_device||info.st_ino!=expiry_fault_inode)return false;
    if(expiry_fault_skip.load()){--expiry_fault_skip;return false;}
    expiry_fault_kind=0;++expiry_fault_hits;return true;
}
void arm_expiry_fault(const std::filesystem::path& path,unsigned kind,unsigned skip=0){
    struct stat info{};check(lstat(path.c_str(),&info)==0,"arm exact native inode fault");
    expiry_fault_device=info.st_dev;expiry_fault_inode=info.st_ino;expiry_fault_skip=skip;expiry_fault_hits=0;
    expiry_fault_kind.store(kind,std::memory_order_release);
}
}
extern "C" int __real_statx(int,const char*,int,unsigned,struct statx*);
extern "C" int __wrap_statx(int fd,const char* path,int flags,unsigned mask,struct statx* output){
    const int result=__real_statx(fd,path,flags,mask,output);
    if(result==0&&expiry_without_birth)output->stx_mask&=~STATX_BTIME;
    return result;
}
extern "C" int __real_fsync(int);
extern "C" int __real_close(int);
extern "C" int __wrap_fsync(int fd){
    if(expiry_fault_match(fd,1)){errno=EIO;return -1;}
    return __real_fsync(fd);
}
extern "C" int __wrap_close(int fd){
    const bool fail=expiry_fault_match(fd,2);const int result=__real_close(fd);
    if(fail&&result==0){errno=EINTR;return -1;}
    return result;
}
