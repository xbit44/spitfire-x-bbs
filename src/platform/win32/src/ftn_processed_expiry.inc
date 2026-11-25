#ifdef SFX_FTN_EXPIRY_CLOSE_QUALIFICATION
// Compiled only in the separate native fault executable, never a shipping
// library. Protect the exact already-verified handle outside the writer to
// make the real CloseHandle fail. No fabricated native outcome/capability.
static std::atomic_bool qualification_protect_next_expiry_close{};
void qualification_expiry_protect_next_close(){qualification_protect_next_expiry_close=true;}
#endif
struct FtnProcessedExpiry::Impl {
    app::FtnRetainedOriginal original;std::shared_ptr<std::atomic_bool> cancelled;
    Observation observation{Observation::missing};std::unique_ptr<FilePayloadLease> lease;
    std::weak_ptr<app::FtnRetentionRemoval> sealed;
    struct Removal final:app::FtnRetentionRemoval::Native {
        std::unique_ptr<FilePayloadLease::Impl> held;
#ifdef SFX_FTN_EXPIRY_CLOSE_QUALIFICATION
        bool qualification_protected{};
#endif
        explicit Removal(std::unique_ptr<FilePayloadLease::Impl> value):held(std::move(value)){
#ifdef SFX_FTN_EXPIRY_CLOSE_QUALIFICATION
            qualification_protected=qualification_protect_next_expiry_close.exchange(false);
            if(qualification_protected)ins::require(SetHandleInformation(held->file.get(),HANDLE_FLAG_PROTECT_FROM_CLOSE,HANDLE_FLAG_PROTECT_FROM_CLOSE)!=FALSE,"qualification_protect_exact_handle_failed");
#endif
        }
        bool close_at_shutdown()noexcept override{
            if(!held->file)return true;
#ifdef SFX_FTN_EXPIRY_CLOSE_QUALIFICATION
            if(qualification_protected&&!SetHandleInformation(held->file.get(),HANDLE_FLAG_PROTECT_FROM_CLOSE,0))return false;
#endif
            const auto raw=held->file.get();if(!CloseHandle(raw))return false;
            held->file.release();return true;
        }
        app::FtnExpiryNativeResult disposition_and_close()noexcept override{
            using State=app::FtnExpiryNativeState;
            FILE_DISPOSITION_INFO disposition{};disposition.DeleteFile=TRUE;
            const bool marked=SetFileInformationByHandle(held->file.get(),FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE;
            const auto disposition_error=marked?DWORD{}:GetLastError();
            // No path lookup, hashing, storage reentry, allocation or lock here.
            // Failure leaves the actual handle owned for fail-closed teardown.
            if(!CloseHandle(held->file.get()))return {State::close_unconfirmed,GetLastError()};
            held->file.release();held->removed=marked;
            return {marked?State::closed:State::disposition_failed,disposition_error};
        }
    };
};
FtnProcessedExpiry::FtnProcessedExpiry(std::unique_ptr<Impl> value):impl_(std::move(value)){}
FtnProcessedExpiry::~FtnProcessedExpiry()=default;
FtnProcessedExpiry::Observation FtnProcessedExpiry::observation()const noexcept{return impl_->observation;}
std::unique_ptr<FtnProcessedExpiry> FtnProcessedExpiry::inspect(const ins::Receipt& installation,const app::FtnRetainedOriginal& original,std::shared_ptr<std::atomic_bool> cancelled){
    ins::require(cancelled&&!cancelled->load()&&original.ingress.valid()&&app::valid_ftn_code(original.network),"ftn_expiry_context_invalid");
    ins::verify_ftn_directory(installation);
    auto result=std::make_unique<Impl>();result->original=original;result->cancelled=std::move(cancelled);
    const auto directory=FilePayloadDirectory::private_store(installation.root/L"FTN"/L"Processed"/std::filesystem::u8path(original.network),installation.owner);
    std::string name;for(const auto byte:original.ingress.bytes){name+="0123456789abcdef"[byte>>4];name+="0123456789abcdef"[byte&15];}name+=".dat";
    const auto legacy=name;std::vector<std::string> names;
    if(app::valid_ftn_leaf_name(original.wire_name)){
        names.push_back(original.wire_name);
        const auto dot=original.wire_name.rfind('.');const auto ext=dot==std::string::npos?std::string{}:original.wire_name.substr(dot);
        if(ext.size()<=16)names.push_back(original.wire_name.substr(0,dot).substr(0,128-33-ext.size())+"-"+legacy.substr(0,32)+ext);
    }
    names.push_back(legacy);bool found{};
    for(const auto& candidate:names){
        if(directory.absent(candidate))continue;
        auto probe=directory.read(candidate);const auto object=probe.object_identity();
        if(object.volume==original.native.volume&&object.file_id==original.native.file_id){name=candidate;found=true;break;}
        result->observation=Observation::changed;
    }
    if(!found)return std::unique_ptr<FtnProcessedExpiry>(new FtnProcessedExpiry(std::move(result)));
    // Dedicated true no-sharing open. Existing ordinary leases retain their
    // original FILE_SHARE_READ behavior and ordinary deletion implementation.
    result->lease.reset(new FilePayloadLease(std::make_unique<FilePayloadLease::Impl>(directory.impl_,name,false,true,0,nullptr,std::function<bool(std::uint64_t,std::uint64_t)>{},nullptr,nullptr,nullptr,0)));
    const auto actual=result->lease->identity([&](auto,auto){return !result->cancelled->load();});
    const auto& expected=original.native;
    result->observation=actual.bytes==expected.bytes&&actual.sha256==expected.sha256&&actual.volume==expected.volume&&actual.file_id==expected.file_id?Observation::exact:Observation::changed;
    return std::unique_ptr<FtnProcessedExpiry>(new FtnProcessedExpiry(std::move(result)));
}
std::shared_ptr<app::FtnRetentionRemoval> FtnProcessedExpiry::seal(const app::FtnExpiryIntent& intent){
    const auto& a=impl_->original;const auto& b=intent.original;
    ins::require(impl_->observation==Observation::exact&&impl_->lease&&!impl_->cancelled->load()&&intent.operation.valid()&&intent.dispatch.valid()&&intent.prepared_utc>0&&intent.outcome==app::FtnExpiryOutcome::prepared&&a.ingress==b.ingress&&a.network==b.network&&a.successful_utc==b.successful_utc&&a.configuration_revision==b.configuration_revision&&a.native.bytes==b.native.bytes&&a.native.sha256==b.native.sha256&&a.native.volume==b.native.volume&&a.native.file_id==b.native.file_id,"ftn_expiry_seal_binding_invalid");
    auto native=std::make_unique<Impl::Removal>(std::move(impl_->lease->impl_));impl_->lease.reset();
    auto result=std::shared_ptr<app::FtnRetentionRemoval>(new app::FtnRetentionRemoval(intent,std::move(native),impl_->cancelled));impl_->sealed=result;return result;
}
bool FtnProcessedExpiry::record_confirmed_removal(const std::shared_ptr<app::FtnRetentionRemoval>& proof)noexcept{
    try{
        const auto exact=impl_->sealed.lock();
        if(!proof||proof!=exact||proof->result().state!=app::FtnExpiryNativeState::closed)return false;
        const auto* native=static_cast<Impl::Removal*>(proof->native_.get());
        if(!native||!native->held||!native->held->removed||native->held->file)return false;
        record_portable_removed_file(native->held->path,native->held->logical_object);return true;
    }catch(...){return false;}
}
