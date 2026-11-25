#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_sfx_baseline.hpp"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
namespace app = sfx::application;
namespace db = sfx::adapters::storage;
namespace internal = db::internal;
unsigned checks{};
void check(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
app::Identifier128 id(unsigned n) { app::Identifier128 v; v.bytes[0] = 1;
    v.bytes[12] = static_cast<std::uint8_t>(n >> 24); v.bytes[13] = static_cast<std::uint8_t>(n >> 16);
    v.bytes[14] = static_cast<std::uint8_t>(n >> 8); v.bytes[15] = static_cast<std::uint8_t>(n); return v; }
app::AuditContext audit(unsigned n) { return {id(n), id(n + 10000), {}, 1788990000, app::AuditReasonCode::initial_setup}; }
app::CredentialEnvelope credential() {
    app::CredentialEnvelope c; c.envelope_format_version = 1; c.scheme_identifier = "journal-fixture";
    c.parameter_encoding_version = 1; c.canonical_parameters = {1}; c.salt = {1, 2}; c.verifier = {3, 4};
    c.created_utc_seconds = 1788990000; return c;
}
void fresh(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    sfx::test::FakeStoragePathPolicy paths;
    db::SqliteStorageSettings settings; settings.data_directory = root; settings.utc_now = [] { return 1788990000; };
    db::SqliteStorage storage(settings, paths);
    check(storage.start().succeeded(), "fresh journal storage startup");
check(storage.health().schema_version == 27, "exact current schema with classic information");
    app::StageInitialSysopRequest stage; stage.display_name = "Journal Sysop"; stage.credential = credential(); stage.audit = audit(1);
    const auto staged = storage.stage_initial_sysop(stage); check(staged && staged.account_id, "fixture account stages");
    const auto policy = storage.security_policy(); check(policy && policy.value, "fixture policy readable");
    app::CompleteFirstRunRequest complete{*staged.account_id, policy.value->revision, policy.value->policy_hash,
        policy.value->policy_hash, audit(2)};
    check(storage.complete_first_run(complete).succeeded(), "fixture ready");
    std::array<app::StartSessionRequest, 4> requests;
    std::array<bool, 4> success{};
    std::array<std::thread, 4> workers;
    for (unsigned n = 0; n < 4; ++n) {
        requests[n] = {id(100 + n), id(200 + n), *staged.account_id, static_cast<std::uint16_t>(n + 1), 1788990000};
        requests[n].journal_transport = n % 2 ? "SSH" : "Telnet";
        requests[n].journal_source_ip = "127.0.0.1"; requests[n].journal_ansi = static_cast<int>(n % 2);
        workers[n] = std::thread([&, n] { success[n] = storage.record_session_started(requests[n]).succeeded(); });
    }
    for (auto& worker : workers) worker.join();
    for (const auto ok : success) check(ok, "four simultaneous atomic session allocations");
    auto pending = storage.pending_caller_journals(true, 16);
    check(pending && pending.value && pending.value->size() == 4, "four durable caller records");
    for (std::size_t i = 0; i < 4; ++i) check((*pending.value)[i].caller_number == static_cast<std::int64_t>(i + 1), "globally contiguous caller numbers");
    check(storage.record_session_started(requests[0]).replayed, "session replay has no second caller number");
    auto altered = requests[0]; altered.journal_transport = "SSH";
    check(!storage.record_session_started(altered), "replay cannot change immutable transport");
    altered = requests[0]; altered.journal_source_ip = "127.0.0.2";
    check(!storage.record_session_started(altered), "replay cannot change immutable source");
    altered = requests[0]; altered.journal_ansi = 1;
    check(!storage.record_session_started(altered), "replay cannot change graphics selection");
    altered = requests[0]; altered.journal_new_registration = true; altered.journal_registration_utc = 1788990000;
    check(!storage.record_session_started(altered), "replay cannot invent registration commit");
    check(storage.find_account_by_id(*staged.account_id).value->statistics.successful_login_count == 4, "authoritative totals unchanged by replay");
    check(!storage.pending_caller_journals(true, 17), "query page bounded");
    const auto session = pending.value->front().session.session_id;
    check(storage.append_caller_activity(session, {app::CallerActivityKind::main_menu, 1788990001, {}}, "localhost").succeeded(), "typed activity retained");
    check(storage.append_caller_activity(session, {app::CallerActivityKind::main_menu, 1788990001, {}}, "localhost").succeeded(), "activity replay idempotent");
    check(!storage.append_caller_activity(session, {app::CallerActivityKind::main_menu, 1788990001, "secret"}, ""), "arbitrary text cannot enter current activity");
    check(storage.finalize_session({session, 8, 1788990008, app::SessionOutcomeCode::normal, id(300)}).succeeded(), "authoritative finalization");
    check(!storage.append_caller_activity(session, {app::CallerActivityKind::main_menu, 1788990009, {}}, ""), "late activity cannot alter finalized assignment");
    auto final = storage.pending_caller_journals(false, 16);
    check(final && final.value && final.value->size() == 1 && final.value->front().activities.size() == 2, "one finalized journal with nonduplicate activity");
    const auto number = final.value->front().caller_number;
    app::CallerJournalPublication intent{number, "1:2", 0, "fixture complete\r\n\r\n---\r\n"};
    check(storage.prepare_caller_publication(intent).succeeded(), "publication intent durable");
    check(storage.prepare_caller_publication(intent).succeeded(), "publication intent exact replay");
    auto changed = intent; changed.block = "different\r\n";
    check(!storage.prepare_caller_publication(changed), "changed append intent rejected");
    check(storage.shutdown().succeeded(), "clean storage stop with retained append intent");
    db::SqliteStorage reopened(settings, paths);
    check(reopened.start().succeeded(), "exact journal schema reopens");
    auto recovered_intent = reopened.caller_publication();
    check(recovered_intent && recovered_intent.value && *recovered_intent.value &&
        (**recovered_intent.value).block == intent.block, "append intent survives Runtime-style restart");
    check(reopened.acknowledge_caller_publication(number).succeeded(), "publication acknowledgement commits");
    check(reopened.pending_caller_journals(false, 16).value->empty(), "acknowledged block never requeued");
    check(reopened.pending_caller_journals(true, 16).value->size() == 3, "remaining independent incomplete callers retained");
    check(reopened.shutdown().succeeded(), "reopened storage clean stop");
}
void migration(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto file = root / "SFX.DB";
    {
        internal::SqliteConnection seed(file, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, std::chrono::milliseconds(100), true);
        seed.exec(internal::sfx_initial_baseline_migration().sql, "test-only exact legacy database");
        seed.exec("INSERT INTO sfx_schema_migrations VALUES(1,'sfx-v1-initial-persistence-baseline',"
            "'c9ed2791ec70d18cb69696f837d27cc74f931d7c3253247768b7245bf0864806',1788990000)", "legacy ledger");
        seed.exec("PRAGMA application_id=1397119042", "legacy identity");
        seed.exec("PRAGMA user_version=1", "legacy version");
        seed.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) "
            "VALUES(1,'Historical Caller','historical caller',10,'active',1,1,1788980000,1788980000);"
            "INSERT INTO sfx_account_statistics(account_id,successful_login_count) VALUES(1,9);"
            "INSERT INTO sfx_account_sessions(session_id,account_id,node_id,started_utc_seconds,last_persisted_utc_seconds,completed_utc_seconds,outcome_code,correlation_id) "
            "VALUES(x'01000000000000000000000000000001',1,1,1788980000,1788980010,1788980010,'normal',x'01000000000000000000000000000002');",
            "legacy retained session plus authoritative aggregate fixture");
        check(internal::validate_sfx_baseline_database(seed.get(), true), "unchanged accepted v1 fixture");
    }
    sfx::test::FakeStoragePathPolicy paths;
    db::SqliteStorageSettings settings; settings.data_directory = root;
    settings.bootstrap_boundary_hook = [](std::string_view point) {
        return point == "before-journal-migration-commit" ? app::StorageStatus{app::StorageErrorCode::io, "fixture migration interruption"} : app::StorageStatus::success();
    };
    { db::SqliteStorage refused(settings, paths); check(!refused.start(), "migration failure injected"); }
    {
        internal::SqliteConnection old(file, SQLITE_OPEN_READONLY, std::chrono::milliseconds(100), false);
        check(internal::validate_sfx_baseline_database(old.get(), true), "failed upgrade preserves exact old schema and ledger");
    }
    settings.bootstrap_boundary_hook = {};
    db::SqliteStorage upgraded(settings, paths);
    check(upgraded.start().succeeded(), "old shipping database upgrades safely on retry");
check(upgraded.health().schema_version == 27, "retry completes current shipping version");
    check(upgraded.pending_caller_journals(false, 16).value->empty() && upgraded.pending_caller_journals(true, 16).value->empty(),
        "migration never manufactures historical detailed blocks");
    app::StartSessionRequest current{id(900), id(901), 1, 1, 1788990000};
    check(upgraded.record_session_started(current).succeeded(), "first post-upgrade session begins");
    const auto numbered = upgraded.pending_caller_journals(true, 16);
    check(numbered && numbered.value && numbered.value->size() == 1 && numbered.value->front().caller_number == 10,
        "seed is maximum of retained sessions and successful-login totals");
    check(upgraded.shutdown().succeeded(), "upgraded storage clean shutdown");
}
}
int main() {
    try {
        const auto parent = std::filesystem::current_path() / "caller-journal-fixtures" /
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fresh(parent / "fresh"); migration(parent / "upgrade");
        std::cout << "PASS caller journal storage: " << checks << " checks; 0 failed; 0 skipped\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL after " << checks << ": " << error.what() << '\n'; return 1; }
}
