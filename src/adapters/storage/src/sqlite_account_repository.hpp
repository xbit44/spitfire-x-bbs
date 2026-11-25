#pragma once

#include <filesystem>
#include <memory>

#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sqlite_internal.hpp"

namespace sfx::adapters::storage::internal {

class SqliteAccountRepository final {
public:
    SqliteAccountRepository();
    ~SqliteAccountRepository();

    SqliteAccountRepository(const SqliteAccountRepository&) = delete;
    SqliteAccountRepository& operator=(const SqliteAccountRepository&) = delete;

    application::StorageStatus start(
        const std::filesystem::path& database_path,
        const SqliteStorageSettings& settings,
        SqliteWriterCoordinator& writer,
        SqlitePagerTelemetry& telemetry);
    application::StorageStatus shutdown();
    SqliteReaderPoolHealth reader_health() const;
#define SFX_QWK_API(type,name,parameters,arguments) type name parameters;
#include "sqlite_qwk_api.inc"
#undef SFX_QWK_API
#define SFX_FILE_API(type,name,parameters,arguments) type name parameters;
#include "sqlite_file_api.inc"
#undef SFX_FILE_API
#define SFX_DOOR_API(type,name,parameters,arguments) type name parameters;
#include "sqlite_door_api.inc"
#undef SFX_DOOR_API
#define SFX_CHAT_API(type,name,parameters,arguments) type name parameters;
#include "sqlite_chat_api.inc"
#undef SFX_CHAT_API
#define SFX_FTN_API(type,name,parameters,arguments) type name parameters;
#include "sqlite_ftn_api.inc"
#undef SFX_FTN_API
#define SFX_FTN_WORK_API(type,name,parameters,arguments) type name parameters;
#include "sqlite_ftn_work_api.inc"
#undef SFX_FTN_WORK_API
    application::StorageResult<std::vector<application::MessageGroup>> message_groups(const application::MessageActor&);
    application::StorageResult<application::MessageConfigurationSnapshot> message_configuration(const application::MessageConfigurationActor& a, std::optional<application::MessageGroupId>, const application::MessageCatalogCursor&);
    application::StorageResult<application::MessageConfigurationResult> change_message_configuration(const application::MessageConfigurationActor& a, const application::MessageConfigurationChange& c);
    application::StorageResult<application::MessageRecipientPage> resolve_message_recipient(const application::MessageActor& a, std::string_view q, const std::optional<application::MessageRecipient>& p, bool name_only);
    application::StorageResult<application::MessageTarget> system_sysop_target(const application::MessageActor& a);
    application::StorageResult<std::optional<application::MessageArea>> saved_message_area(const application::MessageActor&);
    application::StorageStatus remember_message_area(const application::MessageActor&,application::LocalEchoId);
    application::StorageResult<application::MessageArea> message_area(const application::MessageActor& a,application::LocalEchoId e);
    application::StorageResult<application::MessageCallerStatistics> message_caller_statistics(const application::MessageActor& a,std::optional<application::LocalEchoId> e);
    application::StorageResult<application::MessageArrivalPage> message_arrival_page(const application::MessageActor& a,std::int64_t after,std::size_t limit);
    application::StorageResult<bool> message_arrival_eligible(const application::MessageActor& a,application::MessageId id);
    application::StorageResult<std::optional<std::string>> message_arrival_sender(const application::MessageActor&,application::MessageId);
    application::StorageResult<bool> has_addressed_unread(const application::MessageActor& a,bool personal);
    application::StorageResult<application::MessageLoginScanSummary> login_message_scan_summary(const application::MessageActor&,bool);
    application::StorageResult<application::MessageCommitOutcome> message_commit_outcome(const application::CommitMessageRequest&);
    application::StorageResult<application::MessageCatalogPage<application::MessageGroup>> message_group_page(const application::MessageActor&,const application::MessageCatalogCursor&);
    application::StorageResult<application::MessageCatalogPage<application::LocalEcho>> local_echo_page(const application::MessageActor&,application::MessageGroupId,const application::MessageCatalogCursor&);
    application::StorageResult<application::MessageQueryContext> begin_message_query(const application::MessageActor&,const application::MessageQueryOptions&);
    application::StorageResult<application::MessageHeader> message_number_header(const application::MessageActor&,const application::MessageQueryContext&,std::int64_t);
    application::StorageResult<application::OpenedMessage> open_query_message(const application::MessageActor&,const application::MessageQueryContext&,application::MessageId,bool,bool,bool);
    application::StorageResult<application::MessageGroupScanPreview> preview_group_scan_column(const application::MessageActor&,application::MessageGroupId,application::MessageScanColumn);
    application::StorageResult<bool> toggle_group_scan_column(const application::MessageActor&,application::Identifier128,const application::MessageGroupScanPreview&);
    application::StorageResult<application::MessageHeaderPage> query_message_headers(const application::MessageActor&,const application::MessageQueryContext&,const application::MessageQueryCursor&,std::size_t,bool listing=false);
    application::StorageStatus set_message_list_deleted(const application::MessageActor&,const application::MessageQueryContext&,
        application::MessageId,bool expected_deleted,bool deleted,application::MessageListDeleteScope expected_scope);
    application::StorageResult<std::vector<application::LocalEcho>> local_echoes(const application::MessageActor&, application::MessageGroupId);
    application::StorageResult<application::ComposeContext> begin_message(const application::MessageActor&, const application::MessageTarget&, std::optional<application::MessageId>);
    application::StorageResult<application::CommittedMessage> commit_message(const application::CommitMessageRequest&);
    application::StorageResult<application::CommittedMessage> reconcile_message(const application::CommitMessageRequest&);
    application::StorageResult<application::OpenedMessage> open_message(const application::MessageActor&, application::MessageId);
    application::StorageResult<application::OpenedMessage> quote_source(const application::MessageActor&, application::MessageId);
    application::StorageResult<application::MessageScanPreference> message_scan_preference(const application::MessageActor&, application::LocalEchoId);
    application::StorageResult<application::MessageScanPreference> set_message_scan_preference(const application::MessageActor&, application::LocalEchoId, std::int64_t, application::MessageScanColumn, bool);
    application::StorageStatus remove_private_inbox_item(const application::MessageActor&, application::MessageId);
    application::StorageResult<bool> message_delete_authority(const application::MessageActor&);
    application::StorageResult<bool> delete_message(const application::MessageActor&, application::Identifier128, application::MessageId);
    application::StorageResult<bool> message_permanent_state(const application::MessageActor&, application::MessageId);
    application::StorageResult<bool> set_message_permanent(const application::MessageActor&, application::Identifier128, application::MessageId, bool, bool);
#define SFX_MAINTENANCE_API(type,name,parameters,arguments) type name parameters;
#include "sqlite_message_maintenance_api.inc"
#undef SFX_MAINTENANCE_API
    application::StorageResult<application::OtherBoardPage> other_boards(std::int64_t after_id);
    application::StorageResult<application::OpeningDateRecord> opening_date();
    application::StorageStatus change_classic_information(const application::ClassicInformationChange&,const application::ClassicInformationActor&);
    application::StorageResult<application::SystemInformationCounters> system_information_counters(std::int64_t day_begin,std::int64_t day_end);

