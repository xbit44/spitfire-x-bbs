#include "sfx/platform/linux/trusted_display_asset_reader.hpp"
#include "sfx/application/ansi_playback_settings.hpp"
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace sfx::platform::linux_host {
namespace {
bool same_file(int one,int two){struct stat a{},b{};return fstat(one,&a)==0&&fstat(two,&b)==0&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
std::string fold(std::string_view value){std::string result(value);for(auto& c:result)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');return result;}
std::optional<std::string> matching_leaf(int directory,std::string_view logical){
    const int descriptor=openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(descriptor<0)return {};
    DIR* entries=fdopendir(descriptor);if(!entries){close(descriptor);return {};}
    struct Close {DIR* value;~Close(){closedir(value);}} close_entries{entries};
    const auto requested=fold(logical);std::string selected;std::size_t count{};errno=0;
    while(const auto* entry=readdir(entries)){
        if(++count>4096)return {};
        if(fold(entry->d_name)==requested){if(!selected.empty())return {};selected=entry->d_name;}
        errno=0;
    }
    if(errno)return {};
    return selected; // Empty means not found; null means unsafe/ambiguous.
}
}
std::unique_ptr<LinuxTrustedDisplayAssetReader> LinuxTrustedDisplayAssetReader::open(std::filesystem::path root){
    try{if(geteuid()==0)return {};auto directory=open_owned_directory(root,false);
        return std::unique_ptr<LinuxTrustedDisplayAssetReader>(new LinuxTrustedDisplayAssetReader(std::move(root),std::move(directory)));
    }catch(...){return {};}
}
application::TrustedDisplayAssetReadResult LinuxTrustedDisplayAssetReader::read_asset(std::string_view logical,std::size_t maximum){
    using Code=application::TrustedDisplayAssetReadCode;
    if(invalidated_.load())return {Code::unavailable,{},{},{},{}};
    if((!application::valid_include_filename(logical)&&logical!="bg-prelog.ans"&&logical!="chat-emote.txt")||
        maximum==0||maximum>application::kMaximumTrustedDisplaySourceBytes)return {Code::invalid,{},{},{},{}};
    try{
        auto current=open_owned_directory(root_,false);
        if(!same_file(current.get(),directory_.get())){invalidated_=true;return {Code::unavailable,{},{},{},{}};}
        const auto name=matching_leaf(current.get(),logical);
        if(!name)return {Code::unavailable,{},{},{},{}};
        if(name->empty())return {Code::not_found,{},{},{},{}};
        // Reject special files after open without waiting for a FIFO writer.
        FileDescriptor file(openat(current.get(),name->c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));
        struct stat before{},after{},named{};
        if(!file||fstat(file.get(),&before)!=0||!S_ISREG(before.st_mode)||before.st_uid!=geteuid()||
            (before.st_mode&022)!=0||before.st_nlink!=1)return {Code::unavailable,{},{},{},{}};
        if(before.st_size<0||static_cast<std::uint64_t>(before.st_size)>maximum)return {Code::invalid,{},{},{},{}};
        std::string bytes(static_cast<std::size_t>(before.st_size),'\0');std::size_t done{};
        while(done<bytes.size()){
            const auto count=::read(file.get(),bytes.data()+done,bytes.size()-done);
            if(count<0&&errno==EINTR)continue;
            if(count<=0)return {Code::unavailable,{},{},{},{}};
            done+=static_cast<std::size_t>(count);
        }
        auto final_root=open_owned_directory(root_,false);
        if(!same_file(current.get(),final_root.get())||fstat(file.get(),&after)!=0||
            fstatat(current.get(),name->c_str(),&named,AT_SYMLINK_NOFOLLOW)!=0||
            named.st_dev!=before.st_dev||named.st_ino!=before.st_ino||after.st_nlink!=1||
            before.st_size!=after.st_size||before.st_mtim.tv_sec!=after.st_mtim.tv_sec||
            before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec||before.st_ctim.tv_sec!=after.st_ctim.tv_sec||
            before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec){
            invalidated_=true;return {Code::unavailable,{},{},{},{}};
        }
        std::optional<std::int64_t> modified;
        if(before.st_mtim.tv_sec>=0&&before.st_mtim.tv_sec<INT64_MAX/10000000)
            modified=static_cast<std::int64_t>(before.st_mtim.tv_sec)*10000000+before.st_mtim.tv_nsec/100;
        return {Code::available,std::string(logical),std::to_string(before.st_dev)+':'+std::to_string(before.st_ino),
            std::move(bytes),modified,application::ansi_playback_bps(root_ / *name)};
    }catch(...){return {Code::unavailable,{},{},{},{}};}
}
} // namespace sfx::platform::linux_host
