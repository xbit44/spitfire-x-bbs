#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/account_storage.hpp"
#include "sfx/application/phase3a_storage.hpp"
#include "sfx/test/shipping_schema.hpp"
#include "sfx/test/phase3a_storage_fakes.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include "sqlite3.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace app = sfx::application;
namespace storage = sfx::adapters::storage;
namespace internal = sfx::adapters::storage::internal;
namespace test = sfx::test;
using namespace std::chrono_literals;

constexpr std::int64_t base_utc = 1'788'100'000;
std::size_t assertion_count = 0;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++assertion_count;
}

void remove_tree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    require(!error, "remove Phase 3A Window 1 test tree");
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

std::string identifier_hex(const std::uint32_t sequence) {
    static constexpr char digits[] = "0123456789ABCDEF";
    const auto value = identifier(sequence);
    std::string output;
    output.reserve(value.bytes.size() * 2U);
    for (const std::uint8_t byte : value.bytes) {
        output.push_back(digits[(byte >> 4U) & 0x0fU]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

app::AuditContext audit(
    const std::uint32_t sequence,
    const std::int64_t instant,
    const app::AuditReasonCode reason) {
    return {identifier(sequence), identifier(100'000U + sequence), std::nullopt, instant, reason};
}

app::CredentialEnvelope credential(const std::int64_t instant, const std::uint8_t marker = 1) {
    app::CredentialEnvelope value;
    value.envelope_format_version = 1;
    value.scheme_identifier = "phase3a-test-envelope";
    value.parameter_encoding_version = 1;
    value.canonical_parameters = {0x01, marker};
    value.salt = {0x10, 0x11, marker};
    value.verifier = {0x20, 0x21, marker};
    value.created_utc_seconds = instant;
    return value;
}

storage::SqliteStorageSettings settings_for(const std::filesystem::path& directory) {
    storage::SqliteStorageSettings settings;
    settings.data_directory = directory;
    settings.utc_now = [] { return base_utc; };
    settings.queue.maximum_operations = 2048;
    settings.queue.maximum_bytes = 4U * 1024U * 1024U;
    settings.queue.dequeue_batch_limit = 64;
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

struct ReadyStorage {
    std::unique_ptr<storage::SqliteStorage> database;
    app::AccountId sysop_id{0};
};

ReadyStorage create_ready_storage(
    const std::filesystem::path& directory,
    test::FakeStoragePathPolicy& policy,
    storage::SqliteStorageSettings settings) {
    std::filesystem::create_directories(directory);
    auto database = std::make_unique<storage::SqliteStorage>(
        std::move(settings), policy, storage::sfx_storage_migrations());
    require(database->start().succeeded(), "Phase 3A storage starts");
require(database->health().schema_version == sfx::test::shipping_schema_version,
        "Phase 3A operations use the shipping SFX schema including caller journals");

    app::StageInitialSysopRequest stage;
    stage.display_name = "Window One Sysop";
    stage.credential = credential(base_utc);
    stage.audit = audit(1, base_utc, app::AuditReasonCode::initial_setup);
    const app::MutationResult staged = database->stage_initial_sysop(stage);
    require(staged.succeeded() && staged.account_id.has_value(), "Phase 3A Sysop stages");
    const auto policy_record = database->security_policy();
    app::CompleteFirstRunRequest complete;
    complete.staged_sysop_account_id = *staged.account_id;
    complete.expected_policy_revision = policy_record.value->revision;
    complete.expected_policy_hash = policy_record.value->policy_hash;
    complete.published_policy_hash = policy_record.value->policy_hash;
    complete.audit = audit(2, base_utc + 1, app::AuditReasonCode::initial_setup);
    require(database->complete_first_run(complete).succeeded(), "Phase 3A bootstrap completes");
    return {std::move(database), *staged.account_id};
}

ReadyStorage create_ready_storage(
    const std::filesystem::path& directory,
    test::FakeStoragePathPolicy& policy) {
    return create_ready_storage(directory, policy, settings_for(directory));
}

app::MutationResult create_account(
    storage::SqliteStorage& database,
    const std::string& name,
    const std::uint32_t sequence,
    const std::int64_t instant) {
    app::CreateAccountRequest request;
    request.display_name = name;
    request.credential = credential(instant, static_cast<std::uint8_t>(sequence));
    request.audit = audit(sequence, instant, app::AuditReasonCode::registration);
    return database.create_account(request);
}

app::EnqueueFailedLoginNoticeRequest notice(
    const std::uint32_t sequence,
    const app::AccountId account_id,
    const std::int64_t instant,
    std::string source_ip = "192.0.2.1") {
    return {identifier(sequence), account_id, std::move(source_ip), instant};
}

app::ClaimFailedLoginNoticesRequest claim(
    const app::AccountId account_id,
    const std::uint32_t claim_sequence,
    const std::uint32_t session_sequence,
    const std::int64_t instant,
    const std::size_t limit = app::kFailedLoginNoticeMaximum) {
    return {account_id, identifier(claim_sequence), identifier(session_sequence), instant, limit};
}

void start_session(
    storage::SqliteStorage& database,
    const app::AccountId account_id,
    const std::uint32_t session_sequence,
    const std::int64_t instant) {
    const app::StartSessionRequest request{
        identifier(session_sequence), identifier(500'000U + session_sequence),
        account_id, 1, instant};
    require(database.record_session_started(request).succeeded(),
        "durable session starts for failed-login notice claim");
}

void finalize_session(
    storage::SqliteStorage& database,
    const std::uint32_t session_sequence,
    const std::int64_t instant) {
    const app::FinalizeSessionRequest request{
        identifier(session_sequence), 1, instant,
        app::SessionOutcomeCode::disconnected, identifier(700'000U + session_sequence)};
    require(database.finalize_session(request).succeeded(),
        "durable session finalizes for notice lifecycle test");
}

std::int64_t scalar(
    const storage::SqliteStorage& database,
    const std::string& sql,
    const char* operation) {
    internal::SqliteConnection inspect(
        database.database_path(), SQLITE_OPEN_READONLY, 100ms, false);
    return inspect.scalar_int64(sql, operation);
}

void delete_session(
    storage::SqliteStorage& database,
    const std::uint32_t session_sequence) {
    internal::SqliteConnection prune(
        database.database_path(), SQLITE_OPEN_READWRITE, 100ms, true);
    // Shipping migration 2 retains caller history. A raw session prune must not
    // bypass that reference. Exercise the guard before isolating the historical
    // notice-claim cascade with removal of this synthetic test journal only.
    const auto session = identifier(session_sequence);
    {
        internal::SqliteStatement protected_delete(
            prune.get(), "DELETE FROM sfx_account_sessions WHERE session_id=?1",
            "verify journal prevents premature owning-session deletion");
        protected_delete.bind(1, session);
        bool refused = false;
        try { protected_delete.step(); } catch (const std::exception&) {
            refused = sqlite3_extended_errcode(prune.get()) == SQLITE_CONSTRAINT_FOREIGNKEY;
        }
        require(refused, "caller journal reference prevents premature session deletion");
    }
    prune.exec("BEGIN IMMEDIATE", "begin synthetic notice-cascade fixture pruning");
    {
        internal::SqliteStatement journal(
            prune.get(), "DELETE FROM sfx_caller_journals WHERE session_id=?1",
            "remove only synthetic journal before testing notice-claim cascade");
        journal.bind(1, session);
        journal.step();
        require(sqlite3_changes(prune.get()) == 1, "exact synthetic session journal removed");
    }
    {
        internal::SqliteStatement protected_delete(
            prune.get(), "DELETE FROM sfx_account_sessions WHERE session_id=?1",
            "verify Chat usage reference also prevents session deletion");
        protected_delete.bind(1, session);
        bool refused = false;
        try { protected_delete.step(); } catch (const std::exception&) {
            refused = sqlite3_extended_errcode(prune.get()) == SQLITE_CONSTRAINT_FOREIGNKEY;
        }
        require(refused, "Chat usage exclusion prevents premature session deletion");
    }
    {
        internal::SqliteStatement exclusion(
            prune.get(), "DELETE FROM sfx_chat_usage_exclusions WHERE session_id=?1",
            "remove only synthetic Sysop usage exclusion for notice-cascade fixture");
        exclusion.bind(1, session);
        exclusion.step();
        require(sqlite3_changes(prune.get()) == 1, "exact synthetic Chat usage exclusion removed");
    }
    internal::SqliteStatement statement(
        prune.get(), "DELETE FROM sfx_account_sessions WHERE session_id=?1",
        "prune owning session with active notice claim");
    statement.bind(1, identifier(session_sequence));
    statement.step();
    require(sqlite3_changes(prune.get()) == 1,
        "owning session delete succeeds with active notice claim");
    prune.exec("COMMIT", "commit synthetic notice-cascade fixture pruning");
}

void test_portable_contracts_and_fake() {
    const auto ipv4 = app::canonicalize_source_ip("192.0.2.001");
    require(!ipv4.has_value(), "noncanonical IPv4 is rejected");
    require(app::canonicalize_source_ip("192.0.2.1")->text == "192.0.2.1",
        "canonical IPv4 round-trips");
    require(app::canonicalize_source_ip("2001:0DB8:0:0:0:0:0:1")->text == "2001:db8::1",
        "IPv6 normalizes deterministically");
    require(app::canonicalize_source_ip("::ffff:192.0.2.1")->text == "::ffff:c000:201",
        "embedded IPv4 IPv6 input normalizes to binary-canonical text");
    require(!app::canonicalize_source_ip("2001:::1").has_value(), "malformed IPv6 is rejected");
    require(app::storage_code(app::TransferProtocolPreference::ymodem_g) == "ymodem_g",
        "typed YMODEM-G uses the locked physical code");
    require(!app::transfer_preference_from_storage_code("YMODEM-G").has_value(),
        "nonphysical transfer spelling is rejected at storage decoding");

    test::FakePhase3aStorage fake;
    fake.transfer_preference(1);
    fake.update_transfer_preference({});
    fake.enqueue_failed_login_notice({});
    fake.claim_failed_login_notices({});
    fake.acknowledge_failed_login_notices({});
    fake.recover_abandoned_failed_login_claims({});
    require(fake.calls.size() == 6 && fake.calls.front() == "transfer_preference" &&
        fake.calls.back() == "recover_abandoned_failed_login_claims",
        "storage-only fake records every private operation deterministically");
}



void test_transfer_preferences_and_constraints(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "preferences", policy);
    auto& database = *ready.database;
    auto record = database.transfer_preference(ready.sysop_id);
    require(record && record.value->preference == app::TransferProtocolPreference::select_at_transfer,
        "new account trigger stores Select at time of transfer");
    std::int64_t revision = record.value->revision;
    for (const auto preference : {
             app::TransferProtocolPreference::xmodem,
             app::TransferProtocolPreference::ymodem,
             app::TransferProtocolPreference::ymodem_g,
             app::TransferProtocolPreference::zmodem,
             app::TransferProtocolPreference::select_at_transfer}) {
        const auto changed = database.update_transfer_preference(
            {ready.sysop_id, revision, preference, base_utc + revision + 10});
        require(changed.succeeded() && changed.revision == revision + 1,
            "typed transfer preference update succeeds");
        revision = *changed.revision;
        record = database.transfer_preference(ready.sysop_id);
        require(record && record.value->preference == preference && record.value->revision == revision,
            "typed transfer preference read returns exact updated code");
    }
    require(database.update_transfer_preference({ready.sysop_id, revision - 1,
        app::TransferProtocolPreference::xmodem, base_utc + 100}).status.code ==
        app::StorageErrorCode::conflict, "stale preference revision is rejected");
    require(database.update_transfer_preference({ready.sysop_id, revision,
        static_cast<app::TransferProtocolPreference>(255), base_utc + 101}).status.code ==
        app::StorageErrorCode::invalid_argument, "invalid typed transfer code is rejected");
    const auto database_path = database.database_path();
    require(database.shutdown().succeeded(), "preference storage closes for constraint checks");
    internal::SqliteConnection raw(database_path, SQLITE_OPEN_READWRITE, 100ms, true);
    bool rejected = false;
    try {
        raw.exec("UPDATE sfx_account_transfer_preferences SET preference_code='YMODEM-G' WHERE account_id=1",
            "reject invalid physical transfer code");
    } catch (const internal::StorageFailure& failure) {
        rejected = failure.code() == app::StorageErrorCode::constraint;
    }
    require(rejected, "database CHECK rejects noncanonical transfer code");
}

void test_notice_claim_ack_and_pending_delivery(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "notices", policy);
    auto& database = *ready.database;
    const app::AccountId account_id = ready.sysop_id;

    require(database.enqueue_failed_login_notice(notice(100, account_id, base_utc + 100)).succeeded(),
        "first notice enqueues");
    require(database.enqueue_failed_login_notice(notice(100, account_id, base_utc + 100)).succeeded(),
        "identical enqueue retry is idempotent");
    require(database.enqueue_failed_login_notice(notice(100, account_id, base_utc + 101)).code ==
        app::StorageErrorCode::constraint, "same notice ID with different payload is rejected");
    for (std::uint32_t sequence = 101; sequence <= 105; ++sequence) {
        require(database.enqueue_failed_login_notice(notice(
            sequence, account_id, base_utc + sequence,
            sequence == 105 ? "2001:db8::5" : "192.0.2.1")).succeeded(),
            "bounded notice enqueue succeeds");
    }

    start_session(database, account_id, 600, base_utc + 499);
    start_session(database, account_id, 601, base_utc + 499);
    start_session(database, account_id, 602, base_utc + 499);
    const auto first_claim = claim(account_id, 500, 600, base_utc + 500);
    const auto batch = database.claim_failed_login_notices(first_claim);
    require(batch && batch.value->notices.size() == 5,
        "overflow retains and claims exactly five notices");
    require(batch.value->notices.front().event_utc_seconds == base_utc + 105 &&
        batch.value->notices.back().event_utc_seconds == base_utc + 101,
        "overflow deterministically retains newest five notices");
    require(batch.value->notices.front().source_ip == "2001:db8::5",
        "claimed binary IPv6 returns canonical text");
    require(database.enqueue_failed_login_notice(notice(
        106, account_id, base_utc + 106)).succeeded(),
        "sixth notice during an active full claim remains pending");
    require(database.enqueue_failed_login_notice(notice(
        106, account_id, base_utc + 106)).succeeded(),
        "uncertain immediate enqueue retry is idempotent");
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE account_id=1 AND claim_id IS NOT NULL",
        "count immutable active batch") == 5 && scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE account_id=1 AND claim_id IS NULL",
        "count pending notice during active batch") == 1,
        "one pending unread notice coexists with five immutable in-flight notices");
    const auto retried = database.claim_failed_login_notices(first_claim);
    require(retried && retried.value->notices.size() == 5 &&
        retried.value->notices.front().notice_id == batch.value->notices.front().notice_id &&
        retried.value->notices.back().notice_id == batch.value->notices.back().notice_id,
        "same claim token deterministically returns the immutable active batch");
    require(database.claim_failed_login_notices(
        claim(account_id, 500, 601, base_utc + 501)).status.code ==
        app::StorageErrorCode::conflict, "claim token cannot move to another session");
    const auto no_duplicate = database.claim_failed_login_notices(
        claim(account_id, 501, 601, base_utc + 501));
    require(no_duplicate.status.code == app::StorageErrorCode::conflict,
        "second session cannot split a still-active account delivery");
    require(database.acknowledge_failed_login_notices(
        {account_id, identifier(500), identifier(601)}).code == app::StorageErrorCode::conflict,
        "wrong session cannot acknowledge another claim");
    require(database.acknowledge_failed_login_notices(
        {account_id, identifier(500), identifier(600)}).succeeded(),
        "owning session acknowledges claimed batch");
    require(database.acknowledge_failed_login_notices(
        {account_id, identifier(500), identifier(600)}).succeeded(),
        "uncertain acknowledgement retry is idempotent");
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE account_id=1",
        "count notice after acknowledgement") == 1,
        "acknowledgement removes only the claimed batch");
    const auto later = database.claim_failed_login_notices(
        claim(account_id, 502, 602, base_utc + 502));
    require(later && later.value->notices.size() == 1 &&
        later.value->notices.front().notice_id == identifier(106),
        "later successful session claims the pending sixth notice");
    require(database.acknowledge_failed_login_notices(
        {account_id, identifier(502), identifier(602)}).succeeded(),
        "later pending delivery acknowledges independently");
    require(database.shutdown().succeeded(), "notice storage closes");
}

void test_pending_overflow_and_abandoned_recovery(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "pending-overflow-recovery", policy);
    auto& database = *ready.database;
    const app::AccountId account_id = ready.sysop_id;

    for (std::uint32_t sequence = 200; sequence <= 204; ++sequence) {
        require(database.enqueue_failed_login_notice(notice(
            sequence, account_id, base_utc + sequence)).succeeded(),
            "recovery fixture in-flight notice enqueues");
    }
    start_session(database, account_id, 700, base_utc + 499);
    start_session(database, account_id, 701, base_utc + 499);
    const auto active_request = claim(account_id, 600, 700, base_utc + 500);
    const auto active_batch = database.claim_failed_login_notices(active_request);
    require(active_batch && active_batch.value->notices.size() == 5,
        "recovery fixture claims immutable five");

    for (std::uint32_t sequence = 205; sequence <= 212; ++sequence) {
        require(database.enqueue_failed_login_notice(notice(
            sequence, account_id, base_utc + sequence)).succeeded(),
            "pending overflow event enqueues during active claim");
    }
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE account_id=1 AND claim_id IS NOT NULL",
        "count active rows with pending overflow") == 5 && scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE account_id=1 AND claim_id IS NULL",
        "count bounded pending overflow") == 5,
        "storage is bounded to five in-flight plus five unread rows");
    require(scalar(database,
        "SELECT min(event_utc_seconds) FROM sfx_failed_login_notices "
        "WHERE account_id=1 AND claim_id IS NULL",
        "verify oldest retained pending event") == base_utc + 208,
        "unclaimed overflow deterministically retains newest five");
    require(database.enqueue_failed_login_notice(notice(
        205, account_id, base_utc + 205)).succeeded(),
        "retry of an overflow-discarded notice remains an idempotent success");
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE notice_id=x'" +
            identifier_hex(205) + "'",
        "verify overflow retry does not resurrect notice") == 0,
        "overflowed notice cannot reappear while its enqueue receipt is retained");
    const auto active_retry = database.claim_failed_login_notices(active_request);
    require(active_retry && active_retry.value->notices.size() == 5 &&
        active_retry.value->notices.front().notice_id ==
            active_batch.value->notices.front().notice_id &&
        active_retry.value->notices.back().notice_id ==
            active_batch.value->notices.back().notice_id,
        "pending overflow never mutates the original active batch");

    const auto early = database.recover_abandoned_failed_login_claims(
        {base_utc + 799, 256});
    require(early && *early.value == 0, "unexpired five-row claim is not recovered");
    const auto recovered = database.recover_abandoned_failed_login_claims(
        {base_utc + 800, 256});
    require(recovered && *recovered.value == 5,
        "expired five-row claim releases atomically at 300 seconds");
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE account_id=1 AND claim_id IS NULL",
        "count merged recovered unread queue") == 5 && scalar(database,
        "SELECT min(event_utc_seconds) FROM sfx_failed_login_notices WHERE account_id=1",
        "verify merged recovery newest-five floor") == base_utc + 208,
        "recovery merges then deterministically reduces unread rows to newest five");
    const auto terminal_retry = database.claim_failed_login_notices(active_request);
    require(terminal_retry && terminal_retry.value->notices.empty(),
        "recovered claim token cannot reselect newer unread events");
    const auto after_recovery = database.claim_failed_login_notices(
        claim(account_id, 601, 701, base_utc + 801));
    require(after_recovery && after_recovery.value->notices.size() == 5 &&
        after_recovery.value->notices.front().event_utc_seconds == base_utc + 212 &&
        after_recovery.value->notices.back().event_utc_seconds == base_utc + 208,
        "new token claims exact deterministic newest-five recovery result");
    require(database.shutdown().succeeded(), "pending-overflow recovery storage closes");
}

