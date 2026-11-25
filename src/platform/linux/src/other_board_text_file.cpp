#include "sfx/platform/linux/other_board_text_file.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace sfx::platform::linux_host {
namespace {
namespace app=application;
using Code=app::StorageErrorCode;
constexpr const char* leaf="otherbbs.txt";
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
struct Snapshot {std::string bytes;struct stat identity{};std::int64_t revision{};std::vector<app::OtherBoardValues> rows;};
Snapshot load(int directory){
    Snapshot out;
    require(fstatat(directory,leaf,&out.identity,AT_SYMLINK_NOFOLLOW)==0,"otherbbs_file_missing");
    const auto bytes=read_private_file(directory,leaf,app::kOtherBoardTextMaximum);out.bytes.assign(bytes.begin(),bytes.end());
    struct stat after{};require(fstatat(directory,leaf,&after,AT_SYMLINK_NOFOLLOW)==0&&
        out.identity.st_dev==after.st_dev&&out.identity.st_ino==after.st_ino&&out.identity.st_size==after.st_size&&
        out.identity.st_mtim.tv_sec==after.st_mtim.tv_sec&&out.identity.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&
        out.identity.st_ctim.tv_sec==after.st_ctim.tv_sec&&out.identity.st_ctim.tv_nsec==after.st_ctim.tv_nsec,"otherbbs_changed_during_read");
    const auto parsed=app::parse_other_board_text(out.bytes);require(parsed.has_value(),"otherbbs_text_invalid_check_Config_otherbbs_txt");out.rows=*parsed;
    std::array<unsigned char,crypto_hash_sha256_BYTES> hash{};std::array<char,crypto_hash_sha256_BYTES*2+1> hex{};
    require(sodium_init()>=0&&crypto_hash_sha256(hash.data(),bytes.data(),bytes.size())==0,"otherbbs_hash_failed");
    sodium_bin2hex(hex.data(),hex.size(),hash.data(),hash.size());out.revision=std::stoll(std::string(hex.data(),15),nullptr,16)+1;
    return out;
}
FileDescriptor writer_lock(int directory){
    FileDescriptor file(openat(directory,".otherbbs.lock",O_RDWR|O_CREAT|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC,0600));
    require(private_regular_file(file.get(),4096)&&flock(file.get(),LOCK_EX|LOCK_NB)==0,"otherbbs_writer_busy_or_insecure");return file;
}
}
void initialize_other_board_text(const std::filesystem::path& root){
    const auto directory=open_owned_directory(root/"config");struct stat info{};
    if(fstatat(directory.get(),leaf,&info,AT_SYMLINK_NOFOLLOW)==0){
        // Invalid text disables this catalog only. Unsafe objects never open.
        FileDescriptor file(openat(directory.get(),leaf,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC));
        require(private_regular_file(file.get(),app::kOtherBoardTextMaximum),"otherbbs_file_insecure");return;
    }
    require(errno==ENOENT,"otherbbs_inspection_failed");publish_private_file(directory.get(),leaf,{},false);
}
app::StorageResult<app::OtherBoardPage> LinuxOtherBoardTextStorage::read(std::int64_t after){
    std::lock_guard<std::mutex> lock(mutex_);
    try{
        if(after<0||after>99999)return {{Code::invalid_argument,"otherbbs_cursor_invalid"},{}};
        const auto directory=open_owned_directory(root_/"config");const auto file=load(directory.get());app::OtherBoardPage page;
        const auto begin=std::min<std::size_t>(static_cast<std::size_t>(after),file.rows.size());const auto end=std::min(file.rows.size(),begin+app::kOtherBoardPageSize);
        for(auto n=begin;n<end;++n)page.entries.push_back({static_cast<std::int64_t>(n+1),file.revision,file.rows[n]});
        page.has_more=end<file.rows.size();return {{},page};
    }catch(const std::exception& error){return {{Code::io,error.what()},{}};}
}
app::StorageResult<app::OtherBoardLogRecord> LinuxOtherBoardTextStorage::change(const app::ClassicInformationChange& change,
    const app::ClassicInformationActor& actor,std::string_view actor_name){
    std::lock_guard<std::mutex> lock(mutex_);
    try{
        if(!app::valid_classic_information_change(change)||change.action==app::ClassicInformationAction::opening_date)
            return {{Code::invalid_argument,"otherbbs_change_invalid"},{}};
        const auto directory=open_owned_directory(root_/"config");const auto writer=writer_lock(directory.get());const auto before=load(directory.get());
        auto rows=before.rows;std::string name;
        if(change.action==app::ClassicInformationAction::add_board){
            if(rows.size()>=app::kOtherBoardMaximum)return {{Code::full,"otherbbs_list_full"},{}};
            rows.push_back(change.values);name=change.values.name;
        }else{
            if(change.expected_revision!=before.revision||change.id<1||static_cast<std::size_t>(change.id)>rows.size())return {{Code::conflict,"otherbbs_selection_changed"},{}};
            const auto at=static_cast<std::size_t>(change.id-1);name=rows[at].name;
            if(change.action==app::ClassicInformationAction::remove_board)rows.erase(rows.begin()+at);
            else{rows[at]=change.values;name=change.values.name;}
        }
        const auto bytes=app::encode_other_board_text(rows);const auto current=load(directory.get());
        if(current.bytes!=before.bytes||current.identity.st_dev!=before.identity.st_dev||current.identity.st_ino!=before.identity.st_ino)
            return {{Code::conflict,"otherbbs_selection_changed"},{}};
        // SFX writers serialize on the stable lock inode. As with the Windows
        // editor-compatible adapter, recheck manual owner edits immediately
        // before atomic replacement; same-UID uncooperative editors are trusted.
        publish_private_file(directory.get(),leaf,{bytes.begin(),bytes.end()},true);
        return {{},app::OtherBoardLogRecord{change.action,name,std::string(actor_name),actor.caller,actor.now_utc,actor.event_id}};
    }catch(const std::exception& error){return {{Code::io,error.what()},{}};}
}
}
