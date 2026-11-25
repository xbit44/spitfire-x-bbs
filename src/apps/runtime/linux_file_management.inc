app::LocalFileConfigurationSnapshot LinuxRuntimeHost::file_configuration_snapshot(const app::LocalFileConfigurationQuery& query){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    return app::LocalFileAdministration(*store_.storage(),app::LocalAccountAuthority::installation_owner).snapshot(query);
}
LinuxFileCountRulesSnapshot LinuxRuntimeHost::file_count_rules(){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    LinuxSecurityPublication publication;const auto levels=app::LocalSecurityAdministration(*store_.storage(),publication,store_.identifiers(),clock_,app::LocalAccountAuthority::installation_owner).snapshot();
    const auto files=store_.storage()->file_configuration({true,{}});
    if(levels.code!=app::LocalAccountResultCode::ok||levels.has_more||!files||!files.value)return {};
    return {app::LocalAccountResultCode::ok,files.value->policy,levels.levels,files.value->security_rules};
}
app::LocalFileConfigurationResult LinuxRuntimeHost::change_file_configuration(const app::FileConfigurationChange& change){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready()||!store_.file_bindings())return {};
    if(change.action==app::FileConfigurationAction::security_rule){
        const auto current=file_count_rules();if(current.code!=app::LocalAccountResultCode::ok)return {};
        if(change.expected_catalog_revision>=current.policy.catalog_revision&&std::none_of(current.levels.begin(),current.levels.end(),[&](const auto& level){return level.level==change.security.level&&level.state==app::SecurityLevelState::active;}))
            return {app::LocalAccountResultCode::policy_rejected,{},"This security level is no longer active. Reload the level list."};
        // Older requests reach storage only for exact receipt replay or stale CAS.
    }
    if(change.action==app::FileConfigurationAction::caller_exemption){
        const auto current=file_configuration_snapshot({});if(current.code!=app::LocalAccountResultCode::ok)return {};
        if(change.expected_catalog_revision>=current.policy.catalog_revision){app::LocalAccountQuery query;query.account_id=change.exemption.account;const auto account=account_snapshot(query);
            if(account.code!=app::LocalAccountResultCode::ok||account.accounts.size()!=1||account.accounts.front().state==app::AccountState::tombstoned)
                return {app::LocalAccountResultCode::policy_rejected,{},"This caller is no longer available. Find and select a current caller."};}
    }
    const bool deletion=change.action==app::FileConfigurationAction::preview_group_delete||change.action==app::FileConfigurationAction::begin_group_delete;
    if(deletion){
        if(bbs_owned()||(file_administration_&&file_administration_->active()))return {app::LocalAccountResultCode::policy_rejected,{},"Stop the BBS and finish pending File Manager work before deleting a File Group."};
        // No job is active; release idle views before the explicit deletion.
        file_administration_.reset();
    }
    platform::linux_host::LinuxFileConfigurationPaths paths(root_,store_.file_bindings(),!bbs_owned());
    return app::LocalFileAdministration(*store_.storage(),app::LocalAccountAuthority::installation_owner,&paths).execute(change);
}
app::FileManagementReadSnapshot LinuxRuntimeHost::query_file_management(const app::FileManagementReadRequest& query){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    return app::LocalFileManagementQueries(*store_.storage(),app::LocalAccountAuthority::installation_owner,store_.storage()).snapshot(query);
}
app::FileManagementJobStatus LinuxRuntimeHost::start_file_management(const app::FileManagementTask& task){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready()||!store_.file_bindings())return {};
    try{
        if(!file_administration_)file_administration_=std::make_unique<platform::linux_host::LinuxFileAdministration>(*store_.storage(),root_,store_.file_bindings(),app::LocalAccountAuthority::installation_owner,
            platform::linux_host::LinuxFileManager::Boundary{},platform::linux_host::LinuxFileAdministration::IdleClock{},store_.storage(),
            [this](const app::FtnHatchRequest& request,const std::function<bool()>& running,const platform::linux_host::LinuxFileManager::Progress&){
                const auto result=platform::linux_host::hatch_ftn_native_file(*store_.storage(),*store_.storage(),{root_,store_.file_bindings()},request,running);
                return platform::linux_host::LinuxFileAdministration::HatchOutcome{result.queued,result.journaled,result.reason};
            });
        return file_administration_->start(task);
    }catch(...){return {};}
}
app::FileManagementJobStatus LinuxRuntimeHost::file_management_status(app::Identifier128 id){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    if(file_administration_){file_administration_->expire_idle_views();return file_administration_->status(id);}
    app::FileManagementJobStatus result;result.code=app::LocalAccountResultCode::policy_rejected;return result;
}
app::FileManagementJobStatus LinuxRuntimeHost::cancel_file_management(app::Identifier128 id){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    if(file_administration_)return file_administration_->cancel(id);
    app::FileManagementJobStatus result;result.code=app::LocalAccountResultCode::policy_rejected;return result;
}
std::optional<app::FtnHatchCatalog> LinuxRuntimeHost::file_hatch_choices(){
    std::lock_guard<std::recursive_mutex> lock(operations_);if(!store_ready())return {};
    const auto value=store_.storage()->ftn_configuration({true,{}});if(!value||!value.value)return {};
    return app::ftn_hatch_catalog(*value.value);
}
