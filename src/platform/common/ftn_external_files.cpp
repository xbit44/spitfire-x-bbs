#include "ftn_external_files.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#else
#include "sfx/platform/linux/installation_layout.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sfx::platform::external_ftn {
namespace {
namespace app=application;
#ifdef _WIN32
constexpr auto path_style=app::FtnExternalPathStyle::canonical;
#else
constexpr auto path_style=app::FtnExternalPathStyle::linux_lowercase_v1;
#endif
std::string native_relative(std::string_view value){return app::ftn_external_physical_path(value,path_style);}
std::string canonical_relative(std::string_view value){
    // Existing host APIs and persisted records use this canonical namespace.
    // Native hook arguments are translated back before serializing a record.
    if(value.rfind("FTN/External/",0)==0)return std::string(value);
    return app::ftn_external_canonical_path(value,path_style);
}
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
std::string token(const app::Identifier128& id){std::string s;for(auto b:id.bytes){s+="0123456789abcdef"[b>>4];s+="0123456789abcdef"[b&15];}return s;}
void directory(const std::filesystem::path& path,bool create){
#ifdef _WIN32
    auto attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES&&create){require(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND,"ftn_external_directory_unavailable");require(CreateDirectoryW(path.c_str(),nullptr)!=0||GetLastError()==ERROR_ALREADY_EXISTS,"ftn_external_directory_create_failed");attributes=GetFileAttributesW(path.c_str());}
    require(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"ftn_external_directory_invalid");
#else
    struct stat st{};
    if(lstat(path.c_str(),&st)!=0&&create){require(errno==ENOENT,"ftn_external_directory_unavailable");require(mkdir(path.c_str(),0700)==0||errno==EEXIST,"ftn_external_directory_create_failed");}
    require(lstat(path.c_str(),&st)==0&&S_ISDIR(st.st_mode)&&st.st_uid==geteuid()&&(st.st_mode&0022)==0,"ftn_external_directory_invalid");
#endif
}
std::filesystem::path checked(const std::filesystem::path& root,std::string_view relative,bool parents=false){
#ifndef _WIN32
    linux_host::layout::require_current(root);
#endif
    const auto canonical=canonical_relative(relative);relative=canonical;
    require(root.is_absolute()&&root.lexically_normal()==root&&root!=root.root_path(),"ftn_external_root_invalid");
    require(app::valid_ftn_external_path(relative)&&relative.rfind("FTN/External/",0)==0,"ftn_external_path_invalid");
    directory(root,false);directory(root/(path_style==app::FtnExternalPathStyle::canonical?"FTN":"ftn"),false);
    const auto physical=native_relative(relative);require(!physical.empty(),"ftn_external_native_path_invalid");
    const auto path=root/std::filesystem::u8path(physical);auto current=root;
    const auto rel=path.parent_path().lexically_relative(root);
    for(const auto& part:rel){current/=part;directory(current,parents);}
    return path;
}
class File {
public:
    explicit File(const std::filesystem::path& path,bool create=false,bool writable=false,bool secret=false):path_(path){
#ifdef _WIN32
        const auto owner=win32::current_process_user_sid_string();PSECURITY_DESCRIPTOR descriptor{};
        if(create&&secret){const auto sddl=L"O:"+owner+L"D:P(A;;FA;;;"+owner+L")(A;;FA;;;SY)(A;;FA;;;BA)";require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"ftn_external_private_descriptor_failed");}
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
        handle_=CreateFileW(path.c_str(),(create||writable)?(GENERIC_READ|GENERIC_WRITE):GENERIC_READ,FILE_SHARE_READ,descriptor?&attributes:nullptr,create?CREATE_NEW:OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(descriptor)LocalFree(descriptor);
        require(handle_!=INVALID_HANDLE_VALUE,"ftn_external_file_open_failed");
        if(secret&&!win32::WindowsStorageDaclPolicy(owner).assess(path,win32::StorageDaclObjectKind::file).status){CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;require(false,"ftn_external_profile_not_private");}
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(handle_,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||info.nNumberOfLinks!=1){CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;require(false,"ftn_external_file_invalid");}
        before_=info;
#else
        fd_=open(path.c_str(),(create?(O_RDWR|O_CREAT|O_EXCL):(writable?O_RDWR:O_RDONLY))|O_CLOEXEC|O_NOFOLLOW,0600);
        require(fd_>=0,"ftn_external_file_open_failed");
        if(fstat(fd_,&before_)!=0||!S_ISREG(before_.st_mode)||before_.st_uid!=geteuid()||before_.st_nlink!=1||(before_.st_mode&0022)!=0){close(fd_);fd_=-1;require(false,"ftn_external_file_invalid");}
        if(secret&&(before_.st_mode&0077)!=0){close(fd_);fd_=-1;require(false,"ftn_external_profile_not_private");}
#endif
    }
    ~File(){
#ifdef _WIN32
        if(handle_!=INVALID_HANDLE_VALUE)CloseHandle(handle_);
#else
        if(fd_>=0)close(fd_);
#endif
    }
    File(const File&)=delete;File& operator=(const File&)=delete;
    std::uint64_t size()const{
#ifdef _WIN32
        return (static_cast<std::uint64_t>(before_.nFileSizeHigh)<<32)|before_.nFileSizeLow;
#else
        require(before_.st_size>=0,"ftn_external_file_size_invalid");return static_cast<std::uint64_t>(before_.st_size);
#endif
    }
    std::uint64_t timestamp()const{
#ifdef _WIN32
        const auto value=(static_cast<std::uint64_t>(before_.ftLastWriteTime.dwHighDateTime)<<32)|before_.ftLastWriteTime.dwLowDateTime;
        require(value>=116444736000000000ULL,"ftn_external_file_time_invalid");return (value-116444736000000000ULL)/10000000;
#else
        require(before_.st_mtime>=0,"ftn_external_file_time_invalid");return static_cast<std::uint64_t>(before_.st_mtime);
#endif
    }
    std::size_t read(char* data,std::size_t capacity){
#ifdef _WIN32
        DWORD amount{};require(capacity<=65536&&ReadFile(handle_,data,static_cast<DWORD>(capacity),&amount,nullptr),"ftn_external_file_read_failed");return amount;
#else
        ssize_t amount;do{amount=::read(fd_,data,capacity);}while(amount<0&&errno==EINTR);
        require(amount>=0,"ftn_external_file_read_failed");return static_cast<std::size_t>(amount);
#endif
    }
    void write(std::string_view bytes){while(!bytes.empty()){
        const auto length=std::min<std::size_t>(65536,bytes.size());
#ifdef _WIN32
        DWORD amount{};require(WriteFile(handle_,bytes.data(),static_cast<DWORD>(length),&amount,nullptr)&&amount,"ftn_external_file_write_failed");
#else
        ssize_t amount;do{amount=::write(fd_,bytes.data(),length);}while(amount<0&&errno==EINTR);require(amount>0,"ftn_external_file_write_failed");
#endif
        bytes.remove_prefix(static_cast<std::size_t>(amount));
    }}
    void flush(){
#ifdef _WIN32
        require(FlushFileBuffers(handle_)!=0,"ftn_external_file_flush_failed");
#else
        require(fsync(fd_)==0,"ftn_external_file_flush_failed");
#endif
    }
    void verify()const{
#ifdef _WIN32
        BY_HANDLE_FILE_INFORMATION after{};require(GetFileInformationByHandle(handle_,&after)!=0&&after.dwVolumeSerialNumber==before_.dwVolumeSerialNumber&&after.nFileIndexHigh==before_.nFileIndexHigh&&after.nFileIndexLow==before_.nFileIndexLow&&after.nFileSizeHigh==before_.nFileSizeHigh&&after.nFileSizeLow==before_.nFileSizeLow&&CompareFileTime(&after.ftLastWriteTime,&before_.ftLastWriteTime)==0,"ftn_external_file_changed");
#else
        struct stat after{},named{};require(fstat(fd_,&after)==0&&lstat(path_.c_str(),&named)==0&&after.st_dev==before_.st_dev&&after.st_ino==before_.st_ino&&named.st_dev==before_.st_dev&&named.st_ino==before_.st_ino&&after.st_size==before_.st_size&&after.st_mtim.tv_sec==before_.st_mtim.tv_sec&&after.st_mtim.tv_nsec==before_.st_mtim.tv_nsec&&after.st_ctim.tv_sec==before_.st_ctim.tv_sec&&after.st_ctim.tv_nsec==before_.st_ctim.tv_nsec,"ftn_external_file_changed");
#endif
    }
private:
    std::filesystem::path path_;
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};BY_HANDLE_FILE_INFORMATION before_{};
#else
    int fd_{-1};struct stat before_{};
