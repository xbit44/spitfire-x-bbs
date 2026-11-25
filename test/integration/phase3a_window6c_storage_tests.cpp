#include "sfx/adapters/authentication/phase3a_authentication_adapters.hpp"
#include "sfx/adapters/authentication/sodium_authentication.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/application/display.hpp"
#include "sfx/application/phase3a_private_adapters.hpp"
#include "sfx/test/account_storage_fakes.hpp"
#include "sfx/test/authentication_fakes.hpp"
#include "sfx/test/foundation_fakes.hpp"
#include "sfx/test/sqlite_write_fault_vfs.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sfx/test/terminal_stream_fake.hpp"
#include "sqlite_internal.hpp"
#include "sqlite3.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace app = sfx::application;
namespace authentication = sfx::adapters::authentication;
namespace storage = sfx::adapters::storage;
namespace internal = sfx::adapters::storage::internal;
namespace test = sfx::test;
using namespace std::chrono_literals;

constexpr std::int64_t kBaseUtc = 1'788'300'000LL;
std::size_t assertions = 0;

class CountingWelcomeStatistics final
    : public app::ICallerWelcomeStatisticsPort {
public:
    app::StorageResult<std::uint64_t>
    record_successful_call_and_count_today() override {
        return {app::StorageStatus::success(), ++count_};
    }

private:
    std::atomic<std::uint64_t> count_{0};
};

void require(const bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

template <typename HealthSnapshot>
app::PasswordKdfHealth wait_for_kdf_quiescence(
    HealthSnapshot&& snapshot, const std::uint64_t expected_completed_jobs,
    const std::chrono::steady_clock::duration timeout,
    std::ostream& diagnostics) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto health = snapshot();
        if (health.active_jobs == 0 && health.queued_jobs == 0 &&
            health.completed_jobs == expected_completed_jobs) {
            return health;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            diagnostics << "KDF_HEALTH_TIMEOUT"
                        << " expected_completed_jobs=" << expected_completed_jobs
                        << " maximum_active_jobs=" << health.maximum_active_jobs
                        << " queue_capacity=" << health.queue_capacity
                        << " active_jobs=" << health.active_jobs
                        << " queued_jobs=" << health.queued_jobs
                        << " active_high_water=" << health.active_high_water
                        << " queue_high_water=" << health.queue_high_water
                        << " estimated_memory_high_water_bytes="
                        << health.estimated_memory_high_water_bytes
                        << " completed_jobs=" << health.completed_jobs
                        << " rejected_jobs=" << health.rejected_jobs
                        << " cancelled_jobs=" << health.cancelled_jobs
                        << " accepting=" << health.accepting << '\n';
            throw std::runtime_error(
                "bounded KDF health did not reach expected quiescence");
        }
        std::this_thread::sleep_for(1ms);
    }
}

void kdf_quiescence_waiter_contract() {
    app::PasswordKdfHealth wrong;
    wrong.maximum_active_jobs = app::kMaximumConcurrentKdfJobs;
    wrong.queue_capacity = 128U;
    wrong.active_high_water = app::kMaximumConcurrentKdfJobs;
    wrong.queue_high_water = 17U;
    wrong.estimated_memory_high_water_bytes = 64U * 1024U * 1024U;
    wrong.completed_jobs = 130U;
    wrong.rejected_jobs = 3U;
    wrong.cancelled_jobs = 5U;
    wrong.accepting = true;
    std::ostringstream diagnostics;
    bool rejected_wrong_total = false;
    try {
        (void)wait_for_kdf_quiescence(
            [&wrong] { return wrong; }, 131U, 0ms, diagnostics);
    } catch (const std::runtime_error&) {
        rejected_wrong_total = true;
    }
    require(rejected_wrong_total,
        "bounded KDF health wait cannot accept an incorrect completed-job total");

    const auto text = diagnostics.str();
    constexpr const char* fields[] = {"expected_completed_jobs=",
        "maximum_active_jobs=", "queue_capacity=", "active_jobs=",
        "queued_jobs=", "active_high_water=", "queue_high_water=",
        "estimated_memory_high_water_bytes=", "completed_jobs=",
        "rejected_jobs=", "cancelled_jobs=", "accepting="};
    bool complete_diagnostics = text.find("KDF_HEALTH_TIMEOUT") !=
        std::string::npos;
    for (const auto* field : fields) {
        complete_diagnostics = complete_diagnostics &&
            text.find(field) != std::string::npos;
    }
    require(complete_diagnostics,
        "bounded KDF health timeout reports every expected and health field");

    auto exact = wrong;
    exact.completed_jobs = 131U;
    std::ostringstream unused;
    const auto accepted = wait_for_kdf_quiescence(
        [&exact] { return exact; }, 131U, 0ms, unused);
    require(accepted.completed_jobs == 131U && unused.str().empty(),
        "bounded KDF health wait accepts only exact quiescent accounting");
}

void remove_tree(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    require(!error, "remove Window 6C storage test tree");
}

app::Identifier128 identifier(const std::uint32_t sequence) {
    app::Identifier128 value;
    value.bytes[0] = 0x53;
    value.bytes[1] = 0x46;
    value.bytes[2] = 0x58;
    value.bytes[3] = 0x43;
    value.bytes[12] = static_cast<std::uint8_t>((sequence >> 24U) & 0xffU);
    value.bytes[13] = static_cast<std::uint8_t>((sequence >> 16U) & 0xffU);
    value.bytes[14] = static_cast<std::uint8_t>((sequence >> 8U) & 0xffU);
    value.bytes[15] = static_cast<std::uint8_t>(sequence & 0xffU);
    return value;
}

