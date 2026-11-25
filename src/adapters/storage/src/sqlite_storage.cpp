#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sqlite_ftn_serial_state.hpp"

#include "sqlite_internal.hpp"
#include "sqlite_account_repository.hpp"
#include "sqlite_sfx_baseline.hpp"
#include "sqlite_statement.hpp"
#include <sodium.h>
#include <algorithm>
#include <array>
#include <map>
#include <cstring>

#include <chrono>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>

namespace sfx::adapters::storage {
namespace {

#include "sqlite_compaction_vfs.inc"
#include "sqlite_compaction_fingerprint.inc"

std::int64_t system_utc_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool sidecar_is_absent_or_empty(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return !error;
    }
    return std::filesystem::file_size(path, error) == 0 && !error;
}

}  // namespace

class SqliteStorage::Impl final {
public:
    Impl(
        SqliteStorageSettings settings,
        const IStoragePathPolicy& path_policy,
        std::vector<SqliteMigration> migrations)
        : settings_(std::move(settings)), path_policy_(path_policy),
          migrations_(std::move(migrations)) {
        if (!settings_.utc_now) {
            settings_.utc_now = system_utc_seconds;
        }
    }

    application::StorageStatus start() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (started_) {
            return {application::StorageErrorCode::internal, "start SQLite storage twice"};
        }
        preflight_failure_.reset();
        const StorageLocationAssessment assessment = path_policy_.assess(settings_.data_directory);
        if (!assessment.status) {
            return assessment.status;
        }
        data_directory_ = assessment.canonical_directory;
        const auto layout = validate_layout(data_directory_);
        if (!layout) return layout;
        database_path_ = data_directory_ / database_leaf();
        std::error_code file_error;
        const bool database_exists = std::filesystem::exists(database_path_, file_error);
        if (file_error) {
            return {application::StorageErrorCode::io, "inspect SFX database path"};
        }
        if (!database_exists) {
            if (settings_.bootstrap_boundary_hook) {
                const application::StorageStatus hook =
                    settings_.bootstrap_boundary_hook("before-sfx-database-create");
                if (!hook) return hook;
            }
            application::StorageStatus status =
                path_policy_.create_database_file_exclusive(database_path_);
            if (!status) return status;
            if (settings_.bootstrap_boundary_hook) {
                const application::StorageStatus hook =
                    settings_.bootstrap_boundary_hook("after-sfx-database-create");
                if (!hook) return hook;
            }
            status = path_policy_.protect_and_verify_database_file(database_path_);
            if (!status) return status;
            if (settings_.bootstrap_boundary_hook) {
                const application::StorageStatus hook =
                    settings_.bootstrap_boundary_hook("after-sfx-database-protect");
                if (!hook) return hook;
            }
        } else {
            application::StorageStatus status =
                path_policy_.protect_and_verify_database_file(database_path_);
            if (!status) return status;
            const std::uintmax_t bytes = std::filesystem::file_size(database_path_, file_error);
            if (file_error) return {application::StorageErrorCode::io, "measure SFX database"};
            if (bytes != 0) {
                try {
                    internal::SqliteConnection existing(
                        database_path_, SQLITE_OPEN_READONLY,
                        settings_.busy.sqlite_timeout, false);
                    const bool recognized =
                        internal::validate_sfx_prepublication_database(existing.get()) ||
                        internal::validate_sfx_shipping_database(existing.get(), true);
                    existing.close();
                    if (!recognized) {
                        preflight_failure_ = application::StorageStatus{
                            application::StorageErrorCode::incompatible_schema,
                            "reject unknown SFX database state"};
                        return *preflight_failure_;
                    }
                } catch (...) {
                    preflight_failure_ = internal::status_from_current_exception(
                        "inspect existing SFX database state");
                    return *preflight_failure_;
                }
            }
        }
        telemetry_.begin(settings_.readers.pool_size + 1U);
        application::StorageStatus status = coordinator_.start(
            database_path_, settings_, migrations_, telemetry_);
        if (status) {
            status = account_repository_.start(
                database_path_, settings_, coordinator_, telemetry_);
            if (!status) {
                (void)coordinator_.shutdown();
            }
        }
        started_ = status.succeeded();
        return status;
    }

    application::StorageStatus checkpoint() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!started_) {
            return {application::StorageErrorCode::shutting_down, "checkpoint stopped SQLite storage"};
        }
        return coordinator_.checkpoint(false);
    }

    application::StorageStatus shutdown() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        const application::StorageStatus reader_status = account_repository_.shutdown();
        const application::StorageStatus writer_status = coordinator_.shutdown();
        started_ = false;
        if (preflight_failure_.has_value()) return *preflight_failure_;
        return !reader_status ? reader_status : writer_status;
    }

    application::StorageHealth health() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        application::StorageHealth result = coordinator_.health();
        if (account_repository_.reader_health().failed) {
            result.state = application::StorageRuntimeState::recovery_required;
            result.last_error = application::StorageErrorCode::internal;
            result.last_operation = "SQLite account reader worker failed";
        }
        if (result.state == application::StorageRuntimeState::stopped &&
            preflight_failure_.has_value()) {
            result.state = application::StorageRuntimeState::recovery_required;
            result.last_error = preflight_failure_->code;
            result.last_operation = preflight_failure_->operation;
        }
        return result;
    }
    SqliteReaderPoolHealth reader_pool_health() const { return account_repository_.reader_health(); }
    SqlitePagerCounterSnapshot pager_counters() const { return telemetry_.snapshot(); }

    application::StorageResult<std::vector<application::MessageGroup>> message_groups(const application::MessageActor& a) { return account_repository_.message_groups(a); }
    application::StorageResult<application::MessageCommitOutcome> message_commit_outcome(const application::CommitMessageRequest& r) { return account_repository_.message_commit_outcome(r); }
    application::StorageResult<application::MessageConfigurationSnapshot> message_configuration(const application::MessageConfigurationActor& a, std::optional<application::MessageGroupId> g, const application::MessageCatalogCursor& c) { return account_repository_.message_configuration(a,g,c); }
    application::StorageResult<application::MessageConfigurationResult> change_message_configuration(const application::MessageConfigurationActor& a, const application::MessageConfigurationChange& c) { return account_repository_.change_message_configuration(a,c); }
    application::StorageResult<application::MessageRecipientPage> resolve_message_recipient(const application::MessageActor& a, std::string_view q, const std::optional<application::MessageRecipient>& p, bool name_only) { return account_repository_.resolve_message_recipient(a,q,p,name_only); }
    application::StorageResult<application::MessageTarget> system_sysop_target(const application::MessageActor& a) { return account_repository_.system_sysop_target(a); }
    application::StorageResult<std::optional<application::MessageArea>> saved_message_area(const application::MessageActor& a){return account_repository_.saved_message_area(a);}
    application::StorageStatus remember_message_area(const application::MessageActor& a,application::LocalEchoId e){return account_repository_.remember_message_area(a,e);}
    application::StorageResult<application::MessageArea> message_area(const application::MessageActor& a,application::LocalEchoId e) { return account_repository_.message_area(a,e); }
    application::StorageResult<application::MessageCallerStatistics> message_caller_statistics(const application::MessageActor& a,std::optional<application::LocalEchoId> e) { return account_repository_.message_caller_statistics(a,e); }
    application::StorageResult<application::MessageArrivalPage> message_arrival_page(const application::MessageActor& a,std::int64_t after,std::size_t limit) { return account_repository_.message_arrival_page(a,after,limit); }
    application::StorageResult<bool> message_arrival_eligible(const application::MessageActor& a,application::MessageId id) { return account_repository_.message_arrival_eligible(a,id); }
    application::StorageResult<std::optional<std::string>> message_arrival_sender(const application::MessageActor& a,application::MessageId id) { return account_repository_.message_arrival_sender(a,id); }
    application::StorageResult<bool> has_addressed_unread(const application::MessageActor& a,bool personal) { return account_repository_.has_addressed_unread(a,personal); }
    application::StorageResult<application::MessageLoginScanSummary> login_message_scan_summary(const application::MessageActor& a,bool include_authored_echoes) { return account_repository_.login_message_scan_summary(a,include_authored_echoes); }
    application::StorageResult<application::MessageCatalogPage<application::MessageGroup>> message_group_page(const application::MessageActor& a, const application::MessageCatalogCursor& c) { return account_repository_.message_group_page(a,c); }
    application::StorageResult<application::MessageCatalogPage<application::LocalEcho>> local_echo_page(const application::MessageActor& a, application::MessageGroupId g, const application::MessageCatalogCursor& c) { return account_repository_.local_echo_page(a,g,c); }
    application::StorageResult<application::MessageQueryContext> begin_message_query(const application::MessageActor& a, const application::MessageQueryOptions& o) { return account_repository_.begin_message_query(a,o); }
    application::StorageResult<application::MessageHeader> message_number_header(const application::MessageActor& a,const application::MessageQueryContext& c,std::int64_t n) { return account_repository_.message_number_header(a,c,n); }
    application::StorageResult<application::OpenedMessage> open_query_message(const application::MessageActor& a,const application::MessageQueryContext& c,application::MessageId id,bool traversal,bool history,bool preview) { return account_repository_.open_query_message(a,c,id,traversal,history,preview); }
    application::StorageResult<application::MessageGroupScanPreview> preview_group_scan_column(const application::MessageActor& a,application::MessageGroupId g,application::MessageScanColumn c) { return account_repository_.preview_group_scan_column(a,g,c); }
    application::StorageResult<bool> toggle_group_scan_column(const application::MessageActor& a,application::Identifier128 op,const application::MessageGroupScanPreview& p) { return account_repository_.toggle_group_scan_column(a,op,p); }
    application::StorageResult<application::MessageHeaderPage> query_message_headers(const application::MessageActor& a, const application::MessageQueryContext& c, const application::MessageQueryCursor& p, std::size_t n) { return account_repository_.query_message_headers(a,c,p,n); }
    application::StorageResult<application::MessageHeaderPage> query_message_list_headers(const application::MessageActor& a, const application::MessageQueryContext& c, const application::MessageQueryCursor& p, std::size_t n) {
        return account_repository_.query_message_headers(a,c,p,n,true);
    }
    application::StorageStatus set_message_list_deleted(const application::MessageActor& actor,const application::MessageQueryContext& query,
        application::MessageId message,bool expected,bool deleted,application::MessageListDeleteScope scope) {
        return account_repository_.set_message_list_deleted(actor,query,message,expected,deleted,scope);
    }
    application::StorageResult<std::vector<application::LocalEcho>> local_echoes(const application::MessageActor& a, application::MessageGroupId g) { return account_repository_.local_echoes(a,g); }
    application::StorageResult<application::ComposeContext> begin_message(const application::MessageActor& a,const application::MessageTarget& t,std::optional<application::MessageId> p) { return account_repository_.begin_message(a,t,p); }
    application::StorageResult<application::CommittedMessage> commit_message(const application::CommitMessageRequest& r) { return account_repository_.commit_message(r); }
    application::StorageResult<application::CommittedMessage> reconcile_message(const application::CommitMessageRequest& r) { return account_repository_.reconcile_message(r); }
    application::StorageResult<application::OpenedMessage> open_message(const application::MessageActor& a,application::MessageId m) { return account_repository_.open_message(a,m); }
    application::StorageResult<application::OpenedMessage> quote_source(const application::MessageActor& a,application::MessageId m) { return account_repository_.quote_source(a,m); }
    application::StorageResult<application::MessageScanPreference> message_scan_preference(const application::MessageActor& a,application::LocalEchoId e) { return account_repository_.message_scan_preference(a,e); }
    application::StorageResult<application::MessageScanPreference> set_message_scan_preference(const application::MessageActor& a,application::LocalEchoId e,std::int64_t r,application::MessageScanColumn c,bool on) { return account_repository_.set_message_scan_preference(a,e,r,c,on); }
    application::StorageStatus remove_private_inbox_item(const application::MessageActor& a,application::MessageId m) { return account_repository_.remove_private_inbox_item(a,m); }
    application::StorageResult<bool> message_delete_authority(const application::MessageActor& a) {return account_repository_.message_delete_authority(a);}
    application::StorageResult<bool> delete_message(const application::MessageActor& a,application::Identifier128 o,application::MessageId m) {return account_repository_.delete_message(a,o,m);}
    application::StorageResult<bool> message_permanent_state(const application::MessageActor& a,application::MessageId m) {return account_repository_.message_permanent_state(a,m);}
    application::StorageResult<bool> set_message_permanent(const application::MessageActor& a,application::Identifier128 o,application::MessageId m,bool expected,bool permanent) {return account_repository_.set_message_permanent(a,o,m,expected,permanent);}
