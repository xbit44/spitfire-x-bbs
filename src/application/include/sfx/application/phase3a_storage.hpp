#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sfx/application/account_storage.hpp"

namespace sfx::application {

inline constexpr std::size_t kFailedLoginNoticeMaximum = 5;
inline constexpr std::int64_t kFailedLoginClaimLeaseSeconds = 300;
inline constexpr std::size_t kFailedLoginEnqueueReceiptMaximum = 64;
inline constexpr std::size_t kFailedLoginTerminalClaimReceiptMaximum = 64;

enum class TransferProtocolPreference : std::uint8_t {
    xmodem,
    ymodem,
    ymodem_g,
    zmodem,
    select_at_transfer
};

std::string_view storage_code(TransferProtocolPreference value) noexcept;
std::optional<TransferProtocolPreference> transfer_preference_from_storage_code(
    std::string_view value) noexcept;

struct CanonicalSourceAddress {
    std::uint8_t family{0};
    std::array<std::uint8_t, 16> bytes{};
    std::uint8_t size{0};
    std::string text;
};

std::optional<CanonicalSourceAddress> canonicalize_source_ip(std::string_view value);
std::optional<std::string> canonical_source_ip(
    std::uint8_t family,
    const std::vector<std::uint8_t>& bytes);

struct TransferPreferenceRecord {
    AccountId account_id{0};
    TransferProtocolPreference preference{TransferProtocolPreference::select_at_transfer};
    std::int64_t revision{0};
    std::int64_t modified_utc_seconds{0};
};

struct UpdateTransferPreferenceRequest {
    AccountId account_id{0};
    std::int64_t expected_revision{0};
    TransferProtocolPreference preference{TransferProtocolPreference::select_at_transfer};
    std::int64_t modified_utc_seconds{0};
};

struct FailedLoginNoticeRecord {
    Identifier128 notice_id;
    AccountId account_id{0};
    std::string source_ip;
    std::int64_t event_utc_seconds{0};
};

struct EnqueueFailedLoginNoticeRequest {
    Identifier128 notice_id;
    AccountId account_id{0};
    std::string source_ip;
    std::int64_t event_utc_seconds{0};
};

struct ClaimFailedLoginNoticesRequest {
    AccountId account_id{0};
    Identifier128 claim_id;
    Identifier128 session_id;
    std::int64_t claimed_utc_seconds{0};
    std::size_t limit{kFailedLoginNoticeMaximum};
};

struct FailedLoginNoticeBatch {
    Identifier128 claim_id;
    Identifier128 session_id;
    std::vector<FailedLoginNoticeRecord> notices;
};

struct AcknowledgeFailedLoginNoticesRequest {
    AccountId account_id{0};
    Identifier128 claim_id;
    Identifier128 session_id;
};

struct RecoverFailedLoginClaimsRequest {
    std::int64_t now_utc_seconds{0};
    std::size_t limit{256};
};

class IPhase3aStorage {
public:
    virtual ~IPhase3aStorage() = default;

    virtual StorageResult<TransferPreferenceRecord> transfer_preference(AccountId account_id) = 0;
    virtual MutationResult update_transfer_preference(
        const UpdateTransferPreferenceRequest& request) = 0;
    virtual StorageStatus enqueue_failed_login_notice(
        const EnqueueFailedLoginNoticeRequest& request) = 0;
    virtual StorageResult<FailedLoginNoticeBatch> claim_failed_login_notices(
        const ClaimFailedLoginNoticesRequest& request) = 0;
    virtual StorageStatus acknowledge_failed_login_notices(
        const AcknowledgeFailedLoginNoticesRequest& request) = 0;
    virtual StorageResult<std::size_t> recover_abandoned_failed_login_claims(
        const RecoverFailedLoginClaimsRequest& request) = 0;
};

}  // namespace sfx::application
