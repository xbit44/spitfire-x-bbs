#include "sfx/platform/linux/core_restore.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "sfx/platform/linux/installation_layout.hpp"
#include "sfx/application/operational_configuration.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sfx::platform::linux_host {
namespace {
namespace fs=std::filesystem;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
bool same(const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;}
bool stable(const struct stat& a,const struct stat& b){return same(a,b)&&a.st_size==b.st_size&&a.st_mtim.tv_sec==b.st_mtim.tv_sec&&a.st_mtim.tv_nsec==b.st_mtim.tv_nsec&&a.st_ctim.tv_sec==b.st_ctim.tv_sec&&a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;}
std::string hex(const std::uint8_t* bytes,std::size_t length){std::string out;for(std::size_t i=0;i<length;++i){out+="0123456789ABCDEF"[bytes[i]>>4];out+="0123456789ABCDEF"[bytes[i]&15];}return out;}
std::string hash(int fd,const archive::CancelCheck& cancel){
    crypto_hash_sha256_state state{};require(crypto_hash_sha256_init(&state)==0,"restore_hash_unavailable");std::array<unsigned char,65536> buffer{};off_t offset{};
    for(;;){require(!cancel||!cancel(),"restore_cancelled");const auto n=pread(fd,buffer.data(),buffer.size(),offset);if(n<0&&errno==EINTR)continue;require(n>=0,"restore_archive_read_failed");if(!n)break;crypto_hash_sha256_update(&state,buffer.data(),static_cast<unsigned long long>(n));offset+=n;}
    std::array<std::uint8_t,32> digest{};crypto_hash_sha256_final(&state,digest.data());return hex(digest.data(),digest.size());
}
FileDescriptor relative_object(int root,const fs::path& relative,bool directory){
    require(!relative.empty()&&!relative.is_absolute(),"restore_relative_path_required");
    FileDescriptor current(openat(root,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC));require(bool(current),"restore_root_unavailable");
    for(auto it=relative.begin();it!=relative.end();++it){
        require(*it!="."&&*it!=".."&&!it->empty(),"restore_path_alias_refused");
        const bool folder=std::next(it)!=relative.end()||directory;
        FileDescriptor next(openat(current.get(),it->c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC|(folder?O_DIRECTORY:0)));struct stat value{};
        require(next&&fstat(next.get(),&value)==0&&value.st_uid==geteuid()&&(folder?S_ISDIR(value.st_mode):S_ISREG(value.st_mode)&&value.st_nlink==1),"restore_owned_object_unavailable");current=std::move(next);
    }
    return current;
}
}
struct LinuxCoreRestore::Impl {
    fs::path root,source;FileDescriptor root_fd,parent_fd,archive_fd;InstallationLock ownership;
    struct stat root_stamp{},source_stamp{};
    archive::BackupInspection inspected;std::string digest,token,sfx_version;bool consumed{},ftn{};
    std::optional<application::FtnSerialState> prior_ftn;
    void verify(const archive::CancelCheck& cancel={})const{
        auto current=open_owned_directory(root);struct stat a{},b{};
        require(fstat(current.get(),&a)==0&&same(a,root_stamp),"restore_installation_replaced");
        auto parent=open_owned_directory(source.parent_path(),false);
        require(fstat(parent.get(),&a)==0&&fstat(parent_fd.get(),&b)==0&&same(a,b),"restore_archive_parent_replaced");
        require(fstat(archive_fd.get(),&a)==0&&stable(a,source_stamp)&&private_regular_file(archive_fd.get(),64ULL*1024*1024*1024),"restore_archive_changed");
        require(fstatat(parent_fd.get(),source.filename().c_str(),&b,AT_SYMLINK_NOFOLLOW)==0&&same(a,b),"restore_archive_name_replaced");
        require(hash(archive_fd.get(),cancel)==digest,"restore_archive_digest_changed");
    }
    archive::Result protect(const fs::path& path,bool directory,bool live=true,bool executable=false)const{
        try{
            const auto relative=path.lexically_relative(root);require(!relative.empty()&&*relative.begin()!="..","restore_protection_outside_installation");
            if(live){const auto role=relative.begin()->string();require(role=="config"||role=="data"||role=="display"||role=="doors"||role=="logs"||role=="botgate"||(ftn&&role=="ftn"),"restore_protection_unrelated_role");}
            auto file=relative_object(root_fd.get(),relative,directory);struct stat before{},after{};require(fstat(file.get(),&before)==0,"restore_protection_stat");
            require(fchmod(file.get(),directory||executable?0700:0600)==0&&fsync(file.get())==0,"restore_protection_flush");
            auto named=relative_object(root_fd.get(),relative,directory);require(fstat(named.get(),&after)==0&&same(before,after)&&(after.st_mode&077)==0,"restore_protection_changed");
            return {};
        }catch(const std::exception& e){return {archive::Error::publication_failed,e.what()};}
    }
};
LinuxCoreRestore::LinuxCoreRestore(std::unique_ptr<Impl> value):impl_(std::move(value)){}
LinuxCoreRestore::~LinuxCoreRestore()=default;
std::unique_ptr<LinuxCoreRestore> LinuxCoreRestore::preview(const fs::path& root,const fs::path& source,const archive::CancelCheck& cancel,std::string_view sfx_version){
    require(geteuid()!=0&&root.is_absolute()&&source.is_absolute(),"restore_normal_owner_absolute_paths_required");
    layout::require_current(root);
    auto p=std::make_unique<Impl>();p->root=root;p->source=source;p->sfx_version=sfx_version;p->root_fd=open_owned_directory(root);
    require(fstat(p->root_fd.get(),&p->root_stamp)==0&&p->ownership.acquire(root,".sfx-runtime.lock"),"restore_requires_stopped_management");
    // Only verified history is captured under the retained stopped-board lock.
    // Missing/unusable prior history is unknown, never a fabricated zero.
    try{
        auto db=relative_object(p->root_fd.get(),"data/sfx.db",false);
        require(private_regular_file(db.get(),64ULL*1024*1024*1024),"restore_prior_database_not_private");
        LinuxLocalStoragePathPolicy paths;auto settings=layout::storage_settings(root);
        adapters::storage::SqliteStorage storage(settings,paths);
        const auto state=storage.verified_ftn_serial_history(root/"data/sfx.db");
        if(state&&state.value)p->prior_ftn=*state.value;
    }catch(const std::exception&){p->prior_ftn.reset();}
    (void)open_owned_directory(root/"backup");(void)open_owned_directory(root/"work");
    for(const auto* role:{"config","data","display","doors","logs","botgate"})(void)open_owned_directory(root/role);
    // The selected archive must survive the core-role swaps. Sources inside a
    // mutable role are refused, not copied into an implicit alternate location.
    const auto relative=source.lexically_relative(root);
    require(relative.empty()||*relative.begin()==".."||*relative.begin()=="backup","restore_archive_inside_mutable_role_refused");
    p->parent_fd=open_owned_directory(source.parent_path(),false);
    p->archive_fd=FileDescriptor(openat(p->parent_fd.get(),source.filename().c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
    require(p->archive_fd&&private_regular_file(p->archive_fd.get(),64ULL*1024*1024*1024)&&flock(p->archive_fd.get(),LOCK_SH|LOCK_NB)==0&&fstat(p->archive_fd.get(),&p->source_stamp)==0&&p->source_stamp.st_size>0,"restore_archive_not_private_or_busy");
    p->digest=hash(p->archive_fd.get(),cancel);p->inspected=archive::inspect_backup(source,{},cancel);require(p->inspected.result.ok(),"restore_archive_verification_failed");
    const auto contains=[&](std::string_view path){return std::any_of(p->inspected.files.begin(),p->inspected.files.end(),[&](const auto& f){return f.path==path;});};
    require(contains("Data/SFX.DB")&&contains("Config/SFX.conf")&&contains("Config/file-identities-linux.v1"),"restore_native_current_core_required");
    const auto marker=archive::read_zip_member(source,"Config/linux-layout.v1",128);
    require(marker.result.ok()&&std::string(marker.bytes.begin(),marker.bytes.end())==layout::marker_text,"restore_requires_matching_linux_lowercase_layout");
    require(!contains("Config/compaction-pending.v1"),"restore_source_has_unresolved_compaction");
    struct stat role{};
    if(fstatat(p->root_fd.get(),"ftn",&role,AT_SYMLINK_NOFOLLOW)==0){(void)open_owned_directory(root/"ftn");p->ftn=true;}
    else require(errno==ENOENT,"restore_ftn_capability_unavailable");
    LinuxSecureIdentifierGenerator ids;const auto id=ids.generate();p->token=hex(id.bytes.data(),id.bytes.size())+p->digest;
    p->verify(cancel);return std::unique_ptr<LinuxCoreRestore>(new LinuxCoreRestore(std::move(p)));
}
const archive::BackupInspection& LinuxCoreRestore::inspection()const noexcept{return impl_->inspected;}
const std::string& LinuxCoreRestore::confirmation_token()const noexcept{return impl_->token;}
LinuxCoreRestoreResult LinuxCoreRestore::execute(std::string_view confirmation,const archive::CancelCheck& cancel,std::function<archive::Result()> gate,archive::Timestamp preservation_time){
    LinuxCoreRestoreResult result;auto& p=*impl_;
    try{
        require(!p.consumed&&confirmation==p.token,"restore_preview_confirmation_required");p.verify(cancel);p.consumed=true;
        layout::require_current(p.root);
        auto backup=open_owned_directory(p.root/"backup");
        if(mkdirat(backup.get(),"upgrades",0700)!=0)require(errno==EEXIST,"restore_preservation_directory_create_failed");
        (void)open_owned_directory(p.root/"backup/upgrades");require(fsync(backup.get())==0,"restore_preservation_directory_flush_failed");
        archive::RestoreRequest request;request.installation_root=p.root;request.archive_path=p.source;
        request.physical_layout=archive::PhysicalLayout::linux_lowercase_v1;
        request.sfx_version=p.sfx_version;
        request.transaction_id=p.token.substr(0,32);request.ftn_layout=p.ftn;request.cancelled=cancel;
        request.pre_restore_created=std::move(preservation_time);
        request.secure_live_directory=[&](const fs::path& path){return p.protect(path,true);};
        request.secure_live_file=[&](const fs::path& path){
            const auto relative=layout::archive_relative(path.lexically_relative(p.root).generic_u8string());
            const auto found=std::find_if(p.inspected.files.begin(),p.inspected.files.end(),[&](const auto& file){return file.path==relative;});
            require(found!=p.inspected.files.end(),"restore_file_missing_from_verified_manifest");
            return p.protect(path,false,true,found->owner_executable);
        };
        request.verify_live_publication=[&]()->archive::Result{
            try{
                p.verify(cancel);layout::require_current(p.root);auto config=open_owned_directory(p.root/"config");
                const auto raw=read_private_file(config.get(),"sfx.conf",application::kOperationalConfigurationMaximumBytes);
                require(application::parse_operational_configuration(std::string(raw.begin(),raw.end())).valid(),"restore_configuration_invalid");
                require(bool(load_portable_board_key(p.root)),"restore_portable_key_invalid");
                LinuxLocalStoragePathPolicy paths;auto settings=layout::storage_settings(p.root);
                adapters::storage::SqliteStorage storage(settings,paths,adapters::storage::sfx_storage_migrations());
                require(bool(storage.validate_backup(p.root/"data/sfx.db")),"restore_database_validation_failed");
                const auto version=storage.verified_schema_version(p.root/"data/sfx.db");
                require(bool(version)&&version.value.has_value(),"restore_candidate_schema_invalid");
                require(bool(storage.start()),"restore_candidate_database_open_failed");
                auto preserved=storage.preserve_ftn_serial_history({true,{}},p.prior_ftn,*version.value<22);
                if(preserved){
                    const auto state=storage.ftn_serial_state({true,{}});
                    if(!state||!state.value)preserved=state.status;
                    else result.ftn_recovery_required=state.value->recovery_required;
                }
                const auto stopped=storage.shutdown();
                require(bool(preserved)&&bool(stopped),"restore_ftn_history_publication_failed");
                result.file_prepare_required=LinuxFilePayloadBindings::mark_restored_core(p.root);
                try{(void)LinuxFilePayloadBindings::open(p.root);}
                catch(const std::exception& e){if(std::string_view(e.what())!="linux_file_map_prepare_required")throw;result.file_prepare_required=true;}
                return gate?gate():archive::Result{};
            }catch(const std::exception& e){return {archive::Error::publication_failed,e.what()};}
        };
        result.restore=archive::restore_backup(request);
        // These are only the exact new artifacts returned for this transaction;
        // no historic archive or unrelated recovery tree is re-permissioned.
        const std::array<std::pair<fs::path,bool>,3> artifacts{{
            {result.restore.pre_restore_archive,result.restore.pre_restore_archive_created},
            {result.restore.retained_previous_state,result.restore.retained_previous_state_created},
            {result.restore.retained_failed_state,result.restore.retained_failed_state_created}}};
        for(std::size_t i=0;i<artifacts.size();++i){const auto& artifact=artifacts[i];if(!artifact.second)continue;
            const auto secured=p.protect(artifact.first,i!=0,false);
            if(!secured.ok()){result.restore.result=secured;break;}
        }
        require(fsync(p.root_fd.get())==0,"restore_root_flush_failed_state_retained");
    }catch(const std::exception& e){result.restore.result={archive::Error::publication_failed,e.what()};}
    return result;
}
} // namespace sfx::platform::linux_host
