namespace {
class WindowsHistoryRecovery final {
    std::filesystem::path root_;
    std::wstring owner_;
    OfflineInstallationGuard guard_;
    installation::Receipt receipt_;
    installation::Handle database_;
    WindowsLocalStoragePathPolicy paths_;
    std::unique_ptr<adapters::storage::SqliteStorage> storage_;
public:
    WindowsHistoryRecovery(const std::filesystem::path& root,const std::wstring& owner):root_(root),owner_(owner),paths_(owner){
        ftn_history::require(owner==installation::operator_sid(),"ftn_recovery_installation_owner_required");
        installation::verify_runtime_installation(root,owner);
        ftn_history::require(bool(guard_.acquire(root,owner)),"ftn_recovery_requires_stopped_bbs");
        receipt_=installation::read_receipt(root,owner);
        database_=installation::open_object(root/L"Data"/L"SFX.DB",false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);
        LARGE_INTEGER size{};ftn_history::require(GetFileSizeEx(database_.get(),&size)&&size.QuadPart>0,"ftn_recovery_database_missing");
        (void)configure_portable_file_bindings(root);
        adapters::storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";
        storage_=std::make_unique<adapters::storage::SqliteStorage>(settings,paths_,adapters::storage::sfx_storage_migrations());
        ftn_history::require(bool(storage_->validate_backup(root/L"Data"/L"SFX.DB")),"ftn_recovery_database_invalid");
        ftn_history::require(bool(storage_->start()),"ftn_recovery_database_open_failed");
        verify();
    }
    ~WindowsHistoryRecovery(){if(storage_)(void)storage_->shutdown();}
    adapters::storage::SqliteStorage& storage(){return *storage_;}
    app::Identifier128 fresh_id(){return WindowsSecureIdentifierGenerator().generate();}
    void verify(){
        ftn_history::require(guard_.acquired(),"ftn_recovery_exclusive_ownership_lost");
        installation::verify_runtime_installation(root_,owner_);
        const auto current=installation::read_receipt(root_,owner_);
        ftn_history::require(current.root_id==receipt_.root_id&&current.volume==receipt_.volume,"ftn_recovery_installation_changed");
    }
    void verify_writers(){ftn_history::verify_external_stopped(root_);}
    std::vector<FilePayloadLease> payloads(){
        return ftn_history::verify_payloads<FilePayloadLease>(*storage_,[&](const app::FtnOutboundJob& job){
            auto directory=FilePayloadDirectory::private_store(root_/L"FTN"/L"Outbound"/std::filesystem::u8path(job.network),owner_);
            return directory.read(ftn_existing_spool_name(directory,job));
        },[&](const app::FtnIngressReceipt& receipt){
            if(receipt.state!=app::FtnIngressState::partial)
                return open_ftn_received(*storage_,receipt_,receipt,[]{return true;},false);
            const auto folder=receipt.network.empty()?root_/L"FTN"/L"Nonsecure":root_/L"FTN"/L"Partial"/std::filesystem::u8path(receipt.network);
            return FilePayloadDirectory::private_store(folder,owner_).read(ftn_native_artifact_name(receipt.id));
        });
    }
    void report(const app::FtnSerialRecoveryReceipt& receipt){
        const auto path=root_/L"Logs"/std::filesystem::u8path(app::ftn_recovery_report_name(receipt));
        const auto text=app::ftn_recovery_report_text(receipt);
        const std::vector<std::uint8_t> bytes(text.begin(),text.end());
        if(std::filesystem::exists(path)){ftn_history::require(installation::read_file(path,65536)==bytes,"ftn_recovery_existing_report_conflict");return;}
        const auto stage=std::filesystem::path(path.native()+L".tmp");
        if(std::filesystem::exists(stage))ftn_history::require(installation::read_file(stage,65536)==bytes,"ftn_recovery_partial_report_retained");
        else{auto descriptor=installation::mutable_descriptor(owner_);installation::create_file(stage,bytes,descriptor.get());}
        ftn_history::require(MoveFileExW(stage.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)!=0,"ftn_recovery_report_publish_failed");
    }
};
}
std::unique_ptr<app::IFtnHistoryRecovery> preview_ftn_history_recovery(const std::filesystem::path& root,const std::wstring& owner){
    return std::make_unique<ftn_history::Operation<WindowsHistoryRecovery>>(root,owner);
}
