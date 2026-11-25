// Included after the private OfflineSession definition; no public raw storage,
// guard release/reacquire, untrusted path or caller-controlled ownership bit.
class OfflineMessageDeletion::Impl final{
public:
    Impl(const std::filesystem::path& root,const std::wstring& owner,const application::MessageDeletionTarget& target,std::function<bool()> cancelled)
        :session(root,owner){
        const auto value=session.deletion_preview(target,std::move(cancelled));
        installation::require(value.status.succeeded() && value.value.has_value(),"offline_deletion_preview_unavailable");
        preview=*value.value;operation=session.operation();
    }
    OfflineSession session;
    application::MessageDeletionPreview preview;
    application::Identifier128 operation;
    bool backup_failed{};
};
OfflineMessageDeletion::OfflineMessageDeletion(std::unique_ptr<Impl> value):impl_(std::move(value)){}
OfflineMessageDeletion::~OfflineMessageDeletion()=default;
std::unique_ptr<OfflineMessageDeletion> OfflineMessageDeletion::begin(const std::filesystem::path& root,const std::wstring& owner,
    const application::MessageDeletionTarget& target,std::string& diagnostic,std::function<bool()> cancelled){
    try{auto value=std::make_unique<Impl>(root,owner,target,std::move(cancelled));diagnostic="offline_deletion_preview_ready";
        return std::unique_ptr<OfflineMessageDeletion>(new OfflineMessageDeletion(std::move(value)));}
    catch(...){diagnostic="offline_deletion_refused_or_stale";return {};}
}
const application::MessageDeletionPreview& OfflineMessageDeletion::preview()const noexcept{return impl_->preview;}
OfflineMaintenanceBackupResult OfflineMessageDeletion::backup(std::function<bool()> cancelled){
    const auto result=impl_->session.maintenance_backup(std::move(cancelled));
    // A requested failed prerequisite cannot become an implicit No. A new
    // owner-confirmed attempt may deliberately select No after this object ends.
    if(!result.status)impl_->backup_failed=true;return result;
}
application::StorageResult<application::MessageDeletionResult> OfflineMessageDeletion::commit(std::function<bool()> cancelled){
    if(impl_->backup_failed)return {{application::StorageErrorCode::permission_denied,"requested deletion backup failed; new attempt required"},{}};
    return impl_->session.deletion_commit(impl_->operation,impl_->preview,std::move(cancelled));
}

application::MessageMaintenanceStatus offline_message_maintenance_status(const std::filesystem::path& root,const std::wstring& owner,application::MessageMaintenanceKind kind){
    try{OfflineSession session(root,owner);return session.maintenance_status(kind);}catch(...){return {};}
}
application::AlphaControlResponse offline_configure_message_schedule(const std::filesystem::path& root,const std::wstring& owner,const application::MessageMaintenanceScheduleRequest& request){
    try{OfflineSession session(root,owner);return session.configure_maintenance(request);}catch(...){return {application::LocalControlResponseCode::configuration_failure,"offline_message_schedule_unavailable"};}
}
OfflineCompactionResult offline_compact_messages(const std::filesystem::path& root,const std::wstring& owner,std::function<bool()> cancelled){
    try{OfflineSession session(root,owner);return session.compact(std::move(cancelled));}
    catch(...){OfflineCompactionResult result;result.status={application::StorageErrorCode::internal,"offline compaction refused or failed; no success claimed"};return result;}
}
