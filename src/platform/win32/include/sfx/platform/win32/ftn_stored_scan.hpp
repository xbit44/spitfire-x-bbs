#pragma once
#include "sfx/platform/win32/ftn_native_custody.hpp"
namespace sfx::platform::win32 {
struct FtnStoredScanResult {std::size_t queued{},completed{},failed{};std::string reason;};
FtnStoredScanResult scan_ftn_stored_messages(application::IFtnMailStorage&,
    const application::FtnConfiguration&,const installation::Receipt&,
    std::string& after,const std::function<bool()>& running);
}
