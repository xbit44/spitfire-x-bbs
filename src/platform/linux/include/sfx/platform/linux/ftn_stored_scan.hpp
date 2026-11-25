#pragma once
#include "sfx/platform/linux/ftn_native_custody.hpp"
namespace sfx::platform::linux_host {
struct FtnStoredScanResult {std::size_t queued{},completed{},failed{};std::string reason;};
FtnStoredScanResult scan_ftn_stored_messages(application::IFtnMailStorage&,
    const application::FtnConfiguration&,const FtnInstallation&,
    std::string& after,const std::function<bool()>& running);
}
