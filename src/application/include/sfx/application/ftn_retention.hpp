#pragma once
#include "sfx/application/ftn_configuration_transport.hpp"
#include "sfx/application/file_storage.hpp"
#include <atomic>
#include <memory>

namespace sfx::platform::win32 { class FtnProcessedExpiry; }
namespace sfx::platform::linux_host { class FtnProcessedExpiry; }
namespace sfx::adapters::storage::internal { class SqliteWriterCoordinator; }
namespace sfx::application {
inline constexpr std::size_t kFtnRetentionReviewMaximum=4096;
struct FtnRetentionNetworkReview {
    std::string network,name;
    std::uint32_t due_now{},expire_later{},from_forever{};
    std::int64_t earliest{},latest{};
};
// Owner projection contains no native paths, object handles or deletion grant.
struct FtnRetentionPreview {
    FtnTransferToken token{};std::int64_t configuration_revision{},reviewed_utc{};
    std::array<std::uint8_t,32> draft{};
    std::uint32_t changed{},shortened{};
    std::vector<FtnRetentionNetworkReview> networks;
};
std::vector<std::uint8_t> encode_ftn_retention_preview(const FtnRetentionPreview&);
std::optional<FtnRetentionPreview> decode_ftn_retention_preview(const std::vector<std::uint8_t>&);

// Host-only provenance; never decoded from owner IPC.
struct FtnRetainedOriginal {
    Identifier128 ingress;std::string network;FilePayloadIdentity native;
    std::int64_t successful_utc{},configuration_revision{},protection_revision{},deadline{};
    std::string wire_name;
    // deadline==0 is Forever. Off applies only at successful processing and
    // records deadline==successful_utc, never rewrites a previous protection.
};
enum class FtnExpiryOutcome : std::uint8_t { prepared,removed_confirmed,absent_uncertain,changed_held,native_failed,policy_held,present_reconciled };
struct FtnExpiryIntent {
    Identifier128 operation,dispatch;FtnRetainedOriginal original;
    std::int64_t prepared_utc{};FtnExpiryOutcome outcome{FtnExpiryOutcome::prepared};
    std::uint32_t native_error{};
};
enum class FtnExpiryNativeState : std::uint8_t { not_invoked,invoking,closed,disposition_failed,close_unconfirmed };
struct FtnExpiryNativeResult {FtnExpiryNativeState state{FtnExpiryNativeState::not_invoked};std::uint32_t error{};};

// Sealed to one verified Processed object and committed intent/dispatch. Only
// the native host factory can construct it, and only the dedicated singleton
// writer coordinator can invoke it. Ordinary WriteRequest remains SQL-only.
class FtnRetentionRemoval final {
public:
    ~FtnRetentionRemoval()=default;
    FtnRetentionRemoval(const FtnRetentionRemoval&)=delete;
    FtnRetentionRemoval& operator=(const FtnRetentionRemoval&)=delete;
    const FtnExpiryIntent& intent()const noexcept{return intent_;}
    bool cancelled()const noexcept{return !cancel_||cancel_->load();}
    FtnExpiryNativeResult result()const noexcept{return {state_.load(std::memory_order_acquire),error_.load()};}
private:
    struct Native {
        virtual ~Native()=default;
        virtual FtnExpiryNativeResult disposition_and_close()noexcept=0;
        virtual bool close_at_shutdown()noexcept=0;
    };
    FtnRetentionRemoval(FtnExpiryIntent intent,std::unique_ptr<Native> native,std::shared_ptr<std::atomic_bool> cancel):intent_(std::move(intent)),native_(std::move(native)),cancel_(std::move(cancel)){}
    bool invoke()noexcept{
        if(consumed_.exchange(true))return false;
        state_.store(FtnExpiryNativeState::invoking,std::memory_order_release);
        const auto outcome=native_->disposition_and_close();error_.store(outcome.error);
        state_.store(outcome.state,std::memory_order_release);return true;
    }
    FtnExpiryIntent intent_;std::unique_ptr<Native> native_;std::shared_ptr<std::atomic_bool> cancel_;
    std::atomic_bool consumed_{};std::atomic<FtnExpiryNativeState> state_{FtnExpiryNativeState::not_invoked};std::atomic<std::uint32_t> error_{};
    friend class sfx::platform::win32::FtnProcessedExpiry;
    friend class sfx::platform::linux_host::FtnProcessedExpiry;
    friend class sfx::adapters::storage::internal::SqliteWriterCoordinator;
};
}
