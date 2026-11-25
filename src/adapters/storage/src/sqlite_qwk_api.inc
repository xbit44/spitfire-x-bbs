// Dedicated declarations/forwarding, not a caller/config serialization format.
SFX_QWK_API(application::StorageResult<application::QwkPolicy>,qwk_policy,
    (const application::MessageConfigurationActor& a),(a))
SFX_QWK_API(application::StorageResult<application::QwkPolicy>,save_qwk_policy,
    (const application::MessageConfigurationActor& a,application::Identifier128 operation,std::int64_t revision,std::string_view board,std::uint32_t count),(a,operation,revision,board,count))
SFX_QWK_API(application::StorageResult<application::QwkSavedPreferences>,qwk_preferences,
    (const application::MessageActor& a),(a))
SFX_QWK_API(application::StorageResult<application::QwkSavedPreferences>,save_qwk_preferences,
    (const application::MessageActor& a,std::int64_t revision,const application::QwkPreferences& p),(a,revision,p))
SFX_QWK_API(application::StorageResult<std::uint32_t>,qwk_reference,
    (const application::MessageActor& a,application::MessageId m),(a,m))
SFX_QWK_API(application::StorageResult<std::vector<std::uint32_t>>,qwk_references,
    (const application::MessageActor& a,const std::vector<application::MessageId>& ids),(a,ids))
SFX_QWK_API(application::StorageResult<application::QwkPacketPlan>,begin_qwk_packet,
    (const application::MessageActor& a,application::Identifier128 operation),(a,operation))
SFX_QWK_API(application::StorageResult<bool>,validate_qwk_packet_authority,
    (const application::MessageActor& a,const application::QwkPacketPlan& p),(a,p))
SFX_QWK_API(application::StorageResult<std::optional<application::OpenedMessage>>,next_qwk_message,
    (const application::MessageActor& a,const application::QwkPacketPlan& p,std::size_t queue),(a,p,queue))
SFX_QWK_API(application::StorageResult<std::optional<application::QwkMessageCandidate>>,next_qwk_candidate,
    (const application::MessageActor& a,const application::QwkPacketPlan& p,std::size_t queue),(a,p,queue))
SFX_QWK_API(application::StorageResult<application::QwkQueueSelection<application::QwkMessageCandidate>>,select_qwk_candidate,
    (const application::MessageActor& a,const application::QwkPacketPlan& p,std::size_t queue,std::size_t maximum),(a,p,queue,maximum))
SFX_QWK_API(application::StorageResult<application::QwkQueueSelection<application::OpenedMessage>>,select_qwk_message,
    (const application::MessageActor& a,const application::QwkPacketPlan& p,std::size_t queue,std::size_t maximum),(a,p,queue,maximum))
SFX_QWK_API(application::StorageResult<application::QwkDeliveryOutcome>,commit_qwk_delivery,
    (const application::MessageActor& a,const application::QwkDeliveryCommit& r),(a,r))
SFX_QWK_API(application::StorageResult<std::optional<application::QwkDeliveryOutcome>>,qwk_delivery_outcome,
    (const application::MessageActor& a,const application::QwkDeliveryCommit& r),(a,r))
SFX_QWK_API(application::StorageResult<application::QwkPointerChange>,preview_qwk_public_pointers,
    (const application::MessageActor& a,const application::QwkPointerChange& r),(a,r))
SFX_QWK_API(application::StorageStatus,change_qwk_public_pointers,
    (const application::MessageActor& a,const application::QwkPointerChange& r),(a,r))
SFX_QWK_API(application::StorageResult<application::QwkPolicy>,qwk_caller_policy,
    (const application::MessageActor& a),(a))
SFX_QWK_API(application::StorageResult<application::QwkImportOutcome>,import_qwk_reply,
    (const application::MessageActor& a,std::shared_ptr<application::IQwkReplySource> s),(a,std::move(s)))
SFX_QWK_API(application::StorageResult<std::optional<application::QwkImportOutcome>>,qwk_import_outcome,
    (const application::MessageActor& a,const application::QwkReplySourceInfo& s),(a,s))
