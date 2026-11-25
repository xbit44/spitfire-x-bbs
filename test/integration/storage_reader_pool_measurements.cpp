#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace app = sfx::application;
namespace storage = sfx::adapters::storage;
namespace test = sfx::test;
using namespace std::chrono_literals;

constexpr std::int64_t base_utc = 1'787'552'000;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void report_reader_pool_health(
    std::ostream& diagnostics, const char* const label,
    const std::uint64_t expected,
    const storage::SqliteReaderPoolHealth& health) {
    diagnostics << label
                << " expected_completed_operations=" << expected
                << " pool_size=" << health.pool_size
                << " queued_operations=" << health.queued_operations
                << " queue_high_water_operations="
                << health.queue_high_water_operations
                << " completed_operations=" << health.completed_operations
                << " rejected_operations=" << health.rejected_operations
                << " maximum_queue_wait_microseconds="
                << health.maximum_queue_wait_microseconds << '\n';
}

template <typename HealthSnapshot>
storage::SqliteReaderPoolHealth wait_for_reader_pool_quiescence(
    HealthSnapshot&& snapshot, const std::uint64_t expected,
    const std::chrono::steady_clock::duration timeout,
    std::ostream& diagnostics) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto health = snapshot();
        if (health.queued_operations == 0 &&
            health.completed_operations == expected &&
            health.rejected_operations == 0) {
            return health;
        }
        const bool impossible =
            health.completed_operations > expected ||
            health.rejected_operations != 0;
        if (impossible || std::chrono::steady_clock::now() >= deadline) {
            report_reader_pool_health(diagnostics,
                impossible ? "READER_POOL_HEALTH_IMPOSSIBLE"
                           : "READER_POOL_HEALTH_TIMEOUT",
                expected, health);
            throw std::runtime_error(
                "bounded reader-pool health did not reach expected quiescence");
        }
        std::this_thread::sleep_for(1ms);
    }
}

