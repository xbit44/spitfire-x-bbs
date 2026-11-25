#pragma once
#include "sfx/application/phase3a_private_adapters.hpp"
#include "sfx/platform/linux/ftn_native_session.hpp"
namespace sfx::platform::linux_host {

struct FtnNativeTossResult {bool processed{},held{},deferred{};std::uint32_t imported{},replayed{};std::string reason;bool configuration_changed{},completion_reconciled{};std::vector<std::string> diagnostics;};
struct FtnNativeHatchResult {bool queued{},journaled{};std::string reason;};
// Uses the same bounded archive/packet decoder as the tosser; returns only
// envelope addresses, never packet passwords or message contents.
std::vector<std::pair<application::FtnAddress,application::FtnAddress>> inspect_ftn_bundle_routes(std::string_view);
FtnNativeHatchResult hatch_ftn_native_file(application::IFtnMailStorage&,application::IFileStorage&,
    const FtnInstallation&,const application::FtnHatchRequest&,
    const std::function<bool()>& running,
    const std::function<bool(std::uint64_t,std::uint64_t)>& progress={},
    const std::function<void(std::string_view)>& boundary={});
// One receipt, bounded native file leases, actual packet credentials and atomic
// managed import/forwarding intents. No received ACK is a semantic acceptance.
FtnNativeTossResult toss_ftn_native_receipt(application::IFtnMailStorage&,
    const application::FtnConfiguration&,const FtnInstallation&,
    const application::FtnIngressReceipt&,const std::function<bool()>& running);
FtnNativeTossResult reprocess_ftn_native_receipt(application::IFtnMailStorage&,
    const application::FtnConfiguration&,const FtnInstallation&,
    const application::Identifier128& request,const std::function<bool()>& running);
FtnNativeTossResult scan_ftn_native_intent(application::IFtnMailStorage&,
    const application::FtnConfiguration&,const FtnInstallation&,
    const application::FtnScanIntent&,const std::function<bool()>& running,
    const application::BbsLocalTimeAdapters* timezone = nullptr);
FtnNativeTossResult scan_ftn_native_control(application::IFtnMailStorage&,
    const application::FtnConfiguration&,const FtnInstallation&,
    const application::FtnControlIntent&,const std::function<bool()>& running,
    const application::BbsLocalTimeAdapters* timezone = nullptr);
FtnNativeTossResult scan_ftn_native_rescan(application::IFtnMailStorage&,
    const application::FtnConfiguration&,const FtnInstallation&,
    const application::FtnRescanItem&,const std::function<bool()>& running,
    const application::BbsLocalTimeAdapters* timezone = nullptr);
}
