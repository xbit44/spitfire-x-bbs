#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "sfx/platform/linux/installation_layout.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <fcntl.h>
#include <linux/stat.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace sfx::platform::linux_host {
namespace {
namespace fs=std::filesystem;namespace app=application;
constexpr std::size_t maximum_bytes=128U*1024U*1024U,maximum_records=1000000;
constexpr char leaf[]="file-identities-linux.v1";
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
std::string hex(const unsigned char* bytes,std::size_t size){
    constexpr char digits[]="0123456789ABCDEF";std::string out;out.reserve(size*2);
    for(std::size_t i=0;i<size;++i){out+=digits[bytes[i]>>4];out+=digits[bytes[i]&15];}return out;
}
std::string hex(std::string_view text){return hex(reinterpret_cast<const unsigned char*>(text.data()),text.size());}
std::string unhex(std::string_view text){
    require(text.size()%2==0&&text.size()<=8192,"linux_file_map_hex_invalid");
    const auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return static_cast<unsigned>(c-'0');if(c>='A'&&c<='F')return static_cast<unsigned>(c-'A'+10);throw std::runtime_error("linux_file_map_hex_invalid");};
    std::string out;out.reserve(text.size()/2);for(std::size_t i=0;i<text.size();i+=2)out+=static_cast<char>((digit(text[i])<<4)|digit(text[i+1]));return out;
}
std::uint64_t number(std::string_view text){std::uint64_t out{};const auto parsed=std::from_chars(text.data(),text.data()+text.size(),out);require(!text.empty()&&parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()&&std::to_string(out)==text,"linux_file_map_number_invalid");return out;}
std::uint64_t decimal_token(const unsigned char* bytes){
    std::uint64_t out{};for(unsigned i=0;i<8;++i)out=(out<<8)|bytes[i];return out;
}
std::string ftn_volume(const PortableBoardKey& board){
    // The existing FTN schema stores 64-bit decimal object identifiers. Keep
    // these logical and board-bound, never a permanent device/inode lock.
    return std::to_string(decimal_token(board.board.data()));
}
bool valid_logical_identity(const app::FileObjectIdentity& value,const PortableBoardKey& board){
    if(value.volume=="sfx-linux-"+hex(board.board.data(),board.board.size()))
        return value.file_id.size()==32&&unhex(value.file_id).size()==16;
    return value.volume==ftn_volume(board)&&value.file_id.size()<=20&&number(value.file_id)!=0;
}
struct Native {
    // 0=root inode only; 1=birth stamp; 2=strict last mutation stamp.
    std::uint64_t device{},inode{},kind{},seconds{},nanos{};
    bool operator==(const Native& b)const{return device==b.device&&inode==b.inode&&kind==b.kind&&seconds==b.seconds&&nanos==b.nanos;}
};
Native native(int fd,bool root=false){
    struct stat value{};struct statx extended{};
    require(fstat(fd,&value)==0&&value.st_uid==geteuid(),"linux_file_map_owner_invalid");
    const bool birth=statx(fd,"",AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW,STATX_BTIME,&extended)==0&&
        (extended.stx_mask&STATX_BTIME)&&extended.stx_btime.tv_sec>=0;
    if(birth)return {static_cast<std::uint64_t>(value.st_dev),static_cast<std::uint64_t>(value.st_ino),1,static_cast<std::uint64_t>(extended.stx_btime.tv_sec),extended.stx_btime.tv_nsec};
    require(root||value.st_ctim.tv_sec>=0,"linux_file_change_stamp_unavailable");
    return {static_cast<std::uint64_t>(value.st_dev),static_cast<std::uint64_t>(value.st_ino),root?0U:2U,
        root?0:static_cast<std::uint64_t>(value.st_ctim.tv_sec),root?0:static_cast<std::uint64_t>(value.st_ctim.tv_nsec)};
}
bool owned(std::string_view name){
    if(name.empty()||name.size()>4096||name.find_first_of("\\\r\n\0",0,4)!=name.npos)return false;
    const fs::path path=fs::u8path(name);if(path.is_absolute()||path!=path.lexically_normal()||path.generic_u8string()!=name)return false;
    for(const auto& part:path)if(part=="."||part==".."||part.empty())return false;
    return name.rfind("files/",0)==0||name.rfind("ftn/",0)==0||name.rfind("doors/",0)==0||name.rfind("games/",0)==0||name.rfind("data/fileintake/",0)==0||name.rfind("data/fileremoved/",0)==0;
}
struct Record {std::string path;app::FileObjectIdentity logical;Native physical;std::uint64_t bytes{};std::array<std::uint8_t,32> digest{};bool active{true},deferred{};};
bool safe_path(const fs::path& path){
    const auto text=path.generic_u8string();if(text.empty()||text.size()>4096||text.find_first_of("\\\r\n\0",0,4)!=text.npos||path!=path.lexically_normal())return false;
    for(const auto& part:path.relative_path())if(part=="."||part==".."||part.empty())return false;
    return true;
}
bool owned_role(const fs::path& relative){
    if(!safe_path(relative)||relative.is_absolute())return false;
    const auto first=*relative.begin();for(const auto* role:{"config","data","display","files","doors","games","backup","logs","work","botgate","node","ftn"})if(first==role)return true;
    return false;
}
struct State {std::string root;Native physical;std::vector<std::string> roots;std::vector<Record> records;bool restored_core{};};
void emit_native(std::ostream& out,const Native& value){out<<value.device<<' '<<value.inode<<' '<<value.kind<<' '<<value.seconds<<' '<<value.nanos;}
Native parse_native(std::istream& in){std::array<std::string,5> fields;for(auto& field:fields)require(static_cast<bool>(in>>field),"linux_file_map_native_invalid");Native value{number(fields[0]),number(fields[1]),number(fields[2]),number(fields[3]),number(fields[4])};require(value.inode&&value.kind<=2&&value.nanos<1000000000,"linux_file_map_native_invalid");return value;}
struct Mac {
    std::array<unsigned char,32> key{};
    explicit Mac(const PortableBoardKey& board){require(crypto_kdf_derive_from_key(key.data(),key.size(),2,"SFXFILES",board.key.bytes.data())==0,"linux_file_map_mac_key_failed");}
    ~Mac(){sodium_memzero(key.data(),key.size());}
};
std::string encode(const State& state,const PortableBoardKey& board){
    require(state.records.size()<=maximum_records,"linux_file_map_count_limit");
    require(!state.roots.empty()&&state.roots.size()<=64,"linux_file_map_root_history_limit");
    std::ostringstream out;out<<(state.restored_core?"SFX-LINUX-FILE-IDENTITIES-3\n":"SFX-LINUX-FILE-IDENTITIES-2\n")<<hex(board.board.data(),board.board.size())<<'\n'<<hex(state.root)<<'\n';emit_native(out,state.physical);out<<'\n';
    if(state.restored_core)out<<"restored-core-prepare-required\n";
    out<<state.roots.size()<<'\n';
    for(const auto& root:state.roots)out<<hex(root)<<'\n';
    out<<state.records.size()<<'\n';
    for(const auto& record:state.records){
        out<<hex(record.path)<<' '<<hex(record.logical.volume)<<' '<<hex(record.logical.file_id)<<' ';emit_native(out,record.physical);
        out<<' '<<record.bytes<<' '<<(record.deferred?2:record.active?1:0)<<' '<<hex(record.digest.data(),record.digest.size())<<'\n';
        require(static_cast<std::uint64_t>(out.tellp())<=maximum_bytes-69,"linux_file_map_size_limit");
    }
    auto body=out.str();Mac key(board);std::array<unsigned char,32> mac{};crypto_auth_hmacsha256(mac.data(),reinterpret_cast<const unsigned char*>(body.data()),body.size(),key.key.data());
    body+="MAC:"+hex(mac.data(),mac.size())+'\n';return body;
}
State decode(const std::vector<std::uint8_t>& bytes,const PortableBoardKey& board){
    require(bytes.size()>69&&bytes.size()<=maximum_bytes,"linux_file_map_size_invalid");
    const std::string_view all(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto body=all.substr(0,all.size()-69),tail=all.substr(all.size()-69);
    require(tail.substr(0,4)=="MAC:"&&tail.back()=='\n',"linux_file_map_mac_invalid");const auto supplied=unhex(tail.substr(4,64));Mac key(board);
    require(crypto_auth_hmacsha256_verify(reinterpret_cast<const unsigned char*>(supplied.data()),reinterpret_cast<const unsigned char*>(body.data()),body.size(),key.key.data())==0,"linux_file_map_authentication_failed");
    std::istringstream in{std::string(body)};std::string line;const auto next=[&]{require(static_cast<bool>(std::getline(in,line)),"linux_file_map_truncated");return line;};
    const auto format=next();require((format=="SFX-LINUX-FILE-IDENTITIES-1"||format=="SFX-LINUX-FILE-IDENTITIES-2"||format=="SFX-LINUX-FILE-IDENTITIES-3")&&next()==hex(board.board.data(),board.board.size()),"linux_file_map_board_invalid");
    State state;state.root=unhex(next());require(fs::u8path(state.root).is_absolute()&&fs::u8path(state.root)==fs::u8path(state.root).lexically_normal(),"linux_file_map_root_invalid");
    {std::istringstream row(next());state.physical=parse_native(row);require(!(row>>line),"linux_file_map_native_trailing_data");}
    if(format=="SFX-LINUX-FILE-IDENTITIES-3"){require(next()=="restored-core-prepare-required","linux_file_map_restore_state_invalid");state.restored_core=true;}
    if(format=="SFX-LINUX-FILE-IDENTITIES-1")state.roots.push_back(state.root);
    else{
        const auto roots=number(next());require(roots&&roots<=64,"linux_file_map_root_history_limit");std::set<std::string> unique;
        for(std::uint64_t i=0;i<roots;++i){const auto root=unhex(next());const auto path=fs::u8path(root);require(path.is_absolute()&&path!=path.root_path()&&safe_path(path)&&unique.insert(root).second,"linux_file_map_root_history_invalid");state.roots.push_back(root);}
        require(unique.count(state.root)==1,"linux_file_map_current_root_missing");
    }
    const auto count=number(next());require(count<=maximum_records,"linux_file_map_count_limit");std::set<std::string> paths,logical;
    for(std::uint64_t i=0;i<count;++i){
        std::istringstream row(next());std::string path,volume,id,size,active,digest,extra;Record record;
        require(static_cast<bool>(row>>path>>volume>>id),"linux_file_map_record_invalid");record.path=unhex(path);record.logical={unhex(volume),unhex(id)};record.physical=parse_native(row);
        require(static_cast<bool>(row>>size>>active>>digest)&&!(row>>extra),"linux_file_map_record_invalid");record.bytes=number(size);require(active=="0"||active=="1"||(active=="2"&&format!="SFX-LINUX-FILE-IDENTITIES-1"),"linux_file_map_activity_invalid");record.active=active=="1";record.deferred=active=="2";
        const auto raw=unhex(digest);require(raw.size()==record.digest.size()&&owned(record.path),"linux_file_map_record_invalid");std::copy(raw.begin(),raw.end(),record.digest.begin());
        require(valid_logical_identity(record.logical,board),"linux_file_map_logical_id_invalid");
        require(logical.insert(record.logical.file_id).second&&(!record.active||paths.insert(record.path).second),"linux_file_map_duplicate_record");state.records.push_back(std::move(record));
    }
    require(!std::getline(in,line),"linux_file_map_trailing_data");return state;
}
bool import_permissions(const struct stat& value){return S_ISREG(value.st_mode)&&value.st_nlink==1&&value.st_uid==geteuid()&&value.st_size>=0&&(value.st_mode&0400)!=0&&(value.st_mode&07133)==0;}
app::FilePayloadIdentity measure(int fd,const app::FileObjectIdentity& logical,const std::function<bool()>& cancelled,struct stat* observed=nullptr,bool import=false){
    struct stat before{},after{};require(fstat(fd,&before)==0&&S_ISREG(before.st_mode)&&before.st_nlink==1&&before.st_uid==geteuid()&&(import?import_permissions(before):(before.st_mode&077)==0)&&before.st_size>=0,"linux_file_map_payload_invalid");
    app::FilePayloadIdentity result;result.volume=logical.volume;result.file_id=logical.file_id;result.bytes=static_cast<std::uint64_t>(before.st_size);
    crypto_hash_sha256_state hash{};require(crypto_hash_sha256_init(&hash)==0,"linux_file_map_hash_failed");std::array<unsigned char,65536> buffer{};std::uint64_t offset{};
    while(offset<result.bytes){require(!cancelled||!cancelled(),"linux_file_map_cancelled");const auto want=static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),result.bytes-offset));
        const auto count=pread(fd,buffer.data(),want,static_cast<off_t>(offset));if(count<0&&errno==EINTR)continue;require(count>0,"linux_file_map_read_failed");crypto_hash_sha256_update(&hash,buffer.data(),static_cast<std::size_t>(count));offset+=static_cast<std::uint64_t>(count);}
    crypto_hash_sha256_final(&hash,result.sha256.data());require(fstat(fd,&after)==0&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size&&
        before.st_ctim.tv_sec==after.st_ctim.tv_sec&&before.st_ctim.tv_nsec==after.st_ctim.tv_nsec&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec,"linux_file_map_payload_changed");if(observed)*observed=after;return result;
}
}
struct LinuxFilePayloadBindings::Impl {
    fs::path root;FileDescriptor directory;InstallationLock ownership;PortableBoardKey key;State state;std::mutex mutex;bool healthy{true};
    explicit Impl(const fs::path& path):root(path),directory(open_owned_directory(path/"config")){
        layout::require_current(path);
        require(path.is_absolute()&&path==path.lexically_normal(),"linux_file_map_root_invalid");
        require(ownership.acquire(path/"config",".file-identities-linux.lock"),"linux_file_map_already_owned");
        auto loaded=load_portable_board_key(path);require(static_cast<bool>(loaded),"linux_file_map_key_unavailable");key=std::move(*loaded.value);
    }
    std::string relative(const fs::path& path)const{
        require(path.is_absolute()&&path==path.lexically_normal(),"linux_file_map_path_invalid");const auto text=path.lexically_relative(root).generic_u8string();require(owned(text),"linux_file_map_owned_path_required");return text;
    }
    bool managed(const fs::path& path)const{return path.is_absolute()&&path==path.lexically_normal()&&owned(path.lexically_relative(root).generic_u8string());}
    fs::path resolve(const fs::path& input,bool preparing=false)const{
        if(!preparing)live();
        require(safe_path(input),"linux_file_map_configured_path_invalid");
        if(input.is_relative())return root/input;
        // Prefer the longest matching root when copied folders are nested.
        const std::string* match=nullptr;
        for(const auto& alias:state.roots){const auto source=fs::u8path(alias);const auto rel=input.lexically_relative(source);
            if((input==source||(!rel.is_absolute()&&safe_path(rel)))&&(!match||alias.size()>match->size()))match=&alias;}
        if(!match)return input;
        const auto source=fs::u8path(*match);if(input==source)return root;
        const auto relative=input.lexically_relative(source);return owned_role(relative)?root/relative:input;
    }
    Native root_identity()const{const auto held=open_owned_directory(root);return native(held.get(),true);}
    void live()const{require(healthy,"linux_file_map_publication_failed_reopen_required");require(!state.restored_core&&state.root==root.u8string()&&state.physical==root_identity(),"linux_file_map_prepare_required");
        const auto current=open_owned_directory(root/"config");require(native(current.get(),true)==native(directory.get(),true),"linux_file_map_config_replaced");}
    void save(){try{live();const auto text=encode(state,key);struct stat existing{};const bool present=fstatat(directory.get(),leaf,&existing,AT_SYMLINK_NOFOLLOW)==0;require(present||errno==ENOENT,"linux_file_map_presence_failed");publish_private_file(directory.get(),leaf,{text.begin(),text.end()},present);}catch(...){healthy=false;throw;}}
    Record& find(const fs::path& path,int fd,bool held_mutation=false){live();const auto name=relative(path);const auto physical=native(fd);for(auto& record:state.records)if(record.active&&record.path==name){
        require(record.physical==physical||(held_mutation&&record.physical.kind==2&&physical.kind==2&&record.physical.device==physical.device&&record.physical.inode==physical.inode),"linux_file_map_payload_replaced");return record;}throw std::runtime_error("linux_file_map_unknown_payload_preserved");}
    app::FileObjectIdentity import_candidate(const fs::path& path,int fd,const std::optional<app::FilePayloadIdentity>& confirmed={}){
        live();struct stat info{};require(fstat(fd,&info)==0&&import_permissions(info),"linux_file_import_permissions_invalid");
        const auto physical=native(fd);if(!managed(path))return {std::to_string(physical.device),std::to_string(physical.inode)};
        const auto name=relative(path);
        for(const auto& record:state.records)if(record.path==name&&(record.active||record.deferred)){
            require(record.active&&!record.deferred&&record.physical==physical,"linux_file_import_known_payload_changed");
            require((info.st_mode&077)==0||(confirmed&&record.logical.volume==confirmed->volume&&record.logical.file_id==confirmed->file_id&&record.bytes==confirmed->bytes&&record.digest==confirmed->sha256),"linux_file_import_known_permissions_changed");return record.logical;
        }
        std::ostringstream material;material<<"SFX-LINUX-IMPORT-1\n"<<name<<'\n';emit_native(material,physical);const auto bytes=material.str();std::array<unsigned char,16> digest{};
        require(crypto_generichash(digest.data(),digest.size(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size(),key.key.bytes.data(),key.key.bytes.size())==0,"linux_file_import_identity_failed");
        return {"sfx-linux-"+hex(key.board.data(),key.board.size()),hex(digest.data(),digest.size())};
    }
    FileDescriptor pin_path(const fs::path& path,bool import=false){
        const auto parent=open_owned_directory(path.parent_path());
        FileDescriptor fd(openat(parent.get(),path.filename().c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK));
        struct stat info{};require(fd&&(import?(fstat(fd.get(),&info)==0&&import_permissions(info)):private_regular_file(fd.get(),UINT64_MAX))&&flock(fd.get(),LOCK_SH|LOCK_NB)==0,"linux_file_map_payload_unavailable");return fd;
    }
    FileDescriptor pin(const Record& record){return pin_path(root/fs::u8path(record.path));}
};
LinuxFilePayloadBindings::LinuxFilePayloadBindings(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
LinuxFilePayloadBindings::~LinuxFilePayloadBindings()=default;
bool LinuxFilePayloadBindings::can_move(const fs::path& from,const fs::path& to)const{return impl_->managed(from)==impl_->managed(to);}
std::shared_ptr<LinuxFilePayloadBindings> LinuxFilePayloadBindings::initialize_fresh(const fs::path& root){
    auto impl=std::make_unique<Impl>(root);struct stat existing{};require(fstatat(impl->directory.get(),leaf,&existing,AT_SYMLINK_NOFOLLOW)<0&&errno==ENOENT,"linux_file_map_already_exists");
    impl->state.root=root.u8string();impl->state.roots.push_back(impl->state.root);impl->state.physical=impl->root_identity();impl->save();return std::shared_ptr<LinuxFilePayloadBindings>(new LinuxFilePayloadBindings(std::move(impl)));
}
std::shared_ptr<LinuxFilePayloadBindings> LinuxFilePayloadBindings::open(const fs::path& root){
    auto impl=std::make_unique<Impl>(root);impl->state=decode(read_private_file(impl->directory.get(),leaf,maximum_bytes),impl->key);impl->live();return std::shared_ptr<LinuxFilePayloadBindings>(new LinuxFilePayloadBindings(std::move(impl)));
}
bool LinuxFilePayloadBindings::mark_restored_core(const fs::path& root){
    auto impl=std::make_unique<Impl>(root);impl->state=decode(read_private_file(impl->directory.get(),leaf,maximum_bytes),impl->key);
    const bool affected=std::any_of(impl->state.records.begin(),impl->state.records.end(),[](const auto& record){return record.active&&(record.path.rfind("ftn/",0)==0||record.path.rfind("doors/",0)==0);});
    if(!affected&&!impl->state.restored_core)return false;
    impl->state.restored_core=true;const auto text=encode(impl->state,impl->key);
    publish_private_file(impl->directory.get(),leaf,{text.begin(),text.end()},true);return true;
}
std::shared_ptr<LinuxFilePayloadBindings> LinuxFilePayloadBindings::prepare(const fs::path& root,const std::function<bool()>& cancelled,const FileBindingPrepareProofs& proofs){
    auto impl=std::make_unique<Impl>(root);impl->state=decode(read_private_file(impl->directory.get(),leaf,maximum_bytes),impl->key);
    struct Captured {std::size_t record;fs::path path;FileDescriptor file;struct stat stamp;};
    std::vector<Captured> held;std::vector<std::pair<std::size_t,fs::path>> absent_paths;
    std::vector<std::pair<std::size_t,std::size_t>> final_dependencies;
    require(proofs.moves.size()<=maximum_records&&proofs.removals.size()<=maximum_records&&proofs.deletions.size()<=maximum_records&&proofs.imports.size()<=maximum_records,"linux_file_prepare_proof_limit");
    if(std::find(impl->state.roots.begin(),impl->state.roots.end(),root.u8string())==impl->state.roots.end())impl->state.roots.push_back(root.u8string());
    const auto absent=[&](const fs::path& path){const auto parent=open_owned_directory(path.parent_path());struct stat info{};if(fstatat(parent.get(),path.filename().c_str(),&info,AT_SYMLINK_NOFOLLOW)==0)return false;require(errno==ENOENT,"linux_file_prepare_absence_unavailable");return true;};
    const auto matches=[](const Record& record,const app::FilePayloadIdentity& value){return record.logical.volume==value.volume&&record.logical.file_id==value.file_id&&record.bytes==value.bytes&&record.digest==value.sha256;};
    // Only protected, pending in-place import receipts can reconstruct the
    // identity missing when a board was copied before the first map publish.
    std::map<std::string,std::size_t> import_paths,import_ids;
    for(const auto& proof:proofs.imports){++import_paths[impl->resolve(proof.source,true).u8string()];++import_ids[proof.payload.file_id];}
    for(const auto& proof:proofs.imports){
        require(!cancelled||!cancelled(),"linux_file_map_cancelled");
        const auto path=impl->resolve(proof.source,true);const auto& payload=proof.payload;
        if(path!=impl->resolve(proof.configured,true))continue;
        if(!impl->managed(path)||import_paths[path.u8string()]!=1||import_ids[payload.file_id]!=1)continue;
        const auto name=impl->relative(path);
        if(std::any_of(impl->state.records.begin(),impl->state.records.end(),[&](const auto& record){return record.logical.file_id==payload.file_id||((record.active||record.deferred)&&record.path==name);}))continue;
        try{
            require(valid_logical_identity({payload.volume,payload.file_id},impl->key),"linux_file_import_proof_invalid");
            auto file=impl->pin_path(path,true);const auto measured=measure(file.get(),{payload.volume,payload.file_id},cancelled,nullptr,true);
            require(measured.bytes==payload.bytes&&measured.sha256==payload.sha256,"linux_file_import_proof_changed");
            Record record;record.path=name;record.logical={payload.volume,payload.file_id};record.physical=native(file.get());record.bytes=payload.bytes;record.digest=payload.sha256;
            require(impl->state.records.size()<maximum_records,"linux_file_map_count_limit");impl->state.records.push_back(std::move(record));
        }catch(const std::runtime_error& error){if(std::string_view(error.what())=="linux_file_map_cancelled")throw;}
    }
    const auto originals=impl->state.records;
    const auto defer=[&](std::size_t index){impl->state.records[index]=originals[index];impl->state.records[index].active=false;impl->state.records[index].deferred=true;};
    const auto check_cancelled=[&]{require(!cancelled||!cancelled(),"linux_file_map_cancelled");};
    for(std::size_t index=0;index<impl->state.records.size();++index){
        auto& record=impl->state.records[index];if(!record.active&&!record.deferred)continue;
        check_cancelled();
        try{
            const auto original=root/fs::u8path(record.path);record.active=true;record.deferred=false;
            if(absent(original)){
                unsigned matched{};
                for(const auto& proof:proofs.moves)if(impl->resolve(proof.from,true)==original&&matches(record,proof.payload)){
                    require(++matched==1,"linux_file_prepare_ambiguous_intent");record.path=impl->relative(impl->resolve(proof.to,true));
                }
                for(const auto& proof:proofs.removals)if(impl->resolve(proof.from,true)==original&&matches(record,proof.payload)){
                    require(++matched==1,"linux_file_prepare_ambiguous_intent");const auto final_path=impl->resolve(proof.final_path,true);auto final=impl->pin_path(final_path);
                    if(impl->managed(final_path)){
                        const auto known=std::find_if(originals.begin(),originals.end(),[&](const auto& candidate){return (candidate.active||candidate.deferred)&&candidate.path==impl->relative(final_path)&&matches(candidate,proof.final_payload);});
                        require(known!=originals.end(),"linux_file_prepare_final_record_unavailable");
                        final_dependencies.emplace_back(index,static_cast<std::size_t>(known-originals.begin()));
                    }else{
                        const auto actual=native(final.get());require(proof.final_payload.volume==std::to_string(actual.device)&&proof.final_payload.file_id==std::to_string(actual.inode),"linux_file_prepare_external_final_changed");
                    }
                    struct stat stamp{};const auto verified=measure(final.get(),{proof.final_payload.volume,proof.final_payload.file_id},cancelled,&stamp);
                    require(verified.bytes==proof.final_payload.bytes&&verified.sha256==proof.final_payload.sha256&&verified.bytes==record.bytes&&verified.sha256==record.digest,"linux_file_prepare_final_content_changed");
                    held.push_back({index,final_path,std::move(final),stamp});record.active=false;
                }
                for(const auto& proof:proofs.deletions)if(impl->resolve(proof.first,true)==original&&matches(record,proof.second)){
                    require(++matched==1,"linux_file_prepare_ambiguous_intent");record.active=false;
                }
                require(matched==1,"linux_file_prepare_missing_payload_without_intent");absent_paths.emplace_back(index,original);
                if(!record.active)continue;
            }
            const auto path=root/fs::u8path(record.path);bool importing{};
            for(const auto& proof:proofs.imports)if(impl->resolve(proof.source,true)==path&&impl->resolve(proof.configured,true)==path&&matches(record,proof.payload)){
                require(!importing&&import_paths[path.u8string()]==1&&import_ids[record.logical.file_id]==1,"linux_file_prepare_ambiguous_import");importing=true;
            }
            auto fd=impl->pin_path(path,importing);struct stat stamp{};const auto value=measure(fd.get(),record.logical,cancelled,&stamp,importing);
            require(value.bytes==record.bytes&&value.sha256==record.digest,"linux_file_map_changed_payload_preserved");record.physical=native(fd.get());
            held.push_back({index,root/fs::u8path(record.path),std::move(fd),stamp});
        }catch(const std::runtime_error& error){
            if(std::string_view(error.what())=="linux_file_map_cancelled")throw;
            defer(index);
        }
    }
    // No first-match winner when multiple old identities claim one candidate.
    std::map<std::string,std::vector<std::size_t>> names;
    std::map<std::pair<std::uint64_t,std::uint64_t>,std::vector<std::size_t>> objects;
    for(std::size_t i=0;i<impl->state.records.size();++i){const auto& r=impl->state.records[i];if(r.active){names[r.path].push_back(i);objects[{r.physical.device,r.physical.inode}].push_back(i);}}
    for(const auto& group:names)if(group.second.size()>1)for(const auto i:group.second)defer(i);
    for(const auto& group:objects)if(group.second.size()>1)for(const auto i:group.second)defer(i);
    check_cancelled();
    for(const auto& item:held)if(!impl->state.records[item.record].deferred){
        try{
            struct stat now{};require(fstat(item.file.get(),&now)==0,"linux_file_prepare_pin_unavailable");const auto& old=item.stamp;
            require(now.st_dev==old.st_dev&&now.st_ino==old.st_ino&&now.st_size==old.st_size&&now.st_mtim.tv_sec==old.st_mtim.tv_sec&&now.st_mtim.tv_nsec==old.st_mtim.tv_nsec&&now.st_ctim.tv_sec==old.st_ctim.tv_sec&&now.st_ctim.tv_nsec==old.st_ctim.tv_nsec,"linux_file_prepare_captured_content_changed");
            const auto named=impl->pin_path(item.path,(old.st_mode&077)!=0);struct stat current{};require(fstat(named.get(),&current)==0&&current.st_dev==now.st_dev&&current.st_ino==now.st_ino,"linux_file_prepare_name_replaced");
        }catch(const std::runtime_error&){defer(item.record);}
    }
    for(const auto& item:absent_paths)if(!impl->state.records[item.first].deferred){try{require(absent(item.second),"linux_file_prepare_original_reappeared");}catch(const std::runtime_error&){defer(item.first);}}
    for(const auto& dependency:final_dependencies)if(!impl->state.records[dependency.second].active)defer(dependency.first);
    impl->state.root=root.u8string();impl->state.physical=impl->root_identity();impl->state.restored_core=false;check_cancelled();impl->save();
    return std::shared_ptr<LinuxFilePayloadBindings>(new LinuxFilePayloadBindings(std::move(impl)));
}
app::FileObjectIdentity LinuxFilePayloadBindings::existing(const fs::path& path,int fd){std::lock_guard<std::mutex> guard(impl_->mutex);impl_->live();if(!impl_->managed(path)){const auto actual=native(fd);return {std::to_string(actual.device),std::to_string(actual.inode)};}return impl_->find(path,fd).logical;}
app::FileObjectIdentity LinuxFilePayloadBindings::preview_unregistered(const fs::path& path,int fd,const std::optional<app::FilePayloadIdentity>& confirmed){std::lock_guard<std::mutex> guard(impl_->mutex);return impl_->import_candidate(path,fd,confirmed);}
void LinuxFilePayloadBindings::adopt_unregistered(const fs::path& path,int fd,const app::FilePayloadIdentity& expected,const std::function<bool()>& cancelled,const std::function<void(bool)>& boundary){
    std::lock_guard<std::mutex> guard(impl_->mutex);const auto logical=impl_->import_candidate(path,fd,expected);
    require(!cancelled||!cancelled(),"linux_file_import_adoption_cancelled");
    require(logical.volume==expected.volume&&logical.file_id==expected.file_id,"linux_file_import_candidate_changed");
    struct stat before{};const auto value=measure(fd,logical,cancelled,&before,true);require(value.bytes==expected.bytes&&value.sha256==expected.sha256,"linux_file_import_candidate_content_changed");
    require(!cancelled||!cancelled(),"linux_file_import_adoption_cancelled");
    Record* selected=nullptr;
    if(impl_->managed(path)){
        const auto name=impl_->relative(path);for(auto& record:impl_->state.records)if(record.active&&record.path==name){selected=&record;break;}
        if(!selected){
            for(const auto& record:impl_->state.records)require(record.logical.volume!=logical.volume||record.logical.file_id!=logical.file_id,"linux_file_import_retired_identity_preserved");
            Record record;record.path=name;record.logical=logical;record.physical=native(fd);record.bytes=value.bytes;record.digest=value.sha256;
            impl_->state.records.push_back(std::move(record));selected=&impl_->state.records.back();impl_->save();
        }
    }
    // Complete this short custody transition even if cancellation arrives:
    // the durable import receipt precedes it and catalog commit follows it.
    if((before.st_mode&077)!=0){
        if(boundary)boundary(false);
        require(fchmod(fd,before.st_mode&0600)==0&&fsync(fd)==0,"linux_file_import_protection_failed");
        struct stat after{};require(fstat(fd,&after)==0&&import_permissions(after)&&(after.st_mode&077)==0&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&before.st_size==after.st_size&&before.st_mtim.tv_sec==after.st_mtim.tv_sec&&before.st_mtim.tv_nsec==after.st_mtim.tv_nsec,"linux_file_import_protection_changed_payload");
        if(boundary)boundary(true);
    }
    if(selected){selected->bytes=value.bytes;selected->digest=value.sha256;selected->physical=native(fd);impl_->save();}
}
app::FileObjectIdentity LinuxFilePayloadBindings::created(const fs::path& path,int fd){
    std::lock_guard<std::mutex> guard(impl_->mutex);impl_->live();Record record;record.physical=native(fd);
    if(!impl_->managed(path))return {std::to_string(record.physical.device),std::to_string(record.physical.inode)};
    record.path=impl_->relative(path);
    for(const auto& old:impl_->state.records)require(!old.active||old.path!=record.path,"linux_file_map_existing_name_preserved");
    std::array<unsigned char,16> id{};randombytes_buf(id.data(),id.size());
    if(record.path.rfind("ftn/",0)==0){
        require(decimal_token(id.data())!=0,"linux_file_map_identity_collision");
        record.logical={ftn_volume(impl_->key),std::to_string(decimal_token(id.data()))};
    }else record.logical={"sfx-linux-"+hex(impl_->key.board.data(),impl_->key.board.size()),hex(id.data(),id.size())};
    for(const auto& old:impl_->state.records)require(old.logical.file_id!=record.logical.file_id,"linux_file_map_identity_collision");
    const auto payload=measure(fd,record.logical,{});require(payload.bytes==0,"linux_file_map_new_empty_object_required");record.digest=payload.sha256;
    impl_->state.records.push_back(record);impl_->save();return record.logical;
}
void LinuxFilePayloadBindings::checkpoint(const fs::path& path,int fd,const app::FilePayloadIdentity& identity){
    std::lock_guard<std::mutex> guard(impl_->mutex);impl_->live();if(!impl_->managed(path))return;auto& record=impl_->find(path,fd,true);const auto current=native(fd);
    require(identity.volume==record.logical.volume&&identity.file_id==record.logical.file_id,"linux_file_map_checkpoint_identity_changed");
    if(record.bytes==identity.bytes&&record.digest==identity.sha256&&record.physical==current)return;
    record.bytes=identity.bytes;record.digest=identity.sha256;record.physical=current;impl_->save();
}
void LinuxFilePayloadBindings::moved(const fs::path& from,const fs::path& to,int fd,const app::FileObjectIdentity& identity){
    std::lock_guard<std::mutex> guard(impl_->mutex);impl_->live();if(!impl_->managed(from)&&!impl_->managed(to))return;
    require(impl_->managed(from)&&impl_->managed(to),"linux_file_map_boundary_move_requires_copy");
    auto& record=impl_->find(from,fd,true);const auto destination=impl_->relative(to);
    require(record.logical.volume==identity.volume&&record.logical.file_id==identity.file_id,"linux_file_map_move_identity_changed");
    for(const auto& other:impl_->state.records)require(!other.active||other.path!=destination,"linux_file_map_move_collision");
    record.path=destination;record.physical=native(fd);impl_->save();
}
void LinuxFilePayloadBindings::removed(const fs::path& path,const app::FileObjectIdentity& identity){
    std::lock_guard<std::mutex> guard(impl_->mutex);impl_->live();if(!impl_->managed(path))return;const auto name=impl_->relative(path);
    for(auto& record:impl_->state.records)if(record.active&&record.path==name){require(record.logical.volume==identity.volume&&record.logical.file_id==identity.file_id,"linux_file_map_remove_identity_changed");record.active=false;impl_->save();return;}
    throw std::runtime_error("linux_file_map_remove_unknown");
}
void LinuxFilePayloadBindings::refresh(const std::function<bool()>& cancelled){
    std::lock_guard<std::mutex> guard(impl_->mutex);impl_->live();auto next=impl_->state;std::vector<FileDescriptor> held;
    for(auto& record:next.records)if(record.active){require(!cancelled||!cancelled(),"linux_file_map_cancelled");auto fd=impl_->pin(record);require(record.physical==native(fd.get()),"linux_file_map_payload_replaced");const auto value=measure(fd.get(),record.logical,cancelled);record.bytes=value.bytes;record.digest=value.sha256;held.push_back(std::move(fd));}
    for(const auto& record:next.records)if(record.active){const auto current=impl_->pin(record);require(native(current.get())==record.physical,"linux_file_map_snapshot_namespace_changed");}
    require(!cancelled||!cancelled(),"linux_file_map_cancelled");impl_->state=std::move(next);impl_->save();
}
fs::path LinuxFilePayloadBindings::resolve_path(const fs::path& input)const{std::lock_guard<std::mutex> guard(impl_->mutex);return impl_->resolve(input);}
fs::path LinuxFilePayloadBindings::logical_path(const fs::path& input)const{
    std::lock_guard<std::mutex> guard(impl_->mutex);const auto current=impl_->resolve(input);const auto relative=current.lexically_relative(impl_->root);
    return owned_role(relative)?fs::u8path(impl_->state.roots.front())/relative:current;
}
void LinuxFilePayloadBindings::reconcile_recorded_move(const fs::path& from,const fs::path& to,const app::FilePayloadIdentity& expected){
    std::lock_guard<std::mutex> guard(impl_->mutex);impl_->live();const auto before=impl_->relative(from),after=impl_->relative(to);
    auto found=std::find_if(impl_->state.records.begin(),impl_->state.records.end(),[&](const auto& record){return record.logical.volume==expected.volume&&record.logical.file_id==expected.file_id;});
    require(found!=impl_->state.records.end()&&found->active&&found->bytes==expected.bytes&&found->digest==expected.sha256,"linux_file_reconcile_record_mismatch");
    require(found->path==before||found->path==after,"linux_file_reconcile_location_mismatch");
    for(const auto& other:impl_->state.records)require(&other==&*found||!other.active||other.path!=after,"linux_file_reconcile_destination_bound_elsewhere");
    const auto absent=[&]{const auto parent=open_owned_directory(from.parent_path());struct stat named{};require(fstatat(parent.get(),from.filename().c_str(),&named,AT_SYMLINK_NOFOLLOW)<0&&errno==ENOENT,"linux_file_reconcile_original_not_absent");};
    absent();auto intended=*found;intended.path=after;auto held=impl_->pin(intended);const auto physical=native(held.get());
    // A changed fallback ctime cannot prove the moved object after reopen.
    // Preserve that unknown; do not relax replacement detection to dev/inode.
    require(physical==found->physical,"linux_file_reconcile_native_identity_unproved");
    const auto payload=measure(held.get(),found->logical,{});require(payload.bytes==expected.bytes&&payload.sha256==expected.sha256,"linux_file_reconcile_payload_changed");
    absent();const auto named=impl_->pin(intended);require(native(named.get())==physical,"linux_file_reconcile_destination_changed");
    if(found->path!=after){found->path=after;impl_->save();}
}
void LinuxFilePayloadBindings::reconcile_recorded_removal(const fs::path& path,const app::FilePayloadIdentity& expected){
    std::lock_guard<std::mutex> guard(impl_->mutex);impl_->live();const auto relative=impl_->relative(path);
    auto found=std::find_if(impl_->state.records.begin(),impl_->state.records.end(),[&](const auto& record){return record.logical.volume==expected.volume&&record.logical.file_id==expected.file_id;});
    require(found!=impl_->state.records.end()&&!found->deferred&&found->path==relative&&found->bytes==expected.bytes&&found->digest==expected.sha256,"linux_file_reconcile_record_mismatch");
    const auto parent=open_owned_directory(path.parent_path());struct stat named{};require(fstatat(parent.get(),path.filename().c_str(),&named,AT_SYMLINK_NOFOLLOW)<0&&errno==ENOENT,"linux_file_reconcile_removed_not_absent");
    if(found->active){found->active=false;impl_->save();}
}
std::size_t LinuxFilePayloadBindings::deferred_records()const{
    std::lock_guard<std::mutex> guard(impl_->mutex);impl_->live();return static_cast<std::size_t>(std::count_if(impl_->state.records.begin(),impl_->state.records.end(),[](const auto& record){return record.deferred;}));
}
}
