#include "sfx/adapters/authentication/sodium_authentication.hpp"

#include "sfx/application/authentication_storage.hpp"
#include "sfx_pinned_password_list.hpp"

#include <sodium.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <future>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sfx::adapters::authentication {
namespace {

constexpr std::string_view expected_wordlist_sha256 =
    "c63d5e4ccc31344d662583cc39ca4bd5bd20517ff1d24501f0c4e0c22d9b722a";
constexpr std::size_t expected_wordlist_bytes = 76'497;
constexpr std::size_t expected_wordlist_entries = 10'000;

bool printable_password(const std::string_view value) noexcept {
    return value.size() >= application::kPasswordMinimumStandard &&
        value.size() <= application::kPasswordMaximum &&
        std::all_of(value.begin(), value.end(), [](const unsigned char character) {
            return character >= 0x20U && character <= 0x7eU;
        });
}

std::string ascii_lower(const std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const unsigned char character : value) {
        output.push_back(character >= static_cast<unsigned char>('A') &&
                character <= static_cast<unsigned char>('Z')
            ? static_cast<char>(character + 0x20U)
            : static_cast<char>(character));
    }
    return output;
}

std::string hexadecimal(const unsigned char* bytes, const std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output(size * 2U, '0');
    for (std::size_t index = 0; index < size; ++index) {
        output[index * 2U] = digits[(bytes[index] >> 4U) & 0x0fU];
        output[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    return output;
}

class SensitivePassword final {
public:
    explicit SensitivePassword(const std::string_view value) : bytes_(value.begin(), value.end()) {
        if (!bytes_.empty()) (void)sodium_mlock(bytes_.data(), bytes_.size());
    }

    ~SensitivePassword() {
        if (!bytes_.empty()) {
            sodium_memzero(bytes_.data(), bytes_.size());
            (void)sodium_munlock(bytes_.data(), bytes_.size());
        }
    }

    SensitivePassword(const SensitivePassword&) = delete;
    SensitivePassword& operator=(const SensitivePassword&) = delete;

    const char* data() const noexcept { return bytes_.data(); }
    std::size_t size() const noexcept { return bytes_.size(); }

private:
    std::vector<char> bytes_;
};

struct QueueJob {
    std::function<void()> run;
    std::function<void()> cancel;
    std::function<void()> fail;
};

application::PasswordKdfCode envelope_error_code(
    const application::CredentialEnvelopeCode code) noexcept {
    switch (code) {
        case application::CredentialEnvelopeCode::unsupported_version:
        case application::CredentialEnvelopeCode::unsupported_scheme:
        case application::CredentialEnvelopeCode::unsupported_parameters:
            return application::PasswordKdfCode::unsupported_credential;
        case application::CredentialEnvelopeCode::malformed:
        case application::CredentialEnvelopeCode::oversized:
            return application::PasswordKdfCode::malformed_credential;
        case application::CredentialEnvelopeCode::valid:
            return application::PasswordKdfCode::completed;
    }
    return application::PasswordKdfCode::malformed_credential;
}

}  // namespace

class PinnedSecListsBlocklist::Impl final {
public:
    explicit Impl(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return;
        bytes_.resize(expected_wordlist_bytes);
        input.read(bytes_.data(),static_cast<std::streamsize>(bytes_.size()));
        if (input.gcount() != static_cast<std::streamsize>(expected_wordlist_bytes) ||
            input.peek() != std::char_traits<char>::eof() || input.bad()) return;
        validate();
    }
    explicit Impl(std::string_view embedded_bytes) : bytes_(embedded_bytes) { validate(); }
    void validate() {
        if (sodium_init() < 0 || bytes_.size() != expected_wordlist_bytes) return;

        std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
        if (crypto_hash_sha256(
                digest.data(),
                reinterpret_cast<const unsigned char*>(bytes_.data()),
                static_cast<unsigned long long>(bytes_.size())) != 0 ||
            hexadecimal(digest.data(), digest.size()) != expected_wordlist_sha256) {
            sodium_memzero(digest.data(), digest.size());
            return;
        }
        sodium_memzero(digest.data(), digest.size());

        std::size_t offset = 0;
        while (offset < bytes_.size()) {
            const std::size_t newline = bytes_.find('\n', offset);
            const std::size_t end = newline == std::string::npos ? bytes_.size() : newline;
            std::size_t line_end = end;
            if (line_end > offset && bytes_[line_end - 1U] == '\r') --line_end;
            ++entry_count_;
            if (line_end > offset) {
                entries_.insert(ascii_lower(
                    std::string_view(bytes_).substr(offset, line_end - offset)));
            }
            if (newline == std::string::npos) break;
            offset = newline + 1U;
        }
        ready_ = entry_count_ == expected_wordlist_entries;
        if (!ready_) {
            entries_.clear();
            entry_count_ = 0;
        }
    }

    std::string bytes_;
    std::unordered_set<std::string> entries_;
    std::size_t entry_count_{0};
    bool ready_{false};
};

PinnedSecListsBlocklist::PinnedSecListsBlocklist()
    : impl_(std::make_unique<Impl>(std::string_view(
        reinterpret_cast<const char*>(embedded::password_list),sizeof(embedded::password_list)))) {}

PinnedSecListsBlocklist::PinnedSecListsBlocklist(std::filesystem::path wordlist_file)
    : impl_(std::make_unique<Impl>(wordlist_file)) {}

PinnedSecListsBlocklist::~PinnedSecListsBlocklist() = default;

bool PinnedSecListsBlocklist::ready() const noexcept { return impl_->ready_; }

bool PinnedSecListsBlocklist::contains_complete_case_insensitive(
    const std::string_view candidate) const {
    return impl_->ready_ && impl_->entries_.find(ascii_lower(candidate)) != impl_->entries_.end();
}

std::size_t PinnedSecListsBlocklist::entry_count() const noexcept { return impl_->entry_count_; }

class SodiumPasswordKdf::Impl final {
public:
    Impl(const std::size_t maximum_active_jobs, const std::size_t queue_capacity)
        : maximum_active_jobs_(maximum_active_jobs), queue_capacity_(queue_capacity) {
        health_.maximum_active_jobs = maximum_active_jobs_;
        health_.queue_capacity = queue_capacity_;
        if (maximum_active_jobs_ == 0 || maximum_active_jobs_ > application::kMaximumConcurrentKdfJobs ||
            queue_capacity_ == 0 || queue_capacity_ > 4096 || sodium_init() < 0) {
            return;
        }
        try {
            health_.accepting = true;
            for (std::size_t index = 0; index < maximum_active_jobs_; ++index) {
                workers_.emplace_back([this] { worker(); });
            }
        } catch (...) {
            health_.accepting = false;
            stopping_ = true;
            condition_.notify_all();
            for (auto& worker_thread : workers_) {
                if (worker_thread.joinable()) worker_thread.join();
            }
            workers_.clear();
        }
    }

    ~Impl() { shutdown(); }

    bool enqueue(QueueJob job) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!health_.accepting || stopping_) return false;
        if (queue_.size() >= queue_capacity_) {
            ++health_.rejected_jobs;
            return false;
        }
        queue_.push_back(std::move(job));
        health_.queued_jobs = queue_.size();
        health_.queue_high_water = std::max(health_.queue_high_water, queue_.size());
        condition_.notify_one();
        return true;
    }

    void worker() {
        while (true) {
            QueueJob job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop_front();
                health_.queued_jobs = queue_.size();
                ++health_.active_jobs;
                health_.active_high_water = std::max(health_.active_high_water, health_.active_jobs);
                health_.estimated_memory_high_water_bytes = std::max(
                    health_.estimated_memory_high_water_bytes,
                    health_.active_jobs * static_cast<std::size_t>(application::kArgon2idMemoryKib) * 1024U);
            }
            try {
                job.run();
            } catch (...) {
                job.fail();
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --health_.active_jobs;
                ++health_.completed_jobs;
            }
        }
    }

    void note_cancelled() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        ++health_.cancelled_jobs;
    }

    application::PasswordKdfHealth health() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return health_;
    }

    void shutdown() noexcept {
        std::deque<QueueJob> cancelled;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ && workers_.empty()) return;
            health_.accepting = false;
            stopping_ = true;
            cancelled.swap(queue_);
            health_.queued_jobs = 0;
        }
        for (auto& job : cancelled) {
            job.cancel();
            note_cancelled();
        }
        condition_.notify_all();
        for (auto& worker_thread : workers_) {
            if (worker_thread.joinable()) worker_thread.join();
        }
        workers_.clear();
    }

    std::size_t maximum_active_jobs_;
    std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<QueueJob> queue_;
    std::vector<std::thread> workers_;
    application::PasswordKdfHealth health_;
    bool stopping_{false};
};