app::AuditContext audit(const std::uint32_t sequence,
    const std::int64_t instant, const app::AuditReasonCode reason) {
    return {identifier(sequence), identifier(100'000U + sequence), std::nullopt,
        instant, reason};
}

app::CredentialEnvelope credential(const std::int64_t instant) {
    app::CredentialEnvelope value;
    value.envelope_format_version = 1;
    value.scheme_identifier = "argon2id13-window6c-test";
    value.parameter_encoding_version = 1;
    value.canonical_parameters = {1, 2, 3};
    value.salt = {4, 5, 6, 7};
    value.verifier = {8, 9, 10, 11};
    value.created_utc_seconds = instant;
    return value;
}

storage::SqliteStorageSettings settings_for(
    const std::filesystem::path& directory,
    const bool pressure = false) {
    storage::SqliteStorageSettings settings;
    settings.data_directory = directory;
    settings.utc_now = [] { return kBaseUtc; };
    settings.queue.maximum_operations = pressure ? 1U : 1024U;
    settings.queue.maximum_bytes = 4U * 1024U * 1024U;
    settings.queue.dequeue_batch_limit = 32;
    settings.queue.dequeue_batch_window = 1ms;
    settings.queue.enqueue_wait = pressure ? 1ms : 1000ms;
    settings.busy.sqlite_timeout = pressure ? 5ms : 50ms;
    settings.busy.transaction_deadline = pressure ? 40ms : 1000ms;
    settings.busy.maximum_attempts = pressure ? 1U : 3U;
    settings.checkpoint.passive_interval = std::chrono::hours(1);
    settings.readers.pool_size = 1;
    settings.readers.maximum_pending_operations = 1024;
    settings.readers.enqueue_wait = 1000ms;
    return settings;
}

struct ReadyStorage {
    std::unique_ptr<storage::SqliteStorage> database;
    app::AccountId account_id{0};
};

ReadyStorage create_ready(const std::filesystem::path& directory,
    test::FakeStoragePathPolicy& path_policy, const bool pressure = false) {
    std::filesystem::create_directories(directory);
    auto database = std::make_unique<storage::SqliteStorage>(
        settings_for(directory, pressure), path_policy, storage::sfx_storage_migrations());
    require(database->start().succeeded(), "Window 6C SQLite storage starts");
    app::StageInitialSysopRequest stage;
    stage.display_name = "Window Six C Sysop";
    stage.credential = credential(kBaseUtc);
    stage.audit = audit(1, kBaseUtc, app::AuditReasonCode::initial_setup);
    const auto staged = database->stage_initial_sysop(stage);
    require(staged.succeeded() && staged.account_id.has_value(),
        "Window 6C SQLite fixture stages initial account");
    const auto policy = database->security_policy();
    require(policy && policy.value.has_value(), "Window 6C security policy loads");
    app::CompleteFirstRunRequest complete;
    complete.staged_sysop_account_id = *staged.account_id;
    complete.expected_policy_revision = policy.value->revision;
    complete.expected_policy_hash = policy.value->policy_hash;
    complete.published_policy_hash = policy.value->policy_hash;
    complete.audit = audit(2, kBaseUtc + 1, app::AuditReasonCode::initial_setup);
    require(database->complete_first_run(complete).succeeded(),
        "Window 6C SQLite fixture publishes first-run policy");
    return {std::move(database), *staged.account_id};
}

app::AuthenticatedSessionCommitRequest request_for(
    storage::SqliteStorage& database, const app::AccountId account_id,
    const std::uint32_t sequence) {
    const auto account = database.find_account_by_id(account_id);
    const auto policy = database.security_policy();
    if (!account || !account.value.has_value() || !policy || !policy.value.has_value() ||
        !account.value->name_key.has_value() || !account.value->security_level.has_value()) {
        throw std::runtime_error("load Window 6C commit fixture authority");
    }
    app::AuthenticatedSessionCommitRequest request;
    request.session.session_id = identifier(1000U + sequence);
    request.session.correlation_id = identifier(2000U + sequence);
    request.session.account_id = account_id;
    request.session.node_id = static_cast<std::uint16_t>((sequence % 128U) + 1U);
    request.session.started_utc_seconds = kBaseUtc + 100 + sequence;
    request.successful_source = {identifier(3000U + sequence), account_id,
        "192.0.2." + std::to_string((sequence % 200U) + 1U),
        sequence % 2U == 0 ? app::AuthenticationProtocol::telnet
                           : app::AuthenticationProtocol::ssh,
        request.session.started_utc_seconds};
    request.expected_account_revision = account.value->revision;
    request.expected_account_name_key = *account.value->name_key;
    request.expected_security_level = *account.value->security_level;
    request.expected_security_policy_revision = policy.value->revision;
    request.expected_security_policy_hash = policy.value->policy_hash;
    return request;
}

std::int64_t scalar(
    const std::filesystem::path& database_path, const std::string& sql);

class MissingDisplayAssetReader final : public app::ITrustedDisplayAssetReader {
public:
    app::TrustedDisplayAssetReadResult read_asset(
        const std::string_view, const std::size_t) override {
        reads.fetch_add(1U, std::memory_order_relaxed);
        return {app::TrustedDisplayAssetReadCode::not_found, {}, {}, {}};
    }

    std::atomic<std::size_t> reads{0};
};

class DeterministicUtcRuleProvider final : public app::IBbsTimeZoneRuleProvider {
public:
    std::optional<app::BbsTimeZoneYearRules> rules_for_year(
        const std::string_view key, const std::int32_t year) const override {
        if (key != "UTC" || year < 1601 || year > 9999) return std::nullopt;
        return app::BbsTimeZoneYearRules{};
    }
};

app::CredentialEnvelope create_credential(
    app::AuthenticationSecurityService& service,
    const std::string_view password,
    const std::string& account_name,
    const app::CancellationToken& cancellation) {
    app::CredentialEnvelope output;
    const auto created = service.create_password_credential(password,
        {app::PasswordMode::standard, account_name, "Window 6C Test BBS"},
        kBaseUtc, false, cancellation, output);
    require(created.succeeded() && app::valid_credential_envelope(output),
        "accepted KDF creates a valid concrete composition credential");
    return output;
}

class SynchronizedPhaseGate final {
public:
    explicit SynchronizedPhaseGate(const std::size_t participants)
        : participants_(participants) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++arrived_;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
    }

    bool wait_until_all(
        const std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_until(lock, deadline,
            [this] { return arrived_ == participants_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

private:
    const std::size_t participants_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t arrived_{0};
    bool released_{false};
};

struct ConcurrentCompositionOutcome {
    bool existing_path{false};
    app::Phase3aWorkflowCode workflow_code{
        app::Phase3aWorkflowCode::internal_failure};
    bool caller_ready{false};
    bool identity_matches{false};
    bool node_matches{false};
    bool handoff_consumed_once{false};
    bool input_and_secret_state_clean{false};
    bool transcript_secret_free{false};
    bool lease_released_once{false};
    bool duplicate_release_rejected{false};
    bool threw{false};
    app::AccountId account_id{0};
    std::string display_name;
};

std::string concurrent_caller_name(
    const bool existing, const std::size_t index) {
    std::string suffix = std::to_string(index + 1U);
    suffix.insert(0, 3U - suffix.size(), '0');
    return existing ? "Existing Caller" + suffix : "New Caller" + suffix;
}

app::Phase3aConfigurationSnapshot composition_configuration(
    const app::SecurityPolicyRecord& policy) {
    app::Phase3aConfigurationValues values;
    values.foundation.logical_nodes = 128;
    values.foundation.security = sfx::core::SecurityPolicy::fresh_install();
    values.bootstrap_complete = true;
    values.allow_new_users = true;
    values.collect_optional_real_name = false;
    values.transfer_protocol_question_enabled = false;
    values.security_policy = {policy.revision, policy.policy_hash,
        policy.new_user_level, policy.sysop_threshold};
    values.connection_limits.maximum_logical_nodes = 128;
    values.prelogin_identity = {
        "Window 6C Test BBS", "Window 6C Sysop", "0.1-test"};
    auto created = app::Phase3aConfigurationSnapshot::create(std::move(values));
    require(created.valid(), "concrete composition configuration is valid");
    return std::move(*created.snapshot);
}

void concrete_mixed_composition(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy path_policy;
    std::filesystem::create_directories(root / "composition");
    storage::SqliteStorage database(settings_for(root / "composition"),
        path_policy, storage::sfx_storage_migrations());
    require(database.start().succeeded(),
        "concrete composition SQLite storage starts");

    authentication::SodiumPasswordKdf kdf(
        app::kMaximumConcurrentKdfJobs, 8);
    test::FakePasswordBlocklist blocklist;
    test::FakeSessionRevocationSink revocations;
    app::AuthenticationSecurityService security(
        database, database, kdf, &blocklist, revocations);
    app::CancellationSource setup_cancellation;
    const auto setup_token = setup_cancellation.token();
    auto sysop_credential = create_credential(
        security, "SysopPass123", "Window 6C Sysop", setup_token);

    app::StageInitialSysopRequest stage;
    stage.display_name = "Window 6C Sysop";
    stage.credential = std::move(sysop_credential);
    stage.audit = audit(100, kBaseUtc, app::AuditReasonCode::initial_setup);
    const auto staged = database.stage_initial_sysop(stage);
    require(staged.succeeded() && staged.account_id.has_value(),
        "concrete composition stages its authoritative Sysop");
    const auto initial_policy = database.security_policy();
    require(initial_policy && initial_policy.value.has_value(),
        "concrete composition loads the initial policy");
    app::CompleteFirstRunRequest complete;
    complete.staged_sysop_account_id = *staged.account_id;
    complete.expected_policy_revision = initial_policy.value->revision;
    complete.expected_policy_hash = initial_policy.value->policy_hash;
    complete.published_policy_hash = initial_policy.value->policy_hash;
    complete.audit = audit(101, kBaseUtc + 1,
        app::AuditReasonCode::initial_setup);
    require(database.complete_first_run(complete).succeeded(),
        "concrete composition publishes its first-run policy");

    const auto policy = database.security_policy();
    require(policy && policy.value.has_value(),
        "concrete composition reloads the published policy");
    app::RegistrationCommitRequest existing;
    existing.account.display_name = "Existing Caller";
    existing.account.credential = create_credential(
        security, "XXXXXXXXXXXX", existing.account.display_name, setup_token);
    existing.account.audit = audit(
        102, kBaseUtc + 2, app::AuditReasonCode::registration);
    existing.security_level = policy.value->new_user_level;
    existing.expected_security_policy_revision = policy.value->revision;
    existing.expected_security_policy_hash = policy.value->policy_hash;
    const auto existing_committed = database.commit_registration(existing);
    require(existing_committed.succeeded() &&
            existing_committed.account_id.has_value(),
        "concrete composition creates an authoritative ordinary account");

    auto credentials = authentication::Phase3aCredentialOperationsBridge::create(
        security, kdf, kBaseUtc, setup_token);
    require(credentials != nullptr,
        "concrete composition creates the accepted credential bridge");
    authentication::Phase3aRateControlCoordinator rates(database, kBaseUtc, 64);
    require(rates.ready(),
        "concrete composition restores the accepted bounded rate coordinator");
    app::HostIndependentAccountSessionAdmissionCoordinator admission(16);
    MissingDisplayAssetReader assets;
    app::TrustedDisplaySelector displays(assets);
    DeterministicUtcRuleProvider time_rules;
    test::FakeClock clock;
    clock.wall_seconds = kBaseUtc;
    app::BbsLocalTimeAdapters local_time({"UTC"}, time_rules, clock);
    CountingWelcomeStatistics welcome_statistics;
    auto configuration = composition_configuration(*policy.value);
    test::DeterministicIdentifierSource existing_identifiers(10'000);
    test::DeterministicIdentifierSource registration_identifiers(20'000);
    app::Phase3aNonNetworkComposition existing_composition({database,
        *credentials, database, database, database, database, admission,
        database, displays, rates, local_time, welcome_statistics, local_time, clock,
        existing_identifiers});
    app::Phase3aNonNetworkComposition registration_composition({database,
        *credentials, database, database, database, database, admission,
        database, displays, rates, local_time, welcome_statistics, local_time, clock,
        registration_identifiers});

    test::DeterministicInteractiveByteStreamFake existing_stream;
    existing_stream.script_non_secret_text("\rExisting\rCaller\r");
    existing_stream.script_generated_secret_input(12, 3);
    existing_stream.script_non_secret_text("\r");
    test::DeterministicInteractiveByteStreamFake registration_stream;
    registration_stream.script_generated_secret_input(12, 2);
    registration_stream.script_generated_secret_input(12, 4);
    registration_stream.script_non_secret_text("C\r");
    registration_stream.script_non_secret_text("\r");

    sfx::core::Session existing_session("window6c-concrete-existing",
        *sfx::core::NodeNumber::create(1),
        {sfx::core::TransportKind::telnet, "192.0.2.31", {}, "ansi"});
    sfx::core::Session registration_session("window6c-concrete-registration",
        *sfx::core::NodeNumber::create(2),
        {sfx::core::TransportKind::ssh, "192.0.2.32", {}, "ansi"});
    app::CancellationSource existing_cancellation;
    app::CancellationSource registration_cancellation;
    app::Phase3aExistingCallerRequest existing_request;
    existing_request.source_ip = "192.0.2.31";
    existing_request.correlation_id = identifier(10'001);
    app::Phase3aRegistrationRequest registration_request;
    registration_request.continuation = {
        "Concurrent New", app::GraphicsMode::ansi};
    registration_request.source_ip = "192.0.2.32";
    registration_request.correlation_id = identifier(20'001);

    auto existing_future = std::async(std::launch::async, [&] {
        return existing_composition.run_existing(existing_stream, configuration,
            existing_session, existing_request, existing_cancellation.token());
    });
    auto registration_future = std::async(std::launch::async, [&] {
        return registration_composition.run_registration(registration_stream,
            configuration, registration_session, registration_request,
            registration_cancellation.token());
    });
    auto existing_result = existing_future.get();
    auto registration_result = registration_future.get();
    require(existing_result.caller_ready() &&
            registration_result.caller_ready(),
        "mixed concrete existing and registration compositions reach ready handoffs concurrently");
    require(existing_stream.pending_reads() == 0 &&
            registration_stream.pending_reads() == 0 &&
            existing_stream.retained_secret_bytes() == 0 &&
            registration_stream.retained_secret_bytes() == 0,
        "fragmented and coalesced byte streams finish without retained input or secrets");
    require(admission.active_leases() == 2,
        "mixed composition owns exactly one admission lease per ready handoff");
    auto existing_ready = existing_result.ready->take();
    auto registration_ready = registration_result.ready->take();
    require(existing_ready.has_value() && registration_ready.has_value() &&
            existing_ready->account_id == *existing_committed.account_id &&
            existing_ready->display_name == "Existing Caller" &&
            registration_ready->display_name == "Concurrent New",
        "concrete compositions preserve authoritative existing and new account identities");
    require(!existing_result.ready->take().has_value() &&
            !registration_result.ready->take().has_value() &&
            existing_ready->admission_lease->release() &&
            registration_ready->admission_lease->release() &&
            admission.active_leases() == 0,
        "mixed ready handoffs and move-only leases each consume exactly once");
    require(assets.reads.load(std::memory_order_relaxed) >= 4,
        "mixed composition exercises the concrete trusted-display selector concurrently");
    require(scalar(database.database_path(),
                "SELECT count(*) FROM sfx_accounts") == 3 &&
            scalar(database.database_path(),
                "SELECT count(*) FROM sfx_account_sessions") == 2 &&
            scalar(database.database_path(),
                "SELECT count(*) FROM sfx_successful_login_ip_history") == 2,
        "mixed composition commits one durable session/source event for each caller");
    require(kdf.health().maximum_active_jobs ==
            app::kMaximumConcurrentKdfJobs &&
            kdf.health().completed_jobs >= 5,
        "mixed composition uses the accepted bounded KDF scheduler");
    require(database.shutdown().succeeded(),
        "concrete composition SQLite storage closes cleanly");
    kdf.shutdown();
}

void concrete_128_session_composition(const std::filesystem::path& root) {
    constexpr std::size_t session_count = 128U;
    constexpr std::size_t existing_count = session_count / 2U;
    constexpr std::size_t kdf_queue_capacity = session_count;

    test::FakeStoragePathPolicy path_policy;
    const auto directory = root / "composition-128";
    std::filesystem::create_directories(directory);
    storage::SqliteStorage database(settings_for(directory), path_policy,
        storage::sfx_storage_migrations());
    require(database.start().succeeded(),
        "128-session concrete composition SQLite storage starts");
    const auto database_path = database.database_path();

    authentication::SodiumPasswordKdf kdf(
        app::kMaximumConcurrentKdfJobs, kdf_queue_capacity);
    test::FakePasswordBlocklist blocklist;
    test::FakeSessionRevocationSink revocations;
    app::AuthenticationSecurityService security(
        database, database, kdf, &blocklist, revocations);
    app::CancellationSource setup_cancellation;
    const auto setup_token = setup_cancellation.token();

    app::StageInitialSysopRequest stage;
    stage.display_name = "Window 6C Load Sysop";
    stage.credential = create_credential(
        security, "LoadSysop123", stage.display_name, setup_token);
    stage.audit = audit(400'000U, kBaseUtc,
        app::AuditReasonCode::initial_setup);
    const auto staged = database.stage_initial_sysop(stage);
    require(staged.succeeded() && staged.account_id.has_value(),
        "128-session concrete composition stages its authoritative Sysop");
    const auto initial_policy = database.security_policy();
    require(initial_policy && initial_policy.value.has_value(),
        "128-session concrete composition loads the initial policy");
    app::CompleteFirstRunRequest complete;
    complete.staged_sysop_account_id = *staged.account_id;
    complete.expected_policy_revision = initial_policy.value->revision;
    complete.expected_policy_hash = initial_policy.value->policy_hash;
    complete.published_policy_hash = initial_policy.value->policy_hash;
    complete.audit = audit(400'001U, kBaseUtc + 1,
        app::AuditReasonCode::initial_setup);
    require(database.complete_first_run(complete).succeeded(),
        "128-session concrete composition publishes its first-run policy");

    const auto policy = database.security_policy();
    require(policy && policy.value.has_value(),
        "128-session concrete composition reloads the published policy");
    std::vector<std::string> expected_names(session_count);
    std::vector<app::AccountId> expected_account_ids(session_count, 0);
    for (std::size_t index = 0; index < session_count; ++index) {
        const bool existing = index % 2U == 0;
        expected_names[index] = concurrent_caller_name(existing, index);
        if (!existing) continue;
        app::RegistrationCommitRequest request;
        request.account.display_name = expected_names[index];
        request.account.credential = create_credential(security,
            "XXXXXXXXXXXX", request.account.display_name, setup_token);
        request.account.audit = audit(
            static_cast<std::uint32_t>(500'000U + index),
            kBaseUtc + 100 + static_cast<std::int64_t>(index),
            app::AuditReasonCode::registration);
        request.security_level = policy.value->new_user_level;
        request.expected_security_policy_revision = policy.value->revision;
        request.expected_security_policy_hash = policy.value->policy_hash;
        const auto committed = database.commit_registration(request);
        require(committed.succeeded() && committed.account_id.has_value(),
            "128-session concrete composition creates a distinct existing account");
        expected_account_ids[index] = *committed.account_id;
    }

    auto credentials = authentication::Phase3aCredentialOperationsBridge::create(
        security, kdf, kBaseUtc, setup_token);
    require(credentials != nullptr,
        "128-session concrete composition creates the credential bridge");
    const auto setup_health = wait_for_kdf_quiescence(
        [&kdf] { return kdf.health(); }, 131U, 5s, std::cerr);
    require(setup_health.maximum_active_jobs ==
                app::kMaximumConcurrentKdfJobs &&
            setup_health.queue_capacity == kdf_queue_capacity &&
            setup_health.completed_jobs == 131U &&
            setup_health.rejected_jobs == 0 &&
            setup_health.cancelled_jobs == 0,
        "128-session fixture setup uses exact two-job bounded KDF accounting");

    authentication::Phase3aRateControlCoordinator rates(
        database, kBaseUtc, 512U);
    require(rates.ready(),
        "128-session concrete composition restores shared Candidate A state");
    app::HostIndependentAccountSessionAdmissionCoordinator admission(
        session_count);
    MissingDisplayAssetReader assets;
    app::TrustedDisplaySelector displays(assets);
    DeterministicUtcRuleProvider time_rules;
    test::FakeClock clock;
    clock.wall_seconds = kBaseUtc;
    app::BbsLocalTimeAdapters local_time({"UTC"}, time_rules, clock);
    CountingWelcomeStatistics welcome_statistics;
    const auto configuration = composition_configuration(*policy.value);

    std::vector<std::unique_ptr<app::CancellationSource>> cancellations;
    cancellations.reserve(session_count);
    for (std::size_t index = 0; index < session_count; ++index) {
        cancellations.push_back(std::make_unique<app::CancellationSource>());
    }

    SynchronizedPhaseGate start_gate(session_count);
    SynchronizedPhaseGate ready_gate(session_count);
    std::vector<std::future<ConcurrentCompositionOutcome>> futures;
    futures.reserve(session_count);
    try {
        for (std::size_t index = 0; index < session_count; ++index) {
            futures.push_back(std::async(std::launch::async, [&, index] {
                ConcurrentCompositionOutcome outcome;
                outcome.existing_path = index % 2U == 0;
                std::optional<app::AuthenticatedCallerReady> ready;
                start_gate.arrive_and_wait();
                try {
                    const std::string source_ip =
                        "198.51.100." + std::to_string(index + 1U);
                    test::DeterministicIdentifierSource identifiers(
                        static_cast<std::uint32_t>(
                            3'000'000U + index * 100U));
                    app::Phase3aNonNetworkComposition composition({database,
                        *credentials, database, database, database, database,
                        admission, database, displays, rates, local_time,
                        welcome_statistics, local_time, clock, identifiers});
                    test::DeterministicInteractiveByteStreamFake stream;
                    if (outcome.existing_path) {
                        std::string suffix = std::to_string(index + 1U);
                        suffix.insert(0, 3U - suffix.size(), '0');
                        if (index % 4U == 0) {
                            stream.script_non_secret_text(
                                "\rExisting\rCaller" + suffix + "\r");
                        } else {
                            stream.script_non_secret_text("\r");
                            stream.script_non_secret_text("Existing\r");
                            stream.script_non_secret_text(
                                "Caller" + suffix + "\r");
                        }
                        stream.script_generated_secret_input(
                            12U, (index % 7U) + 1U);
                        stream.script_non_secret_text("\r");
                    } else {
                        stream.script_generated_secret_input(
                            12U, (index % 5U) + 1U);
                        stream.script_generated_secret_input(
                            12U, (index % 6U) + 1U);
                        stream.script_non_secret_text("C\r");
                        stream.script_non_secret_text("\r");
                    }

                    const auto node = sfx::core::NodeNumber::create(
                        static_cast<std::uint16_t>(index + 1U));
                    if (!node.has_value()) throw std::runtime_error(
                        "create deterministic 128-session node number");
                    sfx::core::Session session(
                        "window6c-concurrent-" + std::to_string(index + 1U),
                        *node,
                        {outcome.existing_path
                                ? sfx::core::TransportKind::telnet
                                : sfx::core::TransportKind::ssh,
                            source_ip, {}, "ansi"});
                    const auto cancellation = cancellations[index]->token();
                    app::Phase3aWorkflowResult result;
                    if (outcome.existing_path) {
                        app::Phase3aExistingCallerRequest request;
                        request.source_ip = source_ip;
                        request.correlation_id = identifier(
                            static_cast<std::uint32_t>(4'000'000U + index));
                        result = composition.run_existing(stream, configuration,
                            session, request, cancellation);
                    } else {
                        app::Phase3aRegistrationRequest request;
                        request.continuation = {
                            expected_names[index], app::GraphicsMode::ansi};
                        request.source_ip = source_ip;
                        request.correlation_id = identifier(
                            static_cast<std::uint32_t>(4'000'000U + index));
                        result = composition.run_registration(stream,
                            configuration, session, request, cancellation);
                    }
                    outcome.workflow_code = result.code;
                    outcome.caller_ready = result.caller_ready();
                    if (outcome.caller_ready && result.ready.has_value()) {
                        ready = result.ready->take();
                        outcome.handoff_consumed_once = ready.has_value() &&
                            !result.ready->take().has_value();
                    }
                    if (ready.has_value()) {
                        outcome.account_id = ready->account_id;
                        outcome.display_name = ready->display_name;
                        outcome.identity_matches =
                            ready->display_name == expected_names[index] &&
                            (expected_account_ids[index] == 0 ||
                                ready->account_id == expected_account_ids[index]);
                        outcome.node_matches =
                            ready->node_id == index + 1U;
                    }
                    outcome.input_and_secret_state_clean =
                        stream.pending_reads() == 0 &&
                        stream.pending_writes() == 0 &&
                        stream.retained_secret_bytes() == 0;
                    outcome.transcript_secret_free = stream.output().find(
                        std::string(12U, 'X')) == std::string::npos;
                } catch (...) {
                    outcome.threw = true;
                }

                ready_gate.arrive_and_wait();
                if (ready.has_value() && ready->admission_lease.has_value()) {
                    outcome.lease_released_once =
                        ready->admission_lease->release();
                    outcome.duplicate_release_rejected =
                        !ready->admission_lease->release();
                }
                return outcome;
            }));
        }
    } catch (...) {
        for (auto& cancellation : cancellations) cancellation->cancel();
        start_gate.release();
        ready_gate.release();
        throw;
    }

    const bool synchronized_start = start_gate.wait_until_all(
        std::chrono::steady_clock::now() + 30s);
    start_gate.release();
    const bool reached_ready_boundary = ready_gate.wait_until_all(
        std::chrono::steady_clock::now() + 15min);
    if (!reached_ready_boundary) {
        for (auto& cancellation : cancellations) cancellation->cancel();
    }
    const std::size_t simultaneously_held_leases = admission.active_leases();
    ready_gate.release();

    bool completed_after_release = true;
    const auto completion_deadline =
        std::chrono::steady_clock::now() + 1min;
    for (auto& future : futures) {
        if (future.wait_until(completion_deadline) !=
            std::future_status::ready) {
            completed_after_release = false;
        }
    }
    if (!completed_after_release) {
        for (auto& cancellation : cancellations) cancellation->cancel();
        const auto cancellation_deadline =
            std::chrono::steady_clock::now() + 1min;
        for (auto& future : futures) {
            if (future.wait_until(cancellation_deadline) !=
                std::future_status::ready) {
                std::abort();
            }
        }
    }

    require(synchronized_start,
        "all 128 concrete sessions reach the synchronized start gate");
    require(reached_ready_boundary,
        "all 128 concrete sessions reach a bounded workflow outcome");
    require(completed_after_release,
        "all 128 concrete sessions complete within the bounded deadline");
    if (simultaneously_held_leases != session_count) {
        std::cerr << "128-session simultaneous lease count="
                  << simultaneously_held_leases << '\n';
    }
    std::size_t existing_results = 0;
    std::size_t registration_results = 0;
    std::set<app::AccountId> account_ids;
    for (std::size_t index = 0; index < futures.size(); ++index) {
        const auto outcome = futures[index].get();
        const bool valid = !outcome.threw && outcome.caller_ready &&
            outcome.identity_matches && outcome.node_matches &&
            outcome.handoff_consumed_once &&
            outcome.input_and_secret_state_clean &&
            outcome.transcript_secret_free &&
            outcome.lease_released_once &&
            outcome.duplicate_release_rejected && outcome.account_id > 0;
        if (!valid) {
            std::cerr << "128-session outcome failure index=" << index
                      << " existing=" << outcome.existing_path
                      << " code="
                      << static_cast<unsigned int>(outcome.workflow_code)
                      << " ready=" << outcome.caller_ready
                      << " threw=" << outcome.threw << '\n';
        }
        require(valid,
            "each concrete session has an exact clean result and lease lifecycle");
        if (outcome.existing_path) ++existing_results;
        else ++registration_results;
        account_ids.insert(outcome.account_id);
    }
    require(simultaneously_held_leases == session_count,
        "all 128 ready handoffs simultaneously own one admission lease");
    require(existing_results == existing_count &&
            registration_results == session_count - existing_count &&
            account_ids.size() == session_count,
        "128-session result accounting contains 64 distinct callers on each path");
    require(admission.active_leases() == 0,
        "all 128 admission leases release exactly once");
    require(assets.reads.load(std::memory_order_relaxed) >=
            session_count * 2U,
        "all concurrent sessions exercise shared trusted-display selection");

    require(scalar(database_path, "SELECT count(*) FROM sfx_accounts") == 129 &&
            scalar(database_path,
                "SELECT count(*) FROM sfx_account_profiles") == 129 &&
            scalar(database_path,
                "SELECT count(*) FROM sfx_account_credentials") == 129 &&
            scalar(database_path,
                "SELECT count(*) FROM sfx_account_statistics") == 129 &&
            scalar(database_path,
                "SELECT count(*) FROM sfx_registration_commit_receipts") == 128,
        "128-session existing and registration paths create exact complete account state");
    require(scalar(database_path,
                "SELECT count(*) FROM sfx_account_sessions") == 128 &&
            scalar(database_path,
                "SELECT count(DISTINCT account_id) FROM sfx_account_sessions") == 128 &&
            scalar(database_path,
                "SELECT count(DISTINCT node_id) FROM sfx_account_sessions") == 128 &&
            scalar(database_path,
                "SELECT count(*) FROM sfx_successful_login_ip_history") == 128 &&
            scalar(database_path,
                "SELECT count(DISTINCT source_ip) FROM sfx_successful_login_ip_history") == 128 &&
            scalar(database_path,
                "SELECT sum(successful_login_count) FROM sfx_account_statistics") == 128,
        "128-session durable session, source, node, and statistics accounting is exact");
    require(scalar(database_path,
                "SELECT count(*) FROM (SELECT account_id FROM sfx_account_sessions GROUP BY account_id HAVING count(*)<>1)") == 0 &&
            scalar(database_path,
                "SELECT count(*) FROM (SELECT account_id FROM sfx_successful_login_ip_history GROUP BY account_id HAVING count(*)<>1)") == 0 &&
            scalar(database_path,
                "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check='ok'") == 1 &&
            scalar(database_path,
                "SELECT count(*) FROM pragma_foreign_key_check") == 0,
        "128-session run leaves no duplicate, partial, corrupt, or orphaned state");

    const auto completed_health = wait_for_kdf_quiescence(
        [&kdf] { return kdf.health(); }, 323U, 5s, std::cerr);
    require(completed_health.maximum_active_jobs ==
                app::kMaximumConcurrentKdfJobs &&
            completed_health.queue_capacity == kdf_queue_capacity &&
            completed_health.active_high_water ==
                app::kMaximumConcurrentKdfJobs &&
            completed_health.queue_high_water > 0 &&
            completed_health.queue_high_water <= kdf_queue_capacity &&
            completed_health.completed_jobs == 323U &&
            completed_health.rejected_jobs == 0 &&
            completed_health.cancelled_jobs == 0 &&
            completed_health.active_jobs == 0 &&
            completed_health.queued_jobs == 0,
        "128-session run uses exactly two KDF jobs with bounded lossless queueing");

    require(database.shutdown().succeeded(),
        "128-session concrete composition SQLite storage shuts down cleanly");
    kdf.shutdown();
    const auto shutdown_health = kdf.health();
    require(!shutdown_health.accepting && shutdown_health.active_jobs == 0 &&
            shutdown_health.queued_jobs == 0,
        "128-session concrete composition KDF shuts down cleanly");

    storage::SqliteStorage restarted(settings_for(directory), path_policy,
        storage::sfx_storage_migrations());
    require(restarted.start().succeeded() &&
            scalar(restarted.database_path(),
                "SELECT count(*) FROM sfx_account_sessions") == 128 &&
            scalar(restarted.database_path(),
                "SELECT count(*) FROM sfx_successful_login_ip_history") == 128 &&
            scalar(restarted.database_path(),
                "SELECT sum(successful_login_count) FROM sfx_account_statistics") == 128,
        "128-session durable state survives a clean storage restart");
    require(restarted.shutdown().succeeded(),
        "128-session restart verification shuts down cleanly");
}

std::int64_t scalar(const std::filesystem::path& database_path,
    const std::string& sql) {
    internal::SqliteConnection inspect(database_path, SQLITE_OPEN_READONLY, 100ms, false);
    return inspect.scalar_int64(sql, "inspect Window 6C transaction state");
}

void name_classification(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy path_policy;
    auto ready = create_ready(root / "names", path_policy);
    auto& database = *ready.database;
    const auto existing = database.classify("  window   six c sysop  ");
    require(existing && existing.value->classification ==
            app::CallerNameClassification::existing &&
            existing.value->normalized_name == "window six c sysop" &&
            existing.value->account_id == ready.account_id,
        "classification normalizes and resolves authoritative current identity");
    const auto available = database.classify("New Caller");
    require(available && available.value->classification ==
            app::CallerNameClassification::registerable,
        "unreserved normalized identity is registerable");
    app::RenameAccountRequest rename;
    rename.account_id = ready.account_id;
    rename.expected_revision = 1;
    rename.new_display_name = "Renamed Window Six C";
    rename.audit = audit(10, kBaseUtc + 10, app::AuditReasonCode::sysop_action);
    require(database.rename_account(rename).succeeded(),
        "classification fixture renames through authoritative storage");
    const auto retired = database.classify("Window Six C Sysop");
    const auto renamed = database.classify("renamed window six c");
    require(retired && retired.value->classification ==
            app::CallerNameClassification::unavailable &&
            renamed && renamed.value->classification ==
            app::CallerNameClassification::existing,
        "retired permanent name is unavailable while current name resolves");
    require(database.classify(std::string(31, 'X')).status.code ==
            app::StorageErrorCode::invalid_argument,
        "invalid caller name fails before storage classification");
    require(database.shutdown().succeeded(), "name-classification fixture closes");
}

void commit_replay_drift_and_rollback(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy path_policy;
    auto ready = create_ready(root / "commit", path_policy);
    auto& database = *ready.database;
    const auto database_path = database.database_path();
    auto request = request_for(database, ready.account_id, 1);
    require(app::valid_authenticated_session_commit_request(request),
        "complete authenticated-session request validates before writer admission");
    const auto committed = database.commit_authenticated_session(request);
    require(committed.succeeded() && !committed.replayed &&
            committed.account_id == ready.account_id &&
            committed.revision == request.expected_account_revision,
        "one transaction commits session, statistics, and source history");
    const auto replay = database.commit_authenticated_session(request);
    require(replay.succeeded() && replay.replayed,
        "exact composite retry is an idempotent replay");
    {
        internal::SqliteConnection drift(database_path,
            SQLITE_OPEN_READWRITE, 100ms, false);
        drift.exec("UPDATE sfx_accounts SET revision=revision+1 WHERE account_id=1",
            "inject exact-replay account revision drift");
    }
    require(database.commit_authenticated_session(request).status.code ==
            app::StorageErrorCode::conflict,
        "exact replay revalidates the current account snapshot inside the writer");
    require(scalar(database_path,
                "SELECT successful_login_count FROM sfx_account_statistics WHERE account_id=1") == 1 &&
            scalar(database_path, "SELECT count(*) FROM sfx_account_sessions") == 1 &&
            scalar(database_path, "SELECT count(*) FROM sfx_successful_login_ip_history") == 1,
        "exact replay creates no duplicate statistics, session, or source event");

    auto mismatch = request;
    mismatch.successful_source.source_ip = "192.0.2.250";
    require(database.commit_authenticated_session(mismatch).status.code ==
            app::StorageErrorCode::conflict,
        "mismatched replay conflicts rather than succeeding");
    for (std::uint32_t index = 2; index <= 6; ++index) {
        require(database.commit_authenticated_session(
                    request_for(database, ready.account_id, index)).succeeded(),
            "new authenticated transaction commits");
    }
    require(scalar(database_path,
                "SELECT count(*) FROM sfx_successful_login_ip_history") == 5 &&
            scalar(database_path,
                "SELECT successful_login_count FROM sfx_account_statistics WHERE account_id=1") == 6,
        "successful-source history prunes newest five while statistics count once each");
    auto revision_drift = request_for(database, ready.account_id, 20);
    ++revision_drift.expected_account_revision;
    auto name_drift = request_for(database, ready.account_id, 21);
    name_drift.expected_account_name_key = "different name";
    auto security_drift = request_for(database, ready.account_id, 22);
    security_drift.expected_security_level = static_cast<std::uint16_t>(998);
    auto policy_drift = request_for(database, ready.account_id, 23);
    ++policy_drift.expected_security_policy_revision;
    auto hash_drift = request_for(database, ready.account_id, 24);
    hash_drift.expected_security_policy_hash.assign(64, 'f');
    const auto revision_result = database.commit_authenticated_session(revision_drift);
    const auto name_result = database.commit_authenticated_session(name_drift);
    const auto security_result = database.commit_authenticated_session(security_drift);
    const auto policy_result = database.commit_authenticated_session(policy_drift);
    const auto hash_result = database.commit_authenticated_session(hash_drift);
    if (revision_result.status.code != app::StorageErrorCode::conflict ||
        name_result.status.code != app::StorageErrorCode::conflict ||
        security_result.status.code != app::StorageErrorCode::conflict ||
        policy_result.status.code != app::StorageErrorCode::conflict ||
        hash_result.status.code != app::StorageErrorCode::conflict) {
        std::cerr << "drift status codes="
                  << static_cast<int>(revision_result.status.code) << ','
                  << static_cast<int>(name_result.status.code) << ','
                  << static_cast<int>(security_result.status.code) << ','
                  << static_cast<int>(policy_result.status.code) << ','
                  << static_cast<int>(hash_result.status.code) << '\n';
    }
    require(revision_result.status.code == app::StorageErrorCode::conflict &&
            name_result.status.code == app::StorageErrorCode::conflict &&
            security_result.status.code == app::StorageErrorCode::conflict &&
            policy_result.status.code == app::StorageErrorCode::conflict &&
            hash_result.status.code == app::StorageErrorCode::conflict,
        "account and security-policy drift checks all fail closed inside the writer");

    {
        internal::SqliteConnection mutate(database_path, SQLITE_OPEN_READWRITE, 100ms, false);
        mutate.exec("UPDATE sfx_account_statistics SET successful_login_count=9223372036854775807 WHERE account_id=1",
            "inject statistics overflow rollback fixture");
    }
    const auto rollback = request_for(database, ready.account_id, 30);
    require(database.commit_authenticated_session(rollback).status.code ==
            app::StorageErrorCode::constraint,
        "statistics overflow fails the composite transaction");
    require(scalar(database_path,
                "SELECT count(*) FROM sfx_account_sessions") == 6 &&
            scalar(database_path,
                "SELECT count(*) FROM sfx_successful_login_ip_history") == 5,
        "failed request rolls back request-local session and source mutations");

    require(database.shutdown().succeeded(), "authenticated-session fixture closes");
    storage::SqliteStorage restarted(settings_for(root / "commit"), path_policy,
        storage::sfx_storage_migrations());
    require(restarted.start().succeeded() &&
            restarted.session_history({ready.account_id,
                app::kMaximumUtcSeconds, 100}).succeeded() &&
            restarted.successful_login_ips(ready.account_id).value->size() == 5,
        "restart reads complete durable sessions and newest-five source history");
    require(restarted.shutdown().succeeded(), "restart readback fixture closes");
}

void half_present_and_pressure(const std::filesystem::path& root) {
    require(internal::map_sqlite_error(SQLITE_IOERR_WRITE) ==
                app::StorageErrorCode::io &&
            internal::map_sqlite_error(SQLITE_FULL) ==
                app::StorageErrorCode::full,
        "accepted writer status mapping preserves I/O and full failures");

    test::FakeStoragePathPolicy path_policy;
    auto session_ready = create_ready(root / "session-half", path_policy);
    auto session_half = request_for(*session_ready.database, session_ready.account_id, 40);
    require(session_ready.database->record_session_started(session_half.session).succeeded() &&
            session_ready.database->commit_authenticated_session(session_half).status.code ==
                app::StorageErrorCode::corrupt,
        "session-only replay state is corruption, never success");
    (void)session_ready.database->shutdown();

    auto source_ready = create_ready(root / "source-half", path_policy);
    auto source_half = request_for(*source_ready.database, source_ready.account_id, 41);
    require(source_ready.database->record_successful_login_ip(
                source_half.successful_source).succeeded() &&
            source_ready.database->commit_authenticated_session(source_half).status.code ==
                app::StorageErrorCode::corrupt,
        "source-only replay state is corruption, never success");
    (void)source_ready.database->shutdown();

    auto pruned_ready = create_ready(root / "pruned-half", path_policy);
    auto pruned = request_for(*pruned_ready.database, pruned_ready.account_id, 42);
    require(pruned_ready.database->commit_authenticated_session(pruned).succeeded(),
        "pruned-replay fixture commits original composite request");
    for (std::uint32_t index = 43; index < 48; ++index) {
        require(pruned_ready.database->commit_authenticated_session(
                    request_for(*pruned_ready.database,
                        pruned_ready.account_id, index)).succeeded(),
            "pruned-replay fixture advances newest-five history");
    }
    require(pruned_ready.database->commit_authenticated_session(pruned).status.code ==
            app::StorageErrorCode::corrupt,
        "pruned half-present historical replay fails closed as corruption");
    (void)pruned_ready.database->shutdown();

    auto pressure = create_ready(root / "pressure", path_policy, true);
    const auto path = pressure.database->database_path();
    internal::SqliteConnection lock(path, SQLITE_OPEN_READWRITE, 5ms, false);
    lock.exec("BEGIN IMMEDIATE", "hold deterministic writer pressure lock");
    std::vector<std::future<app::MutationResult>> operations;
    for (std::uint32_t index = 50; index < 58; ++index) {
        auto request = request_for(*pressure.database, pressure.account_id, index);
        operations.push_back(std::async(std::launch::async,
            [database = pressure.database.get(), request] {
                return database->commit_authenticated_session(request);
            }));
    }
    std::size_t busy = 0;
    std::size_t backpressure = 0;
    for (auto& operation : operations) {
        const auto result = operation.get();
        if (result.status.code == app::StorageErrorCode::busy) ++busy;
        if (result.status.code == app::StorageErrorCode::backpressure) ++backpressure;
    }
    lock.exec("ROLLBACK", "release deterministic writer pressure lock");
    require(busy >= 1 && backpressure >= 1,
        "bounded writer reports busy and queue backpressure without partial success");
    require(scalar(path, "SELECT count(*) FROM sfx_account_sessions") == 0 &&
            scalar(path, "SELECT count(*) FROM sfx_successful_login_ip_history") == 0,
        "busy/backpressure attempts leave no partial transaction state");
    const auto after_shutdown = request_for(*pressure.database,
        pressure.account_id, 60);
    require(pressure.database->shutdown().succeeded(), "pressure fixture closes");
    require(pressure.database->commit_authenticated_session(after_shutdown).status.code ==
            app::StorageErrorCode::shutting_down,
        "authenticated-session commit fails closed after shutdown");
}

void write_fault_rollback_and_retry(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy path_policy;
    auto ready = create_ready(root / "window6c-fault", path_policy);
    const auto path = ready.database->database_path();

    const auto full_request = request_for(*ready.database, ready.account_id, 70);
    test::inject_next_sqlite_write_fault(SQLITE_FULL);
    const auto full = ready.database->commit_authenticated_session(full_request);
    require(full.status.code == app::StorageErrorCode::full &&
            test::pending_sqlite_write_fault() == SQLITE_OK,
        "authenticated-session writer reports injected SQLITE_FULL");
    require(scalar(path, "SELECT count(*) FROM sfx_account_sessions") == 0 &&
            scalar(path, "SELECT count(*) FROM sfx_successful_login_ip_history") == 0 &&
            scalar(path, "SELECT successful_login_count FROM sfx_account_statistics WHERE account_id=1") == 0,
        "disk-full failure rolls back the entire authenticated-session request");
    require(ready.database->commit_authenticated_session(full_request).succeeded(),
        "disk-full request retries exactly once after fault removal");

    const auto io_request = request_for(*ready.database, ready.account_id, 71);
    test::inject_next_sqlite_write_fault(SQLITE_IOERR_WRITE);
    const auto io = ready.database->commit_authenticated_session(io_request);
    require(io.status.code == app::StorageErrorCode::io &&
            test::pending_sqlite_write_fault() == SQLITE_OK,
        "authenticated-session writer reports injected SQLITE_IOERR_WRITE");
    require(scalar(path, "SELECT count(*) FROM sfx_account_sessions") == 1 &&
            scalar(path, "SELECT count(*) FROM sfx_successful_login_ip_history") == 1 &&
            scalar(path, "SELECT successful_login_count FROM sfx_account_statistics WHERE account_id=1") == 1,
        "I/O failure preserves only the prior complete authenticated session");
    require(ready.database->commit_authenticated_session(io_request).succeeded(),
        "I/O-failed request retries cleanly after fault removal");
    require(scalar(path, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check='ok'") == 1 &&
            scalar(path, "SELECT count(*) FROM pragma_foreign_key_check") == 0,
        "integrity and foreign-key checks pass after authenticated-session write faults");
    require(ready.database->shutdown().succeeded(), "write-fault fixture closes");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Window 6C storage tests require an output directory");
        kdf_quiescence_waiter_contract();
        require(test::install_sqlite_write_fault_vfs("window6c-fault"),
            "test-only Window 6C SQLite write-fault VFS installs");
        const auto root = std::filesystem::absolute(argv[1]);
        remove_tree(root);
        std::filesystem::create_directories(root);
        name_classification(root);
        concrete_mixed_composition(root);
        concrete_128_session_composition(root);
        commit_replay_drift_and_rollback(root);
        half_present_and_pressure(root);
        write_fault_rollback_and_retry(root);
        test::uninstall_sqlite_write_fault_vfs();
        std::cout << "all SFX Phase 3A Window 6C SQLite tests passed assertions="
                  << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        test::uninstall_sqlite_write_fault_vfs();
        std::cerr << "SFX Phase 3A Window 6C SQLite test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
