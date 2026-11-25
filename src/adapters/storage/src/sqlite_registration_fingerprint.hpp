#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "sfx/application/phase3a.hpp"

namespace sfx::adapters::storage::internal {

inline constexpr std::int64_t kRegistrationFingerprintFormatVersion = 1;
using RegistrationRequestFingerprint = std::array<std::uint8_t, 32>;

std::optional<RegistrationRequestFingerprint> registration_request_fingerprint_v1(
    const application::RegistrationCommitRequest& request) noexcept;

bool registration_request_fingerprints_equal(
    const RegistrationRequestFingerprint& left,
    const RegistrationRequestFingerprint& right) noexcept;

}  // namespace sfx::adapters::storage::internal