#define SFX_MAINTENANCE_API(type,name,parameters,arguments) type name parameters {return account_repository_.name arguments;}
#include "sqlite_message_maintenance_api.inc"
#undef SFX_MAINTENANCE_API
#define SFX_QWK_API(type,name,parameters,arguments) type name parameters {return account_repository_.name arguments;}
#include "sqlite_qwk_api.inc"
#undef SFX_QWK_API
#define SFX_FILE_API(type,name,parameters,arguments) type name parameters {return account_repository_.name arguments;}
#include "sqlite_file_api.inc"
#undef SFX_FILE_API
#define SFX_DOOR_API(type,name,parameters,arguments) type name parameters {return account_repository_.name arguments;}
#include "sqlite_door_api.inc"
#undef SFX_DOOR_API
#define SFX_CHAT_API(type,name,parameters,arguments) type name parameters {return account_repository_.name arguments;}
#include "sqlite_chat_api.inc"
#undef SFX_CHAT_API
#define SFX_FTN_API(type,name,parameters,arguments) type name parameters {return account_repository_.name arguments;}
#include "sqlite_ftn_api.inc"
#undef SFX_FTN_API
#define SFX_FTN_WORK_API(type,name,parameters,arguments) type name parameters {return account_repository_.name arguments;}
#include "sqlite_ftn_work_api.inc"
#undef SFX_FTN_WORK_API
    application::StorageResult<application::FirstRunState> first_run_state() { return account_repository_.first_run_state(); }
    application::MutationResult stage_initial_sysop(const application::StageInitialSysopRequest& value) { return account_repository_.stage_initial_sysop(value); }
    application::MutationResult complete_first_run(const application::CompleteFirstRunRequest& value) { return account_repository_.complete_first_run(value); }
    application::MutationResult create_account(const application::CreateAccountRequest& value) { return account_repository_.create_account(value); }
    application::MutationResult commit_registration(const application::RegistrationCommitRequest& value) { return account_repository_.commit_registration(value); }
    application::StorageResult<application::CallerNameResolution> classify_caller_name(std::string_view value) { return account_repository_.classify_caller_name(value); }
    application::MutationResult commit_authenticated_session(const application::AuthenticatedSessionCommitRequest& value) { return account_repository_.commit_authenticated_session(value); }
    application::MutationResult rename_account(const application::RenameAccountRequest& value) { return account_repository_.rename_account(value); }
    application::MutationResult update_profile(const application::UpdateProfileRequest& value) { return account_repository_.update_profile(value); }
    application::MutationResult set_purge_allowed(const application::SetPurgeAllowedRequest& value) { return account_repository_.set_purge_allowed(value); }
    application::MutationResult assign_security_level(const application::AssignAccountSecurityLevelRequest& value) { return account_repository_.assign_security_level(value); }
    application::MutationResult save_account(const application::SaveAccountRequest& value) { return account_repository_.save_account(value); }
    application::MutationResult replace_credential(const application::ReplaceCredentialRequest& value) { return account_repository_.replace_credential(value); }
    application::MutationResult change_account_state(const application::ChangeAccountStateRequest& value) { return account_repository_.change_account_state(value); }
    application::MutationResult tombstone_account(const application::TombstoneAccountRequest& value) { return account_repository_.tombstone_account(value); }
    application::StorageStatus publish_security_policy(const application::PublishSecurityPolicyRequest& value) { return account_repository_.publish_security_policy(value); }
    application::StorageStatus publish_retention_policy(const application::PublishRetentionPolicyRequest& value) { return account_repository_.publish_retention_policy(value); }
    application::MutationResult record_session_started(const application::StartSessionRequest& value) { return account_repository_.record_session_started(value); }
    application::StorageResult<std::vector<application::CallerJournalRecord>> pending_caller_journals(bool unfinished, std::size_t limit) { return account_repository_.pending_caller_journals(unfinished, limit); }
    application::StorageStatus append_caller_activity(application::Identifier128 session, const application::CallerActivity& activity, std::string_view hostname) { return account_repository_.append_caller_activity(session, activity, hostname); }
    application::StorageStatus mark_caller_recovered(application::Identifier128 session) { return account_repository_.mark_caller_recovered(session); }
    application::StorageResult<std::optional<application::CallerJournalPublication>> caller_publication() { return account_repository_.caller_publication(); }
    application::StorageStatus prepare_caller_publication(const application::CallerJournalPublication& value) { return account_repository_.prepare_caller_publication(value); }
    application::StorageStatus acknowledge_caller_publication(std::int64_t number) { return account_repository_.acknowledge_caller_publication(number); }
    application::StorageStatus persist_session_progress(const application::PersistSessionProgressRequest& value) { return account_repository_.persist_session_progress(value); }
    application::MutationResult finalize_session(const application::FinalizeSessionRequest& value) { return account_repository_.finalize_session(value); }
    application::StorageResult<application::AccountRecord> find_account_by_name(std::string_view value) { return account_repository_.find_account_by_name(value); }
    application::StorageResult<application::AccountRecord> find_account_by_id(application::AccountId value) { return account_repository_.find_account_by_id(value); }
    application::StorageResult<std::string> door_user_key(application::AccountId value) { return account_repository_.door_user_key(value); }
    application::StorageResult<application::CredentialEnvelope> load_credential(application::AccountId value) { return account_repository_.load_credential(value); }
    application::StorageResult<std::vector<application::AccountRecord>> list_accounts(const application::AccountListQuery& value) { return account_repository_.list_accounts(value); }
    application::StorageResult<std::vector<application::CallerDirectoryEntry>> caller_directory(const application::CallerDirectoryQuery& value) { return account_repository_.caller_directory(value); }
    application::StorageResult<std::int64_t> caller_day_seconds(application::AccountId account,std::string_view date) { return account_repository_.caller_day_seconds(account,date); }
    application::StorageResult<std::vector<application::AccountRecord>> inactivity_candidates(const application::InactivityCandidateQuery& value) { return account_repository_.inactivity_candidates(value); }
    application::StorageResult<std::vector<application::SessionRecord>> session_history(const application::HistoryQuery& value) { return account_repository_.session_history(value); }
    application::StorageResult<std::vector<application::AuditRecord>> audit_history(const application::AuditHistoryQuery& value) { return account_repository_.audit_history(value); }
    application::StorageResult<std::vector<application::SecurityLevelRecord>> security_levels() { return account_repository_.security_levels(); }
    application::StorageResult<application::SecurityLevelRecord> security_level(std::uint16_t value) { return account_repository_.security_level(value); }
    application::StorageResult<application::SecurityPolicyRecord> security_policy() { return account_repository_.security_policy(); }
    application::StorageResult<application::RetentionPolicyRecord> retention_policy() { return account_repository_.retention_policy(); }
    application::MutationResult replace_account_password(const application::ReplaceAccountPasswordRequest& value) { return account_repository_.replace_account_password(value); }
    application::MutationResult rehash_account_credential(const application::RehashAccountCredentialRequest& value) { return account_repository_.rehash_account_credential(value); }
    application::MutationResult issue_temporary_password(const application::IssueTemporaryPasswordRequest& value) { return account_repository_.issue_temporary_password(value); }
    application::StorageStatus transition_temporary_password(const application::TransitionTemporaryPasswordRequest& value) { return account_repository_.transition_temporary_password(value); }
    application::MutationResult complete_temporary_password(const application::CompleteTemporaryPasswordRequest& value) { return account_repository_.complete_temporary_password(value); }
    application::StorageResult<application::TemporaryPasswordRecord> temporary_password(application::AccountId value) { return account_repository_.temporary_password(value); }
    application::StorageStatus replace_system_password(const application::ReplaceSystemPasswordRequest& value) { return account_repository_.replace_system_password(value); }
    application::StorageStatus rehash_system_credential(const application::RehashSystemCredentialRequest& value) { return account_repository_.rehash_system_credential(value); }
    application::StorageResult<application::SystemCredentialRecord> system_password() { return account_repository_.system_password(); }
    application::StorageStatus record_successful_login_ip(const application::RecordSuccessfulLoginIpRequest& value) { return account_repository_.record_successful_login_ip(value); }
    application::StorageResult<std::vector<application::SuccessfulLoginIpRecord>> successful_login_ips(application::AccountId value) { return account_repository_.successful_login_ips(value); }
    application::StorageStatus store_rate_limit_snapshot(const application::RateLimitSnapshot& value) { return account_repository_.store_rate_limit_snapshot(value); }
    application::StorageStatus remove_rate_limit_snapshot(const application::RateLimitKey& value) { return account_repository_.remove_rate_limit_snapshot(value); }
    application::StorageStatus clear_rate_limit(const application::ClearRateLimitRequest& value) { return account_repository_.clear_rate_limit(value); }
    application::StorageResult<std::vector<application::RateLimitSnapshot>> active_rate_limit_snapshots(std::int64_t now, std::size_t limit) { return account_repository_.active_rate_limit_snapshots(now, limit); }
    application::StorageResult<std::vector<application::AuthenticationAuditRecord>> authentication_audit_history(const application::AuthenticationAuditQuery& value) { return account_repository_.authentication_audit_history(value); }
    application::StorageResult<application::TransferPreferenceRecord> transfer_preference(application::AccountId value) { return account_repository_.transfer_preference(value); }
    application::MutationResult update_transfer_preference(const application::UpdateTransferPreferenceRequest& value) { return account_repository_.update_transfer_preference(value); }
    application::StorageResult<application::MenuPreferenceRecord> menu_preferences(application::AccountId value) { return account_repository_.menu_preferences(value); }
    application::MutationResult update_menu_preferences(const application::UpdateMenuPreferenceRequest& value) { return account_repository_.update_menu_preferences(value); }
    application::StorageStatus enqueue_failed_login_notice(const application::EnqueueFailedLoginNoticeRequest& value) { return account_repository_.enqueue_failed_login_notice(value); }
    application::StorageResult<application::FailedLoginNoticeBatch> claim_failed_login_notices(const application::ClaimFailedLoginNoticesRequest& value) { return account_repository_.claim_failed_login_notices(value); }
    application::StorageStatus acknowledge_failed_login_notices(const application::AcknowledgeFailedLoginNoticesRequest& value) { return account_repository_.acknowledge_failed_login_notices(value); }
    application::StorageResult<std::size_t> recover_abandoned_failed_login_claims(const application::RecoverFailedLoginClaimsRequest& value) { return account_repository_.recover_abandoned_failed_login_claims(value); }

    application::StorageResult<application::OtherBoardPage> other_boards(std::int64_t after) { return account_repository_.other_boards(after); }
    application::StorageResult<application::OpeningDateRecord> opening_date() { return account_repository_.opening_date(); }
    application::StorageStatus change_classic_information(const application::ClassicInformationChange& change,const application::ClassicInformationActor& actor) { return account_repository_.change_classic_information(change,actor); }
    application::StorageResult<application::SystemInformationCounters> system_information_counters(std::int64_t begin,std::int64_t end) { return account_repository_.system_information_counters(begin,end); }
    SqliteBackupResult create_backup(const std::filesystem::path& destination_file) {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        SqliteBackupResult result;
        if (!started_) {
            result.status = {application::StorageErrorCode::shutting_down, "backup stopped SQLite storage"};
            return result;
        }
        std::filesystem::path validated;
        result.status = validate_external_file_location(destination_file, &validated);
        if (!result.status) {
            return result;
        }
        if (validated == database_path_) {
            result.status = {application::StorageErrorCode::invalid_location, "reject backup over active database"};
            return result;
        }
        const internal::InternalBackupResult backup = coordinator_.backup_to(validated);
        result.status = backup.status;
        result.backup_file = validated;
        result.database_bytes = backup.bytes;
        result.schema_version = backup.schema_version;
        return result;
    }

    application::StorageStatus validate_backup(const std::filesystem::path& backup_file) const {
        std::filesystem::path validated;
        const application::StorageStatus location_status = validate_external_file_location(backup_file, &validated);
        if (!location_status) {
            return location_status;
        }
        return internal::verify_database_file(validated, settings_.busy.sqlite_timeout, migrations_);
    }
    application::StorageResult<std::int32_t> verified_schema_version(const std::filesystem::path& file) const {
        std::filesystem::path validated;const auto location=validate_external_file_location(file,&validated);if(!location)return {location,{}};
        std::int32_t version{};const auto verified=internal::verify_database_file(validated,settings_.busy.sqlite_timeout,migrations_,&version);
        return verified?application::StorageResult<std::int32_t>{verified,version}:application::StorageResult<std::int32_t>{verified,{}};
    }
    application::StorageResult<application::FtnSerialState> verified_ftn_serial_history(const std::filesystem::path& file) const {
        const auto version=verified_schema_version(file);if(!version||!version.value)return {version.status,{}};
        if(*version.value<22)return {application::StorageStatus::success(),application::FtnSerialState{0,false,{},true}};
        try{internal::SqliteConnection db(file,SQLITE_OPEN_READONLY,settings_.busy.sqlite_timeout,false);const auto state=internal::read_ftn_serial_state(db.get());return {application::StorageStatus::success(),state};}
        catch(...){return {internal::status_from_current_exception("verify FTN serial history without mutation"),{}};}
    }

    SqliteCompactionResult compact_offline(const std::filesystem::path& private_folder,std::function<bool()> cancelled){
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);SqliteCompactionResult result;
        if(started_ || coordinator_.health().state!=application::StorageRuntimeState::stopped){result.status={application::StorageErrorCode::shutting_down,"compaction requires fully stopped storage"};return result;}
        const auto temporary=path_policy_.assess(private_folder),data=path_policy_.assess(settings_.data_directory);
        if(!temporary.status){result.status=temporary.status;return result;}if(!data.status){result.status=data.status;return result;}
        if(temporary.canonical_directory==data.canonical_directory || temporary.canonical_directory.root_name()!=data.canonical_directory.root_name()){
            result.status={application::StorageErrorCode::invalid_location,"private compaction temporary folder must be separate on the same local volume"};return result;}
        const auto layout=validate_layout(data.canonical_directory);
        if(!layout){result.status=layout;return result;}
        const auto file=data.canonical_directory/database_leaf();
        std::array<std::uint8_t,32> before{};bool fingerprint_ready=false;
        try{
            result.bytes_before=std::filesystem::file_size(file);
            const auto space=std::filesystem::space(temporary.canonical_directory);
            if(result.bytes_before>space.available/2){result.status={application::StorageErrorCode::full,"compaction needs temporary/WAL space; no compaction begun"};return result;}
            PrivateCompactionVfs vfs(temporary.canonical_directory,path_policy_);
            internal::SqliteConnection connection(file,SQLITE_OPEN_READWRITE,settings_.busy.sqlite_timeout,true,vfs.name());
            {
                CompactionBudget budget(connection.get(),cancelled);budget.poll();
                if(!internal::validate_sfx_shipping_database(connection.get(),true))throw internal::StorageFailure(application::StorageErrorCode::corrupt,"compaction current database verification",SQLITE_CORRUPT);
                before=database_logical_fingerprint(connection.get(),&budget);fingerprint_ready=true;budget.poll();
                connection.exec("VACUUM main","manual transactional physical compaction");result.physical_compaction_committed=true;budget.poll();
                if(!internal::validate_sfx_shipping_database(connection.get(),true) || database_logical_fingerprint(connection.get(),&budget)!=before)
                    throw internal::StorageFailure(application::StorageErrorCode::corrupt,"compaction logical verification failed; use supported restore",SQLITE_CORRUPT);
                result.logical_content_verified=true;
            }
            int log{},done{};const auto checkpoint=sqlite3_wal_checkpoint_v2(connection.get(),"main",SQLITE_CHECKPOINT_TRUNCATE,&log,&done);
            if(checkpoint!=SQLITE_OK)throw internal::StorageFailure(internal::map_sqlite_error(checkpoint),"compaction final WAL checkpoint",checkpoint);
            connection.close();result.private_temporary_files=vfs.temporary_files();result.bytes_after=std::filesystem::file_size(file);
            result.status=application::StorageStatus::success();return result;
        }catch(...){
            result.status=internal::status_from_current_exception("manual compaction failed; no success claimed");
            // Do not repair, restore automatically or optimistically claim
            // rollback. Independently reconcile the actual current source.
            try{
                internal::SqliteConnection current(file,SQLITE_OPEN_READONLY,settings_.busy.sqlite_timeout,false);
                result.logical_content_verified=fingerprint_ready && internal::validate_sfx_shipping_database(current.get(),true) && database_logical_fingerprint(current.get())==before;
                result.bytes_after=std::filesystem::file_size(file);
            }catch(...){result.logical_content_verified=false;}
            return result;
        }
    }

    application::StorageStatus restore_backup(const std::filesystem::path& backup_file) {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (started_) {
            return {application::StorageErrorCode::shutting_down, "restore while SQLite storage is active"};
        }
        const application::StorageRuntimeState state = coordinator_.health().state;
        if (state != application::StorageRuntimeState::stopped) {
            (void)coordinator_.shutdown();
            if (coordinator_.health().state != application::StorageRuntimeState::recovery_required) {
                return {
                    application::StorageErrorCode::shutting_down,
                    "restore while SQLite writer has not stopped"};
            }
        }

        const StorageLocationAssessment database_directory = path_policy_.assess(settings_.data_directory);
        if (!database_directory.status) {
            return database_directory.status;
        }
        data_directory_ = database_directory.canonical_directory;
        const auto layout = validate_layout(data_directory_);
        if (!layout) return layout;

        std::filesystem::path validated_backup;
        application::StorageStatus status = validate_external_file_location(backup_file, &validated_backup);
        if (!status) {
            return status;
        }
        database_path_ = data_directory_ / database_leaf();
        status = internal::verify_database_file(validated_backup, settings_.busy.sqlite_timeout, migrations_);
        if (!status) {
            return status;
        }

        const std::filesystem::path wal_file(database_path_.string() + "-wal");
        const std::filesystem::path shm_file(database_path_.string() + "-shm");
        if (!sidecar_is_absent_or_empty(wal_file) || !sidecar_is_absent_or_empty(shm_file)) {
            return {application::StorageErrorCode::io, "reject restore with live SQLite sidecars"};
        }

        const std::filesystem::path staging_file(database_path_.string() + ".restore-staging");
        const std::filesystem::path previous_file(database_path_.string() + ".pre-restore");
        std::error_code file_error;
        if (std::filesystem::exists(staging_file, file_error) || file_error ||
            std::filesystem::exists(previous_file, file_error) || file_error) {
            return {application::StorageErrorCode::io, "prepare recoverable SQLite restore"};
        }

        try {
            internal::SqliteConnection source(
                validated_backup,
                SQLITE_OPEN_READONLY,
                settings_.busy.sqlite_timeout,
                false);
            const internal::InternalBackupResult staged =
                internal::backup_database(source.get(), staging_file, settings_.busy.sqlite_timeout, migrations_);
            if (!staged.status) {
                return staged.status;
            }
            source.close();

            if (std::filesystem::exists(wal_file, file_error)) {
                std::filesystem::remove(wal_file, file_error);
            }
            if (file_error) {
                return {application::StorageErrorCode::io, "remove empty SQLite WAL before restore"};
            }
            if (std::filesystem::exists(shm_file, file_error)) {
                std::filesystem::remove(shm_file, file_error);
            }
            if (file_error) {
                return {application::StorageErrorCode::io, "remove empty SQLite SHM before restore"};
            }

            const bool had_database = std::filesystem::exists(database_path_, file_error);
            if (file_error) {
                return {application::StorageErrorCode::io, "inspect active database before restore"};
            }
            if (had_database) {
                std::filesystem::rename(database_path_, previous_file, file_error);
                if (file_error) {
                    return {application::StorageErrorCode::io, "preserve database before restore"};
                }
            }
            std::filesystem::rename(staging_file, database_path_, file_error);
            if (file_error) {
                if (had_database) {
                    std::error_code rollback_error;
                    std::filesystem::rename(previous_file, database_path_, rollback_error);
                }
                return {application::StorageErrorCode::io, "activate restored database"};
            }

            status = internal::verify_database_file(database_path_, settings_.busy.sqlite_timeout, migrations_);
            if (!status && had_database) {
                const std::filesystem::path failed_file(database_path_.string() + ".failed-restore");
                std::error_code rollback_error;
                std::filesystem::rename(database_path_, failed_file, rollback_error);
                if (!rollback_error) {
                    std::filesystem::rename(previous_file, database_path_, rollback_error);
                }
            }
            if (status) {
                const application::StorageStatus reset_status =
                    coordinator_.reset_after_offline_recovery();
                if (!reset_status) {
                    return reset_status;
                }
                preflight_failure_.reset();
            }
            return status;
        } catch (...) {
            std::filesystem::remove(staging_file, file_error);
            return internal::status_from_current_exception("restore SQLite backup");
        }
    }

    std::filesystem::path database_path() const {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!database_path_.empty()) return database_path_;
        return settings_.data_directory / database_leaf();
    }

