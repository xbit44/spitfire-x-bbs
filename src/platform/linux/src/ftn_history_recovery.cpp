#include "sfx/platform/linux/core_restore.hpp"
#include "sfx/platform/linux/ftn_native_session.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "../../common/ftn_history_recovery.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sfx::platform::linux_host {
namespace {
namespace app=application;
class LinuxHistoryRecovery final {
    std::filesystem::path root_;
    FileDescriptor root_fd_,database_;
    InstallationLock guard_;
    struct stat root_stamp_{},database_stamp_{};
    LinuxLocalStoragePathPolicy paths_;
    std::unique_ptr<adapters::storage::SqliteStorage> storage_;
    std::shared_ptr<LinuxFilePayloadBindings> bindings_;
public:
    explicit LinuxHistoryRecovery(const std::filesystem::path& root):root_(root),root_fd_(open_owned_directory(root)){
        ftn_history::require(geteuid()!=0&&root.is_absolute(),"ftn_recovery_normal_owner_absolute_root_required");
        ftn_history::require(fstat(root_fd_.get(),&root_stamp_)==0&&guard_.acquire(root,".sfx-runtime.lock"),"ftn_recovery_requires_stopped_management");
        auto data=open_owned_directory(root/"data");
        database_=FileDescriptor(openat(data.get(),"sfx.db",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
        ftn_history::require(database_&&private_regular_file(database_.get(),64ULL*1024*1024*1024)&&fstat(database_.get(),&database_stamp_)==0&&database_stamp_.st_size>0,"ftn_recovery_database_missing_or_not_private");
        ftn_history::require(bool(load_portable_board_key(root)),"ftn_recovery_portable_key_invalid");
        layout::require_current(root);auto settings=layout::storage_settings(root);
        storage_=std::make_unique<adapters::storage::SqliteStorage>(settings,paths_,adapters::storage::sfx_storage_migrations());
        ftn_history::require(bool(storage_->validate_backup(root/"data/sfx.db")),"ftn_recovery_database_invalid");
        ftn_history::require(bool(storage_->start()),"ftn_recovery_database_open_failed");
        verify();
    }
    ~LinuxHistoryRecovery(){if(storage_)(void)storage_->shutdown();}
    adapters::storage::SqliteStorage& storage(){return *storage_;}
    app::Identifier128 fresh_id(){return LinuxSecureIdentifierGenerator().generate();}
    void verify(){
        ftn_history::require(bool(guard_),"ftn_recovery_exclusive_ownership_lost");
        auto root=open_owned_directory(root_);auto data=open_owned_directory(root_/"data");struct stat a{},b{};
        ftn_history::require(fstat(root.get(),&a)==0&&a.st_dev==root_stamp_.st_dev&&a.st_ino==root_stamp_.st_ino&&
            fstatat(data.get(),"sfx.db",&b,AT_SYMLINK_NOFOLLOW)==0&&b.st_dev==database_stamp_.st_dev&&b.st_ino==database_stamp_.st_ino,
            "ftn_recovery_installation_or_database_changed");
    }
    void verify_writers(){ftn_history::verify_external_stopped(root_);}
    std::vector<FilePayloadLease> payloads(){
        const auto queue=storage_->ftn_history_recovery_queue({true,{}});
        ftn_history::require(bool(queue)&&queue.value.has_value(),"ftn_recovery_queue_invalid");
        // Preview owns this exclusive map capability until the action ends.
        // Revalidate payloads through that same capability at confirmation;
        // opening a second map would conflict with our own retained lock.
        if(!bindings_&&(!queue.value->outbound.empty()||!queue.value->ingress.empty()))bindings_=LinuxFilePayloadBindings::open(root_);
        return ftn_history::verify_payloads<FilePayloadLease>(*storage_,[&](const app::FtnOutboundJob& job){
            auto directory=FilePayloadDirectory::private_store(root_/"ftn/outbound"/std::filesystem::u8path(layout::owned_name(job.network)),bindings_);
            return directory.read(ftn_existing_spool_name(directory,job));
        },[&](const app::FtnIngressReceipt& receipt){
            if(receipt.state!=app::FtnIngressState::partial)
                return open_ftn_received(*storage_,{root_,bindings_},receipt,[]{return true;},false);
            const auto folder=receipt.network.empty()?root_/"ftn/nonsecure":root_/"ftn/partial"/(receipt.network.empty()?std::filesystem::path{}:std::filesystem::u8path(layout::owned_name(receipt.network)));
            return FilePayloadDirectory::private_store(folder,bindings_).read(ftn_native_artifact_name(receipt.id));
        });
    }
    void report(const app::FtnSerialRecoveryReceipt& receipt){
        auto logs=open_owned_directory(root_/"logs");const auto name=layout::owned_name(app::ftn_recovery_report_name(receipt));
        const auto text=app::ftn_recovery_report_text(receipt);const std::vector<std::uint8_t> bytes(text.begin(),text.end());
        struct stat st{};
        if(fstatat(logs.get(),name.c_str(),&st,AT_SYMLINK_NOFOLLOW)==0){
            ftn_history::require(read_private_file(logs.get(),name,65536)==bytes,"ftn_recovery_existing_report_conflict");return;
        }
        ftn_history::require(errno==ENOENT,"ftn_recovery_report_unavailable");publish_private_file(logs.get(),name,bytes,false);
    }
};
}
std::unique_ptr<app::IFtnHistoryRecovery> preview_ftn_history_recovery(const std::filesystem::path& root){
    return std::make_unique<ftn_history::Operation<LinuxHistoryRecovery>>(root);
}
}