#endif
};
void flush_parent(const std::filesystem::path& path){
#ifndef _WIN32
    const int fd=open(path.parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);require(fd>=0,"ftn_external_parent_open_failed");const int rc=fsync(fd);close(fd);require(rc==0,"ftn_external_parent_flush_failed");
#else
    (void)path; // MoveFileEx WRITE_THROUGH below supplies rename durability.
#endif
}
app::FtnDigest hash_file(File& file,const std::function<void(std::string_view)>& consume={},const std::function<bool()>& running={}){
    require(file.size()<=app::kFtnTransferMaximumBytes,"ftn_external_file_too_large");
    crypto_hash_sha256_state state{};crypto_hash_sha256_init(&state);std::array<char,16384> buffer{};std::uint64_t total{};
    for(;;){require(!running||running(),"ftn_external_cancelled");const auto n=file.read(buffer.data(),buffer.size());if(!n)break;total+=n;require(total<=file.size(),"ftn_external_file_changed");crypto_hash_sha256_update(&state,reinterpret_cast<const unsigned char*>(buffer.data()),n);if(consume)consume({buffer.data(),n});}
    require(total==file.size(),"ftn_external_file_changed");file.verify();app::FtnDigest digest{};crypto_hash_sha256_final(&state,digest.data());return digest;
}
std::string record_path(const app::FtnExternalReceiveRecord& r,const char* directory){return (r.run.empty()?app::ftn_external_root(r.network):std::string("FTN/External"))+"/"+directory+"/"+token(r.id)+".rec";}
// Reconcile only our exact metadata publication pair. General File/open and
// received payloads continue to reject hardlinks. No unrelated link is removed.
void reconcile_metadata_pair(const std::filesystem::path& path,std::string_view relative){
#ifndef _WIN32
    const bool metadata=relative.find("/Records/")!=relative.npos||relative.find("/Consumed/")!=relative.npos||relative.find("/Journal/")!=relative.npos||relative.find("/Runs/")!=relative.npos||relative.find("/Tics/")!=relative.npos||(relative.size()>=8&&relative.substr(relative.size()-8)=="/Profile");
    if(!metadata)return;
    struct stat published{},stage{};const auto temporary=std::filesystem::path(path.string()+".tmp");
    if(lstat(path.c_str(),&published)!=0||published.st_nlink!=2)return;
    require(S_ISREG(published.st_mode)&&published.st_uid==geteuid()&&(published.st_mode&0022)==0&&published.st_size>=0&&published.st_size<=65536&&
        lstat(temporary.c_str(),&stage)==0&&S_ISREG(stage.st_mode)&&stage.st_dev==published.st_dev&&stage.st_ino==published.st_ino&&stage.st_nlink==2,"ftn_external_metadata_links_conflict");
    const int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);require(fd>=0,"ftn_external_metadata_reconcile_failed");
    struct stat held{},current{},named{};const bool exact=fstat(fd,&held)==0&&held.st_dev==published.st_dev&&held.st_ino==published.st_ino&&held.st_nlink==2&&lstat(temporary.c_str(),&current)==0&&current.st_dev==held.st_dev&&current.st_ino==held.st_ino&&lstat(path.c_str(),&named)==0&&named.st_dev==held.st_dev&&named.st_ino==held.st_ino;
    if(!exact){close(fd);require(false,"ftn_external_metadata_reconcile_changed");}
    const int rc=unlink(temporary.c_str());const bool removed=rc==0||errno==ENOENT;
    const bool single=fstat(fd,&held)==0&&held.st_nlink==1;close(fd);
    require(removed&&single,"ftn_external_metadata_reconcile_failed");flush_parent(path);
