#pragma once
#include "sfx/application/ftn_retention.hpp"
#include "sfx/platform/linux/ftn_installation.hpp"
namespace sfx::application {class IFtnMailStorage;}
namespace sfx::platform::linux_host {
struct FtnRetentionPageResult {
    std::uint32_t examined{},removed{},reconciled{};
    std::string reason;
};
FtnRetentionPageResult process_ftn_retention_page(application::IFtnMailStorage&,
    const FtnInstallation&,const std::shared_ptr<std::atomic_bool>&,
    application::Identifier128& cursor);
// Factory for one retained original, not a general path deletion capability.
// Linux first captures the exact original in a recoverable private directory
// after durable intent. Only the singleton writer may unlink that capture.
class FtnProcessedExpiry final {
public:
    enum class Observation {exact,missing,changed};
    static std::unique_ptr<FtnProcessedExpiry> inspect(const FtnInstallation&,
        const application::FtnRetainedOriginal&,std::shared_ptr<std::atomic_bool>);
    ~FtnProcessedExpiry();
    Observation observation()const noexcept;
    std::shared_ptr<application::FtnRetentionRemoval> seal(const application::FtnExpiryIntent&);
    // Call only after execute/future completion, outside the writer. A completed
    // queue item can still own the capability briefly; do not rely on its
    // destructor timing to restore a denied/cancelled capture.
    static bool restore_after_dispatch(const std::shared_ptr<application::FtnRetentionRemoval>&)noexcept;
    bool record_confirmed_removal(const std::shared_ptr<application::FtnRetentionRemoval>&)noexcept;
private:
    struct Impl;explicit FtnProcessedExpiry(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