void test_idempotency_receipt_bounds(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "idempotency-bounds", policy);
    auto& database = *ready.database;
    const app::AccountId account_id = ready.sysop_id;

    for (std::uint32_t sequence = 0; sequence < 70; ++sequence) {
        require(database.enqueue_failed_login_notice(notice(
            3000 + sequence, account_id, base_utc + 3000 + sequence)).succeeded(),
            "bounded enqueue receipt fixture succeeds");
    }
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notice_enqueues WHERE account_id=1",
        "count bounded enqueue receipts") ==
        static_cast<std::int64_t>(app::kFailedLoginEnqueueReceiptMaximum),
        "enqueue idempotency history is strictly bounded to 64 per account");
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE account_id=1 AND claim_id IS NULL",
        "count unread rows after idempotency pressure") == 5,
        "unread notice queue remains independently bounded under receipt pressure");
    require(database.enqueue_failed_login_notice(notice(
        3010, account_id, base_utc + 3010)).succeeded(),
        "retained overflow enqueue receipt makes later uncertain retry idempotent");
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE notice_id=x'" +
            identifier_hex(3010) + "'",
        "verify retained overflow receipt prevents resurrection") == 0,
        "retained overflow receipt prevents a discarded notice from reappearing");

    for (std::uint32_t sequence = 0; sequence < 70; ++sequence) {
        const std::uint32_t session_sequence = 5000 + sequence;
        const std::uint32_t claim_sequence = 6000 + sequence;
        start_session(database, account_id, session_sequence, base_utc + 4000 + sequence);
        const auto result = database.claim_failed_login_notices(claim(
            account_id, claim_sequence, session_sequence, base_utc + 5000 + sequence));
        require(result.succeeded(), "bounded terminal claim receipt fixture succeeds");
        if (!result.value->notices.empty()) {
            require(database.acknowledge_failed_login_notices(
                {account_id, identifier(claim_sequence), identifier(session_sequence)}).succeeded(),
                "nonempty bounded receipt fixture acknowledges");
        }
    }
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notice_claims "
        "WHERE account_id=1 AND claim_state<>'active'",
        "count bounded terminal claim receipts") ==
        static_cast<std::int64_t>(app::kFailedLoginTerminalClaimReceiptMaximum),
        "terminal claim idempotency history is strictly bounded to 64 per account");
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notice_claims "
        "WHERE account_id=1 AND claim_state='active'",
        "count active claims after terminal receipt pressure") == 0,
        "receipt pressure leaves no accidental active claim");
    require(database.shutdown().succeeded(), "idempotency-bound storage closes");
}

