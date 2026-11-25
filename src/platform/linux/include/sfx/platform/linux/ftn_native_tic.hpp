#pragma once
#include "sfx/application/phase3a_private_adapters.hpp"
#include "sfx/platform/linux/ftn_native_session.hpp"
namespace sfx::platform::linux_host {
// Native owner-only result. Supplied TIC password is erased after its separate
// check; neither a directory name nor the claimed From address is peer proof.
struct FtnNativeTicReview {
    application::FtnTic tic;
    application::FtnPeerMatch source;
    application::FilePayloadIdentity identity;
    bool password_verified{},authenticated_same_link{};
    std::optional<FilePayloadLease> lease; // pin verified original through publication
};
FtnNativeTicReview inspect_ftn_native_tic(application::IFtnMailStorage&,
    const application::FtnConfiguration&,const FtnInstallation&,
    const application::FtnIngressReceipt&,const std::function<bool()>& running,
    const application::Identifier128& reviewed_attempt = {});
// Reopens the exact checkpoint-bound native object and returns a CRC computed
// in bounded chunks while the caller retains the verified read lease.
struct FtnNativeTicPayload {
    FilePayloadLease lease;
    application::FilePayloadIdentity identity;
    std::uint32_t crc{};
};
FtnNativeTicPayload inspect_ftn_native_tic_payload(application::IFtnMailStorage&,
    const FtnInstallation&,const application::FtnIngressReceipt&,
    const application::FtnTic&,const std::function<bool()>& running);
struct FtnNativeTicPair {application::FtnIngressReceipt receipt;FtnNativeTicPayload payload;};
// A missing payload remains pending; multiple different same-name payloads are
// ambiguous. The returned lease remains pinned through the publication journal.
std::optional<FtnNativeTicPair> pair_ftn_native_tic_payload(application::IFtnMailStorage&,
    const FtnInstallation&,const application::FtnIngressReceipt& tic_receipt,
    const application::FtnTic&,const std::function<bool()>& running);
struct FtnNativeTossResult;
// A boundary hook may stop only this isolated operation; normal Runtime omits
// it. It cannot substitute native identities or authorize a filesystem action.
FtnNativeTossResult publish_ftn_native_tic(application::IFtnMailStorage&,
    application::IFileStorage&,const application::FtnConfiguration&,
    const FtnInstallation&,const application::FtnIngressReceipt&,
    const std::function<bool()>& running,
    const std::function<void(std::string_view)>& boundary = {},
    bool force_displacement_copy = false,
    const application::Identifier128& reviewed_attempt = {}); // diagnostic copy seam never fabricates a volume
FtnNativeTossResult reprocess_ftn_native_tic(application::IFtnMailStorage&,
    application::IFileStorage&,const application::FtnConfiguration&,
    const FtnInstallation&,const application::FtnTicRetryRecord&,
    const std::function<bool()>& running,
    const std::function<void(std::string_view)>& boundary = {});
}