SodiumPasswordKdf::SodiumPasswordKdf(
    const std::size_t maximum_active_jobs,
    const std::size_t queue_capacity)
    : impl_(std::make_unique<Impl>(maximum_active_jobs, queue_capacity)) {}

SodiumPasswordKdf::~SodiumPasswordKdf() = default;

application::PasswordHashResult SodiumPasswordKdf::create(
    const std::string_view password,
    const std::int64_t created_utc_seconds,
    const bool password_change_required,
    const application::CancellationToken& cancellation) {
    if (!printable_password(password) || !application::valid_utc_seconds(created_utc_seconds)) {
        return {application::PasswordKdfCode::unavailable, std::nullopt};
    }
    auto secret = std::make_shared<SensitivePassword>(password);
    auto promise = std::make_shared<std::promise<application::PasswordHashResult>>();
    std::future<application::PasswordHashResult> result = promise->get_future();
    const application::CancellationToken token = cancellation;
    QueueJob job;
    job.run = [this, secret, promise, token, created_utc_seconds, password_change_required] {
        if (token.is_cancelled()) {
            impl_->note_cancelled();
            promise->set_value({application::PasswordKdfCode::cancelled, std::nullopt});
            return;
        }
        std::vector<std::uint8_t> salt(application::kArgon2idSaltBytes);
        std::vector<std::uint8_t> verifier(application::kArgon2idVerifierBytes);
        randombytes_buf(salt.data(), salt.size());
        const int code = crypto_pwhash(
            verifier.data(),
            static_cast<unsigned long long>(verifier.size()),
            secret->data(),
            static_cast<unsigned long long>(secret->size()),
            salt.data(),
            application::kArgon2idIterations,
            static_cast<std::size_t>(application::kArgon2idMemoryKib) * 1024U,
            crypto_pwhash_ALG_ARGON2ID13);
        if (code != 0 || token.is_cancelled()) {
            sodium_memzero(verifier.data(), verifier.size());
            if (token.is_cancelled()) impl_->note_cancelled();
            promise->set_value({token.is_cancelled()
                    ? application::PasswordKdfCode::cancelled
                    : application::PasswordKdfCode::unavailable,
                std::nullopt});
            return;
        }
        auto envelope = application::make_argon2id_credential_envelope(
            {}, std::move(salt), std::move(verifier), created_utc_seconds, password_change_required);
        promise->set_value({application::PasswordKdfCode::completed, std::move(envelope)});
    };
    job.cancel = [promise] {
        promise->set_value({application::PasswordKdfCode::cancelled, std::nullopt});
    };
    job.fail = [promise] {
        promise->set_value({application::PasswordKdfCode::unavailable, std::nullopt});
    };
    if (!impl_->enqueue(std::move(job))) {
        return {application::PasswordKdfCode::backpressure, std::nullopt};
    }
    return result.get();
}

