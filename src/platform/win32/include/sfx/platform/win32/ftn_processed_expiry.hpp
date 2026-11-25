#pragma once
#include "sfx/application/ftn_retention.hpp"
#include "sfx/platform/win32/installation_security.hpp"
namespace sfx::application {class IFtnMailStorage;}
namespace sfx::platform::win32 {
struct FtnRetentionPageResult {
    std::uint32_t examined{},removed{},reconciled{};
    std::string reason;
};
// Called only by the existing joined FTN processor. At most four recorded
// originals, never directory enumeration or a separate cleanup worker.
FtnRetentionPageResult process_ftn_retention_page(application::IFtnMailStorage&,
    const installation::Receipt&,const std::shared_ptr<std::atomic_bool>&,
    application::Identifier128& cursor);
// No arbitrary path/handle constructor. Exact Processed path comes only from
// the verified installation and retained ingress/network identity.
class FtnProcessedExpiry final {
public:
    enum class Observation { exact,missing,changed };
    static std::unique_ptr<FtnProcessedExpiry> inspect(const installation::Receipt&,const application::FtnRetainedOriginal&,std::shared_ptr<std::atomic_bool> cancelled);
    ~FtnProcessedExpiry();
    Observation observation()const noexcept;
    // After a confirmed durable intent commit, transfer the SAME exclusive
    // verified lease; no close/reopen or second verification-to-use gap.
    std::shared_ptr<application::FtnRetentionRemoval> seal(const application::FtnExpiryIntent&);
    // Host calls only after authoritative execute success, outside the writer's
    // no-allocation/no-lock native commit callback. Uncertain outcomes retain
    // the original namespace proof for review.
    bool record_confirmed_removal(const std::shared_ptr<application::FtnRetentionRemoval>&)noexcept;
private:
    struct Impl;explicit FtnProcessedExpiry(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
}