private:
    const char* database_leaf() const noexcept {
        return settings_.layout_profile == SqliteStorageLayoutProfile::linux_lowercase_v1
            ? "sfx.db" : internal::sfx_baseline_database_filename;
    }

    application::StorageStatus validate_layout(const std::filesystem::path& directory) const {
        if (settings_.layout_profile == SqliteStorageLayoutProfile::canonical) return {};
        if (settings_.layout_profile != SqliteStorageLayoutProfile::linux_lowercase_v1)
            return {application::StorageErrorCode::invalid_argument,
                    "validate SQLite native layout profile"};
        // Inspect actual entry spelling so a Windows adapter test can exercise
        // both profiles without treating sfx.db as its case-insensitive alias.
        // Also refuse orphan legacy sidecars, without creating a parallel DB.
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end;
             !error && it != end; it.increment(error)) {
            const auto name = it->path().filename().u8string();
            if (name == "SFX.DB" || name.compare(0, 7, "SFX.DB-") == 0 ||
                name.compare(0, 7, "SFX.DB.") == 0)
                return {application::StorageErrorCode::incompatible_schema,
                        "linux_lowercase_layout_legacy_database_refused"};
        }
        if (error) return {application::StorageErrorCode::io,
                          "inspect SQLite native layout directory"};
        return {};
    }

    application::StorageStatus validate_external_file_location(
        const std::filesystem::path& file,
        std::filesystem::path* const validated) const {
        if (file.filename().empty() || !file.is_absolute()) {
            return {application::StorageErrorCode::invalid_location, "validate SQLite maintenance file path"};
        }
        const StorageLocationAssessment assessment = path_policy_.assess(file.parent_path());
        if (!assessment.status) {
            return assessment.status;
        }
        *validated = assessment.canonical_directory / file.filename();
        return application::StorageStatus::success();
    }

    SqliteStorageSettings settings_;
    const IStoragePathPolicy& path_policy_;
    std::vector<SqliteMigration> migrations_;
    mutable std::mutex lifecycle_mutex_;
    std::filesystem::path data_directory_;
    std::filesystem::path database_path_;
    internal::SqlitePagerTelemetry telemetry_;
    internal::SqliteWriterCoordinator coordinator_;
    internal::SqliteAccountRepository account_repository_;
    std::optional<application::StorageStatus> preflight_failure_;
    bool started_{false};
};

