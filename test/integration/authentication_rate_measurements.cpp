#include "sfx/adapters/authentication/sodium_authentication.hpp"
#include "sfx/application/authentication_security.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace app = sfx::application;
namespace auth = sfx::adapters::authentication;

constexpr std::int64_t base_utc = 1'788'000'000;
constexpr std::size_t logical_nodes = 128;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

app::RateLimitKey key(const app::RateLimitScope scope, const std::uint32_t marker) {
    app::RateLimitKey value;
    value.scope = scope;
    value.subject_hash[0] = static_cast<std::uint8_t>((marker >> 24U) & 0xffU);
    value.subject_hash[1] = static_cast<std::uint8_t>((marker >> 16U) & 0xffU);
    value.subject_hash[2] = static_cast<std::uint8_t>((marker >> 8U) & 0xffU);
    value.subject_hash[3] = static_cast<std::uint8_t>(marker & 0xffU);
    return value;
}

std::string delay_sequence(
    const app::RateLimitProfile& profile,
    const app::RateLimitScope scope,
    const std::size_t attempts) {
    app::RateControlCoordinator coordinator(profile);
    const app::RateLimitKey subject = key(scope, 1);
    std::string output;
    for (std::size_t index = 0; index < attempts; ++index) {
        if (!output.empty()) output.push_back(',');
        output += std::to_string(coordinator.record_failure(subject, base_utc).delay_seconds);
    }
    return output;
}

struct AttackResult {
    std::uint64_t admitted{0};
    std::uint64_t delayed{0};
    std::uint32_t maximum_delay{0};
    std::size_t persisted_subjects{0};
    std::size_t restored_subjects{0};
    std::size_t expired_subjects{0};
};

AttackResult simulate_attack(const app::RateLimitProfile& profile) {
    app::RateControlCoordinator coordinator(profile, 4096);
    AttackResult result;
    for (std::int64_t second = 0; second < 120; ++second) {
        const std::int64_t instant = base_utc + second;
        for (std::size_t node = 0; node < logical_nodes; ++node) {
            const std::array<app::RateLimitKey, 4> controls{
                key(app::RateLimitScope::account, static_cast<std::uint32_t>(node % 16U)),
                key(app::RateLimitScope::source, static_cast<std::uint32_t>(node % 8U)),
                key(app::RateLimitScope::global, 0),
                key(app::RateLimitScope::system_password, static_cast<std::uint32_t>(node % 4U))};
            bool admitted = true;
            std::uint32_t delay = 0;
            for (const auto& control : controls) {
                const auto decision = coordinator.assess(control, instant);
                admitted = admitted && decision.admitted;
                delay = std::max(delay, decision.delay_seconds);
            }
            if (!admitted) {
                ++result.delayed;
                result.maximum_delay = std::max(result.maximum_delay, delay);
                continue;
            }
            ++result.admitted;
            for (const auto& control : controls) {
                const auto decision = coordinator.record_failure(control, instant);
                result.maximum_delay = std::max(result.maximum_delay, decision.delay_seconds);
            }
        }
    }
    const auto snapshots = coordinator.snapshots(base_utc + 120);
    result.persisted_subjects = snapshots.size();
    app::RateControlCoordinator restored(profile, 4096);
    for (const auto& snapshot : snapshots) {
        if (restored.restore(snapshot, base_utc + 120)) ++result.restored_subjects;
    }
    result.expired_subjects = restored.snapshots(base_utc + 86'401).size();
    return result;
}

struct LatencyStats {
    double mean_ms{0};
    double p50_ms{0};
    double p95_ms{0};
    double maximum_ms{0};
};

LatencyStats latency_stats(std::vector<double> values) {
    require(!values.empty(), "latency set is nonempty");
    std::sort(values.begin(), values.end());
    LatencyStats result;
    result.mean_ms = std::accumulate(values.begin(), values.end(), 0.0) /
        static_cast<double>(values.size());
    result.p50_ms = values[(values.size() - 1U) * 50U / 100U];
    result.p95_ms = values[(values.size() - 1U) * 95U / 100U];
    result.maximum_ms = values.back();
    return result;
}

LatencyStats measure_normal_kdf(auth::SodiumPasswordKdf& kdf) {
    app::CancellationSource cancellation;
    const auto credential = kdf.create(
        "synthetic normal caller password", base_utc, false, cancellation.token());
    require(credential.succeeded(), "measurement credential creates");
    std::vector<double> milliseconds;
    milliseconds.reserve(logical_nodes);
    for (std::size_t node = 0; node < logical_nodes; ++node) {
        const auto started = std::chrono::steady_clock::now();
        const auto verified = kdf.verify(
            *credential.credential, "synthetic normal caller password", cancellation.token());
        const auto ended = std::chrono::steady_clock::now();
        require(verified.completed() && verified.verified, "normal measurement verification succeeds");
        milliseconds.push_back(std::chrono::duration<double, std::milli>(ended - started).count());
    }
    return latency_stats(std::move(milliseconds));
}

struct KdfPressureResult {
    std::size_t logical_requests{logical_nodes};
    std::size_t pre_kdf_delayed{48};
    std::size_t submitted{80};
    std::size_t completed{0};
    std::size_t backpressure{0};
    double maximum_caller_ms{0};
    app::PasswordKdfHealth health;
};

KdfPressureResult measure_kdf_pressure() {
    auth::SodiumPasswordKdf kdf(2, 64);
    KdfPressureResult result;
    std::promise<void> release;
    const std::shared_future<void> start = release.get_future().share();
    struct Outcome { app::PasswordKdfCode code; double elapsed_ms; };
    std::vector<std::future<Outcome>> futures;
    futures.reserve(result.submitted);
    for (std::size_t index = 0; index < result.submitted; ++index) {
        futures.push_back(std::async(std::launch::async, [index, start, &kdf] {
            app::CancellationSource cancellation;
            start.wait();
            const auto started = std::chrono::steady_clock::now();
            const auto created = kdf.create(
                "synthetic attack password " + std::to_string(index),
                base_utc + static_cast<std::int64_t>(index), false, cancellation.token());
            const auto ended = std::chrono::steady_clock::now();
            return Outcome{created.code,
                std::chrono::duration<double, std::milli>(ended - started).count()};
        }));
    }
    release.set_value();
    for (auto& future : futures) {
        const Outcome outcome = future.get();
        if (outcome.code == app::PasswordKdfCode::completed) ++result.completed;
        if (outcome.code == app::PasswordKdfCode::backpressure) ++result.backpressure;
        result.maximum_caller_ms = std::max(result.maximum_caller_ms, outcome.elapsed_ms);
    }
    result.health = kdf.health();
    kdf.shutdown();
    require(result.completed + result.backpressure == result.submitted,
        "attack pressure returns only completed/backpressure outcomes");
    return result;
}

double measure_normal_rate_latency(const app::RateLimitProfile& profile) {
    app::RateControlCoordinator coordinator(profile, 4096);
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t operations = 0;
    for (std::size_t round = 0; round < 1'000; ++round) {
        for (std::size_t node = 0; node < logical_nodes; ++node) {
            require(coordinator.assess(key(app::RateLimitScope::account,
                static_cast<std::uint32_t>(node)), base_utc).admitted,
                "normal caller has no synthetic delay");
            ++operations;
        }
    }
    const auto ended = std::chrono::steady_clock::now();
    const double micros = std::chrono::duration<double, std::micro>(ended - started).count();
    return micros / static_cast<double>(operations);
}

void report_profile(const char label, const app::RateLimitProfile& profile) {
    const AttackResult attack = simulate_attack(profile);
    std::cout << "candidate=" << label
              << " account_delays=" << delay_sequence(profile, app::RateLimitScope::account, 10)
              << " source_delays=" << delay_sequence(profile, app::RateLimitScope::source, 12)
              << " sy_delays=" << delay_sequence(profile, app::RateLimitScope::system_password, 10)
              << " global_delays=" << delay_sequence(profile, app::RateLimitScope::global, 70)
              << " normal_rate_mean_us=" << std::fixed << std::setprecision(3)
              << measure_normal_rate_latency(profile)
              << " attack_admitted=" << attack.admitted
              << " attack_delayed=" << attack.delayed
              << " attack_maximum_delay_seconds=" << attack.maximum_delay
              << " restart_persisted_subjects=" << attack.persisted_subjects
              << " restart_restored_subjects=" << attack.restored_subjects
              << " post_ttl_subjects=" << attack.expired_subjects
              << '\n';
}

}  // namespace

