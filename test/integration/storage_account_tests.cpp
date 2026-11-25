#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/account_storage.hpp"
#include "sfx/application/local_account_administration.hpp"
#include "sfx/test/account_storage_fakes.hpp"
#include "sfx/test/foundation_fakes.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite3.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace app = sfx::application;
namespace storage = sfx::adapters::storage;
namespace internal = sfx::adapters::storage::internal;
namespace test = sfx::test;
using namespace std::chrono_literals;

constexpr std::int64_t base_utc = 1'787'552'000;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void remove_tree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    require(!error, "remove Stage 5B test tree");
}

app::Identifier128 identifier(const std::uint32_t sequence) {
    app::Identifier128 value;
    value.bytes[0] = 0x53;
    value.bytes[1] = 0x46;
    value.bytes[2] = 0x58;
    value.bytes[3] = 0x42;
    value.bytes[12] = static_cast<std::uint8_t>((sequence >> 24U) & 0xffU);
    value.bytes[13] = static_cast<std::uint8_t>((sequence >> 16U) & 0xffU);
    value.bytes[14] = static_cast<std::uint8_t>((sequence >> 8U) & 0xffU);
    value.bytes[15] = static_cast<std::uint8_t>(sequence & 0xffU);
    return value;
}

app::AuditContext audit(
    const std::uint32_t sequence,
    const std::int64_t instant,
    const app::AuditReasonCode reason,
    const std::optional<app::AccountId> actor = std::nullopt) {
    return {identifier(sequence), identifier(100'000U + sequence), actor, instant, reason};
}

app::CredentialEnvelope credential(const std::int64_t instant, const std::uint8_t marker = 1) {
    app::CredentialEnvelope value;
    value.envelope_format_version = 1;
    value.scheme_identifier = "test-deferred-envelope";
    value.parameter_encoding_version = 1;
    value.canonical_parameters = {0x01, 0x02, marker};
    value.salt = {0x10, 0x11, marker};
    value.verifier = {0x20, 0x21, 0x22, marker};
    value.pepper_key_identifier = "test-key-1";
    value.created_utc_seconds = instant;
    return value;
}

app::AccountProfile profile() {
    app::AccountProfile value;
    value.real_name = "Test O'Neil-Smith";
    value.location = "Vancouver, BC";
    value.phone = "+1 604 555 0100 ext. 9";
    value.email = "Test.User@example.invalid";
    value.birth_date = "1980-02-29";
    value.sex = 'X';
    return value;
}

storage::SqliteStorageSettings settings_for(
    const std::filesystem::path& directory,
    const std::size_t reader_count = 2) {
    storage::SqliteStorageSettings settings;
    settings.data_directory = directory;
    settings.utc_now = [] { return base_utc; };
    settings.queue.maximum_operations = 1024;
    settings.queue.maximum_bytes = 2U * 1024U * 1024U;
    settings.queue.dequeue_batch_limit = 32;
    settings.queue.dequeue_batch_window = 2ms;
    settings.queue.enqueue_wait = 2000ms;
    settings.busy.sqlite_timeout = 100ms;
    settings.busy.transaction_deadline = 2000ms;
    settings.busy.maximum_attempts = 5;
    settings.checkpoint.passive_interval = std::chrono::hours(1);
    settings.readers.pool_size = reader_count;
    settings.readers.maximum_pending_operations = 2048;
    settings.readers.enqueue_wait = 2000ms;
    return settings;
}

struct ReadyStorage {
    std::unique_ptr<storage::SqliteStorage> database;
    app::AccountId sysop_id{0};
    std::int64_t sysop_revision{0};
};

ReadyStorage create_ready_storage(
    const std::filesystem::path& directory,
    test::FakeStoragePathPolicy& policy,
    const std::size_t reader_count = 2,
    std::vector<storage::SqliteMigration> migrations=storage::sfx_storage_migrations()) {
    std::filesystem::create_directories(directory);
    auto database = std::make_unique<storage::SqliteStorage>(
        settings_for(directory, reader_count), policy, std::move(migrations));
    const auto started=database->start();
    if(!started) std::cerr<<"storage start code "<<static_cast<unsigned>(started.code)<<" operation "<<started.operation<<'\n';
    require(started.succeeded(), "production account storage starts");

    app::StageInitialSysopRequest stage;
    stage.display_name = "Initial Sysop";
    stage.profile = profile();
    stage.credential = credential(base_utc);
    stage.audit = audit(1, base_utc, app::AuditReasonCode::initial_setup);
    const app::MutationResult staged = database->stage_initial_sysop(stage);
    require(staged.succeeded() && staged.account_id.has_value() && staged.revision == 1,
            "initial Sysop stages atomically");

    const auto policy_record = database->security_policy();
    require(policy_record && policy_record.value.has_value(), "fresh security policy loads");
    app::CompleteFirstRunRequest complete;
    complete.staged_sysop_account_id = *staged.account_id;
    complete.expected_policy_revision = policy_record.value->revision;
    complete.expected_policy_hash = policy_record.value->policy_hash;
    complete.published_policy_hash = policy_record.value->policy_hash;
    complete.audit = audit(2, base_utc + 1, app::AuditReasonCode::initial_setup);
    const app::MutationResult completed = database->complete_first_run(complete);
    require(completed.succeeded(), "first-run completion publishes matching policy");
    return {std::move(database), *staged.account_id, *staged.revision};
}

app::MutationResult create_account(
    storage::SqliteStorage& database,
    std::string name,
    const std::uint32_t sequence,
    const std::int64_t instant) {
    app::CreateAccountRequest request;
    request.display_name = std::move(name);
    request.profile = profile();
    request.credential = credential(instant, static_cast<std::uint8_t>(sequence));
    request.audit = audit(sequence, instant, app::AuditReasonCode::registration);
    return database.create_account(request);
}

void test_portable_validation() {
    test::DeterministicIdentifierSource ids(7);
    require(ids.next() == identifier(7) && ids.next() == identifier(8), "deterministic identifier fake is repeatable");
    test::FakeAccountStorage fake;
    fake.create_account({});
    fake.publish_retention_policy({});
    fake.retention_policy();
    require(fake.calls.size() == 3 && fake.calls[0] == "create_account" &&
            fake.calls[1] == "publish_retention_policy" && fake.calls[2] == "retention_policy",
            "typed account-storage fake records mutation and metadata-read calls");
    require(app::normalize_account_name("  Mixed   Case  ") == "Mixed Case", "account normalization trims and collapses");
    require(app::account_name_comparison_key("Mixed Case") == "mixed case", "account comparison key is ASCII lowercase");
    require(app::normalize_account_name(std::string(30U, 'A')).has_value(), "30-character account name accepted");
    require(!app::normalize_account_name(std::string(31U, 'A')).has_value(), "31-character account name rejected");
    require(app::normalize_real_name(std::string(64U, 'R')).has_value(), "64-character Real Name accepted");
    require(!app::normalize_real_name(std::string(65U, 'R')).has_value(), "65-character Real Name rejected");
    require(app::normalize_optional_profile_text(std::string(254U, 'E'), 254).has_value(), "254-character email storage accepted");
    require(!app::normalize_optional_profile_text(std::string(255U, 'E'), 254).has_value(), "255-character email storage rejected");
    require(app::valid_iso_civil_date("2000-02-29"), "Gregorian leap date accepted");
    require(!app::valid_iso_civil_date("1900-02-29"), "invalid Gregorian leap date rejected");
    const app::RetentionPolicyRecord retention{1,24,84,12,24,base_utc};
    require(app::valid_retention_policy(retention), "locked default retention metadata is valid");
    require(app::retention_disposition(base_utc - 10, base_utc - 20, base_utc - 40, false) ==
            app::RetentionDisposition::keep_online, "new retention record remains online");
    require(app::retention_disposition(base_utc - 30, base_utc - 20, base_utc - 40, false) ==
            app::RetentionDisposition::archive_required, "online-period expiry requires archive");
    require(app::retention_disposition(base_utc - 50, base_utc - 20, base_utc - 40, false) ==
            app::RetentionDisposition::archive_required, "unverified archive evidence blocks pruning");
    require(app::retention_disposition(base_utc - 50, base_utc - 20, base_utc - 40, true) ==
            app::RetentionDisposition::prune_eligible, "verified archive evidence permits deterministic pruning eligibility");
    require(app::retention_disposition(base_utc - 50, base_utc - 40, base_utc - 20, true) ==
            app::RetentionDisposition::keep_online, "invalid cutoff order fails closed");
}

void test_production_migration_and_interrupted_bootstrap(const std::filesystem::path& root) {
    const auto directory = root / "migration-bootstrap";
    std::filesystem::create_directories(directory);
    test::FakeStoragePathPolicy policy;
    storage::SqliteStorage database(settings_for(directory), policy, storage::sfx_storage_migrations());
    require(database.start().succeeded(), "production migration applies");
require(database.health().schema_version == 27, "active shipping SFX schema includes classic information");
    const auto migrations = storage::sfx_storage_migrations();
    require(migrations.size() == 27 && migrations.back().version == 27 && migrations[0].version == 1 &&
            migrations[0].name == "sfx-v1-initial-persistence-baseline" &&
            migrations[0].sql.size() == 25'849U &&
            migrations[0].content_sha256 == "c9ed2791ec70d18cb69696f837d27cc74f931d7c3253247768b7245bf0864806",
            "clean SFX production migration identity and hash are exact");
    require(migrations[1].version == 2 && migrations[1].name == "sfx-caller-journal-foundation" &&
            !migrations[1].sql.empty() && migrations[1].content_sha256.size() == 64,
            "additive caller-journal migration follows the unchanged initial baseline");
    const auto state = database.first_run_state();
    require(state && state.value.has_value() && !state.value->complete &&
            !state.value->staged_sysop_account_id.has_value(), "fresh installation is fail closed");
    require(database.security_levels().value->size() == 2, "fresh catalog has exactly two levels");
    const auto initial_sysop_level = database.security_level(999);
    require(initial_sysop_level.succeeded() && initial_sysop_level.value->label == "Sysop",
            "security-level lookup returns the exact seeded record");
    require(database.security_level(0).status.code == app::StorageErrorCode::invalid_argument &&
            database.security_level(500).status.code == app::StorageErrorCode::not_found,
            "security-level lookup rejects invalid and missing values distinctly");
    const auto retention = database.retention_policy();
    require(retention.succeeded() && retention.value->revision == 1 &&
            retention.value->audit_online_months == 24 && retention.value->audit_total_months == 84 &&
            retention.value->session_online_months == 12 && retention.value->session_total_months == 24,
            "fresh database seeds the locked retention metadata exactly");
    require(database.create_account({}).status.code == app::StorageErrorCode::invalid_argument,
            "unvalidated creation fails closed");

    app::StageInitialSysopRequest stage;
    stage.display_name = "Bootstrap Owner";
    stage.profile = profile();
    stage.credential = credential(base_utc);
    stage.audit = audit(10, base_utc, app::AuditReasonCode::initial_setup);
    const auto staged = database.stage_initial_sysop(stage);
    require(staged.succeeded(), "bootstrap Sysop staging succeeds");
    require(database.shutdown().succeeded(), "interrupted bootstrap storage closes");

    storage::SqliteStorage reopened(settings_for(directory), policy, storage::sfx_storage_migrations());
    const auto reopened_status = reopened.start();
    if (!reopened_status) std::cerr << "restart operation=" << reopened_status.operation << '\n';
    require(reopened_status.succeeded(), "interrupted bootstrap reopens");
    const auto reopened_state = reopened.first_run_state();
    require(reopened_state && reopened_state.value->staged_sysop_account_id == staged.account_id &&
            !reopened_state.value->complete, "interrupted bootstrap remains incomplete and recoverable");
    require(reopened.stage_initial_sysop(stage).status.code == app::StorageErrorCode::conflict,
            "second bootstrap identity is rejected");
    const auto policy_record = reopened.security_policy();
    app::CompleteFirstRunRequest bad;
    bad.staged_sysop_account_id = *staged.account_id;
    bad.expected_policy_revision = 1;
    bad.expected_policy_hash = std::string(64U, 'a');
    bad.published_policy_hash = bad.expected_policy_hash;
    bad.audit = audit(11, base_utc + 1, app::AuditReasonCode::initial_setup);
    require(reopened.complete_first_run(bad).status.code == app::StorageErrorCode::conflict,
            "mismatched bootstrap policy fails closed");
    bad.expected_policy_hash = policy_record.value->policy_hash;
    bad.published_policy_hash = policy_record.value->policy_hash;
    require(reopened.complete_first_run(bad).succeeded(), "matching bootstrap policy completes");
    require(reopened.complete_first_run(bad).status.code == app::StorageErrorCode::conflict,
            "completed bootstrap permanently closes");

    internal::SqliteConnection inspect(reopened.database_path(), SQLITE_OPEN_READONLY, 100ms, false);
    require(inspect.scalar_int64(
        "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='sfx_account_preferences'",
        "verify excluded preferences table") == 0, "account_preferences is not created");
    require(inspect.scalar_int64(
        "SELECT count(*) FROM pragma_table_list WHERE name LIKE 'sfx_%' AND strict<>1",
        "verify production strict tables") == 0, "all production tables are STRICT");
    const auto database_path = reopened.database_path();
    require(reopened.shutdown().succeeded(), "bootstrap test storage closes");

    internal::SqliteConnection constraints(database_path, SQLITE_OPEN_READWRITE, 100ms, true);
    const auto rejects_constraint = [&constraints](const std::string& sql, const char* operation) {
        try {
            constraints.exec(sql, operation);
            return false;
        } catch (const internal::StorageFailure& failure) {
            return failure.code() == app::StorageErrorCode::constraint;
        }
    };
    require(rejects_constraint(
        "UPDATE sfx_accounts SET deletion_scheduled_utc_seconds=1787553000 WHERE account_id=1",
        "reject stray deletion timestamp"),
        "database constraint rejects a deletion timestamp outside deletion_scheduled state");
    require(rejects_constraint(
        "UPDATE sfx_account_profiles SET birth_date='1900-02-29' WHERE account_id=1",
        "reject impossible Gregorian DOB"),
        "database constraint rejects an impossible Gregorian civil DOB");
    require(rejects_constraint(
        "UPDATE sfx_account_retention_policy SET audit_online_months=85,audit_total_months=84 WHERE singleton_id=1",
        "reject inverted retention periods"),
        "database constraint rejects inverted retention periods");
}

void test_accounts_names_revisions_and_tombstone(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "accounts", policy);
    auto& database = *ready.database;
    const auto created = create_account(database, "Thirty Character Name ABCDEFGH", 20, base_utc + 10);
    require(created.succeeded(), "30-character normalized account is created");
    require(database.find_account_by_name("thirty character name abcdefgh").succeeded(),
            "case-insensitive login identity lookup succeeds");
    require(create_account(database, "THIRTY CHARACTER NAME ABCDEFGH", 21, base_utc + 11).status.code ==
            app::StorageErrorCode::constraint, "case-insensitive duplicate name is rejected");
    require(create_account(database, std::string(31U, 'A'), 22, base_utc + 12).status.code ==
            app::StorageErrorCode::invalid_argument, "overlength normalized account is rejected without truncation");

    app::RenameAccountRequest rename;
    rename.account_id = *created.account_id;
    rename.expected_revision = *created.revision;
    rename.new_display_name = "Renamed Caller";
    rename.audit = audit(23, base_utc + 13, app::AuditReasonCode::sysop_action, ready.sysop_id);
    const auto renamed = database.rename_account(rename);
    require(renamed.succeeded() && renamed.revision == 2, "audited rename increments revision");
    require(database.rename_account(rename).status.code == app::StorageErrorCode::conflict,
            "stale rename revision conflicts");
    require(create_account(database, "Thirty Character Name ABCDEFGH", 24, base_utc + 14).status.code ==
            app::StorageErrorCode::constraint, "old name remains permanently reserved");

    app::UpdateProfileRequest bad_profile;
    bad_profile.account_id = *created.account_id;
    bad_profile.expected_revision = 2;
    bad_profile.profile = profile();
    bad_profile.profile.real_name = std::string(65U, 'R');
    bad_profile.audit = audit(25, base_utc + 15, app::AuditReasonCode::sysop_action, ready.sysop_id);
    require(database.update_profile(bad_profile).status.code == app::StorageErrorCode::invalid_argument,
            "overlength Real Name is rejected without revision change");

    const auto before_session = database.find_account_by_id(*created.account_id);
    app::StartSessionRequest start{identifier(30), identifier(30'000), *created.account_id, 1, base_utc + 60};
    require(database.record_session_started(start).succeeded(), "session starts for tombstone evidence");
    app::TombstoneAccountRequest active_tombstone;
    active_tombstone.account_id = *created.account_id;
    active_tombstone.expected_revision = before_session.value->revision;
    active_tombstone.reason = app::AuditReasonCode::sysop_action;
    active_tombstone.audit = audit(35, base_utc + 100, app::AuditReasonCode::sysop_action, ready.sysop_id);
    require(database.tombstone_account(active_tombstone).status.code == app::StorageErrorCode::conflict,
            "account with unfinished authenticated session cannot tombstone");
    app::FinalizeSessionRequest finish{start.session_id, 120, base_utc + 180,
        app::SessionOutcomeCode::normal, identifier(31)};
    require(database.finalize_session(finish).succeeded(), "session completes before tombstone");

    app::TombstoneAccountRequest tombstone;
    tombstone.account_id = *created.account_id;
    tombstone.expected_revision = before_session.value->revision;
    tombstone.reason = app::AuditReasonCode::sysop_action;
    tombstone.audit = audit(32, base_utc + 200, app::AuditReasonCode::sysop_action, ready.sysop_id);
    const auto tombstoned = database.tombstone_account(tombstone);
    require(tombstoned.succeeded(), "account tombstones atomically");
    const auto record = database.find_account_by_id(*created.account_id);
    require(record && record.value->state == app::AccountState::tombstoned &&
            !record.value->display_name.has_value() && !record.value->name_key.has_value() &&
            !record.value->security_level.has_value() && !record.value->purge_allowed.has_value(),
            "tombstone removes live identity/security/profile controls");
    require(!record.value->profile.real_name.has_value() &&
            record.value->statistics.successful_login_count == 1 &&
            record.value->statistics.authenticated_session_seconds == 120 &&
            record.value->statistics.last_successful_login_utc_seconds.has_value() &&
            record.value->statistics.last_completed_session_utc_seconds.has_value() &&
            record.value->statistics.last_session_outcome.has_value(),
            "deletion retains historical statistics and session dates");
    require(database.load_credential(*created.account_id).status.code == app::StorageErrorCode::not_found,
            "tombstone removes live credential envelope");
    require(create_account(database, "Renamed Caller", 33, base_utc + 201).status.code ==
            app::StorageErrorCode::constraint, "tombstoned name remains permanently reserved");
    const auto later = create_account(database, "Later Caller", 34, base_utc + 202);
    require(later.succeeded() && *later.account_id > *created.account_id,
            "AUTOINCREMENT permanent account identity is not reused");
    app::AccountListQuery visible;visible.exclude_tombstoned=true;visible.limit=1;
    const auto first=database.list_accounts(visible);visible.offset=1;const auto second=database.list_accounts(visible);
    require(first&&second&&first.value->size()==1&&second.value->size()==1&&
        first.value->front().account_id!=second.value->front().account_id&&
        first.value->front().state!=app::AccountState::tombstoned&&second.value->front().state!=app::AccountState::tombstoned,
        "visible account pagination excludes tombstones before limit/offset");
    app::AccountListQuery history;history.state=app::AccountState::tombstoned;
    const auto retained=database.list_accounts(history);
    require(retained&&retained.value->size()==1&&retained.value->front().account_id==*created.account_id,
        "history queries still retain deleted identity and session statistics");
    require(database.shutdown().succeeded(), "account test storage closes");
}

void test_sessions_inactivity_and_audit(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "sessions", policy);
    auto& database = *ready.database;
    const auto created = create_account(database, "Session Caller", 40, base_utc + 10);
    require(created.succeeded(), "session account created");
    app::StartSessionRequest start{identifier(41), identifier(41'000), *created.account_id, 128, base_utc + 20};
    const auto started = database.record_session_started(start);
    require(started.succeeded() && !started.replayed, "session start commits once");
    require(database.record_session_started(start).replayed, "session start replay is idempotent");
    app::PersistSessionProgressRequest early{start.session_id, 30, base_utc + 50};
    require(database.persist_session_progress(early).code == app::StorageErrorCode::not_due,
            "sub-minute progress is not durably written");
    app::PersistSessionProgressRequest minute{start.session_id, 60, base_utc + 80};
    require(database.persist_session_progress(minute).succeeded(), "one-minute progress commits");
    require(database.persist_session_progress(minute).succeeded(), "same progress replay is harmless");
    app::FinalizeSessionRequest finish{start.session_id, 125, base_utc + 145,
        app::SessionOutcomeCode::disconnected, identifier(42)};
    require(database.finalize_session(finish).succeeded(), "session finalization commits delta and outcome");
    require(database.finalize_session(finish).replayed, "session finalization replay is idempotent");
    app::FinalizeSessionRequest mismatch = finish;
    mismatch.total_authenticated_seconds = 126;
    require(database.finalize_session(mismatch).status.code == app::StorageErrorCode::conflict,
            "mismatched finalization replay conflicts");
    const auto record = database.find_account_by_id(*created.account_id);
    require(record.value->revision == 1 && record.value->statistics.successful_login_count == 1 &&
            record.value->statistics.authenticated_session_seconds == 125,
            "session statistics are exact and do not change administrative revision");
    const app::HistoryQuery history{*created.account_id, app::kMaximumUtcSeconds, 100};
    require(database.session_history(history).value->size() == 1, "bounded session history returns completion");
    const app::AuditHistoryQuery account_audit{created.account_id, std::nullopt,
        app::kMaximumUtcSeconds, 100};
    const auto account_events = database.audit_history(account_audit);
    require(account_events.succeeded() && account_events.value.has_value(),
            "typed account audit history query succeeds");
    require(!account_events.value->empty(), "typed account audit history returns events");

    const app::InactivityCandidateQuery candidates{base_utc + 20, 100};
    require(database.inactivity_candidates(candidates).value->size() == 1,
            "eligible active caller is selected by last successful login");
    app::ChangeAccountStateRequest schedule;
    schedule.account_id = *created.account_id;
    schedule.expected_revision = 1;
    schedule.new_state = app::AccountState::deletion_scheduled;
    schedule.deletion_scheduled_utc_seconds = base_utc + 200;
    schedule.audit = audit(43, base_utc + 200, app::AuditReasonCode::inactivity_policy);
    require(database.change_account_state(schedule).succeeded(), "inactivity candidate schedules with audited grace start");
    app::InactivityCandidateQuery due_before{base_utc + 199, 100,
        app::InactivitySelectionKind::tombstone_due};
    require(database.inactivity_candidates(due_before).value->empty(), "scheduled account is not due before grace cutoff");
    app::InactivityCandidateQuery due{base_utc + 200, 100,
        app::InactivitySelectionKind::tombstone_due};
    require(database.inactivity_candidates(due).value->size() == 1, "scheduled account becomes due at grace cutoff");
    app::ChangeAccountStateRequest cancel;
    cancel.account_id = *created.account_id;
    cancel.expected_revision = 2;
    cancel.new_state = app::AccountState::active;
    cancel.audit = audit(44, base_utc + 201, app::AuditReasonCode::sysop_action, ready.sysop_id);
    require(database.change_account_state(cancel).succeeded(), "Sysop can cancel scheduled deletion transactionally");
    app::SetPurgeAllowedRequest purge;
    purge.account_id = *created.account_id;
    purge.expected_revision = 3;
    purge.purge_allowed = false;
    purge.audit = audit(45, base_utc + 202, app::AuditReasonCode::sysop_action, ready.sysop_id);
    require(database.set_purge_allowed(purge).succeeded(), "Purge Allowed change is audited and revisioned");
    require(database.inactivity_candidates(candidates).value->empty(), "Purge Allowed No excludes candidate");

    const auto deletion = create_account(database, "Inactivity Delete", 46, base_utc + 203);
    require(deletion.succeeded(), "second account exists for completed inactivity deletion");
    app::ChangeAccountStateRequest deletion_schedule;
    deletion_schedule.account_id = *deletion.account_id;
    deletion_schedule.expected_revision = 1;
    deletion_schedule.new_state = app::AccountState::deletion_scheduled;
    deletion_schedule.deletion_scheduled_utc_seconds = base_utc + 300;
    deletion_schedule.audit = audit(47, base_utc + 250, app::AuditReasonCode::inactivity_policy);
    require(database.change_account_state(deletion_schedule).succeeded(),
            "eligible account enters inactivity grace state");
    app::TombstoneAccountRequest inactivity_tombstone;
    inactivity_tombstone.account_id = *deletion.account_id;
    inactivity_tombstone.expected_revision = 2;
    inactivity_tombstone.reason = app::AuditReasonCode::inactivity_policy;
    inactivity_tombstone.audit = audit(48, base_utc + 300, app::AuditReasonCode::inactivity_policy);
    require(database.tombstone_account(inactivity_tombstone).succeeded(),
            "due inactivity account tombstones after grace");
    const auto deletion_record = database.find_account_by_id(*deletion.account_id);
    require(deletion_record.value->state == app::AccountState::tombstoned &&
            !deletion_record.value->deletion_scheduled_utc_seconds.has_value(),
            "inactivity tombstone clears the completed grace timestamp");
    require(database.shutdown().succeeded(), "session test storage closes");
}

std::string query_plan_detail(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        throw std::runtime_error("prepare query-plan evidence");
    }
    std::string detail;
    int code = SQLITE_ROW;
    while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(statement, 3);
        if (text != nullptr) {
            if (!detail.empty()) detail += " | ";
            detail += reinterpret_cast<const char*>(text);
        }
    }
    const int finalize_code = sqlite3_finalize(statement);
    if (code != SQLITE_DONE || finalize_code != SQLITE_OK) {
        throw std::runtime_error("read query-plan evidence");
    }
    return detail;
}

void test_security_policy_and_queries(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "policy", policy);
    auto& database = *ready.database;
    const auto first = create_account(database, "Alpha Caller", 50, base_utc + 10);
    const auto second = create_account(database, "Beta Caller", 51, base_utc + 11);
    require(first.succeeded() && second.succeeded(), "policy test accounts created");

    app::PublishSecurityPolicyRequest add;
    add.expected_revision = 1;
    add.levels = {{10,"New User",std::nullopt,app::SecurityLevelState::active},
                  {20,"Regular",std::string("Regular callers"),app::SecurityLevelState::active},
                  {999,"Sysop",std::nullopt,app::SecurityLevelState::active}};
    add.new_user_level = 10;
    add.sysop_threshold = 999;
    add.policy_hash = std::string(64U, 'a');
    add.published_policy_hash = add.policy_hash;
    add.audit = audit(52, base_utc + 20, app::AuditReasonCode::security_policy, ready.sysop_id);
    require(database.publish_security_policy(add).succeeded(), "security level addition publishes atomically");

    app::PublishSecurityPolicyRequest retire = add;
    retire.expected_revision = 2;
    retire.levels[0].state = app::SecurityLevelState::retired;
    retire.reassignments = {{10,20}};
    retire.new_user_level = 20;
    retire.policy_hash = std::string(64U, 'b');
    retire.published_policy_hash = retire.policy_hash;
    retire.audit = audit(53, base_utc + 30, app::AuditReasonCode::security_policy, ready.sysop_id);
    require(database.publish_security_policy(retire).succeeded(), "security retirement reassigns and publishes atomically");
    require(database.find_account_by_id(*first.account_id).value->security_level == 20 &&
            database.find_account_by_id(*first.account_id).value->revision == 2,
            "retirement reassigns affected account and increments revision");
    app::PublishSecurityPolicyRequest replace = retire;
    replace.expected_revision = 3;
    replace.levels = {{10,"New User",std::nullopt,app::SecurityLevelState::retired},
                      {20,"Regular",std::string("Regular callers"),app::SecurityLevelState::retired},
                      {30,"Member",std::string("Established callers"),app::SecurityLevelState::active},
                      {999,"Sysop",std::nullopt,app::SecurityLevelState::active}};
    replace.reassignments = {{20,30}};
    replace.new_user_level = 30;
    replace.policy_hash = std::string(64U, 'c');
    replace.published_policy_hash = replace.policy_hash;
    replace.audit = audit(54, base_utc + 40, app::AuditReasonCode::security_policy, ready.sysop_id);
    require(database.publish_security_policy(replace).succeeded(),
            "same publication can add a replacement and retire an assigned level atomically");
    require(database.find_account_by_id(*first.account_id).value->security_level == 30 &&
            database.find_account_by_id(*first.account_id).value->revision == 3,
            "same-publication retirement reassigns accounts to the newly added level");

    app::PublishSecurityPolicyRequest reactivate = replace;
    reactivate.expected_revision = 4;
    reactivate.levels[1].state = app::SecurityLevelState::active;
    reactivate.reassignments.clear();
    reactivate.policy_hash = std::string(64U, 'd');
    reactivate.published_policy_hash = reactivate.policy_hash;
    reactivate.audit = audit(55, base_utc + 50, app::AuditReasonCode::security_policy, ready.sysop_id);
    require(database.publish_security_policy(reactivate).code == app::StorageErrorCode::conflict,
            "retired security level cannot reactivate");
    const app::AuditHistoryQuery all_audit{std::nullopt, std::nullopt,
        app::kMaximumUtcSeconds, 100};
    require(database.audit_history(all_audit).value->size() >= 5,
            "bounded global audit history supports time-ordered review");
    const app::AuditHistoryQuery policy_audit{std::nullopt,
        app::AuditEventKind::security_policy_changed, app::kMaximumUtcSeconds, 100};
    const auto policy_events = database.audit_history(policy_audit);
    require(policy_events.succeeded() && policy_events.value->size() == 3,
            "event-kind audit query returns committed security-policy publications");

    const auto retention_before = database.retention_policy();
    require(retention_before.succeeded() && retention_before.value->revision == 1,
            "retention metadata is readable through its typed port");
    app::PublishRetentionPolicyRequest extend;
    extend.expected_revision = 1;
    extend.audit_online_months = 30;
    extend.audit_total_months = 90;
    extend.session_online_months = 18;
    extend.session_total_months = 30;
    extend.audit = audit(56, base_utc + 60, app::AuditReasonCode::retention_policy, ready.sysop_id);
    require(database.publish_retention_policy(extend).succeeded(),
            "retention extension publishes with versioned audit evidence");
    require(database.publish_retention_policy(extend).code == app::StorageErrorCode::conflict,
            "stale retention revision conflicts");
    app::PublishRetentionPolicyRequest shorten = extend;
    shorten.expected_revision = 2;
    shorten.audit_online_months = 24;
    shorten.audit_total_months = 84;
    shorten.session_online_months = 12;
    shorten.session_total_months = 24;
    shorten.audit = audit(57, base_utc + 70, app::AuditReasonCode::retention_policy, ready.sysop_id);
    require(database.publish_retention_policy(shorten).code == app::StorageErrorCode::conflict,
            "retention shortening fails closed without effect-preview confirmation");
    shorten.shortening_effect_preview_confirmed = true;
    require(database.publish_retention_policy(shorten).succeeded(),
            "confirmed retention shortening commits metadata and audit atomically");
    const app::AuditHistoryQuery retention_audit{std::nullopt,
        app::AuditEventKind::retention_policy_changed, app::kMaximumUtcSeconds, 100};
    require(database.audit_history(retention_audit).value->size() == 2,
            "typed audit query returns only committed retention publications");

    app::AccountListQuery by_name;
    by_name.name_prefix = "Al";
    require(database.list_accounts(by_name).value->size() == 1, "bounded listing filters by normalized name");
    app::AccountListQuery by_state;
    by_state.state = app::AccountState::active;
    require(database.list_accounts(by_state).value->size() >= 3, "bounded listing filters by state");
    app::AccountListQuery by_security;
    by_security.security_level = static_cast<std::uint16_t>(30);
    require(database.list_accounts(by_security).value->size() == 2, "bounded listing filters by security level");

    internal::SqliteConnection inspect(database.database_path(), SQLITE_OPEN_READONLY, 100ms, false);
    const std::array<std::pair<const char*, const char*>, 8> plans = {{
        {"EXPLAIN QUERY PLAN SELECT account_id FROM sfx_accounts WHERE name_key='alpha caller'",
         "sfx_accounts_name_key_unique"},
        {"EXPLAIN QUERY PLAN SELECT account_id FROM sfx_accounts WHERE account_state='active' ORDER BY name_key,account_id",
         "sfx_accounts_state_name"},
        {"EXPLAIN QUERY PLAN SELECT account_id FROM sfx_accounts WHERE security_level=20 ORDER BY name_key,account_id",
         "sfx_accounts_security_name"},
        {"EXPLAIN QUERY PLAN SELECT session_id FROM sfx_account_sessions WHERE account_id=1 ORDER BY started_utc_seconds DESC,session_id",
         "sfx_account_sessions_account_time"},
        {"EXPLAIN QUERY PLAN SELECT event_id FROM sfx_account_audit WHERE subject_account_id=1 ORDER BY occurred_utc_seconds DESC,event_id",
         "sfx_account_audit_subject_time"},
        {"EXPLAIN QUERY PLAN SELECT event_id FROM sfx_account_audit WHERE event_kind='security.policy_changed' ORDER BY occurred_utc_seconds DESC,event_id",
         "sfx_account_audit_kind_time"},
        {"EXPLAIN QUERY PLAN SELECT event_id FROM sfx_account_audit WHERE occurred_utc_seconds<=253402300799 ORDER BY occurred_utc_seconds DESC,event_id LIMIT 100",
         "sfx_account_audit_time"},
        {"EXPLAIN QUERY PLAN SELECT label FROM sfx_security_levels WHERE security_level=20",
         "USING INTEGER PRIMARY KEY"}
    }};
    for (const auto& plan : plans) {
        const std::string detail = query_plan_detail(inspect.get(), plan.first);
        std::cout << "query_plan expected=" << plan.second << " detail=" << detail << std::endl;
        require(detail.find(plan.second) != std::string::npos, "approved query uses its measured index");
    }
    inspect.close();
    require(database.shutdown().succeeded(), "policy/query test storage closes");
    const auto restart_status = database.start();
    if (!restart_status) std::cerr << "restart operation=" << restart_status.operation << '\n';
    require(restart_status.succeeded() && database.retention_policy().succeeded(),
            "reader pool and account repository restart cleanly on the same storage object");
    require(database.shutdown().succeeded(), "restarted policy/query storage closes");
}

void test_security_assignment(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy paths;
    auto ready = create_ready_storage(root / "assignment", paths);
    auto& database = *ready.database;
    const auto caller = create_account(database, "Assignment Caller", 101, base_utc + 10);
    const auto other = create_account(database, "Other Caller", 102, base_utc + 11);
    require(caller && other, "assignment fixture callers created");
    app::AssignAccountSecurityLevelRequest request{*caller.account_id, 1, 1, 999,
        audit(103, base_utc + 20, app::AuditReasonCode::sysop_action, ready.sysop_id)};
    auto malformed = request;
    malformed.security_level = 1000;
    require(!database.assign_security_level(malformed), "out-of-range level refused");
    malformed = request; malformed.security_level = 500;
    require(!database.assign_security_level(malformed), "unconfigured level refused");
    malformed = request; malformed.expected_revision = 2;
    require(!database.assign_security_level(malformed), "stale account revision refused");
    malformed = request; malformed.expected_policy_revision = 2;
    require(!database.assign_security_level(malformed), "stale policy revision refused");
    malformed = request; malformed.audit.reason = app::AuditReasonCode::registration;
    require(!database.assign_security_level(malformed), "wrong audit context refused");
    const auto promoted = database.assign_security_level(request);
    require(promoted && promoted.revision == 2, "configured assignment commits one revision");
    require(database.find_account_by_id(*caller.account_id).value->security_level == 999,
        "exact account now has selected configured level");
    require(database.find_account_by_id(*other.account_id).value->security_level == 10,
        "unrelated account is unchanged");
    const auto policy = database.security_policy();
    require(policy && policy.value && policy.value->revision == 1,
        "assignment does not rewrite global security policy or configuration binding");
    require(!database.assign_security_level(request), "replayed stale assignment refused");
    const auto events = database.audit_history({*caller.account_id,
        app::AuditEventKind::security_policy_changed, app::kMaximumUtcSeconds, 100});
    require(events && events.value && events.value->size() == 1 &&
        events.value->front().actor_account_id == ready.sysop_id,
        "one non-secret subject/actor audit accompanies the committed assignment");
    request.expected_revision = 2; request.security_level = 10;
    request.audit = audit(104, base_utc + 30, app::AuditReasonCode::sysop_action, ready.sysop_id);
    require(database.assign_security_level(request).revision == 3,
        "explicit assignment can return an account to its prior configured level");
    require(database.shutdown().succeeded() && database.start().succeeded(),
        "typed assignment survives actual storage reopen");
    require(database.find_account_by_id(*caller.account_id).value->security_level == 10 &&
        database.find_account_by_id(*caller.account_id).value->revision == 3,
        "reopened assignment and revision are exact");
    request.expected_revision = 3; request.security_level = 999;
    request.audit = audit(103, base_utc + 20, app::AuditReasonCode::sysop_action, ready.sysop_id);
    require(!database.assign_security_level(request) &&
        database.find_account_by_id(*caller.account_id).value->revision == 3 &&
        database.find_account_by_id(*caller.account_id).value->security_level == 10,
        "failed audit insert rolls back assignment and revision atomically");
    require(database.tombstone_account({*caller.account_id, 3, app::AuditReasonCode::sysop_action,
        audit(105, base_utc + 40, app::AuditReasonCode::sysop_action, ready.sysop_id)}).succeeded(),
        "explicit fixture account tombstone completes");
    request.expected_revision = 4;
    request.audit = audit(106, base_utc + 50, app::AuditReasonCode::sysop_action, ready.sysop_id);
    require(!database.assign_security_level(request), "tombstoned accounts cannot be reauthorized");
    require(database.shutdown().succeeded(), "focused assignment storage closes");
}

void test_local_security_catalog(const std::filesystem::path& root) {
    class Publication final : public app::ISecurityPolicyPublication {
    public:
        unsigned fingerprints{}, publications{};
        bool fail{}, invalid_hash{};
        app::LocalSecuritySnapshot last;
        std::string fingerprint(std::string_view definition) override {
            if(definition.find("SFX-LEVEL-REMOVAL-1\n")==0) return std::string(64,'f');
            require(definition.find("new_user_level=") == 0 && definition.find("level.999=") != std::string_view::npos,
                "application supplies canonical complete non-secret definition");
            ++fingerprints;
            return invalid_hash ? "invalid" : std::string(64,static_cast<char>('a' + fingerprints % 6));
        }
        app::StorageStatus publish(const app::LocalSecuritySnapshot& truth) override {
            ++publications; last = truth;
            if (fail) throw std::runtime_error("injected public snapshot interruption");
            return {};
        }
    } publication;
    test::FakeStoragePathPolicy paths;
    auto ready = create_ready_storage(root / "catalog",paths);
    auto& database = *ready.database;
    const auto caller = create_account(database,"Catalog Caller",201,base_utc+10);
    const auto other = create_account(database,"Other Catalog Caller",202,base_utc+11);
    require(caller && other,"catalog fixture creates two real accounts");
    test::DeterministicIdentifierSource ids(1000);
    test::FakeClock clock;
    clock.wall_seconds = base_utc+20;
    app::LocalSecurityAdministration owner(database,publication,ids,clock,app::LocalAccountAuthority::installation_owner);
    app::LocalSecurityAdministration admin(database,publication,ids,clock,app::LocalAccountAuthority::elevated_administrator);
    app::LocalSecurityAdministration denied(database,publication,ids,clock,app::LocalAccountAuthority::none);
    app::LocalSecurityChange change{1,app::LocalSecurityAction::upsert_level,20,0,10,999,"Member",{}};
    require(admin.execute(change).code == app::LocalAccountResultCode::unauthorized &&
        denied.snapshot().code == app::LocalAccountResultCode::unauthorized && publication.fingerprints == 0,
        "SY-only recovery authority does not authorize catalog reads or writes");
    publication.invalid_hash = true;
    require(owner.execute(change).code == app::LocalAccountResultCode::storage_failure &&
        database.security_policy().value->revision == 1,"invalid trusted-host hash cannot reach catalog mutation");
    publication.invalid_hash = false;
    auto result = owner.execute(change);
    require(result.code == app::LocalAccountResultCode::ok && result.committed && result.revision == 2 &&
        publication.last.policy.revision == 2 && publication.last.levels.size() == 3,
        "shared production service commits catalog then publishes exact DB truth");
    require(owner.execute(change).code == app::LocalAccountResultCode::conflict && publication.publications == 1,
        "stale duplicate request cannot republish or mutate");
    change = {2,app::LocalSecurityAction::set_defaults,0,0,20,20,{}, {}};
    result = owner.execute(change);
    require(result.code == app::LocalAccountResultCode::ok && result.revision == 3 &&
        database.security_policy().value->new_user_level == 20 && database.security_policy().value->sysop_threshold == 20,
        "configured defaults and non-magical Sysop threshold publish together");
    change = {3,app::LocalSecurityAction::retire_level,10,20,20,20,{}, {}};
    change.affected_accounts_hash=std::string(64,'f');
    result = owner.execute(change);
    require(result.code == app::LocalAccountResultCode::ok && result.revision == 4 &&
        database.find_account_by_id(*caller.account_id).value->security_level == 20 &&
        database.find_account_by_id(*other.account_id).value->revision == 2 &&
        database.find_account_by_id(ready.sysop_id).value->security_level == 999 &&
        database.security_level(10).value->state == app::SecurityLevelState::retired,
        "retirement reassigns both affected callers atomically without changing unrelated Sysop");
    change = {4,app::LocalSecurityAction::upsert_level,30,0,20,20,"Trusted",{}};
    publication.fail = true;
    result = owner.execute(change);
    require(result.code == app::LocalAccountResultCode::storage_failure && result.committed && result.revision == 5 &&
        database.security_level(30).value->state == app::SecurityLevelState::active,
        "throwing post-commit publication preserves truthful committed result");
    require(owner.execute(change).code == app::LocalAccountResultCode::conflict && publication.publications == 4,
        "interrupted public snapshot is not repaired by duplicate catalog mutation");
    publication.fail = false;
    require(publication.publish(owner.snapshot()).succeeded() && publication.last.policy.revision == 5,
        "recovery republishes authoritative current truth without incrementing revision");
    test::DeterministicIdentifierSource duplicate_ids(1000);
    app::LocalSecurityAdministration collision(database,publication,duplicate_ids,clock,app::LocalAccountAuthority::installation_owner);
    change = {5,app::LocalSecurityAction::upsert_level,40,0,20,20,"Rollback",{}};
    result = collision.execute(change);
    require(result.code != app::LocalAccountResultCode::ok && !result.committed &&
        database.security_policy().value->revision == 5 && !database.security_level(40),
        "failed audit insertion rolls back whole catalog publication");
    const auto events = database.audit_history({{},app::AuditEventKind::security_policy_changed,app::kMaximumUtcSeconds,100});
    require(events && events.value && events.value->size() == 4,
        "exactly four successful catalog mutations produce four non-secret policy audits");
    require(database.shutdown().succeeded() && database.start().succeeded(),"catalog storage reopens");
    const auto reopened = owner.snapshot();
    require(reopened.code == app::LocalAccountResultCode::ok && reopened.policy.revision == 5 &&
        reopened.levels.size() == 4 && database.find_account_by_id(*caller.account_id).value->security_level == 20,
        "catalog, bindings and account reassignments survive real storage reopen");
    require(database.shutdown().succeeded(),"focused catalog storage closes");
}

#include "live_account_editor_cases.inl"
#include "section6_storage_cases.inl"
}  // namespace

