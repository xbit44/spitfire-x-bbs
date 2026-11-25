#include "sfx/adapters/authentication/sodium_authentication.hpp"
#include "sfx/application/authentication_security.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace app = sfx::application;
namespace auth = sfx::adapters::authentication;
using namespace std::chrono_literals;

constexpr std::int64_t base_utc = 1'788'000'000;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<app::PasswordKdfCode> concurrent_creates(
    auth::SodiumPasswordKdf& kdf,
    const std::size_t count,
    std::vector<app::CancellationSource>* cancellations = nullptr) {
    std::promise<void> release;
    const std::shared_future<void> start = release.get_future().share();
    std::vector<std::future<app::PasswordKdfCode>> futures;
    futures.reserve(count);
    if (cancellations != nullptr) cancellations->resize(count);
    for (std::size_t index = 0; index < count; ++index) {
        futures.push_back(std::async(std::launch::async, [&, index, start] {
            start.wait();
            app::CancellationSource local;
            const app::CancellationToken token = cancellations == nullptr
                ? local.token()
                : (*cancellations)[index].token();
            const auto result = kdf.create(
                "synthetic queue pressure password " + std::to_string(index),
                base_utc + static_cast<std::int64_t>(index), false, token);
            return result.code;
        }));
    }
    release.set_value();
    std::vector<app::PasswordKdfCode> output;
    output.reserve(count);
    for (auto& future : futures) output.push_back(future.get());
    return output;
}

void saturation_test() {
    auth::SodiumPasswordKdf kdf(2, 2);
    require(kdf.health().accepting, "saturation KDF initializes");
    const auto results = concurrent_creates(kdf, 24);
    const auto rejected = static_cast<std::size_t>(std::count(
        results.begin(), results.end(), app::PasswordKdfCode::backpressure));
    const auto completed = static_cast<std::size_t>(std::count(
        results.begin(), results.end(), app::PasswordKdfCode::completed));
    require(rejected > 0 && completed > 0, "saturated bounded queue both works and rejects pressure");
    const auto health = kdf.health();
    require(health.active_high_water <= 2 && health.queue_high_water <= 2,
        "KDF active and queued high-water never exceed configured bounds");
    require(health.estimated_memory_high_water_bytes <= 64U * 1024U * 1024U,
        "two active Argon2id jobs stay within the planned 64-MiB KDF ceiling");
    require(health.rejected_jobs == rejected, "KDF health reports every saturation rejection");
    kdf.shutdown();
}

void cancellation_and_shutdown_test() {
    auth::SodiumPasswordKdf kdf(1, 8);
    std::vector<app::CancellationSource> cancellations;
    std::promise<void> release;
    const std::shared_future<void> start = release.get_future().share();
    std::vector<std::future<app::PasswordKdfCode>> futures;
    cancellations.resize(12);
    for (std::size_t index = 0; index < cancellations.size(); ++index) {
        futures.push_back(std::async(std::launch::async, [&, index, start] {
            start.wait();
            return kdf.create(
                "synthetic shutdown password " + std::to_string(index),
                base_utc + static_cast<std::int64_t>(index), false,
                cancellations[index].token()).code;
        }));
    }
    release.set_value();
    std::this_thread::sleep_for(5ms);
    for (auto& source : cancellations) source.cancel();
    kdf.shutdown();
    std::size_t cancelled = 0;
    std::size_t rejected = 0;
    for (auto& future : futures) {
        const auto code = future.get();
        if (code == app::PasswordKdfCode::cancelled) ++cancelled;
        if (code == app::PasswordKdfCode::backpressure) ++rejected;
    }
    require(cancelled > 0, "queued/active cancellation produces bounded cancelled outcomes");
    require(cancelled + rejected == futures.size(),
        "shutdown resolves every in-flight caller without hangs or success after cancellation");
    const auto health = kdf.health();
    require(!health.accepting && health.active_jobs == 0 && health.queued_jobs == 0,
        "shutdown drains all active/queued KDF state");
    app::CancellationSource after;
    require(kdf.create("after-shutdown", base_utc, false, after.token()).code ==
            app::PasswordKdfCode::backpressure,
        "post-shutdown submission fails closed");
}

}  // namespace

int main() {
    try {
        saturation_test();
        cancellation_and_shutdown_test();
        std::cout << "all SFX Stage 5C KDF concurrency tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Stage 5C KDF concurrency test failure: " << error.what() << '\n';
        return 1;
    }
}
