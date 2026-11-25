#include "sfx/platform/linux/file_payload_store.hpp"
#include "sfx/platform/linux/ftn_processed_expiry.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <linux/falloc.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace sfx::platform::linux_host {
namespace {
namespace app=application;
using Path=std::filesystem::path;
using Progress=std::function<bool(std::uint64_t,std::uint64_t)>;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
// The installation owner serializes cooperating payload operations. Actual
// volume free space is checked again before each write; reservations here cover
// all outstanding leases in this process, including different installations.
std::recursive_mutex operations;
std::map<dev_t,std::uint64_t> reserved;
bool same_object(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
bool same_stamp(const struct stat& a,const struct stat& b){
    return same_object(a,b)&&a.st_size==b.st_size&&a.st_mtim.tv_sec==b.st_mtim.tv_sec&&
        a.st_mtim.tv_nsec==b.st_mtim.tv_nsec&&a.st_ctim.tv_sec==b.st_ctim.tv_sec&&a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;
}
struct stat status(int fd){struct stat result{};require(fstat(fd,&result)==0,"file_stat_failed");return result;}
std::string leaf(std::string_view name){
    require(app::valid_file_name(name)&&valid_private_name(name),"file_name_invalid");return std::string(name);
}
Path checked_path(const Path& path){
    require(path.is_absolute()&&path!=path.root_path()&&path.native().size()<=4096&&
        path==path.lexically_normal(),"file_directory_absolute_unaliased_required");
    for(const auto& part:path.relative_path())require(part!="."&&part!=".."&&!part.empty(),"file_directory_alias_refused");
    return path;
}
bool within(const Path& child,const Path& parent){
    const auto& c=child.native();const auto& p=parent.native();
    return c.size()>p.size()&&c.compare(0,p.size(),p)==0&&c[p.size()]=='/';
}
bool overlap(const Path& a,const Path& b){return a==b||within(a,b)||within(b,a);}
app::FileObjectIdentity native_identity(const struct stat& info){return {std::to_string(info.st_dev),std::to_string(info.st_ino)};}
bool equal(const app::FileObjectIdentity& a,const app::FileObjectIdentity& b){return a.volume==b.volume&&a.file_id==b.file_id;}
bool valid_identity(const app::FileObjectIdentity& value){return !value.volume.empty()&&!value.file_id.empty()&&value.volume.size()<=256&&value.file_id.size()<=256;}
std::vector<std::uint8_t> read_at(int fd,std::uint64_t offset,std::size_t maximum){
    require(maximum<=65536&&offset<=static_cast<std::uint64_t>(INT64_MAX),"file_read_bound");
    std::vector<std::uint8_t> bytes(maximum);ssize_t count;
    do{count=pread(fd,bytes.data(),maximum,static_cast<off_t>(offset));}while(count<0&&errno==EINTR);
    require(count>=0,"file_read_failed");bytes.resize(static_cast<std::size_t>(count));return bytes;
}
std::array<std::uint8_t,32> digest(int fd,std::uint64_t length,const Progress& progress){
    crypto_hash_sha256_state hash{};require(crypto_hash_sha256_init(&hash)==0,"file_hash_init_failed");
    std::uint64_t offset{};
    while(offset<length){
        require(!progress||progress(offset,length),"file_hash_cancelled");
        auto bytes=read_at(fd,offset,static_cast<std::size_t>(std::min<std::uint64_t>(65536,length-offset)));
        require(!bytes.empty(),"file_hash_short_read");crypto_hash_sha256_update(&hash,bytes.data(),bytes.size());offset+=bytes.size();
        sodium_memzero(bytes.data(),bytes.size());
    }
    require(!progress||progress(offset,length),"file_hash_cancelled");
    std::array<std::uint8_t,32> result{};crypto_hash_sha256_final(&hash,result.data());return result;
}
struct DirectoryMutation {
    FileDescriptor fd;
    explicit DirectoryMutation(int parent):fd(openat(parent,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC)){
        require(fd&&flock(fd.get(),LOCK_EX|LOCK_NB)==0,"file_directory_busy");
    }
};
// POSIX has no unlink-by-open-fd. First atomically isolate the source name in a
// new unpredictable private directory; verify WHAT was captured before any
// delete or destination publication. A concurrent source-name substitution is
// restored without overwrite, or retained here for recovery. This is not a
// security boundary against a malicious process running as the same Unix user.
struct IsolatedPayload {
    int parent;std::string source,container;FileDescriptor directory;struct stat bound{},captured{};
    bool contains{};
    IsolatedPayload(int p,std::string name,const std::function<void()>& before):parent(p),source(std::move(name)){
        require(sodium_init()>=0,"file_isolation_random_unavailable");std::array<unsigned char,16> random{};
        randombytes_buf(random.data(),random.size());std::array<char,33> hex{};sodium_bin2hex(hex.data(),hex.size(),random.data(),random.size());
        container=".sfx-payload-"+std::string(hex.data());
        require(mkdirat(parent,container.c_str(),0700)==0,"file_isolation_create_failed");
        directory=FileDescriptor(openat(parent,container.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
        require(bool(directory),"file_isolation_open_failed");bound=status(directory.get());
        require(bound.st_uid==geteuid()&&(bound.st_mode&077)==0,"file_isolation_not_private");
        require(fsync(parent)==0,"file_isolation_parent_flush_failed");
        if(before)before();
        require(syscall(SYS_renameat2,parent,source.c_str(),directory.get(),"payload",1U)==0,"file_source_capture_failed");contains=true;
        require(fstatat(directory.get(),"payload",&captured,AT_SYMLINK_NOFOLLOW)==0,"file_capture_stat_failed");
        require(fsync(parent)==0&&fsync(directory.get())==0,"file_capture_flush_failed");
    }
    void verify_directory()const{
        struct stat named{};require(fstatat(parent,container.c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&same_object(bound,named),"file_isolation_directory_changed");
    }
    void verify_capture()const{
        verify_directory();struct stat named{};require(contains&&fstatat(directory.get(),"payload",&named,AT_SYMLINK_NOFOLLOW)==0&&same_stamp(captured,named),"file_isolated_payload_changed");
    }
    void remove_empty(){verify_directory();require(!contains&&unlinkat(parent,container.c_str(),AT_REMOVEDIR)==0&&fsync(parent)==0,"file_empty_isolation_cleanup_failed");}
    void restore(){
        verify_capture();
        if(syscall(SYS_renameat2,directory.get(),"payload",parent,source.c_str(),1U)==0){contains=false;
            require(fsync(directory.get())==0&&fsync(parent)==0,"file_capture_restore_flush_failed");remove_empty();}
        // Never overwrite a new source name. The captured bytes remain private.
    }
    void require_original(const struct stat& original){
        verify_capture();
        if(!same_object(captured,original)||!S_ISREG(captured.st_mode)||captured.st_nlink!=1||
            captured.st_size!=original.st_size||captured.st_mtim.tv_sec!=original.st_mtim.tv_sec||captured.st_mtim.tv_nsec!=original.st_mtim.tv_nsec){
            restore();throw std::runtime_error("file_source_substituted_preserved");
        }
    }
};
}
struct FilePayloadDirectory::Impl {
    Path path;FileDescriptor directory;struct stat bound{};
    std::shared_ptr<IFilePayloadBindings> bindings;
    bool input{},move{},private_custody{},private_import_directory{};
    std::function<std::uint64_t()> ceiling;
    std::function<void()> before_capture;
    Impl(const Path& p,std::shared_ptr<IFilePayloadBindings> b,bool i,bool m,bool custody,
        std::function<std::uint64_t()> c={}):path(checked_path(p)),directory(open_owned_directory(path,!i)),
        bound(status(directory.get())),bindings(std::move(b)),input(i),move(m),private_custody(custody),ceiling(std::move(c)){
        require(bool(bindings),"file_portable_bindings_required");
    }
    void live()const{
        const auto current=open_owned_directory(path,!input||private_import_directory);
        require(same_object(bound,status(current.get())),"file_directory_replaced");
    }
    std::uint64_t raw_available()const{
        live();struct statvfs space{};require(fstatvfs(directory.get(),&space)==0&&space.f_frsize>0,"file_free_space_unavailable");
        const auto blocks=static_cast<std::uint64_t>(space.f_bavail),unit=static_cast<std::uint64_t>(space.f_frsize);
        require(blocks<=UINT64_MAX/unit,"file_free_space_overflow");const auto bytes=blocks*unit;
        return ceiling?std::min(bytes,ceiling()):bytes;
    }
    std::uint64_t available()const{
        const auto bytes=raw_available(),used=reserved[bound.st_dev];return bytes>used?bytes-used:0;
    }
    void space(std::uint64_t bytes)const{const auto free=available();require(free>=kFilePayloadFreeReserve&&bytes<=free-kFilePayloadFreeReserve,"file_free_space_reserve");}
};
struct FilePayloadLease::Impl {
    std::shared_ptr<FilePayloadDirectory::Impl> directory;
    Path path;std::string name;FileDescriptor file;struct stat stamp{};
    app::FileObjectIdentity logical;
    std::uint64_t limit{},reservation{};bool writing{},mutation{},sealed{},removed{},transfer_active{};
    Impl(std::shared_ptr<FilePayloadDirectory::Impl> d,std::string n,bool fresh,bool mutate,std::uint64_t maximum,bool reopen=false):
        directory(std::move(d)),path(directory->path/leaf(n)),name(std::move(n)),limit(maximum),writing(fresh||reopen),mutation(mutate||fresh){
        std::lock_guard<std::recursive_mutex> guard(operations);directory->live();
        require(!directory->input||(!fresh&&!reopen&&(!mutate||directory->move)),"file_import_source_not_output");
        require(!writing||(maximum>0&&maximum<=static_cast<std::uint64_t>(INT64_MAX)),"file_create_limit_invalid");
        if(fresh)directory->space(maximum);
        DirectoryMutation namespace_lock(directory->directory.get());
        file=FileDescriptor(openat(directory->directory.get(),name.c_str(),O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK|
            (writing?O_RDWR:O_RDONLY)|(fresh?(O_CREAT|O_EXCL):0),0600));
        require(bool(file),"file_payload_open_failed");
        require(flock(file.get(),(mutation||writing?LOCK_EX:LOCK_SH)|LOCK_NB)==0,"file_payload_busy");
        stamp=status(file.get());validate();
        logical=fresh?directory->bindings->created(path,file.get()):directory->bindings->existing(path,file.get());
        require(valid_identity(logical),"file_portable_identity_unavailable");
        live();if(fresh){require(fsync(directory->directory.get())==0,"file_create_directory_flush_failed");reserve(maximum);}
    }
    ~Impl(){std::lock_guard<std::recursive_mutex> guard(operations);release();}
    void validate()const{
        const auto current=status(file.get());
        require(S_ISREG(current.st_mode)&&current.st_uid==geteuid()&&current.st_nlink==1&&current.st_size>=0&&
            (current.st_mode&(directory->input?022:077))==0,"file_payload_link_kind_or_permissions");
        struct stat named{};
        require(fstatat(directory->directory.get(),name.c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&
            same_object(current,named)&&S_ISREG(named.st_mode),"file_payload_name_replaced");
    }
    void live()const{
        require(!removed&&bool(file),"file_lease_closed");
        require(!transfer_active,"file_transfer_child_active");directory->live();validate();
        require(same_stamp(stamp,status(file.get())),"file_payload_changed");
    }
    void release(){if(reservation){reserved[stamp.st_dev]-=reservation;reservation=0;}}
    void reserve(std::uint64_t bytes){directory->space(bytes);require(reserved[stamp.st_dev]<=UINT64_MAX-bytes,"file_reservation_overflow");reserved[stamp.st_dev]+=bytes;reservation=bytes;}
    void seal(){
        live();if(writing&&!sealed){require(fsync(file.get())==0,"file_flush_failed");stamp=status(file.get());sealed=true;release();}
    }
    void resume_limit(std::uint64_t maximum){
        require(static_cast<std::uint64_t>(stamp.st_size)<=maximum,"file_resume_size_mismatch");limit=maximum;
        reserve(maximum-static_cast<std::uint64_t>(stamp.st_size));writing=true;mutation=true;
    }
};
FilePayloadDirectory::FilePayloadDirectory(std::shared_ptr<Impl> value):impl_(std::move(value)){}
FilePayloadDirectory FilePayloadDirectory::area(const Path& installation,const Path& configured,const std::vector<Path>& others,
    const std::vector<Path>& protected_paths,std::shared_ptr<IFilePayloadBindings> bindings){
    require(bool(bindings),"file_portable_bindings_required");
    const auto root=checked_path(installation),selected=checked_path(bindings->resolve_path(configured));
    require(selected!=root&&!within(root,selected),"file_area_cannot_contain_installation");
    if(within(selected,root))require(within(selected,root/"files"),"file_area_requires_files_role");
    for(const auto& p:others)require(!overlap(selected,checked_path(bindings->resolve_path(p))),"file_area_overlap_refused");
    for(const auto& p:protected_paths)require(!overlap(selected,checked_path(bindings->resolve_path(p))),"file_area_protected_overlap");
    if(within(selected,root/"files"))for(auto p=selected;p!=root;p=p.parent_path())static_cast<void>(open_owned_directory(p));
    return FilePayloadDirectory(std::make_shared<Impl>(selected,std::move(bindings),false,false,false));
}
FilePayloadDirectory FilePayloadDirectory::private_store(const Path& path,std::shared_ptr<IFilePayloadBindings> bindings,std::function<std::uint64_t()> ceiling,std::function<void()> before_capture){
    require(bool(bindings),"file_portable_bindings_required");const auto resolved=bindings->resolve_path(path);
    auto impl=std::make_shared<Impl>(resolved,std::move(bindings),false,false,true,std::move(ceiling));impl->before_capture=std::move(before_capture);
    return FilePayloadDirectory(std::move(impl));
}
FilePayloadDirectory FilePayloadDirectory::import_source(const Path& path,const std::vector<Path>& protected_paths,std::shared_ptr<IFilePayloadBindings> bindings,bool move){
    require(bool(bindings),"file_portable_bindings_required");const auto selected=checked_path(bindings->resolve_path(path));
    for(const auto& p:protected_paths)require(!overlap(selected,checked_path(bindings->resolve_path(p))),"file_import_protected_source_refused");
    return FilePayloadDirectory(std::make_shared<Impl>(selected,std::move(bindings),true,move,false));
}
const Path& FilePayloadDirectory::path()const noexcept{return impl_->path;}
Path FilePayloadDirectory::resolve_path(const Path& path)const{impl_->live();return impl_->bindings->resolve_path(path);}
void FilePayloadDirectory::verify()const{std::lock_guard<std::recursive_mutex> guard(operations);impl_->live();}
std::vector<std::string> FilePayloadDirectory::strict_names(std::size_t maximum)const{
    std::lock_guard<std::recursive_mutex> guard(operations);impl_->live();require(maximum>0&&maximum<=100000,"file_directory_listing_bound");
    const int fd=openat(impl_->directory.get(),".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);require(fd>=0,"file_directory_list_open_failed");
    DIR* raw=fdopendir(fd);if(!raw){close(fd);throw std::runtime_error("file_directory_list_failed");}
    struct Close{DIR* dir;~Close(){closedir(dir);}} close_dir{raw};std::vector<std::string> result;
    for(;;){
        errno=0;const auto* item=readdir(raw);if(!item){require(errno==0,"file_directory_list_interrupted");break;}
        const std::string name(item->d_name);if(name=="."||name=="..")continue;
        struct stat info{};require(fstatat(impl_->directory.get(),name.c_str(),&info,AT_SYMLINK_NOFOLLOW)==0,"file_directory_entry_unavailable");
        require(app::valid_file_name(name)&&valid_private_name(name)&&S_ISREG(info.st_mode)&&info.st_nlink==1,"group_delete_nested_or_link");
        require(result.size()<maximum,"file_directory_listing_limit");result.push_back(name);
    }
    impl_->live();std::sort(result.begin(),result.end());return result;
}
std::uint64_t FilePayloadDirectory::available_bytes()const{std::lock_guard<std::recursive_mutex> guard(operations);return impl_->available();}
void FilePayloadDirectory::require_space(std::uint64_t n)const{std::lock_guard<std::recursive_mutex> guard(operations);impl_->space(n);}
std::string FilePayloadDirectory::volume()const{impl_->live();return std::to_string(impl_->bound.st_dev);}
bool FilePayloadDirectory::absent(std::string_view name)const{
    impl_->live();const auto n=leaf(name);struct stat info{};if(fstatat(impl_->directory.get(),n.c_str(),&info,AT_SYMLINK_NOFOLLOW)==0)return false;
    require(errno==ENOENT,"file_absence_not_verified");return true;
}
struct FilePayloadEnumeration::Impl {
    std::shared_ptr<FilePayloadDirectory::Impl> directory;DIR* cursor{};bool finished{};
    explicit Impl(std::shared_ptr<FilePayloadDirectory::Impl> d):directory(std::move(d)){
        directory->live();const auto fd=openat(directory->directory.get(),".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        require(fd>=0,"file_directory_list_open_failed");cursor=fdopendir(fd);if(!cursor){close(fd);throw std::runtime_error("file_directory_list_failed");}
    }
    ~Impl(){if(cursor)closedir(cursor);}
};
FilePayloadEnumeration::FilePayloadEnumeration(std::shared_ptr<FilePayloadDirectory::Impl> d):impl_(std::make_unique<Impl>(std::move(d))){}
FilePayloadEnumeration::~FilePayloadEnumeration()=default;
FilePayloadEnumeration::FilePayloadEnumeration(FilePayloadEnumeration&&)noexcept=default;
FilePayloadEnumeration& FilePayloadEnumeration::operator=(FilePayloadEnumeration&&)noexcept=default;
FilePayloadEnumeration FilePayloadDirectory::enumerate()const{return FilePayloadEnumeration(impl_);}
FilePayloadNamePage FilePayloadEnumeration::next(std::size_t limit,const std::function<bool()>& keep_going){
    require(limit>0&&limit<=10000,"file_directory_page_bound");impl_->directory->live();FilePayloadNamePage result;
    for(std::size_t visited=0;!impl_->finished&&result.names.size()<limit&&visited<10000;++visited){
        require(!keep_going||keep_going(),"file_directory_listing_cancelled");errno=0;const auto* entry=readdir(impl_->cursor);
        if(!entry){require(errno==0,"file_directory_list_interrupted");impl_->finished=true;break;}
        const std::string name(entry->d_name);if(!app::valid_file_name(name)||!valid_private_name(name))continue;
        struct stat info{};if(fstatat(impl_->directory->directory.get(),name.c_str(),&info,AT_SYMLINK_NOFOLLOW)!=0){require(errno==ENOENT,"file_directory_entry_unavailable");continue;}
        if(S_ISREG(info.st_mode)&&info.st_nlink==1)result.names.push_back(name);
    }
    result.finished=impl_->finished;return result;
}
std::vector<std::string> FilePayloadDirectory::names(std::size_t maximum)const{
    require(maximum>0&&maximum<=100000,"file_directory_listing_bound");auto cursor=enumerate();std::vector<std::string> result;
    for(;;){auto page=cursor.next();require(page.names.size()<=maximum-result.size(),"file_directory_listing_limit");
        result.insert(result.end(),page.names.begin(),page.names.end());if(page.finished)break;}
    std::sort(result.begin(),result.end());return result;
}
FilePayloadLease FilePayloadDirectory::read(std::string_view name,bool mutate)const{return FilePayloadLease(std::make_unique<FilePayloadLease::Impl>(impl_,std::string(name),false,mutate,0));}
FilePayloadDirectory FilePayloadDirectory::for_in_place_import(std::shared_ptr<IFilePayloadBindings> bindings)const{
    std::lock_guard<std::recursive_mutex> guard(operations);impl_->live();require(!impl_->input&&bool(bindings),"file_in_place_import_area_required");
    auto input=std::make_shared<Impl>(impl_->path,std::move(bindings),true,false,false);
    input->private_import_directory=true;input->live();
    require(same_object(input->bound,impl_->bound),"file_import_area_changed");return FilePayloadDirectory(std::move(input));
}
FilePayloadLease FilePayloadDirectory::create(std::string_view name,std::uint64_t maximum)const{return FilePayloadLease(std::make_unique<FilePayloadLease::Impl>(impl_,std::string(name),true,true,maximum));}
FilePayloadLease FilePayloadDirectory::resume_private(std::string_view name,std::uint64_t maximum,const app::FilePayloadIdentity& prefix,const Progress& progress)const{
    std::lock_guard<std::recursive_mutex> guard(operations);require(impl_->private_custody&&prefix.bytes<=maximum,"file_resume_requires_private_receipt_capability");
    auto value=std::make_unique<FilePayloadLease::Impl>(impl_,std::string(name),false,true,maximum,true);
    require(equal(value->logical,{prefix.volume,prefix.file_id}),"file_resume_object_identity_changed");
    const auto bytes=static_cast<std::uint64_t>(value->stamp.st_size);require(bytes>=prefix.bytes&&bytes<=maximum,"file_resume_size_mismatch");
    const auto hash=digest(value->file.get(),prefix.bytes,progress);value->live();
    require(sodium_memcmp(hash.data(),prefix.sha256.data(),hash.size())==0,"file_resume_prefix_changed");
    // Reserve before trimming. On refusal the entire failed/uncommitted object
    // remains untouched. Only the exact receipt-bound suffix may be discarded.
    value->reserve(maximum-prefix.bytes);
    if(bytes!=prefix.bytes){require(ftruncate(value->file.get(),static_cast<off_t>(prefix.bytes))==0&&fsync(value->file.get())==0,"file_resume_tail_recovery_failed");value->stamp=status(value->file.get());}
    return FilePayloadLease(std::move(value));
}
FilePayloadLease FilePayloadDirectory::resume_owned_copy(std::string_view name,const app::FileObjectIdentity& expected,const FilePayloadLease& source,const Progress& progress)const{
    const auto identity=source.identity(progress);std::lock_guard<std::recursive_mutex> guard(operations);
    auto value=std::make_unique<FilePayloadLease::Impl>(impl_,std::string(name),false,true,std::max<std::uint64_t>(1,identity.bytes),true);
    require(equal(value->logical,expected)&&!equal(native_identity(value->stamp),source.native_object_identity()),"file_copy_resume_object_changed_or_alias");
    const auto actual=static_cast<std::uint64_t>(value->stamp.st_size);require(actual<=identity.bytes,"file_copy_resume_excess_bytes");
    for(std::uint64_t offset=0;offset<actual;){require(!progress||progress(offset,identity.bytes),"file_copy_resume_cancelled");
        const auto amount=static_cast<std::size_t>(std::min<std::uint64_t>(65536,actual-offset));const auto got=read_at(value->file.get(),offset,amount),wanted=source.read(offset,amount);
        require(got.size()==amount&&got==wanted,"file_copy_resume_prefix_changed");offset+=amount;}
    require(!progress||progress(actual,identity.bytes),"file_copy_resume_cancelled");value->live();value->resume_limit(std::max<std::uint64_t>(1,identity.bytes));return FilePayloadLease(std::move(value));
}
FilePayloadLease FilePayloadDirectory::resume_owned_bytes(std::string_view name,const app::FilePayloadIdentity& expected,std::string_view regenerated,const Progress& progress)const{
    require(impl_->private_custody&&regenerated.size()<=65536&&expected.bytes==regenerated.size(),"file_generated_intent_size_changed");
    std::array<std::uint8_t,32> hash{};crypto_hash_sha256(hash.data(),reinterpret_cast<const unsigned char*>(regenerated.data()),regenerated.size());
    require(sodium_memcmp(hash.data(),expected.sha256.data(),hash.size())==0,"file_generated_intent_digest_changed");
    std::lock_guard<std::recursive_mutex> guard(operations);
    auto value=std::make_unique<FilePayloadLease::Impl>(impl_,std::string(name),false,true,std::max<std::uint64_t>(1,expected.bytes),true);
    require(equal(value->logical,{expected.volume,expected.file_id}),"file_generated_object_changed");
    const auto actual=static_cast<std::uint64_t>(value->stamp.st_size);require(actual<=expected.bytes,"file_generated_resume_excess_bytes");
    require(!progress||progress(0,expected.bytes),"file_generated_resume_cancelled");auto bytes=read_at(value->file.get(),0,static_cast<std::size_t>(actual));
    struct Wipe{std::vector<std::uint8_t>& bytes;~Wipe(){sodium_memzero(bytes.data(),bytes.size());}} wipe{bytes};
    require(bytes.size()==actual&&sodium_memcmp(bytes.data(),regenerated.data(),bytes.size())==0,"file_generated_resume_prefix_changed");
    require(!progress||progress(actual,expected.bytes),"file_generated_resume_cancelled");value->live();value->resume_limit(std::max<std::uint64_t>(1,expected.bytes));return FilePayloadLease(std::move(value));
}
FilePayloadLease::FilePayloadLease(std::unique_ptr<Impl> value):impl_(std::move(value)){}
FilePayloadLease::~FilePayloadLease()=default;
FilePayloadLease::FilePayloadLease(FilePayloadLease&&)noexcept=default;
FilePayloadLease& FilePayloadLease::operator=(FilePayloadLease&&)noexcept=default;
std::string FilePayloadLease::name()const{return impl_->name;}
const Path& FilePayloadLease::path()const noexcept{return impl_->path;}
std::vector<std::uint8_t> FilePayloadLease::read(std::uint64_t offset,std::size_t maximum)const{
    impl_->live();require(offset<=static_cast<std::uint64_t>(impl_->stamp.st_size),"file_read_bound");const auto bytes=read_at(impl_->file.get(),offset,maximum);impl_->live();return bytes;
}
void FilePayloadLease::append(const std::uint8_t* bytes,std::size_t count){
    std::lock_guard<std::recursive_mutex> guard(operations);impl_->live();
    require(impl_->writing&&!impl_->sealed&&count<=65536&&(bytes||!count),"file_append_not_authorized");
    const auto old=static_cast<std::uint64_t>(impl_->stamp.st_size);require(old<=impl_->limit&&count<=impl_->limit-old&&count<=impl_->reservation,"file_write_limit");
    const auto free=impl_->directory->raw_available();require(free>=kFilePayloadFreeReserve&&count<=free-kFilePayloadFreeReserve,"file_free_space_reserve");
    std::size_t offset{};while(offset<count){const auto n=pwrite(impl_->file.get(),bytes+offset,count-offset,static_cast<off_t>(old+offset));
        if(n<0&&errno==EINTR)continue;
        require(n>0,"file_write_failed");offset+=static_cast<std::size_t>(n);}
    impl_->validate();impl_->stamp=status(impl_->file.get());require(static_cast<std::uint64_t>(impl_->stamp.st_size)==old+count,"file_write_size_changed");
    impl_->reservation-=count;reserved[impl_->stamp.st_dev]-=count;
}
void FilePayloadLease::flush(){impl_->live();require(fsync(impl_->file.get())==0,"file_flush_failed");impl_->live();}
FileDescriptor FilePayloadLease::begin_receive_transfer(){
    std::lock_guard<std::recursive_mutex> guard(operations);impl_->live();
    require(impl_->directory->private_custody&&impl_->writing&&!impl_->sealed&&impl_->stamp.st_size==0&&
        impl_->limit>0&&impl_->limit<=1073741824ULL&&impl_->reservation==impl_->limit,"file_transfer_fresh_private_stage_required");
    // Physical allocation makes this reservation visible across processes and
    // installations. Other applications can still consume free space, so the
    // helper separately enforces the64MiB reserve before every bounded write.
    impl_->transfer_active=true;
    try{
        require(fallocate(impl_->file.get(),FALLOC_FL_KEEP_SIZE,0,static_cast<off_t>(impl_->limit))==0,"file_transfer_preallocation_failed");
        impl_->directory->live();impl_->validate();
        const auto allocated=status(impl_->file.get());
        require(same_object(impl_->stamp,allocated)&&allocated.st_size==0,"file_transfer_stage_changed");
        impl_->stamp=allocated;impl_->release();
        require(impl_->directory->available()>=kFilePayloadFreeReserve,"file_transfer_reserve_after_allocation");
        require(fsync(impl_->file.get())==0,"file_transfer_preallocation_flush_failed");
        FileDescriptor child(open(("/proc/self/fd/"+std::to_string(impl_->file.get())).c_str(),O_WRONLY|O_CLOEXEC));
        require(child&&same_stamp(impl_->stamp,status(child.get())),"file_transfer_descriptor_changed");
        return child;
    }catch(...){
        // No descriptor escaped. Only this still-empty owned object's newly
        // allocated extent may be released. Uncertain objects stay preserved.
        try{
            impl_->directory->live();impl_->validate();
            const auto current=status(impl_->file.get());
            require(same_object(current,impl_->stamp)&&current.st_size==0,"file_transfer_failed_stage_changed");
            require(ftruncate(impl_->file.get(),0)==0&&
                fsync(impl_->file.get())==0,"file_transfer_failed_allocation_preserved");
            impl_->stamp=status(impl_->file.get());impl_->sealed=true;impl_->transfer_active=false;impl_->release();
        }catch(...){/* Preserve failed allocation/bytes; caller must retain recovery evidence. */}
        throw;
    }
}
void FilePayloadLease::finish_receive_transfer_after_reap(){
    std::lock_guard<std::recursive_mutex> guard(operations);
    require(impl_->transfer_active&&!impl_->removed&&impl_->writing&&!impl_->sealed,"file_transfer_not_active");
    impl_->directory->live();impl_->validate();const auto current=status(impl_->file.get());
    require(same_object(impl_->stamp,current)&&current.st_size>=0&&static_cast<std::uint64_t>(current.st_size)<=impl_->limit,"file_transfer_actual_size_limit");
    require(fsync(impl_->file.get())==0,"file_transfer_finish_flush_failed");
    const auto actual=static_cast<std::uint64_t>(current.st_size);
    const auto before=digest(impl_->file.get(),actual,{});
    require(same_stamp(current,status(impl_->file.get())),"file_transfer_finish_changed");
    // Native ext4 clamps hole punching at EOF and leaves KEEP_SIZE extents
    // beyond it allocated. Same-size truncate retires those unwritten extents;
    // never truncate to an advertised/protocol length or discard actual bytes.
    if(actual<impl_->limit)require(ftruncate(impl_->file.get(),current.st_size)==0,"file_transfer_unused_allocation_preserved");
    require(fsync(impl_->file.get())==0,"file_transfer_allocation_release_flush_failed");
    impl_->directory->live();impl_->validate();const auto final=status(impl_->file.get());
    // Same-size truncate can update mtime/ctime. Verify content
    // across that known metadata mutation instead of accepting a stale stamp.
    require(same_object(current,final)&&current.st_size==final.st_size&&
        before==digest(impl_->file.get(),actual,{})&&same_stamp(final,status(impl_->file.get())),"file_transfer_finish_changed");
    impl_->stamp=final;impl_->sealed=true;impl_->transfer_active=false;impl_->release();impl_->live();
}
FileDescriptor FilePayloadLease::send_transfer_descriptor()const{
    std::lock_guard<std::recursive_mutex> guard(operations);impl_->live();
    require(!impl_->writing||impl_->sealed,"file_transfer_unsealed_send");
    FileDescriptor child(open(("/proc/self/fd/"+std::to_string(impl_->file.get())).c_str(),O_RDONLY|O_CLOEXEC));
    require(child&&same_stamp(impl_->stamp,status(child.get())),"file_transfer_send_descriptor_changed");impl_->live();return child;
}
app::FileObjectIdentity FilePayloadLease::object_identity()const{impl_->live();return impl_->logical;}
app::FileObjectIdentity FilePayloadLease::native_object_identity()const{impl_->live();return native_identity(impl_->stamp);}
std::uint64_t FilePayloadLease::byte_count()const{impl_->live();return static_cast<std::uint64_t>(impl_->stamp.st_size);}
app::FilePayloadIdentity FilePayloadLease::identity(const Progress& progress)const{
    std::lock_guard<std::recursive_mutex> guard(operations);impl_->seal();app::FilePayloadIdentity result;
    result.bytes=static_cast<std::uint64_t>(impl_->stamp.st_size);result.volume=impl_->logical.volume;result.file_id=impl_->logical.file_id;
    require(!impl_->writing||result.bytes<=impl_->limit,"file_external_actual_size_limit");result.sha256=digest(impl_->file.get(),result.bytes,progress);impl_->live();
    const auto before=impl_->stamp;
    impl_->directory->bindings->checkpoint(impl_->path,impl_->file.get(),result);
    if(impl_->directory->input&&impl_->directory->bindings->protects_import_permissions()&&(before.st_mode&077)!=0){
        const auto now=status(impl_->file.get());require(same_object(before,now)&&before.st_size==now.st_size&&before.st_mtim.tv_sec==now.st_mtim.tv_sec&&before.st_mtim.tv_nsec==now.st_mtim.tv_nsec&&(now.st_mode&07777)==(before.st_mode&0600),"file_import_protection_changed_payload");
        impl_->stamp=now;
    }
    impl_->live();return result;
}
bool FilePayloadLease::can_rename_to(const FilePayloadDirectory& destination)const{
    std::lock_guard<std::recursive_mutex> guard(operations);impl_->live();destination.impl_->live();
    return impl_->directory->bindings==destination.impl_->bindings&&impl_->stamp.st_dev==destination.impl_->bound.st_dev&&
        impl_->directory->bindings->can_move(impl_->path,destination.path()/impl_->path.filename());
}
void FilePayloadLease::rename_to(const FilePayloadDirectory& destination,std::string_view name){
    std::lock_guard<std::recursive_mutex> guard(operations);impl_->live();destination.impl_->live();const auto n=leaf(name);
    require(impl_->mutation&&!destination.impl_->input&&!impl_->directory->input,"file_rename_not_authorized");
    require(impl_->directory->bindings==destination.impl_->bindings,"file_rename_binding_context_changed");
    require(impl_->directory->bindings->can_move(impl_->path,destination.path()/n),"file_rename_binding_boundary_requires_copy");
    require(impl_->stamp.st_dev==destination.impl_->bound.st_dev,"file_rename_cross_volume_requires_copy");
    impl_->seal();DirectoryMutation source_lock(impl_->directory->directory.get());
    std::unique_ptr<DirectoryMutation> destination_lock;if(!same_object(impl_->directory->bound,destination.impl_->bound))destination_lock=std::make_unique<DirectoryMutation>(destination.impl_->directory.get());
    impl_->live();const auto old=impl_->path;
    IsolatedPayload isolated(impl_->directory->directory.get(),impl_->name,impl_->directory->before_capture);isolated.require_original(impl_->stamp);
    if(syscall(SYS_renameat2,isolated.directory.get(),"payload",destination.impl_->directory.get(),n.c_str(),1U)!=0){
        isolated.restore();impl_->stamp=status(impl_->file.get());throw std::runtime_error("file_rename_no_overwrite_failed");
    }
    isolated.contains=false;
    require(fsync(isolated.directory.get())==0&&fsync(destination.impl_->directory.get())==0,"file_rename_durability_failed");
    isolated.remove_empty();
    impl_->directory=destination.impl_;impl_->name=n;impl_->path=destination.path()/n;impl_->stamp=status(impl_->file.get());impl_->live();
    impl_->directory->bindings->moved(old,impl_->path,impl_->file.get(),impl_->logical);
}
void FilePayloadLease::remove_exact(){
    std::lock_guard<std::recursive_mutex> guard(operations);impl_->live();require(impl_->mutation,"file_delete_not_authorized");
    DirectoryMutation namespace_lock(impl_->directory->directory.get());impl_->live();
    IsolatedPayload isolated(impl_->directory->directory.get(),impl_->name,impl_->directory->before_capture);isolated.require_original(impl_->stamp);isolated.verify_capture();
    require(unlinkat(isolated.directory.get(),"payload",0)==0,"file_delete_failed");isolated.contains=false;
    impl_->removed=true;impl_->release();require(fsync(isolated.directory.get())==0,"file_delete_durability_failed");isolated.remove_empty();
    struct stat named{};require(fstatat(impl_->directory->directory.get(),impl_->name.c_str(),&named,AT_SYMLINK_NOFOLLOW)<0&&errno==ENOENT,"file_delete_absence_unconfirmed");
    impl_->directory->bindings->removed(impl_->path,impl_->logical);impl_->file=FileDescriptor();
}
#include "ftn_processed_expiry.inc"
} // namespace sfx::platform::linux_host