SqliteStorage::SqliteStorage(
    SqliteStorageSettings settings,
    const IStoragePathPolicy& path_policy,
    std::vector<SqliteMigration> migrations)
    : impl_(std::make_unique<Impl>(
          std::move(settings), path_policy, std::move(migrations))) {}

SqliteStorage::~SqliteStorage() = default;

application::StorageStatus SqliteStorage::start() { return impl_->start(); }
application::StorageStatus SqliteStorage::checkpoint() { return impl_->checkpoint(); }
application::StorageStatus SqliteStorage::shutdown() { return impl_->shutdown(); }
application::StorageHealth SqliteStorage::health() const { return impl_->health(); }
SqliteReaderPoolHealth SqliteStorage::reader_pool_health() const { return impl_->reader_pool_health(); }
SqlitePagerCounterSnapshot SqliteStorage::pager_counters() const { return impl_->pager_counters(); }

application::StorageResult<std::vector<application::MessageGroup>> SqliteStorage::message_groups(const application::MessageActor& a) { return impl_->message_groups(a); }
application::StorageResult<application::MessageCommitOutcome> SqliteStorage::message_commit_outcome(const application::CommitMessageRequest& r) { return impl_->message_commit_outcome(r); }
application::StorageResult<application::MessageConfigurationSnapshot> SqliteStorage::message_configuration(const application::MessageConfigurationActor& a, std::optional<application::MessageGroupId> g, const application::MessageCatalogCursor& c) { return impl_->message_configuration(a,g,c); }
application::StorageResult<application::MessageConfigurationResult> SqliteStorage::change_message_configuration(const application::MessageConfigurationActor& a, const application::MessageConfigurationChange& c) { return impl_->change_message_configuration(a,c); }
application::StorageResult<application::MessageRecipientPage> SqliteStorage::resolve_message_recipient(const application::MessageActor& a, std::string_view q, const std::optional<application::MessageRecipient>& p, bool name_only) { return impl_->resolve_message_recipient(a,q,p,name_only); }
application::StorageResult<application::MessageTarget> SqliteStorage::system_sysop_target(const application::MessageActor& a) { return impl_->system_sysop_target(a); }
application::StorageResult<std::optional<application::MessageArea>> SqliteStorage::saved_message_area(const application::MessageActor& a){return impl_->saved_message_area(a);}
application::StorageStatus SqliteStorage::remember_message_area(const application::MessageActor& a,application::LocalEchoId e){return impl_->remember_message_area(a,e);}
application::StorageResult<application::MessageArea> SqliteStorage::message_area(const application::MessageActor& a,application::LocalEchoId e) { return impl_->message_area(a,e); }
application::StorageResult<application::MessageCallerStatistics> SqliteStorage::message_caller_statistics(const application::MessageActor& a,std::optional<application::LocalEchoId> e) { return impl_->message_caller_statistics(a,e); }
application::StorageResult<application::MessageArrivalPage> SqliteStorage::message_arrival_page(const application::MessageActor& a,std::int64_t after,std::size_t limit) { return impl_->message_arrival_page(a,after,limit); }
application::StorageResult<bool> SqliteStorage::message_arrival_eligible(const application::MessageActor& a,application::MessageId id) { return impl_->message_arrival_eligible(a,id); }
application::StorageResult<std::optional<std::string>> SqliteStorage::message_arrival_sender(const application::MessageActor& a,application::MessageId id) { return impl_->message_arrival_sender(a,id); }
application::StorageResult<bool> SqliteStorage::has_addressed_unread(const application::MessageActor& a,bool personal) { return impl_->has_addressed_unread(a,personal); }
application::StorageResult<application::MessageLoginScanSummary> SqliteStorage::login_message_scan_summary(const application::MessageActor& a,bool include_authored_echoes) { return impl_->login_message_scan_summary(a,include_authored_echoes); }
application::StorageResult<application::MessageCatalogPage<application::MessageGroup>> SqliteStorage::message_group_page(const application::MessageActor& a, const application::MessageCatalogCursor& c) { return impl_->message_group_page(a,c); }
application::StorageResult<application::MessageCatalogPage<application::LocalEcho>> SqliteStorage::local_echo_page(const application::MessageActor& a, application::MessageGroupId g, const application::MessageCatalogCursor& c) { return impl_->local_echo_page(a,g,c); }
application::StorageResult<application::MessageQueryContext> SqliteStorage::begin_message_query(const application::MessageActor& a, const application::MessageQueryOptions& o) { return impl_->begin_message_query(a,o); }
application::StorageResult<application::MessageHeader> SqliteStorage::message_number_header(const application::MessageActor& a,const application::MessageQueryContext& c,std::int64_t n) { return impl_->message_number_header(a,c,n); }
application::StorageResult<application::OpenedMessage> SqliteStorage::open_query_message(const application::MessageActor& a,const application::MessageQueryContext& c,application::MessageId id,bool traversal,bool history,bool preview) { return impl_->open_query_message(a,c,id,traversal,history,preview); }
application::StorageResult<application::MessageGroupScanPreview> SqliteStorage::preview_group_scan_column(const application::MessageActor& a,application::MessageGroupId g,application::MessageScanColumn c) { return impl_->preview_group_scan_column(a,g,c); }
application::StorageResult<bool> SqliteStorage::toggle_group_scan_column(const application::MessageActor& a,application::Identifier128 op,const application::MessageGroupScanPreview& p) { return impl_->toggle_group_scan_column(a,op,p); }
application::StorageResult<application::MessageHeaderPage> SqliteStorage::query_message_headers(const application::MessageActor& a, const application::MessageQueryContext& c, const application::MessageQueryCursor& p, std::size_t n) { return impl_->query_message_headers(a,c,p,n); }
application::StorageResult<application::MessageHeaderPage> SqliteStorage::query_message_list_headers(const application::MessageActor& a, const application::MessageQueryContext& c, const application::MessageQueryCursor& p, std::size_t n) {
    return impl_->query_message_list_headers(a,c,p,n);
}
application::StorageStatus SqliteStorage::set_message_list_deleted(const application::MessageActor& actor,const application::MessageQueryContext& query,
    application::MessageId message,bool expected,bool deleted,application::MessageListDeleteScope scope) {
    return impl_->set_message_list_deleted(actor,query,message,expected,deleted,scope);
}
application::StorageResult<std::vector<application::LocalEcho>> SqliteStorage::local_echoes(const application::MessageActor& a, application::MessageGroupId g) { return impl_->local_echoes(a,g); }
application::StorageResult<application::ComposeContext> SqliteStorage::begin_message(const application::MessageActor& a,const application::MessageTarget& t,std::optional<application::MessageId> p) { return impl_->begin_message(a,t,p); }
application::StorageResult<application::CommittedMessage> SqliteStorage::commit_message(const application::CommitMessageRequest& r) { return impl_->commit_message(r); }
application::StorageResult<application::CommittedMessage> SqliteStorage::reconcile_message(const application::CommitMessageRequest& r) { return impl_->reconcile_message(r); }
application::StorageResult<application::OpenedMessage> SqliteStorage::open_message(const application::MessageActor& a,application::MessageId m) { return impl_->open_message(a,m); }
application::StorageResult<application::OpenedMessage> SqliteStorage::quote_source(const application::MessageActor& a,application::MessageId m) { return impl_->quote_source(a,m); }
application::StorageResult<application::MessageScanPreference> SqliteStorage::message_scan_preference(const application::MessageActor& a,application::LocalEchoId e) { return impl_->message_scan_preference(a,e); }
application::StorageResult<application::MessageScanPreference> SqliteStorage::set_message_scan_preference(const application::MessageActor& a,application::LocalEchoId e,std::int64_t r,application::MessageScanColumn c,bool on) { return impl_->set_message_scan_preference(a,e,r,c,on); }
application::StorageStatus SqliteStorage::remove_private_inbox_item(const application::MessageActor& a,application::MessageId m) { return impl_->remove_private_inbox_item(a,m); }
application::StorageResult<bool> SqliteStorage::message_delete_authority(const application::MessageActor& a) {return impl_->message_delete_authority(a);}
application::StorageResult<bool> SqliteStorage::delete_message(const application::MessageActor& a,application::Identifier128 o,application::MessageId m) {return impl_->delete_message(a,o,m);}
application::StorageResult<bool> SqliteStorage::message_permanent_state(const application::MessageActor& a,application::MessageId m) {return impl_->message_permanent_state(a,m);}
application::StorageResult<bool> SqliteStorage::set_message_permanent(const application::MessageActor& a,application::Identifier128 o,application::MessageId m,bool expected,bool permanent) {return impl_->set_message_permanent(a,o,m,expected,permanent);}
#define SFX_MAINTENANCE_API(type,name,parameters,arguments) type SqliteStorage::name parameters {return impl_->name arguments;}
#include "sqlite_message_maintenance_api.inc"
#undef SFX_MAINTENANCE_API
#define SFX_QWK_API(type,name,parameters,arguments) type SqliteStorage::name parameters {return impl_->name arguments;}
#include "sqlite_qwk_api.inc"
#undef SFX_QWK_API
#define SFX_FILE_API(type,name,parameters,arguments) type SqliteStorage::name parameters {return impl_->name arguments;}
#include "sqlite_file_api.inc"
#undef SFX_FILE_API
#define SFX_DOOR_API(type,name,parameters,arguments) type SqliteStorage::name parameters {return impl_->name arguments;}
#include "sqlite_door_api.inc"
#undef SFX_DOOR_API
#define SFX_CHAT_API(type,name,parameters,arguments) type SqliteStorage::name parameters {return impl_->name arguments;}
#include "sqlite_chat_api.inc"
#undef SFX_CHAT_API
#define SFX_FTN_API(type,name,parameters,arguments) type SqliteStorage::name parameters {return impl_->name arguments;}
#include "sqlite_ftn_api.inc"
#undef SFX_FTN_API
#define SFX_FTN_WORK_API(type,name,parameters,arguments) type SqliteStorage::name parameters {return impl_->name arguments;}
#include "sqlite_ftn_work_api.inc"
#undef SFX_FTN_WORK_API
application::StorageResult<application::FirstRunState> SqliteStorage::first_run_state() { return impl_->first_run_state(); }
application::MutationResult SqliteStorage::stage_initial_sysop(const application::StageInitialSysopRequest& value) { return impl_->stage_initial_sysop(value); }
application::MutationResult SqliteStorage::complete_first_run(const application::CompleteFirstRunRequest& value) { return impl_->complete_first_run(value); }
application::MutationResult SqliteStorage::create_account(const application::CreateAccountRequest& value) { return impl_->create_account(value); }
application::MutationResult SqliteStorage::commit_registration(const application::RegistrationCommitRequest& value) { return impl_->commit_registration(value); }
application::StorageResult<application::CallerNameResolution> SqliteStorage::classify(std::string_view value) { return impl_->classify_caller_name(value); }
application::MutationResult SqliteStorage::commit_authenticated_session(const application::AuthenticatedSessionCommitRequest& value) { return impl_->commit_authenticated_session(value); }
application::MutationResult SqliteStorage::rename_account(const application::RenameAccountRequest& value) { return impl_->rename_account(value); }
application::MutationResult SqliteStorage::update_profile(const application::UpdateProfileRequest& value) { return impl_->update_profile(value); }
application::MutationResult SqliteStorage::set_purge_allowed(const application::SetPurgeAllowedRequest& value) { return impl_->set_purge_allowed(value); }
application::MutationResult SqliteStorage::assign_security_level(const application::AssignAccountSecurityLevelRequest& value) { return impl_->assign_security_level(value); }
application::MutationResult SqliteStorage::save_account(const application::SaveAccountRequest& value) { return impl_->save_account(value); }
application::MutationResult SqliteStorage::replace_credential(const application::ReplaceCredentialRequest& value) { return impl_->replace_credential(value); }
application::MutationResult SqliteStorage::change_account_state(const application::ChangeAccountStateRequest& value) { return impl_->change_account_state(value); }
application::MutationResult SqliteStorage::tombstone_account(const application::TombstoneAccountRequest& value) { return impl_->tombstone_account(value); }
application::StorageStatus SqliteStorage::publish_security_policy(const application::PublishSecurityPolicyRequest& value) { return impl_->publish_security_policy(value); }
application::StorageStatus SqliteStorage::publish_retention_policy(const application::PublishRetentionPolicyRequest& value) { return impl_->publish_retention_policy(value); }
application::MutationResult SqliteStorage::record_session_started(const application::StartSessionRequest& value) { return impl_->record_session_started(value); }
application::StorageResult<std::vector<application::CallerJournalRecord>> SqliteStorage::pending_caller_journals(bool unfinished, std::size_t limit) { return impl_->pending_caller_journals(unfinished, limit); }
application::StorageStatus SqliteStorage::append_caller_activity(application::Identifier128 session, const application::CallerActivity& activity, std::string_view hostname) { return impl_->append_caller_activity(session, activity, hostname); }
application::StorageStatus SqliteStorage::mark_caller_recovered(application::Identifier128 session) { return impl_->mark_caller_recovered(session); }
application::StorageResult<std::optional<application::CallerJournalPublication>> SqliteStorage::caller_publication() { return impl_->caller_publication(); }
application::StorageStatus SqliteStorage::prepare_caller_publication(const application::CallerJournalPublication& value) { return impl_->prepare_caller_publication(value); }
application::StorageStatus SqliteStorage::acknowledge_caller_publication(std::int64_t number) { return impl_->acknowledge_caller_publication(number); }
application::StorageStatus SqliteStorage::persist_session_progress(const application::PersistSessionProgressRequest& value) { return impl_->persist_session_progress(value); }
application::MutationResult SqliteStorage::finalize_session(const application::FinalizeSessionRequest& value) { return impl_->finalize_session(value); }
application::StorageResult<application::AccountRecord> SqliteStorage::find_account_by_name(std::string_view value) { return impl_->find_account_by_name(value); }
application::StorageResult<application::AccountRecord> SqliteStorage::find_account_by_id(application::AccountId value) { return impl_->find_account_by_id(value); }
application::StorageResult<std::string> SqliteStorage::door_user_key(application::AccountId value) { return impl_->door_user_key(value); }
application::StorageResult<application::CredentialEnvelope> SqliteStorage::load_credential(application::AccountId value) { return impl_->load_credential(value); }
application::StorageResult<std::vector<application::AccountRecord>> SqliteStorage::list_accounts(const application::AccountListQuery& value) { return impl_->list_accounts(value); }
application::StorageResult<std::vector<application::CallerDirectoryEntry>> SqliteStorage::caller_directory(const application::CallerDirectoryQuery& value) { return impl_->caller_directory(value); }
application::StorageResult<std::int64_t> SqliteStorage::caller_day_seconds(application::AccountId account,std::string_view date) { return impl_->caller_day_seconds(account,date); }
application::StorageResult<std::vector<application::AccountRecord>> SqliteStorage::inactivity_candidates(const application::InactivityCandidateQuery& value) { return impl_->inactivity_candidates(value); }
application::StorageResult<std::vector<application::SessionRecord>> SqliteStorage::session_history(const application::HistoryQuery& value) { return impl_->session_history(value); }
application::StorageResult<std::vector<application::AuditRecord>> SqliteStorage::audit_history(const application::AuditHistoryQuery& value) { return impl_->audit_history(value); }
application::StorageResult<std::vector<application::SecurityLevelRecord>> SqliteStorage::security_levels() { return impl_->security_levels(); }
application::StorageResult<application::SecurityLevelRecord> SqliteStorage::security_level(std::uint16_t value) { return impl_->security_level(value); }
application::StorageResult<application::SecurityPolicyRecord> SqliteStorage::security_policy() { return impl_->security_policy(); }
application::StorageResult<application::RetentionPolicyRecord> SqliteStorage::retention_policy() { return impl_->retention_policy(); }
application::MutationResult SqliteStorage::replace_account_password(const application::ReplaceAccountPasswordRequest& value) { return impl_->replace_account_password(value); }
application::MutationResult SqliteStorage::rehash_account_credential(const application::RehashAccountCredentialRequest& value) { return impl_->rehash_account_credential(value); }
application::MutationResult SqliteStorage::issue_temporary_password(const application::IssueTemporaryPasswordRequest& value) { return impl_->issue_temporary_password(value); }
application::StorageStatus SqliteStorage::transition_temporary_password(const application::TransitionTemporaryPasswordRequest& value) { return impl_->transition_temporary_password(value); }
application::MutationResult SqliteStorage::complete_temporary_password(const application::CompleteTemporaryPasswordRequest& value) { return impl_->complete_temporary_password(value); }
application::StorageResult<application::TemporaryPasswordRecord> SqliteStorage::temporary_password(application::AccountId value) { return impl_->temporary_password(value); }
application::StorageStatus SqliteStorage::replace_system_password(const application::ReplaceSystemPasswordRequest& value) { return impl_->replace_system_password(value); }
application::StorageStatus SqliteStorage::rehash_system_credential(const application::RehashSystemCredentialRequest& value) { return impl_->rehash_system_credential(value); }
application::StorageResult<application::SystemCredentialRecord> SqliteStorage::system_password() { return impl_->system_password(); }
application::StorageStatus SqliteStorage::record_successful_login_ip(const application::RecordSuccessfulLoginIpRequest& value) { return impl_->record_successful_login_ip(value); }
application::StorageResult<std::vector<application::SuccessfulLoginIpRecord>> SqliteStorage::successful_login_ips(application::AccountId value) { return impl_->successful_login_ips(value); }
application::StorageStatus SqliteStorage::store_rate_limit_snapshot(const application::RateLimitSnapshot& value) { return impl_->store_rate_limit_snapshot(value); }
application::StorageStatus SqliteStorage::remove_rate_limit_snapshot(const application::RateLimitKey& value) { return impl_->remove_rate_limit_snapshot(value); }
application::StorageStatus SqliteStorage::clear_rate_limit(const application::ClearRateLimitRequest& value) { return impl_->clear_rate_limit(value); }
application::StorageResult<std::vector<application::RateLimitSnapshot>> SqliteStorage::active_rate_limit_snapshots(std::int64_t now, std::size_t limit) { return impl_->active_rate_limit_snapshots(now, limit); }
application::StorageResult<std::vector<application::AuthenticationAuditRecord>> SqliteStorage::authentication_audit_history(const application::AuthenticationAuditQuery& value) { return impl_->authentication_audit_history(value); }
application::StorageResult<application::TransferPreferenceRecord> SqliteStorage::transfer_preference(application::AccountId value) { return impl_->transfer_preference(value); }
application::MutationResult SqliteStorage::update_transfer_preference(const application::UpdateTransferPreferenceRequest& value) { return impl_->update_transfer_preference(value); }
application::StorageResult<application::MenuPreferenceRecord> SqliteStorage::menu_preferences(application::AccountId value) { return impl_->menu_preferences(value); }
application::MutationResult SqliteStorage::update_menu_preferences(const application::UpdateMenuPreferenceRequest& value) { return impl_->update_menu_preferences(value); }
application::StorageStatus SqliteStorage::enqueue_failed_login_notice(const application::EnqueueFailedLoginNoticeRequest& value) { return impl_->enqueue_failed_login_notice(value); }
application::StorageResult<application::FailedLoginNoticeBatch> SqliteStorage::claim_failed_login_notices(const application::ClaimFailedLoginNoticesRequest& value) { return impl_->claim_failed_login_notices(value); }
application::StorageStatus SqliteStorage::acknowledge_failed_login_notices(const application::AcknowledgeFailedLoginNoticesRequest& value) { return impl_->acknowledge_failed_login_notices(value); }
application::StorageResult<std::size_t> SqliteStorage::recover_abandoned_failed_login_claims(const application::RecoverFailedLoginClaimsRequest& value) { return impl_->recover_abandoned_failed_login_claims(value); }

