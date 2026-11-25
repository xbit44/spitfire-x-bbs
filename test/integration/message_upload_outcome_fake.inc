// Narrow compose-boundary fault injector. Real SQLite owns atomic publication;
// this adapter only hides the first result, simulating loss at the port boundary.
namespace sfx::application {
struct UploadOutcomeStorage final:app::IMessageStorage {
    app::IMessageStorage& real;bool hide_commit=true;unsigned hidden_outcomes=1,commits{},outcomes{};
    std::optional<app::CommitMessageRequest> request;
    explicit UploadOutcomeStorage(app::IMessageStorage& s):real(s){}
    app::StorageResult<app::ComposeContext> begin_message(const app::MessageActor& a,const app::MessageTarget& t,std::optional<app::MessageId> p)override{return real.begin_message(a,t,p);}
    app::StorageResult<app::CommittedMessage> commit_message(const app::CommitMessageRequest& r)override{
        ++commits;request=r;auto result=real.commit_message(r);if(hide_commit)return {{app::StorageErrorCode::io,"injected lost commit result"},{}};return result;
    }
    app::StorageResult<app::MessageCommitOutcome> message_commit_outcome(const app::CommitMessageRequest& r)override{
        ++outcomes;check(request && r.operation==request->operation && r.original_source && request->original_source && r.original_source->bytes==request->original_source->bytes,"upload reconciliation retains the exact operation and original source");
        if(hidden_outcomes){--hidden_outcomes;return {{app::StorageErrorCode::io,"injected unavailable receipt read"},{}};}return real.message_commit_outcome(r);
    }
    app::StorageResult<app::CommittedMessage> reconcile_message(const app::CommitMessageRequest& r)override{return real.reconcile_message(r);}
#define UPLOAD_UNUSED(result,name,arguments) app::result name arguments override {throw std::runtime_error("unexpected upload-only test port: " #name);}
    UPLOAD_UNUSED(StorageResult<MessageConfigurationSnapshot>,message_configuration,(const app::MessageConfigurationActor&,std::optional<app::MessageGroupId>,const app::MessageCatalogCursor&))
    UPLOAD_UNUSED(StorageResult<MessageConfigurationResult>,change_message_configuration,(const app::MessageConfigurationActor&,const app::MessageConfigurationChange&))
    UPLOAD_UNUSED(StorageResult<MessageRecipientPage>,resolve_message_recipient,(const app::MessageActor&,std::string_view,const std::optional<app::MessageRecipient>&,bool))
    UPLOAD_UNUSED(StorageResult<MessageTarget>,system_sysop_target,(const app::MessageActor&))
    UPLOAD_UNUSED(StorageResult<MessageArea>,message_area,(const app::MessageActor&,app::LocalEchoId))
    UPLOAD_UNUSED(StorageResult<MessageCallerStatistics>,message_caller_statistics,(const app::MessageActor&,std::optional<app::LocalEchoId>))
    UPLOAD_UNUSED(StorageResult<MessageArrivalPage>,message_arrival_page,(const app::MessageActor&,std::int64_t,std::size_t))
    UPLOAD_UNUSED(StorageResult<bool>,message_arrival_eligible,(const app::MessageActor&,app::MessageId))
    UPLOAD_UNUSED(StorageResult<std::optional<std::string>>,message_arrival_sender,(const app::MessageActor&,app::MessageId))
    UPLOAD_UNUSED(StorageResult<bool>,has_addressed_unread,(const app::MessageActor&,bool))
    UPLOAD_UNUSED(StorageResult<MessageLoginScanSummary>,login_message_scan_summary,(const app::MessageActor&,bool))
    // Commas inside templates need aliases for the three macro arguments.
    using Groups=app::StorageResult<std::vector<app::MessageGroup>>;
    using Echoes=app::StorageResult<std::vector<app::LocalEcho>>;
    Groups message_groups(const app::MessageActor&)override{throw std::runtime_error("unused groups");}
    Echoes local_echoes(const app::MessageActor&,app::MessageGroupId)override{throw std::runtime_error("unused echoes");}
    UPLOAD_UNUSED(StorageResult<MessageCatalogPage<MessageGroup>>,message_group_page,(const app::MessageActor&,const app::MessageCatalogCursor&))
    UPLOAD_UNUSED(StorageResult<MessageCatalogPage<LocalEcho>>,local_echo_page,(const app::MessageActor&,app::MessageGroupId,const app::MessageCatalogCursor&))
    UPLOAD_UNUSED(StorageResult<MessageQueryContext>,begin_message_query,(const app::MessageActor&,const app::MessageQueryOptions&))
    UPLOAD_UNUSED(StorageResult<MessageHeaderPage>,query_message_headers,(const app::MessageActor&,const app::MessageQueryContext&,const app::MessageQueryCursor&,std::size_t))
    UPLOAD_UNUSED(StorageResult<MessageHeader>,message_number_header,(const app::MessageActor&,const app::MessageQueryContext&,std::int64_t))
    UPLOAD_UNUSED(StorageResult<OpenedMessage>,open_query_message,(const app::MessageActor&,const app::MessageQueryContext&,app::MessageId,bool,bool,bool))
    UPLOAD_UNUSED(StorageResult<OpenedMessage>,open_message,(const app::MessageActor&,app::MessageId))
    UPLOAD_UNUSED(StorageResult<OpenedMessage>,quote_source,(const app::MessageActor&,app::MessageId))
    UPLOAD_UNUSED(StorageResult<MessageScanPreference>,message_scan_preference,(const app::MessageActor&,app::LocalEchoId))
    UPLOAD_UNUSED(StorageResult<MessageScanPreference>,set_message_scan_preference,(const app::MessageActor&,app::LocalEchoId,std::int64_t,app::MessageScanColumn,bool))
    UPLOAD_UNUSED(StorageResult<MessageGroupScanPreview>,preview_group_scan_column,(const app::MessageActor&,app::MessageGroupId,app::MessageScanColumn))
    UPLOAD_UNUSED(StorageResult<bool>,toggle_group_scan_column,(const app::MessageActor&,app::Identifier128,const app::MessageGroupScanPreview&))
    UPLOAD_UNUSED(StorageStatus,remove_private_inbox_item,(const app::MessageActor&,app::MessageId))
    UPLOAD_UNUSED(StorageResult<bool>,message_delete_authority,(const app::MessageActor&))
    UPLOAD_UNUSED(StorageResult<bool>,delete_message,(const app::MessageActor&,app::Identifier128,app::MessageId))
    UPLOAD_UNUSED(StorageResult<bool>,message_permanent_state,(const app::MessageActor&,app::MessageId))
    UPLOAD_UNUSED(StorageResult<bool>,set_message_permanent,(const app::MessageActor&,app::Identifier128,app::MessageId,bool,bool))
#undef UPLOAD_UNUSED
};
} // namespace sfx::application
using UploadOutcomeStorage=app::UploadOutcomeStorage;
