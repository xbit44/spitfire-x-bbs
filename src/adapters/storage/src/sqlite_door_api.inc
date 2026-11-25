SFX_DOOR_API(application::StorageResult<application::DoorCatalog>,door_configuration,
    (const application::MessageConfigurationActor& a),(a))
SFX_DOOR_API(application::StorageResult<application::DoorChangeOutcome>,change_door_configuration,
    (const application::MessageConfigurationActor& a,const application::DoorConfigurationChange& c),(a,c))
SFX_DOOR_API(application::StorageResult<application::DoorCallerMenu>,door_menu,
    (const application::MessageActor& a,application::DoorMenuId m),(a,m))
SFX_DOOR_API(application::StorageResult<application::DoorLaunch>,door_launch,
    (const application::MessageActor& a,const application::DoorLaunchRequest& r),(a,r))
SFX_DOOR_API(application::StorageResult<application::DoorEventList>,door_events,
    (const application::MessageActor& a,application::DoorAutomaticEvent event),(a,event))
SFX_DOOR_API(application::StorageResult<application::DoorLaunch>,door_event_launch,
    (const application::MessageActor& a,const application::DoorEventLaunchRequest& r),(a,r))
SFX_DOOR_API(application::StorageResult<application::DoorBulletinCatalog>,door_bulletin_configuration,
    (const application::MessageConfigurationActor& a),(a))
SFX_DOOR_API(application::StorageResult<application::DoorChangeOutcome>,change_door_bulletin,
    (const application::MessageConfigurationActor& a,const application::DoorBulletinChange& r),(a,r))
SFX_DOOR_API(application::StorageResult<application::DoorBulletinListing>,door_bulletins,
    (const application::MessageActor& a),(a))
SFX_DOOR_API(application::StorageResult<std::vector<application::DoorBulletinNotice>>,door_bulletin_changes,
    (const application::MessageActor& a,std::int64_t since),(a,since))
SFX_DOOR_API(application::StorageResult<application::DoorBulletinSource>,door_bulletin_source,
    (const application::MessageConfigurationActor& a,application::DoorBulletinId id),(a,id))
SFX_DOOR_API(application::StorageResult<application::DoorBulletinPublicationResult>,publish_door_bulletin,
    (const application::MessageConfigurationActor& a,const application::DoorBulletinPublication& r),(a,r))