    application::StorageResult<application::FirstRunState> first_run_state();
    application::MutationResult stage_initial_sysop(const application::StageInitialSysopRequest& request);
    application::MutationResult complete_first_run(const application::CompleteFirstRunRequest& request);
    application::MutationResult create_account(const application::CreateAccountRequest& request);
    application::MutationResult commit_registration(
        const application::RegistrationCommitRequest& request);
    application::StorageResult<application::CallerNameResolution> classify_caller_name(
        std::string_view submitted_name);
    application::MutationResult commit_authenticated_session(
        const application::AuthenticatedSessionCommitRequest& request);
    application::MutationResult rename_account(const application::RenameAccountRequest& request);
    application::MutationResult update_profile(const application::UpdateProfileRequest& request);
    application::StorageResult<std::vector<application::CallerDirectoryEntry>> caller_directory(
        const application::CallerDirectoryQuery&);
    application::StorageResult<std::int64_t> caller_day_seconds(application::AccountId,std::string_view);
    application::MutationResult set_purge_allowed(const application::SetPurgeAllowedRequest& request);
    application::MutationResult assign_security_level(const application::AssignAccountSecurityLevelRequest& request);
    application::MutationResult save_account(const application::SaveAccountRequest& request);
    application::MutationResult replace_credential(const application::ReplaceCredentialRequest& request);
    application::MutationResult change_account_state(const application::ChangeAccountStateRequest& request);
    application::MutationResult tombstone_account(const application::TombstoneAccountRequest& request);
    application::StorageStatus publish_security_policy(
        const application::PublishSecurityPolicyRequest& request);
    application::StorageStatus publish_retention_policy(
        const application::PublishRetentionPolicyRequest& request);
    application::MutationResult record_session_started(const application::StartSessionRequest& request);
    application::StorageStatus persist_session_progress(
        const application::PersistSessionProgressRequest& request);
    application::MutationResult finalize_session(const application::FinalizeSessionRequest& request);
    application::StorageResult<std::vector<application::CallerJournalRecord>> pending_caller_journals(bool unfinished, std::size_t limit);
    application::StorageStatus append_caller_activity(application::Identifier128 session,
        const application::CallerActivity&, std::string_view hostname_hint);
    application::StorageStatus mark_caller_recovered(application::Identifier128 session);
    application::StorageResult<std::optional<application::CallerJournalPublication>> caller_publication();
    application::StorageStatus prepare_caller_publication(const application::CallerJournalPublication&);
    application::StorageStatus acknowledge_caller_publication(std::int64_t caller_number);
    application::StorageResult<application::AccountRecord> find_account_by_name(std::string_view candidate);
    application::StorageResult<application::AccountRecord> find_account_by_id(application::AccountId account_id);
    application::StorageResult<std::string> door_user_key(application::AccountId account_id);
    application::StorageResult<application::CredentialEnvelope> load_credential(application::AccountId account_id);
    application::StorageResult<std::vector<application::AccountRecord>> list_accounts(
        const application::AccountListQuery& query);
    application::StorageResult<std::vector<application::AccountRecord>> inactivity_candidates(
        const application::InactivityCandidateQuery& query);
    application::StorageResult<std::vector<application::SessionRecord>> session_history(
        const application::HistoryQuery& query);
    application::StorageResult<std::vector<application::AuditRecord>> audit_history(
        const application::AuditHistoryQuery& query);
    application::StorageResult<std::vector<application::SecurityLevelRecord>> security_levels();
    application::StorageResult<application::SecurityLevelRecord> security_level(std::uint16_t level);
    application::StorageResult<application::SecurityPolicyRecord> security_policy();
    application::StorageResult<application::RetentionPolicyRecord> retention_policy();

