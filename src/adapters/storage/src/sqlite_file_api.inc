SFX_FILE_API(application::StorageResult<application::FileCatalog>,file_catalog,
    (const application::FileActor& a),(a))
SFX_FILE_API(application::StorageResult<application::FileConfiguration>,file_configuration,
    (const application::FileConfigurationActor& a),(a))
SFX_FILE_API(application::StorageResult<application::FileGroupDeletionState>,file_group_deletion,
    (const application::FileConfigurationActor& a,application::FileGroupId group),(a,group))
SFX_FILE_API(application::StorageResult<application::FileCallerExemption>,file_caller_exemption,
    (const application::FileConfigurationActor& a,application::AccountId account),(a,account))
SFX_FILE_API(application::StorageResult<std::uint64_t>,file_content_duplicate_count,
    (const application::FileConfigurationActor& a,const application::FilePayloadIdentity& payload),(a,payload))
SFX_FILE_API(application::StorageResult<application::FileChangeOutcome>,change_file_configuration,
    (const application::FileConfigurationActor& a,const application::FileConfigurationChange& r),(a,r))
SFX_FILE_API(application::StorageResult<application::FileCallerState>,file_caller_state,
    (const application::FileActor& a),(a))
SFX_FILE_API(application::StorageResult<application::FileCallerState>,save_file_caller_state,
    (const application::FileActor& a,const application::FileCallerState& r),(a,r))
SFX_FILE_API(application::StorageResult<std::vector<application::FileScanArea>>,file_scan_areas,
    (const application::FileActor& a),(a))
SFX_FILE_API(application::StorageResult<application::FileScanArea>,save_file_scan_area,
    (const application::FileActor& a,const application::FileScanArea& r),(a,r))
SFX_FILE_API(application::StorageStatus,acknowledge_file_scan,
    (const application::FileActor& a,const std::vector<application::FileScanSeen>& r),(a,r))
SFX_FILE_API(application::StorageStatus,complete_file_scan,
    (const application::FileActor& a,const std::vector<application::FileAreaId>& r,std::int64_t f,std::int64_t s),(a,r,f,s))
SFX_FILE_API(application::StorageResult<application::FilePage>,file_page,
    (const application::FileActor& a,const application::FileQuery& r,const application::FileQueryCursor& c,std::size_t n),(a,r,c,n))
SFX_FILE_API(application::StorageResult<application::FileEntry>,file_information,
    (const application::FileActor& a,application::FileId f),(a,f))
SFX_FILE_API(application::StorageResult<application::FileChangeOutcome>,edit_own_file_description,
    (const application::FileActor& a,application::Identifier128 o,application::FileId f,std::int64_t r,const application::FileDescriptions& d),(a,o,f,r,d))
SFX_FILE_API(application::StorageResult<application::FileEntry>,register_file,
    (const application::FileConfigurationActor& a,const application::FileRegistration& r),(a,r))
SFX_FILE_API(application::StorageResult<application::FileCallerStatistics>,file_caller_statistics,
    (const application::FileActor& a,std::string_view d),(a,d))
SFX_FILE_API(application::StorageResult<application::FileDownloadStart>,reserve_file_download,
    (const application::FileActor& a,application::Identifier128 o,application::FileId f,std::string_view d),(a,o,f,d))
SFX_FILE_API(application::StorageResult<application::FileTransferOutcome>,finish_file_download,
    (const application::FileActor& a,application::Identifier128 o,const application::FilePayloadIdentity& p,bool s),(a,o,p,s))
SFX_FILE_API(application::StorageStatus,recover_file_transfers,
    (const application::FileConfigurationActor& a),(a))
SFX_FILE_API(application::StorageResult<application::FileIntake>,begin_file_intake,
    (const application::FileActor& a,const application::FileIntakeStart& r),(a,r))
SFX_FILE_API(application::StorageResult<application::FileIntake>,file_intake,
    (const application::FileConfigurationActor& a,application::Identifier128 o),(a,o))
SFX_FILE_API(application::StorageStatus,bind_file_intake_object,
    (const application::FileActor& a,application::Identifier128 o,bool p,const application::FileObjectIdentity& i),(a,o,p,i))
SFX_FILE_API(application::StorageStatus,finish_file_intake_cleanup,
    (const application::FileConfigurationActor& a,application::Identifier128 o,const application::FileObjectIdentity& i),(a,o,i))
SFX_FILE_API(application::StorageResult<application::FileIntake>,record_file_intake,
    (const application::FileActor& a,const application::FileIntakeReceived& r),(a,r))
SFX_FILE_API(application::StorageResult<application::FileIntake>,prepare_file_intake,
    (const application::FileActor& a,application::Identifier128 o,const application::FileDescriptions& d,bool s),(a,o,d,s))
SFX_FILE_API(application::StorageResult<application::FileEntry>,commit_file_intake,
    (const application::FileActor& a,application::Identifier128 o,const application::FilePayloadIdentity& p),(a,o,p))
