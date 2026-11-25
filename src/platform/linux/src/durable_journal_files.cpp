#include "sfx/platform/linux/durable_journal_files.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/application/operational_log.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace sfx::platform::linux_host {
namespace {
namespace app=application;
using Kind=app::DurableJournalKind;
void require(bool value,const char* why) { if(!value)throw std::runtime_error(why); }
std::size_t slot(Kind kind) {
    require(kind==Kind::callers||kind==Kind::attention||kind==Kind::runtime,"journal_kind_invalid");
    return kind==Kind::callers?0U:kind==Kind::attention?1U:2U;
}
const char* leaf(Kind kind) { return kind==Kind::runtime?"sfxruntime.log":kind==Kind::callers?"callers.log":"heysysop.log"; }
std::string base(Kind kind) { return kind==Kind::runtime?"sfxruntime":kind==Kind::callers?"callers":"heysysop"; }
std::string header(Kind kind) { return kind==Kind::runtime?"":kind==Kind::callers?"; SFX CALLERS.LOG v1\r\n":"; SFX HEYSYSOP.LOG v1\r\n"; }
app::JournalFileState state(int fd) {
    require(private_regular_file(fd,app::kDurableJournalMaximumFileBytes),"journal_file_security_or_size_invalid");
    struct stat info{};require(fstat(fd,&info)==0,"journal_stat_failed");
    return {std::to_string(info.st_dev)+':'+std::to_string(info.st_ino),static_cast<std::uint64_t>(info.st_size)};
}
std::string read_at(int fd,std::uint64_t offset,std::size_t bytes) {
    require(bytes<=app::kCallerJournalRecordBytes&&offset<=app::kDurableJournalMaximumFileBytes&&
        bytes<=app::kDurableJournalMaximumFileBytes-offset,"journal_read_bound");
    std::string result(bytes,'\0');std::size_t done{};
    while(done<bytes) {
        const auto count=pread(fd,result.data()+done,bytes-done,static_cast<off_t>(offset+done));
        if(count<0&&errno==EINTR)continue;
        require(count>0,"journal_read_failed");done+=static_cast<std::size_t>(count);
    }
    return result;
}
void write_suffix(int fd,std::string_view text) {
    // O_APPEND binds each write to EOF. Even an empty recovery retry must flush.
    while(!text.empty()) {
        const auto count=write(fd,text.data(),text.size());
        if(count<0&&errno==EINTR)continue;
        require(count>0,"journal_append_failed");text.remove_prefix(static_cast<std::size_t>(count));
    }
    require(fsync(fd)==0,"journal_flush_failed");
}
bool complete_file(int fd,Kind kind) {
    const auto info=state(fd);const auto prefix=header(kind);
    if(!info.bytes)return kind==Kind::runtime;
    if(info.bytes<prefix.size()||read_at(fd,0,prefix.size())!=prefix)return false;
    if(info.bytes==prefix.size())return true;
    const std::string tail=kind==Kind::callers?"\r\n---\r\n":"\r\n";
    if(info.bytes<tail.size()||read_at(fd,info.bytes-tail.size(),tail.size())!=tail)return false;
    if(kind!=Kind::runtime)return true;
    std::string pending;std::uint64_t sequence{};
    for(std::uint64_t offset=0;offset<info.bytes;) {
        const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(8192,info.bytes-offset));
        pending+=read_at(fd,offset,count);offset+=count;
        for(;;) {
            const auto end=pending.find('\n');if(end==pending.npos)break;
            auto line=std::string_view(pending.data(),end);
            if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
            if(!app::decode_operational_record(line,++sequence))return false;
            pending.erase(0,end+1);
        }
        if(pending.size()>app::kOperationalRecordMaximum+1)return false;
    }
    return pending.empty();
}
bool archive_name(const std::string& value,Kind kind) {
    const auto prefix=base(kind)+'-';
    if(value.size()!=prefix.size()+19+1+32+4||value.compare(0,prefix.size(),prefix))return false;
    const auto begin=prefix.size();
    for(std::size_t i=0;i<19;++i) {
        const auto c=value[begin+i];
        if(i==8?c!='t':i==18?c!='z':(c<'0'||c>'9'))return false;
    }
    if(value[begin+19]!='-'||value.substr(value.size()-4)!=".log")return false;
    for(std::size_t i=begin+20;i<begin+52;++i)
        if(!((value[i]>='0'&&value[i]<='9')||(value[i]>='a'&&value[i]<='f')))return false;
    return true;
}
std::string archive_stamp() {
    const auto now=std::chrono::system_clock::now();
    const auto seconds=std::chrono::system_clock::to_time_t(now);std::tm utc{};
    require(gmtime_r(&seconds,&utc)!=nullptr,"journal_archive_time_failed");
    const auto millis=std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()%1000;
    char stamp[32]{};
    require(std::snprintf(stamp,sizeof(stamp),"%04d%02d%02dt%02d%02d%02d%03dz",utc.tm_year+1900,
        utc.tm_mon+1,utc.tm_mday,utc.tm_hour,utc.tm_min,utc.tm_sec,static_cast<int>(millis))==19,"journal_archive_time_invalid");
    return stamp;
}
void prune(int directory,Kind kind,std::size_t retention) {
    FileDescriptor listing(openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    require(static_cast<bool>(listing),"journal_archive_directory_failed");
    const int copy=fcntl(listing.get(),F_DUPFD_CLOEXEC,0);require(copy>=0,"journal_archive_dup_failed");
    DIR* raw=fdopendir(copy);
    if(!raw){close(copy);throw std::runtime_error("journal_archive_scan_failed");}
    struct CloseDirectory { DIR* value; ~CloseDirectory(){closedir(value);} } entries{raw};
    std::vector<std::string> archives;std::size_t examined{};
    errno=0;
    while(const auto* entry=readdir(raw)) {
        const std::string name=entry->d_name;
        if(name=="."||name=="..")continue;
        require(++examined<=4096,"journal_directory_entry_bound");
        if(archive_name(name,kind)) {
            require(archives.size()<256,"journal_archive_count_bound");archives.push_back(name);
        }
        errno=0;
    }
    require(errno==0,"journal_archive_scan_failed");std::sort(archives.begin(),archives.end());
    while(archives.size()>retention) {
        const auto& name=archives.front();
        FileDescriptor file(openat(directory,name.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));
        require(complete_file(file.get(),kind),"journal_archive_incomplete_or_insecure");
        struct stat held{},named{};
        require(fstat(file.get(),&held)==0&&fstatat(directory,name.c_str(),&named,AT_SYMLINK_NOFOLLOW)==0&&
            held.st_dev==named.st_dev&&held.st_ino==named.st_ino,"journal_archive_identity_changed");
        require(unlinkat(directory,name.c_str(),0)==0&&fsync(directory)==0,"journal_retention_flush_failed");
        archives.erase(archives.begin());
    }
}
}
class LinuxDurableJournalFiles::Impl final {
public:
    explicit Impl(std::filesystem::path value):root(std::move(value)){}
    int open_file(Kind kind) {
        const auto index=slot(kind);auto directory=open_owned_directory(root/"logs");
        auto& held=files[index];
        if(!held) {
            bool created=false;
            FileDescriptor candidate(openat(directory.get(),leaf(kind),O_RDWR|O_APPEND|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));
            if(!candidate&&errno==ENOENT) {
                candidate=FileDescriptor(openat(directory.get(),leaf(kind),O_RDWR|O_APPEND|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
                created=true;
            }
            auto info=state(candidate.get());
            if(created) {
                write_suffix(candidate.get(),header(kind));require(fsync(directory.get())==0,"journal_creation_flush_failed");
            }
            info=state(candidate.get());const auto prefix=header(kind);
            require(info.bytes>=prefix.size()&&read_at(candidate.get(),0,prefix.size())==prefix,"journal_header_invalid");
            if(kind==Kind::runtime)require(complete_file(candidate.get(),kind),"runtime_log_history_invalid");
            held=std::move(candidate);
        }
        FileDescriptor current(openat(directory.get(),leaf(kind),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));
        require(state(current.get()).identity==state(held.get()).identity,"journal_identity_changed");
        if(prune_needed[index]) {prune(directory.get(),kind,retention[index]);prune_needed[index]=false;}
        return held.get();
    }
    std::filesystem::path root;
    std::array<FileDescriptor,3> files;
    std::array<std::size_t,3> retention{10,10,10};
    std::array<bool,3> prune_needed{true,true,true};
};
LinuxDurableJournalFiles::LinuxDurableJournalFiles(std::filesystem::path root,std::wstring owner):impl_(std::make_unique<Impl>(std::move(root))){
    require(geteuid()!=0&&(owner.empty()||owner==std::to_wstring(geteuid())),"journal_owner_identity_mismatch");
}
LinuxDurableJournalFiles::~LinuxDurableJournalFiles()=default;
bool LinuxDurableJournalFiles::configure(const app::LoggingConfiguration& value) noexcept {
    if(!app::valid_logging_configuration(value))return false;
    const std::array<std::size_t,3> next{value.callers.archives,value.attention.archives,value.runtime.archives};
    if(impl_->retention!=next){impl_->retention=next;impl_->prune_needed={true,true,true};}return true;
}
std::optional<app::JournalFileState> LinuxDurableJournalFiles::inspect(Kind kind) {
    try{return state(impl_->open_file(kind));}catch(...){return {};}
}
std::optional<std::string> LinuxDurableJournalFiles::read(Kind kind,std::uint64_t offset,std::size_t bytes) {
    try{const auto fd=impl_->open_file(kind);const auto info=state(fd);
        if(offset>info.bytes||bytes>info.bytes-offset)return {};
        return read_at(fd,offset,bytes);
    }catch(...){return {};}
}
bool LinuxDurableJournalFiles::append(Kind kind,const app::JournalFileState& expected,std::string_view text) {
    try{const auto fd=impl_->open_file(kind);const auto actual=state(fd);
        if(expected.identity!=actual.identity||expected.bytes!=actual.bytes||
            text.size()>(kind==Kind::callers?app::kCallerJournalRecordBytes:kind==Kind::runtime?
                app::kOperationalRecordMaximum+2:app::kAttentionJournalRecordBytes)||
            text.size()>app::kDurableJournalMaximumFileBytes-actual.bytes)return false;
        write_suffix(fd,text);return true;
    }catch(...){return false;}
}
bool LinuxDurableJournalFiles::archive_and_reopen(Kind kind) {
    try{const auto fd=impl_->open_file(kind);
        if(!complete_file(fd,kind)||fsync(fd)!=0)return false;
        auto directory=open_owned_directory(impl_->root/"logs");
        FileDescriptor current(openat(directory.get(),leaf(kind),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));
        if(state(current.get()).identity!=state(fd).identity)return false;
        const auto id=LinuxSecureIdentifierGenerator{}.generate();constexpr char hex[]="0123456789abcdef";
        std::string nonce;for(const auto byte:id.bytes){nonce+=hex[byte>>4];nonce+=hex[byte&15];}
        const auto archive=base(kind)+'-'+archive_stamp()+'-'+nonce+".log";
        if(syscall(SYS_renameat2,directory.get(),leaf(kind),directory.get(),archive.c_str(),1U)!=0)return false;
        impl_->files[slot(kind)]=FileDescriptor{};impl_->prune_needed[slot(kind)]=true;
        if(fsync(directory.get())!=0)return false;
        (void)impl_->open_file(kind);return true;
    }catch(...){return false;}
}
app::NewCallerNames load_new_caller_names(const std::filesystem::path& root) noexcept {
    try{auto directory=open_owned_directory(root/"config");struct stat info{};
        if(fstatat(directory.get(),"joker.dat",&info,AT_SYMLINK_NOFOLLOW)!=0&&errno==ENOENT)
            return app::NewCallerNames::parse("");
        const auto bytes=read_private_file(directory.get(),"joker.dat",app::NewCallerNames::maximum_bytes);
        return app::NewCallerNames::parse(std::string(bytes.begin(),bytes.end()));
    }catch(...){return app::NewCallerNames::unavailable();}
}
} // namespace sfx::platform::linux_host