#else
    (void)path;(void)relative;
#endif
}
}
std::string read_small(const std::filesystem::path& root,std::string_view relative,std::size_t maximum){
    const auto canonical=canonical_relative(relative);relative=canonical;
    const auto path=checked(root,relative);reconcile_metadata_pair(path,relative);
    const bool secret=relative.find("/Runs/")!=relative.npos&&(path.filename()=="binkd.cfg"||path.filename()=="binkd.cfg.tmp");
    File file(path,false,false,secret);require(file.size()<=maximum,"ftn_external_record_too_large");std::string result;std::array<char,4096> buffer{};
    for(;;){const auto n=file.read(buffer.data(),buffer.size());if(!n)break;require(n<=maximum-result.size(),"ftn_external_record_too_large");result.append(buffer.data(),n);}file.verify();return result;
}
void publish_once(const std::filesystem::path& root,std::string_view relative,std::string_view bytes,const std::function<void(std::string_view)>& boundary){
    const auto canonical=canonical_relative(relative);relative=canonical;
    require(bytes.size()<=65536,"ftn_external_record_too_large");const auto path=checked(root,relative,true);
    if(std::filesystem::exists(path)){require(read_small(root,relative,65536)==bytes,"ftn_external_existing_record_conflict");return;}
    const auto temporary=std::string(relative)+".tmp";const auto stage=checked(root,temporary,true);
    if(std::filesystem::exists(stage))require(read_small(root,temporary,65536)==bytes,"ftn_external_partial_record_retained");
    else{const bool secret=relative.find("/Runs/")!=relative.npos&&path.filename()=="binkd.cfg";File file(stage,true,false,secret);file.write(bytes);file.flush();}
#ifdef _WIN32
    require(MoveFileExW(stage.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)!=0,"ftn_external_record_publish_failed");
#else
    require(link(stage.c_str(),path.c_str())==0,"ftn_external_record_publish_failed");flush_parent(path);
    if(boundary)boundary("after-link-before-stage-unlink");
    require(unlink(stage.c_str())==0||errno==ENOENT,"ftn_external_record_stage_retained");
#endif
    (void)boundary;
    flush_parent(path);
}
void prepare_profile(const std::filesystem::path& root,std::string_view network){
    const auto base=app::ftn_external_root(network);require(!base.empty(),"ftn_external_network_invalid");
    for(const auto* leaf:{"Inbound/Secure","Inbound/Nonsecure","Temp","Records","Consumed","Copies","Outbound","Journal"}){
        const auto sentinel=base+"/"+leaf+"/directory-check";(void)checked(root,sentinel,true);
    }
    publish_once(root,base+"/Profile","SFX-BINKD-PROFILE-1\n"+std::string(network)+"\n");
    for(const auto* leaf:{"Inbound/Secure","Inbound/Nonsecure","Temp","Records","Consumed","Runs"})(void)checked(root,std::string("FTN/External/")+leaf+"/probe",true);
}
#include "ftn_external_run.inc"
app::FtnExternalReceiveRecord record_completed(const std::filesystem::path& root,std::string_view network,std::string relative,bool protected_session,const std::vector<app::FtnAddress>& remote){
    const auto base=app::ftn_external_root(network);require(!base.empty()&&read_small(root,base+"/Profile")=="SFX-BINKD-PROFILE-1\n"+std::string(network)+"\n","ftn_external_profile_mismatch");
    std::replace(relative.begin(),relative.end(),'\\','/');
    app::FtnExternalReceiveRecord r;r.id.bytes[0]=1;r.network=network;r.relative_file=canonical_relative(relative);r.protected_session=protected_session;r.remote=remote;
    require(app::encode_ftn_external_receive(r).has_value(),"ftn_external_hook_context_invalid");
    File file(checked(root,r.relative_file));r.size=file.size();r.timestamp=file.timestamp();r.digest=hash_file(file);
    const auto initial=app::encode_ftn_external_receive(r);require(initial.has_value(),"ftn_external_hook_context_invalid");
    app::FtnDigest identity{};crypto_hash_sha256(identity.data(),reinterpret_cast<const unsigned char*>(initial->data()),initial->size());std::copy_n(identity.begin(),16,r.id.bytes.begin());
    const auto encoded=app::encode_ftn_external_receive(r);require(encoded.has_value(),"ftn_external_record_invalid");publish_once(root,record_path(r,"Records"),*encoded);return r;
}
std::vector<app::FtnExternalReceiveRecord> pending_records(const std::filesystem::path& root,std::string_view network,std::size_t maximum,const std::function<void(std::string_view)>& report,std::string_view after){
    require(maximum&&maximum<=128,"ftn_external_record_page_invalid");const auto base=network.empty()?std::string("FTN/External"):app::ftn_external_root(network);require(!base.empty(),"ftn_external_network_invalid");
    const auto directory_path=checked(root,base+"/Records/probe").parent_path();std::vector<std::string> names;
    for(const auto& entry:std::filesystem::directory_iterator(directory_path))if(entry.path().extension()==".rec"){require(names.size()<100000,"ftn_external_record_catalog_full");names.push_back(entry.path().filename().u8string());}
    std::sort(names.begin(),names.end());std::vector<app::FtnExternalReceiveRecord> result;
    for(const auto& name:names){
        if(!after.empty()&&name<=after)continue;
        try{
        const auto r=app::decode_ftn_external_receive(read_small(root,base+"/Records/"+name));require(r&&r->network==network&&token(r->id)+".rec"==name,"ftn_external_record_invalid");
        const auto consumed=record_path(*r,"Consumed");
        if(std::filesystem::exists(checked(root,consumed))){require(read_small(root,consumed)==*app::encode_ftn_external_receive(*r),"ftn_external_consumed_record_conflict");continue;}
        if(!r->run.empty())verify_run_record(root,*r);
        else require(!r->protected_session,"ftn_external_unbound_session_retained");
        result.push_back(*r);if(result.size()==maximum)break;
        }catch(const std::exception&){if(report)report("ftn_external_record_retained_for_review");}
    }return result;
}
void read_completed(const std::filesystem::path& root,const app::FtnExternalReceiveRecord& r,const std::function<void(std::string_view)>& consume,const std::function<bool()>& running){
    require(app::encode_ftn_external_receive(r).has_value(),"ftn_external_record_invalid");File file(checked(root,r.relative_file));require(file.size()==r.size,"ftn_external_received_size_changed");
    require(hash_file(file,consume,running)==r.digest,"ftn_external_received_hash_changed");
}
void mark_record_consumed(const std::filesystem::path& root,const app::FtnExternalReceiveRecord& r){
    const auto text=app::encode_ftn_external_receive(r);require(text.has_value(),"ftn_external_record_invalid");publish_once(root,record_path(r,"Consumed"),*text);
}
#include "ftn_external_publish.inc"
}
