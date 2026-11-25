// One declaration/forwarding list; never an external wire or SQL interface.
SFX_MAINTENANCE_API(application::StorageResult<application::MessageMaintenanceRun>,begin_message_retention,
    (const application::MessageConfigurationActor& a,application::Identifier128 r),(a,r))
SFX_MAINTENANCE_API(application::StorageResult<application::MessageRetentionBatch>,message_retention_batch,
    (const application::MessageConfigurationActor& a,application::Identifier128 r,std::int64_t sequence,application::MessageMaintenanceStop stop),(a,r,sequence,std::move(stop)))
SFX_MAINTENANCE_API(application::StorageResult<application::MessageMaintenanceRun>,finish_message_retention,
    (const application::MessageConfigurationActor& a,application::Identifier128 r,application::MessageMaintenanceOutcome outcome,application::MessageMaintenanceReason reason),(a,r,outcome,reason))
SFX_MAINTENANCE_API(application::StorageResult<application::MessageMaintenanceRun>,message_maintenance_run,
    (const application::MessageConfigurationActor& a,application::Identifier128 r),(a,r))
SFX_MAINTENANCE_API(application::StorageResult<std::vector<application::MessageMaintenanceRun>>,message_maintenance_history,
    (const application::MessageConfigurationActor& a,application::MessageMaintenanceKind kind),(a,kind))
SFX_MAINTENANCE_API(application::StorageResult<application::MessageIntegrityResult>,check_message_integrity,
    (const application::MessageConfigurationActor& a,application::Identifier128 r,application::MessageMaintenanceStop stop),(a,r,std::move(stop)))
SFX_MAINTENANCE_API(application::StorageResult<std::vector<application::MessageIntegrityFinding>>,message_integrity_holds,
    (const application::MessageConfigurationActor& a),(a))
SFX_MAINTENANCE_API(application::StorageStatus,recover_interrupted_message_maintenance,
    (const application::MessageConfigurationActor& a),(a))
SFX_MAINTENANCE_API(application::StorageResult<application::MessageDeletionPreview>,preview_message_deletion,
    (const application::MessageConfigurationActor& a,const application::MessageDeletionTarget& target,std::function<bool()> cancelled),(a,target,std::move(cancelled)))
SFX_MAINTENANCE_API(application::StorageResult<application::MessageDeletionResult>,commit_message_deletion,
    (const application::MessageConfigurationActor& a,application::Identifier128 operation,const application::MessageDeletionPreview& preview,std::function<bool()> cancelled),(a,operation,preview,std::move(cancelled)))
