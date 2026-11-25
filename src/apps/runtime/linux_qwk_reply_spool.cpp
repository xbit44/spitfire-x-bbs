#include "linux_qwk_reply_spool.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace sfx::apps::runtime {
namespace {
namespace app=application;
namespace host=platform::linux_host;
constexpr std::string_view magic="SFX-REP-SPOOL-1\n";
constexpr std::array<std::size_t,9> maximums{255,255,app::kMessageSubjectCells,app::kMessageBodyBytes,8,5,1024,1024,64};
std::array<const std::string*,9> fields(const app::QwkRecord& m){return {&m.from,&m.to,&m.subject,&m.body,&m.date,&m.time,&m.message_id,&m.in_reply_to,&m.when_written};}
std::array<std::string*,9> fields(app::QwkRecord& m){return {&m.from,&m.to,&m.subject,&m.body,&m.date,&m.time,&m.message_id,&m.in_reply_to,&m.when_written};}
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
bool same_object(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
bool private_directory(const struct stat& s){return S_ISDIR(s.st_mode)&&s.st_uid==geteuid()&&(s.st_mode&07777)==0700;}
bool same_file(const struct stat& a,const struct stat& b){
    return same_object(a,b)&&S_ISREG(b.st_mode)&&b.st_uid==geteuid()&&b.st_gid==a.st_gid&&
        b.st_nlink==1&&(b.st_mode&07777)==0400&&a.st_size==b.st_size&&
        a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec&&
        a.st_ctim.tv_sec==b.st_ctim.tv_sec&&a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;
}
void cancel_check(const std::function<bool()>& callback){require(!callback||!callback(),"reply_spool_cancelled");}
void write_bytes(int file,std::string_view bytes,const std::function<bool()>& callback){
    for(std::size_t at=0;at<bytes.size();){
        cancel_check(callback);const auto n=std::min<std::size_t>(4096,bytes.size()-at);
        const auto wrote=::write(file,bytes.data()+at,n);if(wrote<0&&errno==EINTR)continue;
        require(wrote>0&&static_cast<std::size_t>(wrote)<=n,"reply_spool_write");at+=static_cast<std::size_t>(wrote);
    }
}
void number(int file,std::uint64_t value,unsigned width,const std::function<bool()>& callback){
    std::array<char,8> bytes{};for(unsigned n=0;n<width;++n)bytes[n]=static_cast<char>(value>>(n*8));write_bytes(file,{bytes.data(),width},callback);
}
class SealedSource final:public app::IQwkReplySource {
public:
    // Released last, after every owned descriptor. Unconfirmed work is retained.
    std::shared_ptr<void> job_lifetime;
    host::FileDescriptor directory,file;
    std::filesystem::path job;
    struct stat directory_identity{},identity{};
    app::QwkReplySourceInfo metadata;
    std::function<bool()> cancellation;
    std::function<void()> confirmed_completion;
    std::uint64_t position{},record_position{},size{};
    bool disposable{};
    ~SealedSource()override{
        try{if(disposable&&verify_identity()&&unlinkat(directory.get(),"reply.spool",0)==0)(void)fsync(directory.get());}
        catch(...){/* Retain uncertain work rather than removing another object. */}
    }
    void confirm_completed()noexcept override{
        try{if(!disposable&&verify_identity()){if(confirmed_completion)confirmed_completion();disposable=true;}}catch(...){}
    }
    app::QwkReplySourceInfo info()const override{return metadata;}
    bool cancelled()const noexcept override{try{return cancellation&&cancellation();}catch(...){return true;}}
    app::StorageStatus verify_identity()const override{
        try{
            // Linux does not deny rename or chmod by the owning account. Check
            // the held object AND its no-follow namespace at every read boundary.
            auto named_directory=host::open_owned_directory(job);struct stat current_directory{},held_directory{},current{},named{};
            if(!file||!directory||fstat(named_directory.get(),&current_directory)!=0||fstat(directory.get(),&held_directory)!=0||
                !private_directory(current_directory)||!private_directory(held_directory)||
                !same_object(directory_identity,current_directory)||!same_object(directory_identity,held_directory)||
                fstat(file.get(),&current)!=0||!same_file(identity,current)||
                fstatat(directory.get(),"reply.spool",&named,AT_SYMLINK_NOFOLLOW)!=0||!same_file(identity,named))
                return {app::StorageErrorCode::io,"sealed reply file or directory changed"};
            return app::StorageStatus::success();
        }catch(...){return {app::StorageErrorCode::permission_denied,"sealed reply namespace unavailable"};}
    }
    std::string read(std::size_t n){
        require(!cancelled()&&verify_identity()&&position<=size&&n<=size-position,"reply_spool_read_bounds");std::string bytes(n,'\0');
        for(std::size_t at=0;at<n;){
            require(!cancelled(),"reply_spool_cancelled");const auto request=std::min<std::size_t>(4096,n-at);
            const auto actual=pread(file.get(),bytes.data()+at,request,static_cast<off_t>(position));
            if(actual<0&&errno==EINTR)continue;
            require(actual>0&&static_cast<std::size_t>(actual)<=request,"reply_spool_read");
            at+=static_cast<std::size_t>(actual);position+=static_cast<std::uint64_t>(actual);
        }
        require(!cancelled()&&verify_identity(),"reply_spool_changed_during_read");return bytes;
    }
    std::uint64_t integer(unsigned width){const auto bytes=read(width);std::uint64_t value{};for(unsigned n=0;n<width;++n)value|=static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[n]))<<(n*8);return value;}
    app::StorageStatus rewind()override{
        try{
            require(!cancelled()&&verify_identity(),"reply_spool_rewind");position=record_position=0;
            require(read(magic.size())==magic,"reply_spool_magic");const auto length=integer(1);
            require(length==metadata.board_id.size()&&read(static_cast<std::size_t>(length))==metadata.board_id,"reply_spool_board");
            require(integer(8)==metadata.records&&integer(8)==metadata.compressed_bytes&&integer(8)==metadata.expanded_bytes,"reply_spool_bounds_header");
            const auto digest=read(32);require(sodium_memcmp(digest.data(),metadata.logical_sha256.data(),32)==0,"reply_spool_digest_header");return app::StorageStatus::success();
        }catch(...){return {cancelled()?app::StorageErrorCode::shutting_down:app::StorageErrorCode::io,"sealed reply rewind refused"};}
    }
    app::StorageResult<std::optional<app::QwkReplyRecord>> next()override{
        try{
            require(!cancelled()&&verify_identity(),"reply_spool_identity");
            if(record_position==metadata.records){require(position==size,"reply_spool_exact_eof");return {app::StorageStatus::success(),std::optional<app::QwkReplyRecord>{}};}
            app::QwkReplyRecord r;auto& m=r.message;m.conference=static_cast<std::uint16_t>(integer(2));m.reference=static_cast<std::uint32_t>(integer(4));
            const auto private_flag=integer(1),command=integer(1);require(private_flag<=1&&command<=2,"reply_spool_flags");m.private_mail=private_flag==1;if(command)r.command=command==1?app::QwkScanCommand::add:app::QwkScanCommand::drop;
            const auto destinations=fields(m);
            for(std::size_t n=0;n<destinations.size();++n){const auto length=integer(4);require(length<=maximums[n],"reply_spool_bounded_field");*destinations[n]=read(static_cast<std::size_t>(length));}
            ++record_position;return {app::StorageStatus::success(),std::optional<app::QwkReplyRecord>{std::move(r)}};
        }catch(...){return {{cancelled()?app::StorageErrorCode::shutting_down:app::StorageErrorCode::io,"sealed reply record refused"},std::nullopt};}
    }
};
}
application::StorageResult<std::shared_ptr<application::IQwkReplySource>> seal_linux_qwk_reply_spool(
    const std::filesystem::path& job,const application::QwkReplyPacket& packet,std::uint64_t compressed,
    std::uint64_t expanded,std::function<bool()> cancelled,std::shared_ptr<void> job_lifetime,
    int pinned_directory,std::function<void()> confirmed_completion){
    try{
        cancel_check(cancelled);require(packet&&app::valid_qwk_board_id(packet.board_id)&&packet.board_id==app::canonical_qwk_board_id(packet.board_id)&&
            !packet.records.empty()&&packet.records.size()<=app::kQwkExpandedBytes/256&&compressed>0&&compressed<=app::kQwkCompressedBytes&&
            expanded>=128&&expanded<=app::kQwkExpandedBytes,"reply_spool_packet_bounds");
        require(job.is_absolute()&&job==job.lexically_normal()&&job.native().size()<4096&&job.native().find('\0')==std::string::npos,"reply_spool_host_path");
        auto source=std::make_shared<SealedSource>();source->job=job;source->job_lifetime=std::move(job_lifetime);source->cancellation=cancelled;
        source->confirmed_completion=std::move(confirmed_completion);source->metadata={packet.board_id,{},packet.records.size(),compressed,expanded};
        source->directory=host::open_owned_directory(job);
        require(fstat(source->directory.get(),&source->directory_identity)==0&&private_directory(source->directory_identity),"reply_spool_private_directory");
        if(pinned_directory>=0){
            host::FileDescriptor duplicate(fcntl(pinned_directory,F_DUPFD_CLOEXEC,3));struct stat pinned{};
            require(duplicate&&fstat(duplicate.get(),&pinned)==0&&private_directory(pinned)&&same_object(pinned,source->directory_identity),"reply_spool_host_directory_identity");
            source->directory=std::move(duplicate);
        }
        std::uint64_t field_bytes{};
        for(const auto& r:packet.records){
            require(!r.command||*r.command==app::QwkScanCommand::add||*r.command==app::QwkScanCommand::drop,"reply_spool_command");
            const auto values=fields(r.message);
            for(std::size_t n=0;n<values.size();++n){require(values[n]->size()<=maximums[n]&&values[n]->size()<=expanded-field_bytes,"reply_spool_field_bounds");field_bytes+=values[n]->size();}
        }
        const std::uint64_t normalized=magic.size()+1+packet.board_id.size()+24+32+packet.records.size()*44+field_bytes;
        require(normalized<=app::kQwkExpandedBytes+packet.records.size()*44+128,"reply_spool_normalized_bounds");
        require(sodium_init()>=0&&crypto_hash_sha256(source->metadata.logical_sha256.data(),reinterpret_cast<const unsigned char*>(packet.logical_identity.data()),packet.logical_identity.size())==0,"reply_spool_logical_digest");
        // Every failed/partial write stays in this owned job for diagnosis.
        host::FileDescriptor creation(openat(source->directory.get(),"reply.spool",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600));
        require(creation&&host::private_regular_file(creation.get(),0),"reply_spool_create_new");
        write_bytes(creation.get(),magic,cancelled);number(creation.get(),packet.board_id.size(),1,cancelled);write_bytes(creation.get(),packet.board_id,cancelled);
        number(creation.get(),packet.records.size(),8,cancelled);number(creation.get(),compressed,8,cancelled);number(creation.get(),expanded,8,cancelled);
        write_bytes(creation.get(),{reinterpret_cast<const char*>(source->metadata.logical_sha256.data()),32},cancelled);
        for(const auto& r:packet.records){const auto& m=r.message;number(creation.get(),m.conference,2,cancelled);number(creation.get(),m.reference,4,cancelled);
            number(creation.get(),m.private_mail?1:0,1,cancelled);number(creation.get(),r.command?(*r.command==app::QwkScanCommand::add?1:2):0,1,cancelled);
            for(const auto* v:fields(m)){number(creation.get(),v->size(),4,cancelled);write_bytes(creation.get(),*v,cancelled);}}
        struct stat before{};require(fchmod(creation.get(),0400)==0&&fsync(creation.get())==0&&fstat(creation.get(),&before)==0&&
            before.st_size>=0&&static_cast<std::uint64_t>(before.st_size)==normalized,"reply_spool_flush_exact_length");
        source->file=host::FileDescriptor(openat(source->directory.get(),"reply.spool",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
        require(source->file&&fstat(source->file.get(),&source->identity)==0&&same_file(before,source->identity),"reply_spool_readonly_reopen");
        creation=host::FileDescriptor{};source->size=normalized;
        require(fsync(source->directory.get())==0&&source->rewind(),"reply_spool_initial_readback");
        return {app::StorageStatus::success(),std::static_pointer_cast<app::IQwkReplySource>(source)};
    }catch(const std::exception& e){
        bool is_cancelled=true;try{is_cancelled=cancelled&&cancelled();}catch(...){}
        const std::string_view phase=e.what();const auto diagnostic=phase.size()<=64&&phase.substr(0,12)=="reply_spool_"?std::string(phase):std::string("protected reply spool could not be sealed");
        return {{is_cancelled?app::StorageErrorCode::shutting_down:app::StorageErrorCode::io,diagnostic},std::nullopt};
    }catch(...){return {{app::StorageErrorCode::io,"protected reply spool could not be sealed"},std::nullopt};}
}
}