int main(int argc, char** argv) {
    try {
        if(argc==3&&std::string(argv[2])=="--owner-list-only"){
            const auto root=std::filesystem::absolute(argv[1]);require(!std::filesystem::exists(root),"fresh owner-list evidence only");std::filesystem::create_directories(root);
            test_accounts_names_revisions_and_tombstone(root);
            std::cout<<"PASS owner account list, pagination, tombstone and retained history checks\n";return 0;
        }
        if (argc == 3 && (std::string(argv[2]) == "--assignment-only" || std::string(argv[2]) == "--catalog-only" || std::string(argv[2])=="--live-editor-only" || std::string(argv[2])=="--section6-only")) {
            const bool catalog = std::string(argv[2]) == "--catalog-only";
            const bool editor=std::string(argv[2])=="--live-editor-only";
            const bool section6=std::string(argv[2])=="--section6-only";
            const auto parent = std::filesystem::absolute(argv[1]).lexically_normal();
            require(parent == std::filesystem::path(SFX_ACCOUNT_TEST_SCRATCH).lexically_normal(),
                "assignment fixture is confined to this build's test-output");
            const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            const auto root = parent / ((section6?"section6-":editor ? "section45-editor-" : catalog ? "section3-catalog-" : "section3-assignment-") + std::to_string(stamp));
            require(!std::filesystem::exists(root) && std::filesystem::create_directory(root),
                "assignment fixture must be fresh, never replace old evidence");
            if(section6) test_section6_storage(root);else if(editor) test_live_account_editor_storage(root); else if (catalog) test_local_security_catalog(root); else test_security_assignment(root);
            std::cout << "PASS focused " << (section6?"Section6 caller utilities":editor?"live account editor":catalog?"security catalog":"account security assignment") << "; retained fixture: " << root << '\n';
            return 0;
        }
        require(argc == 2, "Stage 5B account tests require an output directory");
        const std::filesystem::path root = std::filesystem::absolute(argv[1]);
        remove_tree(root);
        std::filesystem::create_directories(root);
        std::cout << "stage5b validation" << std::endl;
        test_portable_validation();
        std::cout << "stage5b migration-bootstrap" << std::endl;
        test_production_migration_and_interrupted_bootstrap(root);
        std::cout << "stage5b accounts-tombstone" << std::endl;
        test_accounts_names_revisions_and_tombstone(root);
        std::cout << "stage5b sessions-inactivity" << std::endl;
        test_sessions_inactivity_and_audit(root);
        std::cout << "stage5b policy-queries" << std::endl;
        test_security_policy_and_queries(root);
        remove_tree(root);
        std::cout << "all SFX Stage 5B account-storage tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Stage 5B account-storage test failure: " << error.what() << '\n';
        return 1;
    }
}