void test_concurrent_enqueue_and_crash_retry(const std::filesystem::path& root) {
    const auto directory = root / "concurrency";
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(directory, policy);
    auto& database = *ready.database;
    std::atomic<std::uint32_t> failures{0};
    std::vector<std::thread> workers;
    for (std::uint32_t index = 0; index < 32; ++index) {
        workers.emplace_back([&database, &failures, account_id = ready.sysop_id, index] {
            if (!database.enqueue_failed_login_notice(notice(
                    1000U + index, account_id, base_utc + 1000 + index,
                    "198.51.100." + std::to_string(index + 1))).succeeded()) {
                ++failures;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    require(failures.load() == 0, "concurrent serialized enqueues all succeed");
    start_session(database, ready.sysop_id, 1300, base_utc + 1199);
    start_session(database, ready.sysop_id, 1301, base_utc + 1199);
    const auto claim_a = claim(ready.sysop_id, 1200, 1300, base_utc + 1200);
    const auto claim_b = claim(ready.sysop_id, 1201, 1301, base_utc + 1200);
    app::StorageResult<app::FailedLoginNoticeBatch> result_a;
    app::StorageResult<app::FailedLoginNoticeBatch> result_b;
    std::thread claimant_a([&] { result_a = database.claim_failed_login_notices(claim_a); });
    std::thread claimant_b([&] { result_b = database.claim_failed_login_notices(claim_b); });
    claimant_a.join();
    claimant_b.join();
    const bool a_won = result_a && result_a.value->notices.size() == 5 &&
        result_b.status.code == app::StorageErrorCode::conflict;
    const bool b_won = result_b && result_b.value->notices.size() == 5 &&
        result_a.status.code == app::StorageErrorCode::conflict;
    require(a_won != b_won,
        "concurrent claims produce exactly one complete winner and one conflict");
    const auto& winning_request = a_won ? claim_a : claim_b;
    const auto& batch = a_won ? result_a : result_b;
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notice_claims WHERE account_id=1 AND claim_state='active'",
        "count one active claim after concurrency") == 1 && scalar(database,
        "SELECT count(DISTINCT claim_id) FROM sfx_failed_login_notices WHERE account_id=1 AND claim_id IS NOT NULL",
        "count one claimed batch after concurrency") == 1,
        "concurrent claims neither duplicate nor split delivery");
    for (std::size_t index = 0; index < 5; ++index) {
        require(batch.value->notices[index].event_utc_seconds ==
            base_utc + 1031 - static_cast<std::int64_t>(index),
            "concurrent overflow order is deterministic");
    }
    require(database.shutdown().succeeded(), "claimed batch persists across simulated crash");

    auto reopen_settings = settings_for(directory);
    storage::SqliteStorage reopened(
        reopen_settings, policy, storage::sfx_storage_migrations());
    require(reopened.start().succeeded(), "storage reopens after unacknowledged claim");
    const auto retried = reopened.claim_failed_login_notices(winning_request);
    require(retried && retried.value->notices.size() == 5 &&
        retried.value->notices.front().notice_id == batch.value->notices.front().notice_id,
        "same token after crash returns the original claim without duplicate selection");
    require(reopened.shutdown().succeeded(), "crash-retry storage closes");
}

void test_session_delete_releases_active_claim(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy policy;
    ReadyStorage ready = create_ready_storage(root / "session-delete", policy);
    auto& database = *ready.database;
    const app::AccountId account_id = ready.sysop_id;

    for (std::uint32_t sequence = 300; sequence <= 304; ++sequence) {
        require(database.enqueue_failed_login_notice(notice(
            sequence, account_id, base_utc + sequence)).succeeded(),
            "session-delete in-flight notice enqueues");
    }
    start_session(database, account_id, 800, base_utc + 499);
    const auto active_request = claim(account_id, 700, 800, base_utc + 500);
    require(database.claim_failed_login_notices(active_request).value->notices.size() == 5,
        "session-delete fixture claims five notices");
    for (std::uint32_t sequence = 305; sequence <= 309; ++sequence) {
        require(database.enqueue_failed_login_notice(notice(
            sequence, account_id, base_utc + sequence)).succeeded(),
            "session-delete pending notice enqueues");
    }
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE account_id=1",
        "count session-delete precondition rows") == 10,
        "session-delete fixture contains bounded in-flight plus unread sets");

    delete_session(database, 800);
    require(scalar(database, "SELECT count(*) FROM pragma_foreign_key_check",
        "verify session-delete foreign keys") == 0,
        "owning-session deletion leaves foreign-key checks clean");
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE account_id=1 AND claim_id IS NOT NULL",
        "verify session-delete releases claim") == 0 && scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notices WHERE account_id=1 AND claim_id IS NULL",
        "verify session-delete bounded unread") == 5,
        "session deletion releases then reduces undelivered rows to newest five");
    require(scalar(database,
        "SELECT min(event_utc_seconds) FROM sfx_failed_login_notices WHERE account_id=1",
        "verify session-delete newest-five floor") == base_utc + 305,
        "session deletion preserves deterministic newest-five pending notices");
    require(scalar(database,
        "SELECT count(*) FROM sfx_failed_login_notice_claims WHERE claim_id=x'" +
            identifier_hex(700) + "'",
        "verify session-delete claim receipt removal") == 0,
        "session deletion removes the resolved claim receipt");
    require(database.claim_failed_login_notices(active_request).status.code ==
        app::StorageErrorCode::conflict,
        "deleted session and old token cannot reclaim released notices");

    start_session(database, account_id, 801, base_utc + 599);
    const auto replacement = database.claim_failed_login_notices(
        claim(account_id, 701, 801, base_utc + 600));
    require(replacement && replacement.value->notices.size() == 5 &&
        replacement.value->notices.front().event_utc_seconds == base_utc + 309 &&
        replacement.value->notices.back().event_utc_seconds == base_utc + 305,
        "later session claims exact newest-five rows preserved by session pruning");
    require(database.shutdown().succeeded(), "session-delete storage closes");
}