SFX_FILE_API(application::StorageResult<std::vector<application::FileIntake>>,pending_file_intake,
    (const application::FileConfigurationActor& a,std::size_t l,application::Identifier128 after),(a,l,after))
SFX_FILE_API(application::StorageStatus,resolve_file_intake,
    (const application::FileConfigurationActor& a,application::Identifier128 o,const application::FilePayloadIdentity& p,bool d,std::string_view t),(a,o,p,d,t))
SFX_FILE_API(application::StorageStatus,recover_file_intake,
    (const application::FileConfigurationActor& a),(a))
SFX_FILE_API(application::StorageResult<application::FileIntakePurge>,file_intake_purge,
    (const application::FileConfigurationActor& a,application::Identifier128 o),(a,o))
SFX_FILE_API(application::StorageStatus,prepare_file_intake_purge,
    (const application::FileConfigurationActor& a,const application::FileIntakePurge& r),(a,r))
SFX_FILE_API(application::StorageStatus,finish_file_intake_purge,
    (const application::FileConfigurationActor& a,const application::FileIntakePurge& r),(a,r))
SFX_FILE_API(application::StorageResult<application::ManagedFilePage>,managed_file_page,
    (const application::FileConfigurationActor& a,const application::FileManagementQuery& q,application::FileId after,std::size_t n),(a,q,after,n))
SFX_FILE_API(application::StorageResult<application::ManagedFile>,managed_file,
    (const application::FileConfigurationActor& a,application::FileId id),(a,id))
SFX_FILE_API(application::StorageResult<std::vector<application::FileNameStatus>>,file_name_status,
    (const application::FileConfigurationActor& a,application::FileAreaId id,const std::vector<std::string>& n),(a,id,n))
SFX_FILE_API(application::StorageResult<application::FileChangeOutcome>,set_file_reupload_block,
    (const application::FileConfigurationActor& a,application::Identifier128 o,application::FileId f,std::int64_t r,bool b),(a,o,f,r,b))
SFX_FILE_API(application::StorageResult<application::FileOperation>,prepare_file_operation,
    (const application::FileConfigurationActor& a,const application::FileOperationRequest& r),(a,r))
SFX_FILE_API(application::StorageResult<application::FileOperation>,file_operation,
    (const application::FileConfigurationActor& a,application::Identifier128 o),(a,o))
SFX_FILE_API(application::StorageStatus,bind_file_operation_destination,
    (const application::FileConfigurationActor& a,application::Identifier128 o,const application::FileObjectIdentity& r),(a,o,r))
SFX_FILE_API(application::StorageStatus,cancel_file_operation,
    (const application::FileConfigurationActor& a,application::Identifier128 o,const application::FileOperationCompletion& r),(a,o,r))
SFX_FILE_API(application::StorageResult<application::FileOperation>,commit_file_operation,
    (const application::FileConfigurationActor& a,application::Identifier128 o,const application::FileOperationCompletion& r),(a,o,r))
SFX_FILE_API(application::StorageStatus,hold_file_operation,
    (const application::FileConfigurationActor& a,application::Identifier128 o,std::string_view d),(a,o,d))
SFX_FILE_API(application::StorageResult<std::vector<application::FileOperation>>,pending_file_operations,
    (const application::FileConfigurationActor& a,application::Identifier128 o,std::size_t n),(a,o,n))
SFX_FILE_API(application::StorageResult<std::string>,file_importer_name,
    (const application::FileConfigurationActor& a),(a))
SFX_FILE_API(application::StorageResult<application::FileImport>,prepare_file_import,
    (const application::FileConfigurationActor& a,const application::FileImportRequest& r),(a,r))
SFX_FILE_API(application::StorageResult<application::FileImport>,file_import,
    (const application::FileConfigurationActor& a,application::Identifier128 o),(a,o))
SFX_FILE_API(application::StorageResult<std::vector<application::FileImport>>,pending_file_imports,
    (const application::FileConfigurationActor& a,application::Identifier128 o,std::size_t n),(a,o,n))
SFX_FILE_API(application::StorageStatus,bind_file_import_destination,
    (const application::FileConfigurationActor& a,application::Identifier128 o,const application::FileObjectIdentity& r),(a,o,r))
SFX_FILE_API(application::StorageResult<application::FileEntry>,commit_file_import,
    (const application::FileConfigurationActor& a,application::Identifier128 o,const application::FileOperationCompletion& r),(a,o,r))
SFX_FILE_API(application::StorageStatus,hold_file_import,
    (const application::FileConfigurationActor& a,application::Identifier128 o,std::string_view d),(a,o,d))
SFX_FILE_API(application::StorageStatus,cancel_file_import,
    (const application::FileConfigurationActor& a,application::Identifier128 o,const application::FileOperationCompletion& r),(a,o,r))
