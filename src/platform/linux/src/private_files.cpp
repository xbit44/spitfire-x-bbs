#include "sfx/platform/linux/private_files.hpp"
#include <sodium.h>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/magic.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace sfx::platform::linux_host {
namespace {
void require(bool value,const char* why) { if(!value)throw std::runtime_error(why); }
application::StorageStatus io_error(const char* operation) {
    return {application::StorageErrorCode::io,operation};
}
bool local_filesystem(int fd) {
    struct statfs info{};
    if(fstatfs(fd,&info)!=0)return false;
    // Only known local filesystem families; unknown/FUSE/network mounts need
    // separate qualification rather than silently claiming local semantics.
    return info.f_type==EXT4_SUPER_MAGIC || info.f_type==XFS_SUPER_MAGIC ||
        info.f_type==BTRFS_SUPER_MAGIC || info.f_type==TMPFS_MAGIC ||
        info.f_type==RAMFS_MAGIC || info.f_type==OVERLAYFS_SUPER_MAGIC;
}
std::string name_string(std::string_view name) {
    require(valid_private_name(name),"private_filename_invalid");return std::string(name);
}
}
FileDescriptor::~FileDescriptor(){if(value_>=0)close(value_);}
FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept:value_(std::exchange(other.value_,-1)){}
FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    if(this!=&other){if(value_>=0)close(value_);value_=std::exchange(other.value_,-1);}return *this;
}
bool valid_private_name(std::string_view name) noexcept {
    return !name.empty()&&name.size()<=240&&name!="."&&name!=".."&&
        name.find_first_of("/\\") == name.npos && name.find('\0')==name.npos;
}
FileDescriptor open_owned_directory(const std::filesystem::path& path,bool private_access) {
    require(path.is_absolute(),"private_directory_absolute_required");
    FileDescriptor current(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    require(static_cast<bool>(current),"private_directory_open_failed");
    for(const auto& component:path.relative_path()) {
        require(component!="."&&component!=".."&&!component.empty(),"private_directory_alias_refused");
        FileDescriptor next(openat(current.get(),component.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        require(static_cast<bool>(next),"private_directory_component_unavailable");current=std::move(next);
    }
    struct stat info{};
    require(fstat(current.get(),&info)==0&&info.st_uid==geteuid()&&
        (info.st_mode&(private_access?077:022))==0,"private_directory_permissions");
    require(local_filesystem(current.get()),"private_directory_filesystem_unqualified");
    return current;
}
bool private_regular_file(int fd,std::uint64_t maximum) noexcept {
    struct stat info{};
    return fd>=0&&fstat(fd,&info)==0&&S_ISREG(info.st_mode)&&info.st_uid==geteuid()&&
        (info.st_mode&077)==0&&info.st_nlink==1&&info.st_size>=0&&
        static_cast<std::uint64_t>(info.st_size)<=maximum;
}
std::vector<std::uint8_t> read_private_file(int directory,std::string_view name,std::size_t maximum) {
    const auto file_name=name_string(name);
    // Do not block opening an unexpected FIFO before fstat can reject its type.
    // O_NONBLOCK has no effect on the ordinary regular files accepted below.
    const FileDescriptor file(openat(directory,file_name.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
    require(private_regular_file(file.get(),maximum),"private_file_unreadable_or_insecure");
    struct stat before{},after{};require(fstat(file.get(),&before)==0,"private_file_stat_failed");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(before.st_size));
    std::size_t offset{};
    while(offset<bytes.size()) {
        const auto count=read(file.get(),bytes.data()+offset,bytes.size()-offset);
        if(count<0&&errno==EINTR)continue;
        require(count>0,"private_file_read_failed");offset+=static_cast<std::size_t>(count);
    }
    require(fstat(file.get(),&after)==0&&before.st_size==after.st_size&&
        before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&
        before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec,
        "private_file_changed_during_read");
    return bytes;
}
FileDescriptor open_owned_resource_directory(int parent,std::string_view name){
    const auto component=name_string(name);
    FileDescriptor directory(openat(parent,component.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    struct stat info{};
    require(directory&&fstat(directory.get(),&info)==0&&info.st_uid==geteuid()&&(info.st_mode&022)==0,
        "resource_directory_unavailable_or_writable");
    require(local_filesystem(directory.get()),"resource_directory_filesystem_unqualified");return directory;
}
std::vector<std::uint8_t> read_owned_resource(int directory,std::string_view name,std::size_t maximum){
    const auto file_name=name_string(name);
    struct stat parent{};require(fstat(directory,&parent)==0&&S_ISDIR(parent.st_mode)&&parent.st_uid==geteuid()&&
        (parent.st_mode&022)==0,"resource_parent_unavailable_or_writable");
    const FileDescriptor file(openat(directory,file_name.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
    struct stat before{},after{};
    require(file&&fstat(file.get(),&before)==0&&S_ISREG(before.st_mode)&&before.st_uid==geteuid()&&
        (before.st_mode&022)==0&&before.st_nlink==1&&before.st_size>=0&&
        static_cast<std::uint64_t>(before.st_size)<=maximum,"resource_file_unreadable_or_writable");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(before.st_size));std::size_t offset{};
    while(offset<bytes.size()){
        const auto count=read(file.get(),bytes.data()+offset,bytes.size()-offset);
        if(count<0&&errno==EINTR)continue;
        require(count>0,"resource_file_read_failed");offset+=static_cast<std::size_t>(count);
    }
    require(fstat(file.get(),&after)==0&&before.st_size==after.st_size&&before.st_uid==after.st_uid&&
        before.st_mode==after.st_mode&&after.st_nlink==1&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&
        before.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&before.st_ctim.tv_sec==after.st_ctim.tv_sec&&
        before.st_ctim.tv_nsec==after.st_ctim.tv_nsec,"resource_file_changed_during_read");return bytes;
}
void publish_private_file(int directory,std::string_view name,const std::vector<std::uint8_t>& bytes,bool replace) {
    const auto destination=name_string(name);
    require(sodium_init()>=0,"private_file_rng_unavailable");
    std::array<unsigned char,16> random{};randombytes_buf(random.data(),random.size());
    std::array<char,33> hex{};sodium_bin2hex(hex.data(),hex.size(),random.data(),random.size());
    const std::string temporary=".sfx-pending-"+std::string(hex.data());
    const FileDescriptor file(openat(directory,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    require(static_cast<bool>(file),"private_file_create_failed");
    // Failure leaves the owned pending data for recovery, never erases another
    // file or turns a partial write into a successful save.
    std::size_t offset{};
    while(offset<bytes.size()) {
        const auto count=write(file.get(),bytes.data()+offset,bytes.size()-offset);
        if(count<0&&errno==EINTR)continue;
        require(count>0,"private_file_write_failed");offset+=static_cast<std::size_t>(count);
    }
    require(fsync(file.get())==0,"private_file_flush_failed");
    if(replace) {
        struct stat prior{};
        if(fstatat(directory,destination.c_str(),&prior,AT_SYMLINK_NOFOLLOW)==0)
            require(S_ISREG(prior.st_mode)&&prior.st_uid==geteuid()&&(prior.st_mode&077)==0&&prior.st_nlink==1,
                "private_file_replacement_target_insecure");
        else require(errno==ENOENT,"private_file_replacement_stat_failed");
        require(renameat(directory,temporary.c_str(),directory,destination.c_str())==0,"private_file_publish_failed");
    } else {
        const auto renamed=syscall(SYS_renameat2,directory,temporary.c_str(),directory,destination.c_str(),1U);
        if(renamed!=0&&errno==ENOSYS) {
            // Older kernels can still publish a regular file atomically without
            // replacing an existing name. Both names remain private; failure
            // to retire the temporary leaves recoverable, unaccepted state.
            require(linkat(directory,temporary.c_str(),directory,destination.c_str(),0)==0,
                "private_file_exists_or_publish_failed");
            require(unlinkat(directory,temporary.c_str(),0)==0,"private_file_temporary_retirement_failed");
        } else require(renamed==0,"private_file_exists_or_publish_failed");
    }
    require(fsync(directory)==0,"private_directory_flush_failed");
}
adapters::storage::StorageLocationAssessment LinuxLocalStoragePathPolicy::assess(const std::filesystem::path& path) const {
    try {auto dir=open_owned_directory(path);(void)dir;return {{},path};}
    catch(const std::exception& error){return {{application::StorageErrorCode::invalid_location,error.what()}, {}};}
}
application::StorageStatus LinuxLocalStoragePathPolicy::create_database_file_exclusive(const std::filesystem::path& path) const {
    try {
        auto directory=open_owned_directory(path.parent_path());const auto name=name_string(path.filename().string());
        const FileDescriptor file(openat(directory.get(),name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
        if(!file)return {errno==EEXIST?application::StorageErrorCode::conflict:application::StorageErrorCode::io,"database_create"};
        if(!private_regular_file(file.get(),0)||fsync(file.get())!=0||fsync(directory.get())!=0)return io_error("database_create_flush");
        return {};
    }catch(const std::exception& error){return {application::StorageErrorCode::invalid_location,error.what()};}
}
application::StorageStatus LinuxLocalStoragePathPolicy::protect_and_verify_database_file(const std::filesystem::path& path) const {
    try {
        auto directory=open_owned_directory(path.parent_path());const auto name=name_string(path.filename().string());
        // O_PATH permits fstat without opening database content. Closing a
        // regular descriptor here could release another live SQLite handle's
        // process-scoped POSIX locks, even though this code never locks it.
        const FileDescriptor file(openat(directory.get(),name.c_str(),O_PATH|O_NOFOLLOW|O_CLOEXEC));
        // Do not repair an unexpected shared object in place. Restore/preparation
        // is explicit; ordinary startup only accepts the private installation.
        if(!private_regular_file(file.get(),UINT64_MAX))return {application::StorageErrorCode::permission_denied,"database_not_private"};
        return {};
    }catch(const std::exception& error){return {application::StorageErrorCode::invalid_location,error.what()};}
}
application::Identifier128 LinuxSecureIdentifierGenerator::generate() {
    require(sodium_init()>=0,"system_rng_unavailable");application::Identifier128 identifier;
    do {randombytes_buf(identifier.bytes.data(),identifier.bytes.size());}while(!identifier.valid());return identifier;
}
bool InstallationLock::acquire(const std::filesystem::path& path,std::string_view name) {
    if(file_)return false;
    try {
        auto dir=open_owned_directory(path);const auto file_name=name_string(name);
        FileDescriptor candidate(openat(dir.get(),file_name.c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600));
        if(!private_regular_file(candidate.get(),4096)||flock(candidate.get(),LOCK_EX|LOCK_NB)!=0)return false;
        file_=std::move(candidate);return true;
    }catch(...){return false;}
}
void InstallationLock::release() noexcept {file_=FileDescriptor{};}
} // namespace sfx::platform::linux_host