void test_tombstone_constraints_busy_and_failures(const std::filesystem::path& root) {
    const auto directory = root / "tombstone";
    test::FakeStoragePathPolicy policy;
    auto busy_settings = settings_for(directory);
    busy_settings.busy.sqlite_timeout = 10ms;
    busy_settings.busy.transaction_deadline = 100ms;
    busy_settings.busy.maximum_attempts = 2;
    ReadyStorage ready = create_ready_storage(directory, policy, busy_settings);
    auto& database = *ready.database;
    const auto caller = create_account(database, "Notice Target", 2000, base_utc + 2000);
    require(caller.succeeded(), "tombstone fixture account creates");
    require(database.enqueue_failed_login_notice(notice(
        2001, *caller.account_id, base_utc + 2001)).succeeded(),
        "tombstone fixture notice enqueues");

    internal::SqliteConnection lock(database.database_path(), SQLITE_OPEN_READWRITE, 10ms, true);
    lock.exec("BEGIN IMMEDIATE", "hold Window 1 busy lock");
    require(database.enqueue_failed_login_notice(notice(
        2002, *caller.account_id, base_utc + 2002)).code == app::StorageErrorCode::busy,
        "bounded busy handling reports busy without partial enqueue");
    lock.exec("ROLLBACK", "release Window 1 busy lock");
    require(database.enqueue_failed_login_notice(notice(
        2002, *caller.account_id, base_utc + 2002)).succeeded(),
        "busy operation retries safely after lock release");

    require(database.enqueue_failed_login_notice(notice(
        2003, *caller.account_id, base_utc + 2003, "999.1.1.1")).code ==
        app::StorageErrorCode::invalid_argument, "out-of-range IPv4 is rejected");
    require(database.enqueue_failed_login_notice(notice(
        2004, *caller.account_id, app::kMaximumUtcSeconds + 1)).code ==
        app::StorageErrorCode::invalid_argument, "invalid notice timestamp is rejected");
    start_session(database, ready.sysop_id, 2100, base_utc + 2004);
    start_session(database, *caller.account_id, 2200, base_utc + 2004);
    require(database.claim_failed_login_notices(
        claim(*caller.account_id, 2201, 2200, base_utc + 2005)).value->notices.size() == 2,
        "tombstone fixture creates an active claim receipt");
    require(database.enqueue_failed_login_notice(notice(
        2202, *caller.account_id, base_utc + 2006)).succeeded(),
        "tombstone fixture retains a separate unread notice");
    finalize_session(database, 2200, base_utc + 2010);

    app::TombstoneAccountRequest tombstone;
    tombstone.account_id = *caller.account_id;
    tombstone.expected_revision = *caller.revision;
    tombstone.reason = app::AuditReasonCode::sysop_action;
    tombstone.audit = audit(2203, base_utc + 2011, app::AuditReasonCode::sysop_action);
    require(database.tombstone_account(tombstone).succeeded(),
        "account tombstone atomically removes Phase 3A private rows");
    require(database.transfer_preference(*caller.account_id).status.code ==
        app::StorageErrorCode::not_found, "tombstoned transfer preference is removed");
    internal::SqliteConnection inspect(database.database_path(), SQLITE_OPEN_READONLY, 100ms, false);
    require(inspect.scalar_int64("SELECT count(*) FROM sfx_failed_login_notices WHERE account_id=2",
        "verify tombstone notice deletion") == 0, "tombstone deletes every associated notice");
    require(inspect.scalar_int64("SELECT count(*) FROM sfx_failed_login_notice_claims WHERE account_id=2",
        "verify tombstone claim receipt deletion") == 0,
        "tombstone deletes every associated claim receipt");
    require(inspect.scalar_int64("SELECT count(*) FROM sfx_failed_login_notice_enqueues WHERE account_id=2",
        "verify tombstone enqueue receipt deletion") == 0,
        "tombstone deletes every associated enqueue idempotency receipt");
    inspect.close();
    const auto database_path = database.database_path();
    require(database.shutdown().succeeded(), "tombstone storage closes");
    require(database.enqueue_failed_login_notice(notice(
        2204, ready.sysop_id, base_utc + 2012)).code == app::StorageErrorCode::shutting_down,
        "storage operation after shutdown fails closed");

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
        "INSERT INTO sfx_failed_login_notices(notice_id,account_id,source_address_family,source_address,event_utc_seconds)"
        " VALUES(randomblob(16),1,4,x'010203',1788100000)",
        "reject malformed binary IP"), "schema rejects malformed source-address length");
    require(rejects_constraint(
        "INSERT INTO sfx_failed_login_notices(notice_id,account_id,source_address_family,source_address,event_utc_seconds,claim_id)"
        " VALUES(randomblob(16),1,4,x'01020304',1788100000,randomblob(16))",
        "reject partial claim"), "schema rejects partial claim state");
    require(rejects_constraint(
        "INSERT INTO sfx_failed_login_notice_claims(claim_id,account_id,session_id,claim_state,"
        "claimed_utc_seconds,claim_expires_utc_seconds) VALUES("
        "x'53464e47000000000000000000000835',1,x'53464e47000000000000000000000834',"
        "'active',1788100000,1788100200)",
        "reject wrong claim expiry"), "claim receipt enforces exact 300-second lease");
    require(internal::map_sqlite_error(SQLITE_FULL) == app::StorageErrorCode::full,
        "accepted SQLite error map preserves disk-full reporting for Window 1 writes");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Phase 3A Window 1 tests require an output directory");
        const std::filesystem::path root = std::filesystem::absolute(argv[1]);
        remove_tree(root);
        std::filesystem::create_directories(root);
        std::cout << "phase3a-window1 portable-contracts\n";
        test_portable_contracts_and_fake();
        std::cout << "phase3a-window1 transfer-preferences\n";
        test_transfer_preferences_and_constraints(root);
        std::cout << "phase3a-window1 notice-claim-pending-delivery\n";
        test_notice_claim_ack_and_pending_delivery(root);
        std::cout << "phase3a-window1 pending-overflow-recovery\n";
        test_pending_overflow_and_abandoned_recovery(root);
        std::cout << "phase3a-window1 idempotency-receipt-bounds\n";
        test_idempotency_receipt_bounds(root);
        std::cout << "phase3a-window1 concurrency-crash-retry\n";
        test_concurrent_enqueue_and_crash_retry(root);
        std::cout << "phase3a-window1 session-delete-release\n";
        test_session_delete_releases_active_claim(root);
        std::cout << "phase3a-window1 tombstone-constraints-failures\n";
        test_tombstone_constraints_busy_and_failures(root);
        remove_tree(root);
        std::cout << "all SFX Phase 3A Window 1 storage tests passed assertions="
                  << assertion_count << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Phase 3A Window 1 storage test failure: " << error.what() << '\n';
        return 1;
    }
}
