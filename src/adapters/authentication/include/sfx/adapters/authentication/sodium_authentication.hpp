#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>

#include "sfx/application/authentication_security.hpp"

namespace sfx::adapters::authentication {

class PinnedSecListsBlocklist final : public application::IPasswordBlocklist {
public:
    PinnedSecListsBlocklist(); // reviewed, hash-checked bytes embedded at build time
    explicit PinnedSecListsBlocklist(std::filesystem::path wordlist_file);
    ~PinnedSecListsBlocklist() override;

    PinnedSecListsBlocklist(const PinnedSecListsBlocklist&) = delete;
    PinnedSecListsBlocklist& operator=(const PinnedSecListsBlocklist&) = delete;

    bool ready() const noexcept override;
    bool contains_complete_case_insensitive(std::string_view candidate) const override;
    std::size_t entry_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class SodiumPasswordKdf final : public application::IPasswordKdf {
public:
    explicit SodiumPasswordKdf(
        std::size_t maximum_active_jobs = application::kMaximumConcurrentKdfJobs,
        std::size_t queue_capacity = 64);
    ~SodiumPasswordKdf() override;

    SodiumPasswordKdf(const SodiumPasswordKdf&) = delete;
    SodiumPasswordKdf& operator=(const SodiumPasswordKdf&) = delete;

    application::PasswordHashResult create(
        std::string_view password,
        std::int64_t created_utc_seconds,
        bool password_change_required,
        const application::CancellationToken& cancellation) override;
    application::PasswordVerifyResult verify(
        const application::CredentialEnvelope& credential,
        std::string_view candidate,
        const application::CancellationToken& cancellation) override;
    application::PasswordKdfHealth health() const override;
    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::array<std::uint8_t, 32> rate_limit_subject_hash(
    application::RateLimitScope scope,
    std::string_view normalized_subject);

}  // namespace sfx::adapters::authentication