application::PasswordVerifyResult SodiumPasswordKdf::verify(
    const application::CredentialEnvelope& credential,
    const std::string_view candidate,
    const application::CancellationToken& cancellation) {
    const application::DecodedCredentialEnvelope decoded =
        application::decode_argon2id_credential_envelope(credential);
    if (!decoded.valid()) {
        return {envelope_error_code(decoded.code), false, false};
    }
    if (!printable_password(candidate)) {
        return {application::PasswordKdfCode::completed, false, false};
    }
    auto secret = std::make_shared<SensitivePassword>(candidate);
    auto promise = std::make_shared<std::promise<application::PasswordVerifyResult>>();
    std::future<application::PasswordVerifyResult> result = promise->get_future();
    const application::CancellationToken token = cancellation;
    const application::CredentialEnvelope envelope = credential;
    QueueJob job;
    job.run = [this, secret, promise, token, envelope, decoded] {
        if (token.is_cancelled()) {
            impl_->note_cancelled();
            promise->set_value({application::PasswordKdfCode::cancelled, false, false});
            return;
        }
        std::vector<std::uint8_t> derived(decoded.profile.verifier_bytes);
        const int code = crypto_pwhash(
            derived.data(),
            static_cast<unsigned long long>(derived.size()),
            secret->data(),
            static_cast<unsigned long long>(secret->size()),
            envelope.salt.data(),
            decoded.profile.iterations,
            static_cast<std::size_t>(decoded.profile.memory_kib) * 1024U,
            crypto_pwhash_ALG_ARGON2ID13);
        if (code != 0 || token.is_cancelled()) {
            sodium_memzero(derived.data(), derived.size());
            if (token.is_cancelled()) impl_->note_cancelled();
            promise->set_value({token.is_cancelled()
                    ? application::PasswordKdfCode::cancelled
                    : application::PasswordKdfCode::unavailable,
                false,
                false});
            return;
        }
        const bool verified = sodium_memcmp(
            derived.data(), envelope.verifier.data(), derived.size()) == 0;
        sodium_memzero(derived.data(), derived.size());
        promise->set_value({
            application::PasswordKdfCode::completed,
            verified,
            verified && application::credential_needs_rehash(envelope, {})});
    };
    job.cancel = [promise] {
        promise->set_value({application::PasswordKdfCode::cancelled, false, false});
    };
    job.fail = [promise] {
        promise->set_value({application::PasswordKdfCode::unavailable, false, false});
    };
    if (!impl_->enqueue(std::move(job))) {
        return {application::PasswordKdfCode::backpressure, false, false};
    }
    return result.get();
}

application::PasswordKdfHealth SodiumPasswordKdf::health() const { return impl_->health(); }

void SodiumPasswordKdf::shutdown() noexcept { impl_->shutdown(); }

std::array<std::uint8_t, 32> rate_limit_subject_hash(
    const application::RateLimitScope scope,
    const std::string_view normalized_subject) {
    if (sodium_init() < 0) return {};
    std::string input = "SFX/AuthenticationRateSubject/v1";
    input.push_back('\0');
    input += application::storage_code(scope);
    input.push_back('\0');
    input.append(normalized_subject.data(), normalized_subject.size());
    std::array<std::uint8_t, 32> output{};
    (void)crypto_hash_sha256(
        output.data(),
        reinterpret_cast<const unsigned char*>(input.data()),
        static_cast<unsigned long long>(input.size()));
    sodium_memzero(input.data(), input.size());
    return output;
}

}  // namespace sfx::adapters::authentication