SqliteBackupResult SqliteStorage::create_backup(const std::filesystem::path& destination_file) {
    return impl_->create_backup(destination_file);
}
SqliteCompactionResult SqliteStorage::compact_offline(const std::filesystem::path& folder,std::function<bool()> cancelled){return impl_->compact_offline(folder,std::move(cancelled));}

application::StorageStatus SqliteStorage::validate_backup(const std::filesystem::path& backup_file) const {
    return impl_->validate_backup(backup_file);
}
application::StorageResult<std::int32_t> SqliteStorage::verified_schema_version(const std::filesystem::path& file) const {return impl_->verified_schema_version(file);}
application::StorageResult<application::FtnSerialState> SqliteStorage::verified_ftn_serial_history(const std::filesystem::path& file) const {return impl_->verified_ftn_serial_history(file);}

application::StorageStatus SqliteStorage::restore_backup(const std::filesystem::path& backup_file) {
    return impl_->restore_backup(backup_file);
}

std::filesystem::path SqliteStorage::database_path() const { return impl_->database_path(); }
application::StorageResult<application::OtherBoardPage> SqliteStorage::other_boards(std::int64_t after) { return impl_->other_boards(after); }
application::StorageResult<application::OpeningDateRecord> SqliteStorage::opening_date() { return impl_->opening_date(); }
application::StorageStatus SqliteStorage::change_classic_information(const application::ClassicInformationChange& change,const application::ClassicInformationActor& actor) { return impl_->change_classic_information(change,actor); }
application::StorageResult<application::SystemInformationCounters> SqliteStorage::system_information_counters(std::int64_t begin,std::int64_t end) { return impl_->system_information_counters(begin,end); }

}  // namespace sfx::adapters::storage