void reader_pool_health_waiter_contract() {
    storage::SqliteReaderPoolHealth pending;
    pending.pool_size = 2;
    pending.queued_operations = 1;
    pending.queue_high_water_operations = 128;
    pending.completed_operations = 7'807;
    pending.maximum_queue_wait_microseconds = 31'337;
    std::ostringstream timeout_diagnostics;
    bool rejected_timeout = false;
    try {
        (void)wait_for_reader_pool_quiescence(
            [&pending] { return pending; }, 7'808, 0ms,
            timeout_diagnostics);
    } catch (const std::runtime_error&) {
        rejected_timeout = true;
    }
    require(rejected_timeout,
        "reader-pool waiter rejects incomplete health at its deadline");
    const auto timeout_text = timeout_diagnostics.str();
    constexpr const char* fields[] = {
        "expected_completed_operations=", "pool_size=",
        "queued_operations=", "queue_high_water_operations=",
        "completed_operations=", "rejected_operations=",
        "maximum_queue_wait_microseconds="};
    bool complete_timeout =
        timeout_text.find("READER_POOL_HEALTH_TIMEOUT") !=
        std::string::npos;
    for (const auto* field : fields) {
        complete_timeout =
            complete_timeout && timeout_text.find(field) != std::string::npos;
    }
    require(complete_timeout,
        "reader-pool timeout reports expected total and every health field");

    auto impossible = pending;
    impossible.queued_operations = 0;
    impossible.completed_operations = 7'809;
    impossible.rejected_operations = 1;
    std::ostringstream impossible_diagnostics;
    bool rejected_impossible = false;
    try {
        (void)wait_for_reader_pool_quiescence(
            [&impossible] { return impossible; }, 7'808, 5s,
            impossible_diagnostics);
    } catch (const std::runtime_error&) {
        rejected_impossible = true;
    }
    require(rejected_impossible &&
            impossible_diagnostics.str().find(
                "READER_POOL_HEALTH_IMPOSSIBLE") != std::string::npos,
        "reader-pool waiter rejects and diagnoses impossible accounting");

    auto exact = pending;
    exact.queued_operations = 0;
    exact.completed_operations = 7'808;
    std::ostringstream unused;
    const auto accepted = wait_for_reader_pool_quiescence(
        [&exact] { return exact; }, 7'808, 0ms, unused);
    require(accepted.completed_operations == 7'808 &&
            accepted.queued_operations == 0 &&
            accepted.rejected_operations == 0 && unused.str().empty(),
        "reader-pool waiter accepts only exact quiescent accounting");
}

app::Identifier128 identifier(const std::uint32_t sequence) {
    app::Identifier128 value;
    value.bytes[0] = 0x52;
    value.bytes[1] = 0x45;
    value.bytes[2] = 0x41;
    value.bytes[3] = 0x44;
    value.bytes[12] = static_cast<std::uint8_t>((sequence >> 24U) & 0xffU);
    value.bytes[13] = static_cast<std::uint8_t>((sequence >> 16U) & 0xffU);
    value.bytes[14] = static_cast<std::uint8_t>((sequence >> 8U) & 0xffU);
    value.bytes[15] = static_cast<std::uint8_t>(sequence & 0xffU);
    return value;
}

app::AuditContext audit(const std::uint32_t sequence, const app::AuditReasonCode reason) {
    return {identifier(sequence), identifier(100'000U + sequence), std::nullopt,
        base_utc + static_cast<std::int64_t>(sequence), reason};
}

app::CredentialEnvelope credential(const std::uint32_t sequence) {
    app::CredentialEnvelope value;
    value.envelope_format_version = 1;
    value.scheme_identifier = "reader-test-envelope";
    value.parameter_encoding_version = 1;
    value.canonical_parameters = {1, 2, 3};
    value.salt = {4, 5, static_cast<std::uint8_t>(sequence)};
    value.verifier = {6, 7, 8, static_cast<std::uint8_t>(sequence)};
    value.created_utc_seconds = base_utc;
    return value;
}

storage::SqliteStorageSettings settings_for(
    const std::filesystem::path& directory,
    const std::size_t readers) {
    storage::SqliteStorageSettings settings;
    settings.data_directory = directory;
    settings.utc_now = [] { return base_utc; };
    settings.queue.maximum_operations = 2048;
    settings.queue.maximum_bytes = 4U * 1024U * 1024U;
    settings.queue.dequeue_batch_limit = 32;
    settings.queue.dequeue_batch_window = 2ms;
    settings.queue.enqueue_wait = 5000ms;
    settings.busy.sqlite_timeout = 100ms;
    settings.busy.transaction_deadline = 2000ms;
    settings.busy.maximum_attempts = 5;
    settings.checkpoint.passive_interval = std::chrono::hours(1);
    settings.readers.pool_size = readers;
    settings.readers.maximum_pending_operations = 8192;
    settings.readers.enqueue_wait = 5000ms;
    return settings;
}

std::string caller_name(const int node) {
    std::ostringstream name;
    name << "Reader Caller " << std::setw(3) << std::setfill('0') << node;
    return name.str();
}

void seed_database(
    const std::filesystem::path& directory,
    test::FakeStoragePathPolicy& policy,
    std::vector<app::AccountId>& account_ids) {
    storage::SqliteStorage database(
        settings_for(directory, 2), policy, storage::sfx_storage_migrations());
    require(database.start().succeeded(), "reader seed storage starts");
    app::StageInitialSysopRequest stage;
    stage.display_name = "Reader Test Sysop";
    stage.credential = credential(1);
    stage.audit = audit(1, app::AuditReasonCode::initial_setup);
    const auto staged = database.stage_initial_sysop(stage);
    require(staged.succeeded(), "reader test initial Sysop stages");
    const auto policy_record = database.security_policy();
    app::CompleteFirstRunRequest complete;
    complete.staged_sysop_account_id = *staged.account_id;
    complete.expected_policy_revision = policy_record.value->revision;
    complete.expected_policy_hash = policy_record.value->policy_hash;
    complete.published_policy_hash = policy_record.value->policy_hash;
    complete.audit = audit(2, app::AuditReasonCode::initial_setup);
    require(database.complete_first_run(complete).succeeded(), "reader test setup completes");

    account_ids.reserve(128);
    for (int node = 1; node <= 128; ++node) {
        app::CreateAccountRequest request;
        request.display_name = caller_name(node);
        request.credential = credential(static_cast<std::uint32_t>(node + 10));
        request.audit = audit(static_cast<std::uint32_t>(node + 10), app::AuditReasonCode::registration);
        const auto created = database.create_account(request);
        require(created.succeeded(), "reader workload account creates");
        account_ids.push_back(*created.account_id);
    }
    require(database.shutdown().succeeded(), "reader seed storage closes");
}

struct Measurement {
    std::size_t readers{0};
    std::uint64_t operations{0};
    std::int64_t elapsed_microseconds{0};
    std::int64_t maximum_node_microseconds{0};
    storage::SqliteReaderPoolHealth health;
};

Measurement measure(
    const std::filesystem::path& directory,
    test::FakeStoragePathPolicy& policy,
    const std::vector<app::AccountId>& account_ids,
    const std::size_t reader_count) {
    storage::SqliteStorage database(
        settings_for(directory, reader_count), policy, storage::sfx_storage_migrations());
    require(database.start().succeeded(), "reader measurement storage starts");

    constexpr int iterations = 40;
    constexpr std::uint64_t operations_per_node = iterations + 8 + 4 + 2 + 2 + 2 + 1 + 1 + 1;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<std::uint64_t> successes{0};
    std::vector<std::int64_t> node_times(128, 0);
    std::vector<std::thread> nodes;
    nodes.reserve(128);
    for (int node = 0; node < 128; ++node) {
        nodes.emplace_back([&, node] {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            const auto started = std::chrono::steady_clock::now();
            for (int iteration = 0; iteration < iterations; ++iteration) {
                if (database.find_account_by_id(account_ids[static_cast<std::size_t>(node)]).succeeded())
                    successes.fetch_add(1);
                if (iteration % 5 == 0 && database.find_account_by_name(caller_name(node + 1)).succeeded())
                    successes.fetch_add(1);
                if (iteration % 10 == 0) {
                    app::AccountListQuery query;
                    query.state = app::AccountState::active;
                    query.limit = 16;
                    query.offset = static_cast<std::size_t>(node % 8);
                    if (database.list_accounts(query).succeeded()) successes.fetch_add(1);
                }
                if (iteration % 20 == 0) {
                    const app::HistoryQuery sessions{account_ids[static_cast<std::size_t>(node)],
                        app::kMaximumUtcSeconds, 8};
                    if (database.session_history(sessions).succeeded()) successes.fetch_add(1);
                    const app::AuditHistoryQuery audit_history{account_ids[static_cast<std::size_t>(node)],
                        std::nullopt, app::kMaximumUtcSeconds, 8};
                    if (database.audit_history(audit_history).succeeded()) successes.fetch_add(1);
                    if (database.security_level(10).succeeded()) successes.fetch_add(1);
                }
                if (iteration == 0) {
                    if (database.security_levels().succeeded()) successes.fetch_add(1);
                    const app::InactivityCandidateQuery inactivity{app::kMaximumUtcSeconds, 16};
                    if (database.inactivity_candidates(inactivity).succeeded()) successes.fetch_add(1);
                    if (database.retention_policy().succeeded()) successes.fetch_add(1);
                }
            }
            node_times[static_cast<std::size_t>(node)] =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count();
        });
    }
    while (ready.load() != 128) std::this_thread::yield();
    const auto started = std::chrono::steady_clock::now();
    go.store(true);
    for (std::thread& node : nodes) node.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    const std::uint64_t expected = 128U * operations_per_node;
    require(successes.load() == expected, "every reader workload operation succeeds");

    const storage::SqliteReaderPoolHealth health =
        wait_for_reader_pool_quiescence(
            [&database] { return database.reader_pool_health(); },
            expected, 5s, std::cerr);
    require(health.completed_operations == expected, "reader pool completes every queued operation");
    require(health.rejected_operations == 0, "reader pool rejects no workload operations");
    require(health.queue_high_water_operations <= 8192, "reader queue remains bounded");
    require(health.maximum_queue_wait_microseconds < 5'000'000, "reader queue has no starvation");
    require(elapsed < 30'000'000, "reader workload completes within bounded acceptance time");
    const auto maximum_node = *std::max_element(node_times.begin(), node_times.end());
    require(maximum_node < 30'000'000, "every logical node completes without starvation");
    require(database.shutdown().succeeded(), "reader measurement storage closes");
    return {reader_count, expected, elapsed, maximum_node, health};
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "reader-pool measurements require an output directory");
        reader_pool_health_waiter_contract();
        const std::filesystem::path root = std::filesystem::absolute(argv[1]);
        std::error_code error;
        std::filesystem::remove_all(root, error);
        require(!error, "remove reader measurement tree");
        std::filesystem::create_directories(root);
        test::FakeStoragePathPolicy policy;
        std::vector<app::AccountId> accounts;
        seed_database(root, policy, accounts);

        std::vector<Measurement> measurements;
        for (const std::size_t readers : {1U, 2U, 4U, 8U, 16U}) {
            std::vector<Measurement> samples;
            for (std::size_t sample = 1; sample <= 5; ++sample) {
                samples.push_back(measure(root, policy, accounts, readers));
                const Measurement& result = samples.back();
                std::cout << "stage5b_reader_pool readers=" << result.readers
                          << " sample=" << sample
                          << " operations=" << result.operations
                          << " elapsed_us=" << result.elapsed_microseconds
                          << " maximum_node_us=" << result.maximum_node_microseconds
                          << " queue_high_water=" << result.health.queue_high_water_operations
                          << " maximum_wait_us=" << result.health.maximum_queue_wait_microseconds
                          << '\n';
            }
            std::sort(samples.begin(), samples.end(), [](const Measurement& left, const Measurement& right) {
                return left.elapsed_microseconds < right.elapsed_microseconds;
            });
            measurements.push_back(samples[samples.size() / 2U]);
            std::cout << "stage5b_reader_pool readers=" << readers
                      << " median_elapsed_us=" << measurements.back().elapsed_microseconds << '\n';
        }

        std::size_t selected = measurements.back().readers;
        for (std::size_t index = 0; index + 1U < measurements.size(); ++index) {
            const double improvement = measurements[index].elapsed_microseconds <= 0 ? 0.0 :
                static_cast<double>(measurements[index].elapsed_microseconds -
                    measurements[index + 1U].elapsed_microseconds) /
                static_cast<double>(measurements[index].elapsed_microseconds);
            if (improvement <= 0.10) {
                selected = measurements[index].readers;
                break;
            }
        }
        std::cout << "stage5b_reader_pool observed_recommendation=" << selected
                  << " production_default=" << storage::SqliteReaderSettings{}.pool_size
                  << " criterion=median_of_five_no_starvation_and_next_improvement_at_most_10_percent"
                  << " recommendation_is_informational_due_to_timing_variance\n";
        std::filesystem::remove_all(root, error);
        require(!error, "remove completed reader measurement tree");
        std::cout << "all SFX Stage 5B reader-pool measurements passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Stage 5B reader-pool measurement failure: " << error.what() << '\n';
        return 1;
    }
}