    application::MutationResult replace_account_password(const application::ReplaceAccountPasswordRequest& request);
    application::MutationResult rehash_account_credential(const application::RehashAccountCredentialRequest& request);
    application::MutationResult issue_temporary_password(const application::IssueTemporaryPasswordRequest& request);
    application::StorageStatus transition_temporary_password(const application::TransitionTemporaryPasswordRequest& request);
    application::MutationResult complete_temporary_password(const application::CompleteTemporaryPasswordRequest& request);
    application::StorageResult<application::TemporaryPasswordRecord> temporary_password(application::AccountId account_id);
    application::StorageStatus replace_system_password(const application::ReplaceSystemPasswordRequest& request);
    application::StorageStatus rehash_system_credential(const application::RehashSystemCredentialRequest& request);
    application::StorageResult<application::SystemCredentialRecord> system_password();
    application::StorageStatus record_successful_login_ip(const application::RecordSuccessfulLoginIpRequest& request);
    application::StorageResult<std::vector<application::SuccessfulLoginIpRecord>> successful_login_ips(application::AccountId account_id);
    application::StorageStatus store_rate_limit_snapshot(const application::RateLimitSnapshot& snapshot);
    application::StorageStatus remove_rate_limit_snapshot(const application::RateLimitKey& key);
    application::StorageStatus clear_rate_limit(const application::ClearRateLimitRequest& request);
    application::StorageResult<std::vector<application::RateLimitSnapshot>> active_rate_limit_snapshots(std::int64_t now_utc_seconds, std::size_t limit);
    application::StorageResult<std::vector<application::AuthenticationAuditRecord>> authentication_audit_history(const application::AuthenticationAuditQuery& query);
    application::StorageResult<application::TransferPreferenceRecord> transfer_preference(application::AccountId account_id);
    application::MutationResult update_transfer_preference(const application::UpdateTransferPreferenceRequest& request);
    application::StorageResult<application::MenuPreferenceRecord> menu_preferences(application::AccountId account_id);
    application::MutationResult update_menu_preferences(const application::UpdateMenuPreferenceRequest& request);
    application::StorageStatus enqueue_failed_login_notice(const application::EnqueueFailedLoginNoticeRequest& request);
    application::StorageResult<application::FailedLoginNoticeBatch> claim_failed_login_notices(const application::ClaimFailedLoginNoticesRequest& request);
    application::StorageStatus acknowledge_failed_login_notices(const application::AcknowledgeFailedLoginNoticesRequest& request);
    application::StorageResult<std::size_t> recover_abandoned_failed_login_claims(const application::RecoverFailedLoginClaimsRequest& request);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sfx::adapters::storage::internal
