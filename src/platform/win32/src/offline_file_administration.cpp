#include "sfx/platform/win32/offline_file_administration.hpp"
#include "sfx/platform/win32/file_administration.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/durable_journal_files.hpp"
#include "sfx/platform/win32/bbs_log_timestamp.hpp"
#include <chrono>
#include "sfx/adapters/storage/sqlite_storage.hpp"
namespace sfx::platform::win32 {
namespace app=application;namespace ins=installation;
struct OfflineFileAdministration::Impl {
    std::filesystem::path root;
    std::wstring owner;
    RuntimeInstanceGuard guard;
    WindowsLocalStoragePathPolicy paths;
    ins::Handle database;
    std::unique_ptr<adapters::storage::SqliteStorage> storage;
    std::unique_ptr<WindowsFileAdministration> worker;
    bool finished{};std::string finish_diagnostic;
    Impl(const std::filesystem::path& r,const std::wstring& sid):root(r),owner(sid),paths(sid){
        ins::require(owner==current_process_user_sid_string(),"file_manager_offline_owner_mismatch");ins::verify_runtime_installation(root,owner);
        ins::require(guard.acquire(root,owner).succeeded(),"file_manager_offline_runtime_active");
        (void)configure_portable_file_bindings(root);
        const auto file=root/L"Data"/L"SFX.DB";database=ins::open_object(file,false,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        LARGE_INTEGER size{};ins::require(GetFileSizeEx(database.get(),&size)&&size.QuadPart>0,"file_manager_database_missing_or_empty");
        ins::require(WindowsStorageDaclPolicy(owner).assess(file,StorageDaclObjectKind::file).status.succeeded(),"file_manager_database_security_refused");
        adapters::storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";auto migrations=adapters::storage::sfx_storage_migrations();
        ins::require(!migrations.empty(),"file_manager_schema_contract_unavailable");const auto current=migrations.back().version;
        storage=std::make_unique<adapters::storage::SqliteStorage>(settings,paths,std::move(migrations));
        struct FailedStartup {adapters::storage::SqliteStorage& storage;bool complete{};~FailedStartup(){if(!complete)(void)storage.shutdown();}} failed_startup{*storage};
        const auto schema=storage->verified_schema_version(file);ins::require(schema&&schema.value&&*schema.value==current,"file_manager_requires_setup_migration");
        ins::require(storage->start().succeeded(),"file_manager_offline_storage_unavailable");
        ins::require(storage->file_configuration({true,{}}).succeeded(),"file_manager_offline_catalog_unavailable");
        worker=std::make_unique<WindowsFileAdministration>(*storage,root,owner,app::LocalAccountAuthority::installation_owner,WindowsFileManager::Boundary{},WindowsFileAdministration::IdleClock{},storage.get());
        failed_startup.complete=true;
    }
    std::string finish()noexcept{
        if(finished)return finish_diagnostic;finished=true;
        try{
        if(worker){worker->stop();worker.reset();}
        if(storage){const auto checkpoint=storage->checkpoint();const auto stopped=storage->shutdown();
            PortableFileBindingResult snapshot;
            if(checkpoint&&stopped)snapshot=refresh_portable_file_snapshot(root,owner);
            if(!checkpoint||!stopped||!snapshot.published||snapshot.deferred){
                finish_diagnostic="File Manager has stopped.\n\nSome copy-recovery file identities could not be confirmed.\nFiles and earlier proofs were preserved.\n\nRun BBS Health Check before copying or zipping this folder.";
                const auto configuration=read_operational_configuration_file(root);
                WindowsDurableJournalFiles files(root,owner);WindowsBbsLogTimestamp timestamp;
                bool logged{};
                if(configuration.configuration&&timestamp.configure(configuration.configuration->bbs_time_zone)&&files.configure(configuration.configuration->logging)){
                    app::DurableJournalPublisher publisher(*storage,files,[&](std::int64_t utc){return timestamp.format(utc);});(void)publisher.configure(configuration.configuration->logging);
                    app::AttentionRecord notice;notice.kind=app::AttentionKind::work_preserved;notice.resource="Offline File Manager copy-recovery snapshot";notice.diagnostic="offline_file_snapshot_incomplete_preserved";
                    notice.utc_seconds=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();logged=publisher.publish_attention(notice);
                }
                if(!logged)finish_diagnostic+="\n\nHEYSYSOP.LOG could not record this warning.\nRetain this message.";
            }
            storage.reset();
        }
        }catch(...){finish_diagnostic="File Manager has stopped.\n\nStorage or copy-recovery snapshot completion was not confirmed.\nFiles were preserved.\n\nRun BBS Health Check before copying this folder.\nA HEYSYSOP.LOG notice could not be confirmed.";}
        return finish_diagnostic;
    }
    ~Impl(){(void)finish();}
};
OfflineFileAdministration::OfflineFileAdministration(std::unique_ptr<Impl> p):impl_(std::move(p)){}
OfflineFileAdministration::~OfflineFileAdministration()=default;
std::string OfflineFileAdministration::finish()noexcept{return impl_->finish();}
std::unique_ptr<OfflineFileAdministration> OfflineFileAdministration::begin(const std::filesystem::path& root,const std::wstring& owner,std::string& diagnostic){
    try{auto result=std::unique_ptr<OfflineFileAdministration>(new OfflineFileAdministration(std::make_unique<Impl>(root,owner)));diagnostic.clear();return result;}
    catch(const std::exception& e){diagnostic=app::sanitize_file_text(e.what(),192,false);return {};}
    catch(...){diagnostic="file_manager_offline_unavailable";return {};}
}
application::LocalFileConfigurationSnapshot OfflineFileAdministration::configuration(const application::LocalFileConfigurationQuery& q){try{return app::LocalFileAdministration(*impl_->storage,app::LocalAccountAuthority::installation_owner).snapshot(q);}catch(...){return {};}}
application::StorageResult<application::FileConfiguration> OfflineFileAdministration::configuration_catalog(){return impl_->storage->file_configuration({true,{}});}
std::optional<application::FtnHatchCatalog> OfflineFileAdministration::ftn_hatch_catalog(){const auto config=impl_->storage->ftn_configuration({true,{}});return config&&config.value?std::optional<app::FtnHatchCatalog>(app::ftn_hatch_catalog(*config.value)):std::nullopt;}
application::FileManagementReadSnapshot OfflineFileAdministration::query(const application::FileManagementReadRequest& q){return app::LocalFileManagementQueries(*impl_->storage,app::LocalAccountAuthority::installation_owner,impl_->storage.get()).snapshot(q);}
application::StorageResult<application::ManagedFilePage> OfflineFileAdministration::page(const application::FileManagementQuery& q,application::FileId after,std::size_t limit){return impl_->storage->managed_file_page({true,{}},q,after,limit);}
application::StorageResult<application::ManagedFile> OfflineFileAdministration::detail(application::FileId id){return impl_->storage->managed_file({true,{}},id);}
application::StorageResult<std::vector<application::FileOperation>> OfflineFileAdministration::operations(application::Identifier128 after,std::size_t limit){return impl_->storage->pending_file_operations({true,{}},after,limit);}
application::StorageResult<std::vector<application::FileImport>> OfflineFileAdministration::imports(application::Identifier128 after,std::size_t limit){return impl_->storage->pending_file_imports({true,{}},after,limit);}
application::StorageResult<std::vector<application::FileIntake>> OfflineFileAdministration::intake(application::Identifier128 after,std::size_t limit){return impl_->storage->pending_file_intake({true,{}},limit,after);}
application::FileManagementJobStatus OfflineFileAdministration::start(const application::FileManagementTask& task){try{return impl_->worker->start(task);}catch(...){return {};}}
application::FileManagementJobStatus OfflineFileAdministration::status(application::Identifier128 id){try{return impl_->worker->status(id);}catch(...){return {};}}
application::FileManagementJobStatus OfflineFileAdministration::cancel(application::Identifier128 id){try{return impl_->worker->cancel(id);}catch(...){return {};}}
} // namespace sfx::platform::win32