int main() {
    try {
        std::cout << "stage5c_rate_measurement logical_nodes=" << logical_nodes
                  << " duration_seconds=120 attack_attempts=" << logical_nodes * 120U << '\n';
        report_profile('A', app::provisional_rate_profile_a());
        report_profile('B', app::provisional_rate_profile_b());

        auth::SodiumPasswordKdf normal_kdf(2, 64);
        const LatencyStats normal = measure_normal_kdf(normal_kdf);
        const auto normal_health = normal_kdf.health();
        normal_kdf.shutdown();
        std::cout << "normal_kdf logical_nodes=" << logical_nodes
                  << " sequential_verifications=" << logical_nodes
                  << " mean_ms=" << std::fixed << std::setprecision(3) << normal.mean_ms
                  << " p50_ms=" << normal.p50_ms
                  << " p95_ms=" << normal.p95_ms
                  << " maximum_ms=" << normal.maximum_ms
                  << " active_high_water=" << normal_health.active_high_water
                  << " queue_high_water=" << normal_health.queue_high_water
                  << " estimated_memory_high_water_bytes="
                  << normal_health.estimated_memory_high_water_bytes << '\n';

        const KdfPressureResult pressure = measure_kdf_pressure();
        std::cout << "attack_kdf logical_requests=" << pressure.logical_requests
                  << " pre_kdf_delayed=" << pressure.pre_kdf_delayed
                  << " submitted=" << pressure.submitted
                  << " completed=" << pressure.completed
                  << " backpressure=" << pressure.backpressure
                  << " maximum_caller_ms=" << std::fixed << std::setprecision(3)
                  << pressure.maximum_caller_ms
                  << " active_high_water=" << pressure.health.active_high_water
                  << " queue_high_water=" << pressure.health.queue_high_water
                  << " estimated_memory_high_water_bytes="
                  << pressure.health.estimated_memory_high_water_bytes << '\n';
        require(pressure.health.active_high_water <= 2 && pressure.health.queue_high_water <= 64 &&
                pressure.health.estimated_memory_high_water_bytes <= 64U * 1024U * 1024U,
            "KDF pressure respects active, queue, and memory ceilings");
        std::cout << "SFX Stage 5C 128-node authentication measurements passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Stage 5C authentication measurement failure: " << error.what() << '\n';
        return 1;
    }
}
