#include "sfx/adapters/authentication/sodium_authentication.hpp"
#include "sfx/adapters/authentication/phase3a_authentication_adapters.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/authentication_security.hpp"
#include "sfx/application/authentication_storage.hpp"
#include "sfx/application/local_account_administration.hpp"
#include "sfx/test/account_storage_fakes.hpp"
#include "sfx/test/authentication_fakes.hpp"
#include "sfx/test/foundation_fakes.hpp"
#include "sfx/test/storage_fakes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace app = sfx::application;
namespace auth = sfx::adapters::authentication;
namespace storage = sfx::adapters::storage;
namespace test = sfx::test;
using namespace std::chrono_literals;

constexpr std::int64_t base_utc = 1'788'000'000;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void remove_tree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    require(!error, "remove Stage 5C integration tree");
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

app::AuditContext account_audit(
    const std::uint32_t sequence,
    const std::int64_t instant,
    const app::AuditReasonCode reason) {
    return {identifier(sequence), identifier(100'000U + sequence), std::nullopt, instant, reason};
}

app::AuthenticationAuditContext authentication_audit(
    const std::uint32_t sequence,
    const std::int64_t instant,
    const std::optional<app::AccountId> actor = std::nullopt) {
    return {identifier(sequence), identifier(200'000U + sequence), actor, instant};
}

storage::SqliteStorageSettings settings_for(const std::filesystem::path& directory) {
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
    settings.readers.pool_size = 1;
    settings.readers.maximum_pending_operations = 2048;
    settings.readers.enqueue_wait = 2000ms;
    return settings;
}

app::CredentialEnvelope create_credential(
    auth::SodiumPasswordKdf& kdf,
    const std::string_view password,
    const std::int64_t instant,
    const bool change_required = false) {
    app::CancellationSource cancellation;
    const auto created = kdf.create(password, instant, change_required, cancellation.token());
    require(created.succeeded(), "real Argon2id credential generation succeeds");
    const auto verified = kdf.verify(*created.credential, password, cancellation.token());
    require(verified.completed() && verified.verified, "new real credential self-verifies");
    return *created.credential;
}

struct ReadyStorage {
    std::unique_ptr<storage::SqliteStorage> database;
    app::AccountId account_id{0};
    std::int64_t revision{0};
};

ReadyStorage create_ready_storage(
    const std::filesystem::path& directory,
    test::FakeStoragePathPolicy& policy,
    const app::CredentialEnvelope& credential) {
    std::filesystem::create_directories(directory);
    auto database = std::make_unique<storage::SqliteStorage>(
        settings_for(directory), policy, storage::sfx_storage_migrations());
    require(database->start().succeeded(), "Stage 5C SQLite storage starts");
    require(database->health().schema_version == storage::sfx_storage_migrations().back().version,
        "Stage 5C operations run against the active clean SFX schema");
    app::StageInitialSysopRequest stage;
    stage.display_name = "Initial Sysop";
    stage.credential = credential;
    stage.audit = account_audit(1, base_utc, app::AuditReasonCode::initial_setup);
    const auto staged = database->stage_initial_sysop(stage);
    require(staged.succeeded() && staged.account_id.has_value() && staged.revision == 1,
        "real Argon2id initial account stages");
    const auto policy_record = database->security_policy();
    app::CompleteFirstRunRequest complete;
    complete.staged_sysop_account_id = *staged.account_id;
    complete.expected_policy_revision = policy_record.value->revision;
    complete.expected_policy_hash = policy_record.value->policy_hash;
    complete.published_policy_hash = policy_record.value->policy_hash;
    complete.audit = account_audit(2, base_utc + 1, app::AuditReasonCode::initial_setup);
    require(database->complete_first_run(complete).succeeded(), "Stage 5C first-run state completes");
    return {std::move(database), *staged.account_id, *staged.revision};
}

void blocklist_tests(
    const std::filesystem::path& output_root,
    const std::filesystem::path& pinned) {
    auth::PinnedSecListsBlocklist list(pinned);
    auth::PinnedSecListsBlocklist embedded;
    require(embedded.ready() && embedded.entry_count() == 10'000 &&
        embedded.contains_complete_case_insensitive("PASSWORD") &&
        !embedded.contains_complete_case_insensitive("prefix123456suffix"),
        "shipping embedded list validates exact pinned bytes and complete-candidate semantics");
    require(list.ready() && list.entry_count() == 10'000, "exact pinned SecLists top-10k loads");
    require(list.contains_complete_case_insensitive("123456"), "known complete blocklisted password matches");
    require(!list.contains_complete_case_insensitive("prefix123456suffix"),
        "listed substring does not match whole-candidate lookup");

    auth::PinnedSecListsBlocklist missing(output_root / "missing-password-list.txt");
    require(!missing.ready(), "missing blocklist fails closed");
    const std::filesystem::path corrupt = output_root / "corrupt-password-list.txt";
    {
        std::ofstream output(corrupt, std::ios::binary);
        output << "123456\n";
    }
    auth::PinnedSecListsBlocklist damaged(corrupt);
    require(!damaged.ready(), "corrupt blocklist fails exact hash and size validation");

    app::PasswordPolicyContext strong{app::PasswordMode::strong, "Caller", "Test BBS"};
    require(app::evaluate_password_policy("long unique passphrase value",strong,&embedded) == app::PasswordPolicyCode::accepted &&
        app::evaluate_password_policy("short",strong,&embedded) != app::PasswordPolicyCode::accepted,
        "shipping data enables locked Strong length and passphrase rules");
    require(app::evaluate_password_policy("long unique passphrase value", strong, &missing) ==
            app::PasswordPolicyCode::blocklist_unavailable,
        "strong creation fails closed with missing data");
}

void rate_subject_domain_test() {
    constexpr char hexadecimal[] = "0123456789abcdef";
    const auto digest = auth::rate_limit_subject_hash(
        app::RateLimitScope::source, "203.0.113.7");
    std::string encoded;
    encoded.reserve(digest.size() * 2U);
    for (const std::uint8_t byte : digest) {
        encoded.push_back(hexadecimal[(byte >> 4U) & 0x0fU]);
        encoded.push_back(hexadecimal[byte & 0x0fU]);
    }
    require(encoded ==
        "2fac1c2642fb45a9081022c1f51bef6b9555100fa502e82988702e64b198bee9",
        "SFX v1 rate-subject domain matches its deterministic golden vector");
}

void strong_administration_test(const std::filesystem::path& root) {
    auth::SodiumPasswordKdf kdf(2,4);
    auth::PinnedSecListsBlocklist blocklist;
    test::FakeStoragePathPolicy paths;
    const auto credential = create_credential(kdf,"abcd",base_utc);
    auto ready = create_ready_storage(root / "storage",paths,credential);
    auto& database = *ready.database;
    app::ReplaceSystemPasswordRequest initial_system{0,credential,authentication_audit(10,base_utc+1,ready.account_id)};
    require(database.replace_system_password(initial_system).succeeded(),"seed isolated existing Standard SY");
    test::FakeSessionRevocationSink revocations;
    test::DeterministicIdentifierSource identifiers(1000);
    test::FakeClock clock; clock.wall_seconds = base_utc+20;
    app::OperationalConfiguration configuration; configuration.password_mode = app::PasswordMode::strong;
    app::AuthenticationSecurityService authentication(database,database,kdf,&blocklist,revocations);
    app::AuthenticationSecurityService unavailable(database,database,kdf,nullptr,revocations);
    app::LocalAccountAdministration service(database,database,authentication,revocations,identifiers,clock,
        configuration,app::LocalAccountAuthority::installation_owner);
    app::CancellationSource cancellation;
    require(unavailable.verify_account_password(ready.account_id,"abcd",clock.wall_seconds,
        authentication_audit(20,clock.wall_seconds,ready.account_id),cancellation.token()).authenticated(),
        "existing short Standard credential still verifies without Strong blocklist");
    app::LocalAccountRequest request;
    request.action = app::LocalAccountAction::issue_temporary_password;
    request.account_id = ready.account_id; request.expected_revision = 1;
    request.password = request.confirmation = "short";
    auto result = service.execute(request,cancellation.token());
    require(result.code == app::LocalAccountResultCode::policy_rejected && !result.committed &&
        revocations.commands.empty() && database.find_account_by_id(ready.account_id).value->revision == 1,
        "Strong reset refusal does not change storage or revoke sessions");
    request.password = request.confirmation = "manual temporary phrase 527";
    result = service.execute(request,cancellation.token());
    const auto temporary = database.temporary_password(ready.account_id);
    require(result.code == app::LocalAccountResultCode::ok && result.committed && result.sessions_revoked &&
        temporary && temporary.value && temporary.value->expires_utc_seconds == clock.wall_seconds+72*60*60 &&
        database.load_credential(ready.account_id).value->password_change_required && request.password.empty() &&
        revocations.commands.back().kind == app::SessionRevocationKind::account_all,
        "embedded-list Strong reset uses real Argon2id/SQLite and retains 72-hour/forced-change/revocation contract");
    request = {}; request.action = app::LocalAccountAction::replace_system_password;
    request.expected_revision = 1; request.password = request.confirmation = "permanent private phrase 683";
    result = service.execute(request,cancellation.token());
    require(result.code == app::LocalAccountResultCode::ok && result.committed && result.sessions_revoked &&
        !database.system_password().value->credential.password_change_required &&
        revocations.commands.back().kind == app::SessionRevocationKind::sysop_level_all &&
        unavailable.verify_system_password("permanent private phrase 683",clock.wall_seconds,
            authentication_audit(21,clock.wall_seconds,ready.account_id),cancellation.token()).authenticated(),
        "Strong permanent SY replacement remains distinct and verifies without blocklist availability");
    app::CredentialEnvelope rejected;
    const auto failed = unavailable.create_password_credential("long unique phrase for outage",
        {app::PasswordMode::strong,"Caller","Test BBS"},clock.wall_seconds,false,cancellation.token(),rejected);
    require(failed.code == app::AuthenticationOperationCode::policy_rejected &&
        failed.policy_code == app::PasswordPolicyCode::blocklist_unavailable,
        "missing Strong data refuses credential creation while existing verification remains available");
    require(database.shutdown().succeeded(),"strong administration fixture closes");
    kdf.shutdown();
}

void kdf_tests(auth::SodiumPasswordKdf& kdf) {
    app::CancellationSource cancellation;
    const auto first = kdf.create("shared-password", base_utc, false, cancellation.token());
    const auto second = kdf.create("shared-password", base_utc + 1, false, cancellation.token());
    require(first.succeeded() && second.succeeded(), "real account and SY credentials generate");
    require(first.credential->salt != second.credential->salt,
        "independent credentials receive independent random salts");
    require(first.credential->verifier != second.credential->verifier,
        "independent salts produce independent verifier records");
    const auto correct = kdf.verify(*first.credential, "shared-password", cancellation.token());
    const auto incorrect = kdf.verify(*first.credential, "wrong-password", cancellation.token());
    require(correct.completed() && correct.verified, "correct real account password verifies");
    require(incorrect.completed() && !incorrect.verified, "incorrect real account password rejects");

    auto malformed = *first.credential;
    malformed.canonical_parameters.pop_back();
    require(kdf.verify(malformed, "shared-password", cancellation.token()).code ==
            app::PasswordKdfCode::malformed_credential,
        "real verifier rejects malformed envelope before KDF work");
    malformed = *first.credential;
    malformed.envelope_format_version = 77;
    require(kdf.verify(malformed, "shared-password", cancellation.token()).code ==
            app::PasswordKdfCode::unsupported_credential,
        "real verifier rejects future envelope version");

    app::CancellationSource already_cancelled;
    already_cancelled.cancel();
    require(kdf.create("cancelled-password", base_utc, false, already_cancelled.token()).code ==
            app::PasswordKdfCode::cancelled,
        "pre-cancelled real KDF request returns a bounded cancellation result");
}

void storage_tests(
    const std::filesystem::path& directory,
    auth::SodiumPasswordKdf& kdf) {
    test::FakeStoragePathPolicy policy;
    const auto initial = create_credential(kdf, "account-password", base_utc);
    ReadyStorage ready = create_ready_storage(directory, policy, initial);
    auto& database = *ready.database;
    const app::AccountId account_id = ready.account_id;
    std::int64_t revision = ready.revision;

    const auto system_credential = create_credential(kdf, "system-password", base_utc + 2);
    app::ReplaceSystemPasswordRequest system;
    system.expected_revision = 0;
    system.credential = system_credential;
    system.audit = authentication_audit(10, base_utc + 2, account_id);
    require(database.replace_system_password(system).succeeded(), "global SY credential stores independently");
    const auto loaded_system = database.system_password();
    require(loaded_system && loaded_system.value->revision == 1 &&
            loaded_system.value->credential.salt != initial.salt,
        "stored global SY record and salt remain independent from account credential");

    app::CancellationSource cancellation;
    require(kdf.verify(loaded_system.value->credential, "system-password", cancellation.token()).verified,
        "correct stored global SY verifies");
    require(!kdf.verify(loaded_system.value->credential, "incorrect-system", cancellation.token()).verified,
        "incorrect stored global SY rejects");

    const auto system_rehash = create_credential(kdf, "system-password", base_utc + 3);
    app::RehashSystemCredentialRequest rehash_system;
    rehash_system.expected_revision = 1;
    rehash_system.credential = system_rehash;
    rehash_system.audit = authentication_audit(11, base_utc + 3, account_id);
    require(database.rehash_system_credential(rehash_system).succeeded(),
        "global SY rehash replaces atomically");
    rehash_system.expected_revision = 1;
    rehash_system.credential = create_credential(kdf, "system-password", base_utc + 4);
    rehash_system.audit = authentication_audit(12, base_utc + 4, account_id);
    require(database.rehash_system_credential(rehash_system).code == app::StorageErrorCode::conflict,
        "stale global SY rehash fails without replacing current record");
    require(database.system_password().value->revision == 2, "failed SY rehash preserves current revision");

    const auto temporary = create_credential(kdf, "manual-temporary", base_utc + 5, true);
    app::IssueTemporaryPasswordRequest issue;
    issue.account_id = account_id;
    issue.expected_account_revision = revision;
    issue.credential = temporary;
    issue.expires_utc_seconds = base_utc + 5 + app::kTemporaryPasswordLifetimeSeconds;
    issue.audit = authentication_audit(20, base_utc + 5, account_id);
    const auto issued = database.issue_temporary_password(issue);
    require(issued.succeeded() && issued.revision.has_value(), "temporary password issuance is atomic");
    revision = *issued.revision;
    const auto reset = database.temporary_password(account_id);
    require(reset && reset.value->state == app::TemporaryPasswordState::active &&
            reset.value->expires_utc_seconds - reset.value->issued_utc_seconds ==
                app::kTemporaryPasswordLifetimeSeconds,
        "temporary password has exact 72-hour active lifetime");
    const auto loaded_temporary = database.load_credential(account_id);
    require(loaded_temporary && loaded_temporary.value->password_change_required,
        "temporary account credential forces replacement");

    const auto permanent = create_credential(kdf, "new-permanent-password", base_utc + 6);
    app::CompleteTemporaryPasswordRequest stale;
    stale.account_id = account_id;
    stale.expected_account_revision = revision + 1;
    stale.expected_reset_revision = reset.value->revision;
    stale.credential = permanent;
    stale.consumed_audit = authentication_audit(21, base_utc + 6, account_id);
    stale.changed_audit = authentication_audit(22, base_utc + 6, account_id);
    require(database.complete_temporary_password(stale).status.code == app::StorageErrorCode::conflict,
        "stale temporary completion fails atomically");
    require(database.temporary_password(account_id).value->state == app::TemporaryPasswordState::active &&
            database.load_credential(account_id).value->verifier == temporary.verifier,
        "failed temporary completion preserves active reset and verifier");

    stale.expected_account_revision = revision;
    const auto completed = database.complete_temporary_password(stale);
    require(completed.succeeded() && completed.revision.has_value(),
        "temporary consumption and permanent replacement commit atomically");
    revision = *completed.revision;
    require(database.temporary_password(account_id).value->state == app::TemporaryPasswordState::consumed &&
            !database.load_credential(account_id).value->password_change_required,
        "consumed reset retains lifecycle state and permanent credential");

    const auto second_temporary = create_credential(kdf, "second-temporary", base_utc + 7, true);
    issue.expected_account_revision = revision;
    issue.credential = second_temporary;
    issue.expires_utc_seconds = base_utc + 7 + app::kTemporaryPasswordLifetimeSeconds;
    issue.audit = authentication_audit(23, base_utc + 7, account_id);
    const auto second_issued = database.issue_temporary_password(issue);
    revision = *second_issued.revision;
    auto second_reset = database.temporary_password(account_id);
    require(database.transition_temporary_password({
        account_id,
        second_reset.value->revision,
        app::TemporaryPasswordState::cancelled,
        authentication_audit(24, base_utc + 8, account_id)}).succeeded(),
        "unused temporary password can be cancelled");
    require(database.temporary_password(account_id).value->state == app::TemporaryPasswordState::cancelled,
        "cancelled temporary state is durable");

    const auto third_temporary = create_credential(kdf, "third-temporary", base_utc + 9, true);
    issue.expected_account_revision = revision;
    issue.credential = third_temporary;
    issue.expires_utc_seconds = base_utc + 9 + app::kTemporaryPasswordLifetimeSeconds;
    issue.audit = authentication_audit(25, base_utc + 9, account_id);
    const auto third_issued = database.issue_temporary_password(issue);
    revision = *third_issued.revision;
    const auto third_reset = database.temporary_password(account_id);
    require(database.transition_temporary_password({
        account_id,
        third_reset.value->revision,
        app::TemporaryPasswordState::expired,
        authentication_audit(26, third_reset.value->expires_utc_seconds, account_id)}).succeeded(),
        "temporary expiry is a typed durable transition");

    for (std::uint32_t index = 0; index < 6; ++index) {
        app::RecordSuccessfulLoginIpRequest ip;
        ip.event_id = identifier(100U + index);
        ip.account_id = account_id;
        ip.source_ip = "192.0.2." + std::to_string(index + 1U);
        ip.protocol = index % 2U == 0 ? app::AuthenticationProtocol::telnet : app::AuthenticationProtocol::ssh;
        ip.succeeded_utc_seconds = base_utc + 20 + index;
        require(database.record_successful_login_ip(ip).succeeded(), "successful source history inserts");
    }
    const auto ips = database.successful_login_ips(account_id);
    require(ips && ips.value->size() == 5 && ips.value->front().source_ip == "192.0.2.6" &&
            ips.value->back().source_ip == "192.0.2.2",
        "successful source history retains exactly the last five records");

    app::RateLimitSnapshot rate;
    rate.key.scope = app::RateLimitScope::source;
    rate.key.subject_hash.fill(0x5a);
    rate.failure_count = 4;
    rate.window_started_utc_seconds = base_utc + 30;
    rate.last_failure_utc_seconds = base_utc + 31;
    rate.blocked_until_utc_seconds = base_utc + 35;
    rate.expires_utc_seconds = base_utc + 900;
    require(database.store_rate_limit_snapshot(rate).succeeded(), "short-lived throttle snapshot persists");
    const auto rates = database.active_rate_limit_snapshots(base_utc + 32, 16);
    require(rates && rates.value->size() == 1 && rates.value->front().failure_count == 4,
        "restart throttle snapshot reloads exactly");
    app::ClearRateLimitRequest clear_rate;
    clear_rate.key = rate.key;
    clear_rate.audit = authentication_audit(27, base_utc + 32, account_id);
    require(database.clear_rate_limit(clear_rate).succeeded() &&
            database.active_rate_limit_snapshots(base_utc + 32, 16).value->empty(),
        "local throttle clearing atomically removes persisted state and writes audit");

    const auto audit_rows = database.authentication_audit_history({account_id, std::nullopt,
        app::kMaximumUtcSeconds, 100});
    require(audit_rows && audit_rows.value->size() >= 7,
        "credential/reset lifecycle writes typed durable audit without details");
    const auto clear_audit = database.authentication_audit_history({std::nullopt,
        app::AuthenticationAuditEventKind::throttling_cleared, app::kMaximumUtcSeconds, 10});
    require(clear_audit && clear_audit.value->size() == 1,
        "local throttle clear has a typed durable audit record");

    const auto before_repair = database.find_account_by_id(account_id);
    require(database.replace_account_password({account_id,before_repair.value->revision,permanent,
        authentication_audit(199,base_utc+99,account_id)}).succeeded(),
        "restore usable permanent Sysop credential after expired reset fixture");
    const auto before_tombstone = database.find_account_by_id(account_id);
    app::TombstoneAccountRequest tombstone;
    tombstone.account_id = account_id;
    tombstone.expected_revision = before_tombstone.value->revision;
    tombstone.reason = app::AuditReasonCode::sysop_action;
    tombstone.audit = account_audit(200, base_utc + 100, app::AuditReasonCode::sysop_action);
    require(database.tombstone_account(tombstone).status.code == app::StorageErrorCode::system_sysop_protected,
        "designated System Sysop deletion requires explicit reassignment");
    app::CreateAccountRequest replacement;
    replacement.display_name = "Replacement Sysop";
    replacement.credential = initial;
    replacement.audit = account_audit(201, base_utc + 101, app::AuditReasonCode::registration);
    const auto created_replacement = database.create_account(replacement);
    require(created_replacement.succeeded(), "replacement account created before deletion");
    require(database.assign_security_level({*created_replacement.account_id, *created_replacement.revision,
        database.security_policy().value->revision, 999,
        account_audit(202, base_utc + 102, app::AuditReasonCode::sysop_action)}).succeeded(),
        "replacement usable Sysop assigned before deletion");
    const auto other=*created_replacement.account_id;
    require(database.tombstone_account(tombstone).status.code == app::StorageErrorCode::system_sysop_protected,
        "another usable Sysop alone does not bypass the designated System Sysop guard");
    const auto configuration = database.message_configuration({true,{}},{},{});
    require(configuration && configuration.value->system_sysop == account_id,
        "initial account remains the designated System Sysop after refused deletion");
    app::MessageConfigurationChange assignment;
    assignment.action = app::MessageConfigurationAction::system_sysop;
    assignment.operation = identifier(207);
    assignment.expected_configuration_revision = configuration.value->revision;
    assignment.expected_assignment_revision = configuration.value->assignment_revision;
    assignment.sysop = other;
    assignment.expected_account_revision = database.find_account_by_id(other).value->revision;
    require(static_cast<bool>(database.change_message_configuration({true,{}},assignment)),
        "eligible replacement explicitly receives the System Sysop assignment");
    require(database.issue_temporary_password({other,2,temporary,
        base_utc+103+app::kTemporaryPasswordLifetimeSeconds,authentication_audit(204,base_utc+103,other)}).succeeded(),
        "fallback Sysop temporary reset fixture");
    const auto other_reset=database.temporary_password(other);
    require(database.transition_temporary_password({other,other_reset.value->revision,app::TemporaryPasswordState::cancelled,
        authentication_audit(205,base_utc+104,other)}).succeeded(),"fallback reset cancelled");
    const auto last_usable = database.tombstone_account(tombstone);
    require(!last_usable && last_usable.last_sysop_protected &&
        database.find_account_by_id(account_id).value->revision == tombstone.expected_revision &&
        database.load_credential(account_id).succeeded(),
        "cancelled replacement credential cannot permit deletion of the last usable Sysop; refusal preserves account and credential");
    require(database.replace_account_password({other,3,initial,authentication_audit(206,base_utc+105,other)}).succeeded(),
        "fallback Sysop permanent credential repaired");
    tombstone.audit = account_audit(203, base_utc + 103, app::AuditReasonCode::sysop_action);
    require(database.tombstone_account(tombstone).succeeded(), "account deletes with another usable Sysop preserved");
    require(database.successful_login_ips(account_id).value->size() == 5,
        "deletion preserves successful source-IP history");
    require(database.temporary_password(account_id).status.code == app::StorageErrorCode::not_found,
        "tombstone erases temporary-password state");
    require(database.load_credential(account_id).status.code == app::StorageErrorCode::not_found,
        "tombstone erases account verifier");

    require(database.checkpoint().succeeded(), "Stage 5C checkpoint succeeds");
    require(database.shutdown().succeeded(), "Stage 5C storage shuts down cleanly");

    std::ifstream db_bytes(directory / "SFX.DB", std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(db_bytes)), std::istreambuf_iterator<char>());
    for (const std::string secret : {
             "account-password", "system-password", "manual-temporary",
             "new-permanent-password", "second-temporary", "third-temporary"}) {
        require(raw.find(secret) == std::string::npos,
            "plaintext/candidate secret is absent from durable database bytes");
    }
}

void local_throttle_tests(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy paths;
    auto settings = settings_for(root);
    storage::SqliteStorage database(settings,paths,storage::sfx_storage_migrations());
    require(database.start().succeeded(),"local throttle fixture storage starts");
    const auto now = base_utc + 10;
    auth::Phase3aRateControlCoordinator rates(database,now,64);
    const app::Phase3aRateScopeSet all{1,"192.0.2.7",true,true,true};
    for (unsigned index=0;index<9;++index) require(rates.record_failure(all,now).succeeded(),"seed all four throttle scopes");
    auto snapshot = database.active_rate_limit_snapshots(now,64);
    require(snapshot && snapshot.value && snapshot.value->size()==4,"four real durable scopes exist");
    const auto clear_audit = authentication_audit(600,now);
    require(rates.clear_locally({app::RateLimitScope::source,"192.0.2.7"},clear_audit).succeeded(),"source clear commits audit and memory");
    snapshot = database.active_rate_limit_snapshots(now,64);
    require(snapshot && snapshot.value && snapshot.value->size()==3 &&
        std::none_of(snapshot.value->begin(),snapshot.value->end(),[](const auto& row) { return row.key.scope==app::RateLimitScope::source; }),
        "source clear leaves account SY and global exactly present");
    require(rates.record_failure({{},"192.0.2.7",true,false,false},now).succeeded(),"new source failure after clear");
    snapshot = database.active_rate_limit_snapshots(now,64);
    const auto source = std::find_if(snapshot.value->begin(),snapshot.value->end(),[](const auto& row) { return row.key.scope==app::RateLimitScope::source; });
    require(source!=snapshot.value->end() && source->failure_count==1,"memory was cleared; new source failure starts at one");
    for (const auto& invalid : {app::LocalThrottleClearRequest{app::RateLimitScope::source,"example.test"},
            {app::RateLimitScope::global,"192.0.2.7"},{app::RateLimitScope::account,"01"}})
        require(rates.clear_locally(invalid,authentication_audit(601,now)).code==app::StorageErrorCode::invalid_argument,
            "invalid local throttle clear never reaches storage");
    require(!rates.clear_locally({app::RateLimitScope::source,"192.0.2.7"},clear_audit) && !rates.ready(),
        "audit collision refuses and fails coordinator closed without silently opening admission");
    snapshot = database.active_rate_limit_snapshots(now,64);
    require(snapshot && snapshot.value && snapshot.value->size()==4,"failed clear rolls back deletion");
    require(database.shutdown().succeeded() && database.start().succeeded(),"reopen durable throttle state");
    auth::Phase3aRateControlCoordinator recovered(database,now,64);
    require(recovered.ready(),"restart loads preserved state after refused transaction");
    std::uint32_t event=610;
    for (const auto& selected : {app::LocalThrottleClearRequest{app::RateLimitScope::source,"192.0.2.7"},
            {app::RateLimitScope::account,"1"},{app::RateLimitScope::system_password,{}},{app::RateLimitScope::global,{}}})
        require(recovered.clear_locally(selected,authentication_audit(event++,now)).succeeded(),"each scope can be cleared separately");
    require(database.active_rate_limit_snapshots(now,64).value->empty(),"all explicitly selected scopes gone");
    const auto audit = database.authentication_audit_history({{},app::AuthenticationAuditEventKind::throttling_cleared,app::kMaximumUtcSeconds,100});
    require(audit && audit.value && audit.value->size()==5,"exactly five successful non-secret clear audit records; no failed duplicate");
    const auto gate = recovered.await_admission(all,now,app::CancellationSource{}.token());
    require(gate.code==app::Phase3aRateGateCode::admitted && gate.applied_delay_seconds==0,"cleared memory admits with zero delay");
    require(recovered.record_failure(all,now).succeeded(),"policy remains active immediately after clearing");
    snapshot = database.active_rate_limit_snapshots(now,64);
    require(snapshot && snapshot.value && snapshot.value->size()==4 &&
        std::all_of(snapshot.value->begin(),snapshot.value->end(),[](const auto& row) { return row.failure_count==1; }),
        "all four original policies start fresh, no persistent bypass");
    require(database.shutdown().succeeded(),"throttle fixture normal shutdown");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[2]) == "--throttle-only") {
            const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
            require(root.parent_path() == std::filesystem::path(SFX_AUTH_TEST_SCRATCH).lexically_normal() &&
                root.filename().string().find("section3-throttle-") == 0 && !std::filesystem::exists(root),
                "fresh bounded throttle fixture only");
            require(std::filesystem::create_directory(root),"create focused throttle fixture");
            local_throttle_tests(root);
            std::cout << "PASS local throttle scopes: durable audit, memory isolation, invalid targets, rollback, restart and continued policy\n";
            return 0;
        }
        if (argc == 4 && std::string(argv[3]) == "--blocklist-only") {
            const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
            require(root.parent_path() == std::filesystem::path(SFX_AUTH_TEST_SCRATCH).lexically_normal() &&
                root.filename().string().find("section3-blocklist-") == 0 && !std::filesystem::exists(root),
                "fresh bounded blocklist fixture only");
            require(std::filesystem::create_directory(root),"create focused blocklist fixture");
            blocklist_tests(root,std::filesystem::path(argv[2]));
            strong_administration_test(root);
            std::cout << "PASS packaged Strong-password data, missing/corrupt refusal, existing verification, real-KDF/SQLite account and SY administration\n";
            return 0;
        }
        require(argc == 3,
            "usage: sfx_authentication_integration_tests <output-root> <pinned-blocklist>");
        const std::filesystem::path output_root(argv[1]);
        const std::filesystem::path pinned_blocklist(argv[2]);
        remove_tree(output_root);
        std::filesystem::create_directories(output_root);
        rate_subject_domain_test();
        blocklist_tests(output_root, pinned_blocklist);
        auth::SodiumPasswordKdf kdf(2, 16);
        require(kdf.health().accepting, "pinned libsodium KDF boundary initializes");
        kdf_tests(kdf);
        storage_tests(output_root / "storage", kdf);
        const auto health = kdf.health();
        require(health.maximum_active_jobs == 2 &&
                health.estimated_memory_high_water_bytes <= 64U * 1024U * 1024U,
            "KDF production boundary preserves two-job/64-MiB ceiling");
        kdf.shutdown();
        std::cout << "all SFX Stage 5C authentication integration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Stage 5C authentication integration test failure: " << error.what() << '\n';
        return 1;
    }
}
